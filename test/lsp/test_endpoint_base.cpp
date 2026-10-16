#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <test/test.h>
#include "loopback_stream.h"
#include <lsp/connection.h>
#include <lsp/endpoint_base.h>
#include <lsp/server_endpoint.h>
#include <lsp/error.h>
#include <lsp/json/json.h>
#include <lsp/message_base.h>
#include <lsp/message_handler.h>
#include <lsp/types.h>

using namespace lsp;
using lsptest::LoopbackStream;

namespace{

struct DummyMessage{};

// Mirrors the internal state of the endpoint which is not accessible from outside
enum class State{ Uninitialized, Active, Shutdown, Inactive };

void gotoState(ServerEndpointBase& server, State state)
{
	if(state == State::Uninitialized)
		return;

	server.preMethodCall<requests::Initialize>(InitializeParams{});
	server.postMethodCall<requests::Initialize>();

	if(state == State::Shutdown)
		server.preMethodCall<requests::Shutdown>();
	else if(state == State::Inactive)
		server.preMethodCall<notifications::Exit>();
}

/*
 * Counts pre/post method calls and exposes the protected message hook.
 */
class HookedEndpoint : public EndpointBase{
public:
	using EndpointBase::EndpointBase;

	int preCount  = 0;
	int postCount = 0;

	template<typename M>
	void preMethodCall(){ ++preCount; }

	template<typename M>
	void postMethodCall(){ ++postCount; }

	template<typename M, typename F>
	void withHook(F&& f)
	{
		const auto hook = messageHook<M>(*this);
		f();
	}
};

template<typename F>
void expectRequestError(F&& f, int expectedCode, std::string_view expectedMessage)
{
	try
	{
		f();
		test::fail("Expected RequestError to be thrown");
	}
	catch(const RequestError& e)
	{
		test::compare(e.code(), expectedCode);
		test::compare(e.message(), expectedMessage);
	}
}

} // namespace

int main(int argc, char** argv)
{
	auto app = test::TestApp();

	/*
	 * EndpointBase (exercised through ServerEndpointBase, the concrete vehicle,
	 * since bare EndpointBase can't leave the Uninitialized state via public API)
	 */

	app.addTest("EndpointBase/StartsUninitialized", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		test::check(server.isActive(), "active");
		test::check(!server.isInitialized(), "uninitialized");
	});

	app.addTest("EndpointBase/IsActive", [](State state, bool expected){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		gotoState(server, state);
		test::compare(server.isActive(), expected);
	})({
		{"Uninitialized", {State::Uninitialized, true }},
		{"Active",        {State::Active,        true }},
		{"Shutdown",      {State::Shutdown,      true }},
		{"Inactive",      {State::Inactive,      false}},
	});

	app.addTest("EndpointBase/ProcessNextMessageSwallowsConnectionErrorWhenInactive", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		// No more messages are expected once inactive, so a dropped connection is not an error
		stream.close();
		gotoState(server, State::Inactive);
		server.processNextMessage();

		test::compare(stream.readCount, 1);
	});

	app.addTest("EndpointBase/ProcessNextMessageRethrowsAndDeactivatesWhenActive", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		stream.close();
		gotoState(server, State::Active);
		test::expectException<ConnectionError>([&](){ server.processNextMessage(); });

		test::compare(stream.readCount, 1);

		// A connection error while active marks the endpoint inactive before propagating
		test::check(!server.isActive(), "inactive");
	});

	app.addTest("EndpointBase/RunMessageLoopReturnsImmediatelyWhenInactive", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		gotoState(server, State::Inactive);
		server.runMessageLoop();

		test::compare(stream.readCount, 0);
	});

	app.addTest("EndpointBase/RunMessageLoopRethrowsWhileActive", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		stream.close();
		gotoState(server, State::Active);
		test::expectException<ConnectionError>([&](){ server.runMessageLoop(); });

		// The connection error propagates out of the loop, leaving the endpoint inactive
		test::check(!server.isActive(), "inactive");
	});

	app.addTest("EndpointBase/RunMessageLoopSwallowsConnectionErrorAfterExit", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		gotoState(server, State::Active);

		// Simulate the connection dropping right as the endpoint transitions to Inactive
		stream.close();
		stream.onRead = [&server](){ server.preMethodCall<notifications::Exit>(); };

		server.runMessageLoop();

		test::compare(stream.readCount, 1);
	});

	app.addTest("EndpointBase/RunMessageLoopProcessesUntilInactive", [](){
		auto stream  = LoopbackStream();
		auto server  = ServerEndpointBase(stream);
		auto handled = 0;

		gotoState(server, State::Active);

		server.onCustomNotification("test/stop", [&](json::Value&&)
		{
			++handled;
			server.preMethodCall<notifications::Exit>();
		});

		server.customNotification("test/stop", json::Value(json::Object()));
		server.runMessageLoop();

		test::compare(handled, 1);
		test::check(!server.isActive(), "inactive");
	});

	app.addTest("EndpointBase/MessageHookCallsPreAndPost", [](){
		auto stream   = LoopbackStream();
		auto endpoint = HookedEndpoint(stream);

		endpoint.withHook<DummyMessage>([&]()
		{
			test::compare(endpoint.preCount, 1);
			test::compare(endpoint.postCount, 0);
		});

		test::compare(endpoint.preCount, 1);
		test::compare(endpoint.postCount, 1);
	});

	/*
	 * ClientEndpointBase
	 */

	app.addTest("Client/GenericCallRequiresInitialize", [](){
		auto stream = LoopbackStream();
		auto client = ClientEndpointBase(stream);

		test::expectException<std::logic_error>(
			[&](){ client.preMethodCall<DummyMessage>(); },
			"Initialize request must be sent first");
	});

	app.addTest("Client/InitializeActivates", [](){
		auto stream = LoopbackStream();
		auto client = ClientEndpointBase(stream);

		client.postMethodCall<requests::Initialize>();
		test::check(client.isActive(), "active");

		// Generic calls are allowed once active
		client.preMethodCall<DummyMessage>();
	});

	app.addTest("Client/ShutdownBlocksFurtherCalls", [](){
		auto stream = LoopbackStream();
		auto client = ClientEndpointBase(stream);

		client.postMethodCall<requests::Initialize>();
		client.preMethodCall<requests::Shutdown>();
		test::check(client.isActive(), "still active after shutdown");

		test::expectException<std::logic_error>(
			[&](){ client.preMethodCall<DummyMessage>(); },
			"Only 'exit' request must be sent after 'shutdown'");
	});

	app.addTest("Client/ExitResetsToInactive", [](){
		auto stream = LoopbackStream();
		auto client = ClientEndpointBase(stream);

		client.postMethodCall<requests::Initialize>();
		client.preMethodCall<notifications::Exit>();

		test::check(!client.isActive(), "inactive");

		// Generic calls are rejected again once inactive
		test::expectException<std::logic_error>(
			[&](){ client.preMethodCall<DummyMessage>(); },
			"Initialize request must be sent first");
	});

	/*
	 * ServerEndpointBase
	 */

	app.addTest("Server/StartsUninitialized", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		test::check(!server.isInitialized(), "!isInitialized");
	});

	app.addTest("Server/GenericCallBeforeInitializeIsRejected", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		expectRequestError(
			[&](){ server.preMethodCall<DummyMessage>(); },
			MessageError::ServerNotInitialized, "Server not initialized");
	});

	app.addTest("Server/InitializeThenActive", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		server.preMethodCall<requests::Initialize>(InitializeParams{});
		test::check(!server.isInitialized(), "notInitializedUntilPost");

		server.postMethodCall<requests::Initialize>();
		test::check(server.isInitialized(), "isInitialized");
		test::check(server.isActive(), "active");

		// Generic calls are allowed once active
		server.preMethodCall<DummyMessage>();
	});

	app.addTest("Server/DoubleInitializeIsRejected", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		gotoState(server, State::Active);

		expectRequestError(
			[&](){ server.preMethodCall<requests::Initialize>(InitializeParams{}); },
			MessageError::InvalidRequest, "Server already initialized");
	});

	app.addTest("Server/InitializeTraceLevels", [](std::string trace, int expectedLogs, bool expectedPayload){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);
		server.messageHandler().setMessageLogLevel(MessageHandler::MessageLogLevel::InfoAndPayload);

		auto logs = 0;
		auto hasPayload = false;
		server.messageHandler().addMessageLogCallback([&](const MessageHandler::MessageLog& log){
			if(log.method == "test/trace")
			{
				++logs;
				hasPayload = log.payload.has_value();
			}
		});

		auto params = InitializeParams();
		params.trace.emplace(std::move(trace));
		server.preMethodCall<requests::Initialize>(params);
		server.customNotification("test/trace", json::Object({{"value", 1}}));

		test::compare(logs, expectedLogs);
		test::compare(hasPayload, expectedPayload);
	})({
		{"Off", {"off", 0, false}},
		{"Messages", {"messages", 1, false}},
		{"Verbose", {"verbose", 1, true}},
		{"CustomKeepsPreviousLevel", {"custom", 1, true}},
	});

	app.addTest("Server/ShutdownRequiresInitialize", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		expectRequestError(
			[&](){ server.preMethodCall<requests::Shutdown>(); },
			MessageError::ServerNotInitialized, "Server not initialized");
	});

	app.addTest("Server/ShutdownBlocksFurtherCalls", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		gotoState(server, State::Shutdown);
		test::check(server.isActive(), "still active after shutdown");

		expectRequestError(
			[&](){ server.preMethodCall<DummyMessage>(); },
			MessageError::InvalidRequest, "Server has received shutdown request");
	});

	app.addTest("Server/ExitDeactivates", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);

		gotoState(server, State::Inactive);

		test::check(!server.isActive(), "inactive");
	});

	/*
	 * Custom messages
	 */

	app.addTest("Custom/RequestRoundTrip", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);
		gotoState(server, State::Active);

		auto receivedPing = 0;
		server.onCustomRequest("test/echo", [&](json::Value&& params) -> json::Value
		{
			receivedPing = params.object().get("ping").integer();
			return json::Object({{"pong", receivedPing}});
		});

		auto pong      = 0;
		auto gotResult = false;
		server.customRequest("test/echo", json::Object({{"ping", 7}}),
			[&](json::Value&& result){ pong = result.object().get("pong").integer(); gotResult = true; });

		server.processNextMessage(); // handle the request, send the response
		server.processNextMessage(); // handle the response, invoke the callback

		test::check(gotResult, "gotResult");
		test::compare(receivedPing, 7);
		test::compare(pong, 7);
	});

	app.addTest("Custom/RequestNoParamsRoundTrip", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);
		gotoState(server, State::Active);

		auto handled = false;
		server.onCustomRequest("test/ping", [&]() -> json::Value
		{
			handled = true;
			return json::Value(json::Integer(1));
		});

		auto result = 0;
		server.customRequest("test/ping", [&](json::Value&& r){ result = r.integer(); });

		server.processNextMessage();
		server.processNextMessage();

		test::check(handled, "handled");
		test::compare(result, 1);
	});

	app.addTest("Custom/NotificationRoundTrip", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);
		gotoState(server, State::Active);

		auto received = 0;
		auto notified = false;
		server.onCustomNotification("test/touch", [&](json::Value&& params)
		{
			received = params.object().get("v").integer();
			notified = true;
		});

		server.customNotification("test/touch", json::Object({{"v", 3}}));
		server.processNextMessage();

		test::check(notified, "notified");
		test::compare(received, 3);
	});

	app.addTest("Custom/NotificationNoParamsRoundTrip", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);
		gotoState(server, State::Active);

		auto notified = false;
		server.onCustomNotification("test/tick", [&](){ notified = true; });

		server.customNotification("test/tick");
		server.processNextMessage();

		test::check(notified, "notified");
	});

	app.addTest("Custom/CancelRequestNotificationCancelsActiveRequest", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpointBase(stream);
		gotoState(server, State::Active);

		auto started            = std::promise<void>();
		auto startedFuture      = started.get_future();
		auto released           = std::promise<void>();
		auto releasedFuture     = released.get_future();
		auto canceledDuringWork = false;

		server.onCustomRequest("test/longOp", [&](json::Value&&) -> std::future<json::Value>
		{
			return std::async(std::launch::deferred, [&]() -> json::Value
			{
				started.set_value();
				(void)releasedFuture.wait_for(std::chrono::seconds(5));
				canceledDuringWork = RequestContext::get().isCanceled();
				return json::Value(json::Null{});
			});
		});

		const auto requestId = server.customRequest("test/longOp", json::Value(json::Null{}),
			[](json::Value&&){});

		server.processNextMessage(); // Dispatch the request; deferred work starts on a worker thread

		test::check(startedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready, "workStarted");

		const auto idInt = std::get<json::Integer>(requestId);
		server.customNotification("$/cancelRequest", json::Object({{"id", idInt}}));
		server.processNextMessage(); // Handle the cancel notification

		released.set_value();
		server.processNextMessage(); // Response for test/longOp

		test::check(canceledDuringWork, "canceledDuringWork");
	});

	app.addTest("EndpointBase/MessageHookSkipsPostWhenCallbackThrows", [](){
		auto stream = LoopbackStream();
		auto endpoint = HookedEndpoint(stream);
		test::expectException<std::runtime_error>([&]{
			endpoint.withHook<DummyMessage>([]{ throw std::runtime_error("failed"); });
		});
		test::compare(endpoint.preCount, 1);
		test::compare(endpoint.postCount, 0);
	});

	app.addTest("Server/InitializeWaitsForSuccessfulDeferredResult", [](bool succeeds){
		auto stream = LoopbackStream();
		auto server = ServerEndpoint(stream);
		auto promise = std::promise<InitializeResult>();
		server.onInitialize([&](InitializeParams&&){ return promise.get_future(); });
		auto params = json::Object({{"processId", json::Null()}, {"rootUri", json::Null()}, {"capabilities", json::Object()}});
		server.customRequest("initialize", params, [](json::Value&&){}, [](const ResponseError&){});
		server.processNextMessage();
		test::check(!server.isInitialized(), "notInitializedWhilePending");

		if(succeeds)
			promise.set_value(InitializeResult());
		else
			promise.set_exception(std::make_exception_ptr(RequestError(1234, "failed")));

		server.processNextMessage();
		test::compare(server.isInitialized(), succeeds);
	})({
		{"Success", {true}},
		{"Failure", {false}},
	});

	app.addTest("Server/PendingInitializeRejectsAnotherInitialize", [](){
		auto stream = LoopbackStream();
		auto server = ServerEndpoint(stream);
		auto promise = std::promise<InitializeResult>();
		auto calls = 0;
		auto error = 0;
		server.onInitialize([&](InitializeParams&&){ ++calls; return promise.get_future(); });
		const auto params = json::Object({{"processId", json::Null()}, {"rootUri", json::Null()}, {"capabilities", json::Object()}});
		server.customRequest("initialize", params, [](json::Value&&){});
		server.processNextMessage();
		server.customRequest("initialize", params, [](json::Value&&){}, [&](const ResponseError& e){ error = e.code(); });
		server.processNextMessage();
		server.processNextMessage();
		test::compare(error, MessageError::InvalidRequest);
		test::compare(calls, 1);
		promise.set_value(InitializeResult());
		server.processNextMessage();
		test::check(server.isInitialized(), "firstInitializationSucceeded");
	});

	app.addTest("EndpointBase/LogHookCanReplaceItself", [](){
		auto stream = LoopbackStream();
		auto endpoint = EndpointBase(stream);
		auto capture = std::make_shared<int>(42);
		auto observer = std::weak_ptr<int>(capture);
		auto aliveWhileRunning = false;
		endpoint.setLogHook([&, capture = std::move(capture)](std::string_view, std::string_view){
			endpoint.setLogHook({});
			aliveWhileRunning = !observer.expired();
		});
		endpoint.messageHandler().setMessageLogLevel(MessageHandler::MessageLogLevel::Info);
		endpoint.customNotification("test/log");
		test::check(aliveWhileRunning, "runningHookRetainsItsCapture");
		test::check(observer.expired(), "oldHookReleasedAfterCall");
	});

	app.addTest("Server/InitializeResultKinds", [](int kind, bool succeeds){
		auto stream = LoopbackStream();
		auto server = ServerEndpoint(stream);
		const auto compute = [succeeds]() -> InitializeResult {
			if(!succeeds)
				throw RequestError(1234, "failed");
			return InitializeResult();
		};

		switch(kind)
		{
		case 0:
			server.onInitialize([compute](InitializeParams&&){ return compute(); });
			break;
		case 1:
			server.onInitialize([compute](InitializeParams&&){ return compute; });
			break;
		case 2:
			server.onInitialize([compute](InitializeParams&&){ return std::async(std::launch::deferred, compute); });
			break;
		default:
			server.onInitialize([compute](InitializeParams&&) -> RequestResult<InitializeResult> {
				return TaskFunction<InitializeResult>(compute);
			});
			break;
		}

		auto successful = false;
		auto error = 0;
		const auto params = json::Object({{"processId", json::Null()}, {"rootUri", json::Null()}, {"capabilities", json::Object()}});
		server.customRequest("initialize", params, [&](json::Value&&){ successful = true; }, [&](const ResponseError& e){ error = e.code(); });
		server.processNextMessage();
		server.processNextMessage();
		test::compare(successful, succeeds);
		test::compare(error, succeeds ? 0 : 1234);
		test::compare(server.isInitialized(), succeeds);

		if(!succeeds)
		{
			server.onInitialize([](InitializeParams&&){ return InitializeResult(); });
			server.customRequest("initialize", params, [&](json::Value&&){ successful = true; });
			server.processNextMessage();
			server.processNextMessage();
			test::check(successful, "retrySucceeded");
			test::check(server.isInitialized(), "initializedAfterRetry");
		}
	})({
		{"DirectSuccess", {0, true}},
		{"DirectFailure", {0, false}},
		{"CallableSuccess", {1, true}},
		{"CallableFailure", {1, false}},
		{"FutureSuccess", {2, true}},
		{"FutureFailure", {2, false}},
		{"ExplicitSuccess", {3, true}},
		{"ExplicitFailure", {3, false}},
	});

	app.addTest("Server/ReadinessPublicationCannotAdmitAnotherInitialize", [](){
		class ReusableServer : public ServerEndpointBase{
		public:
			using ServerEndpointBase::ServerEndpointBase;
			void prepareInitialize()
			{
				postMethodCall<requests::Initialize>();
				setState(EndpointBase::State::Uninitialized);
				preMethodCall<requests::Initialize>(InitializeParams());
			}
		};
		auto stream = LoopbackStream();
		auto server = ReusableServer(stream);
		auto phase = std::barrier(2);
		auto accepted = 0;
		auto wrongError = 0;
		constexpr auto iterations = 10000;
		auto publisher = std::jthread([&]{
			for(auto i = 0; i < iterations; ++i)
			{
				server.prepareInitialize();
				phase.arrive_and_wait();
				server.postMethodCall<requests::Initialize>();
				phase.arrive_and_wait();
			}
		});

		for(auto i = 0; i < iterations; ++i)
		{
			phase.arrive_and_wait();
			try
			{
				server.preMethodCall<requests::Initialize>(InitializeParams());
				++accepted;
			}
			catch(const RequestError& error)
			{
				if(error.code() != MessageError::InvalidRequest)
					++wrongError;
			}
			phase.arrive_and_wait();
		}

		publisher.join();
		test::compare(accepted, 0);
		test::compare(wrongError, 0);
	});

	app.addTest("EndpointBase/ReleasedLogCaptureCanReplaceTheHook", [](){
		auto stream = LoopbackStream();
		auto endpoint = EndpointBase(stream);
		auto released = false;
		auto capture = std::shared_ptr<int>(new int(42), [&](int* value){
			delete value;
			endpoint.setLogHook({});
			released = true;
		});
		endpoint.setLogHook([capture = std::move(capture)](std::string_view, std::string_view){});
		endpoint.setLogHook({});
		test::check(released, "captureReleasedWithoutHookLock");
	});

	return app.main(argc, argv);
}
