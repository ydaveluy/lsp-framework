#pragma once

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <unordered_set>
#include <utility>
#include <lsp/concepts.h>
#include <lsp/connection.h>
#include <lsp/error.h>
#include <lsp/jsonrpc/jsonrpc.h>
#include <lsp/messagebase.h>
#include <lsp/requestresult.h>
#include <lsp/serialization.h>
#include <lsp/strmap.h>
#include <lsp/threadpool.h>

namespace lsp{

using MessageId = jsonrpc::MessageId;

/*
 * MessageHandler
 */
class MessageHandler{
public:
	explicit MessageHandler(Connection& connection, unsigned int maxResponseThreads = std::thread::hardware_concurrency() / 2);
	~MessageHandler() = default;

	void processIncomingMessages();

	struct GenericMessage{
		using Params = json::Value;
		using Result = json::Value;
	};

	// A generic callback is handed everything the dispatcher knows about the
	// request: which one it is and whether it has been cancelled. There is no
	// shorter form, because a shorter form could only get them from ambient
	// state. Typed callbacks may drop either parameter -- see concepts.h --
	// since the wrapper keeps both regardless of what the callback asked for.
	using GenericMessageCallback       = std::function<json::Value(const MessageId&, json::Value&&, std::stop_token)>;
	using GenericAsyncMessageCallback  = std::function<AsyncRequestResult<GenericMessage>(const MessageId&, json::Value&&, std::stop_token)>;
	using GenericResponseCallback      = std::function<void(json::Value&&)>;
	using GenericErrorResponseCallback = std::function<void(const ResponseError&)>;

	/*
	 * Callback registration
	 */

	template<typename M, typename F>
	MessageHandler& add(F&& handlerFunc) requires IsRequestCallback<M, F>;

	template<typename M, typename F>
	MessageHandler& add(F&& handlerFunc) requires IsNoParamsRequestCallback<M, F>;

	template<typename M, typename F>
	MessageHandler& add(F&& handlerFunc) requires IsNotificationCallback<M, F>;

	template<typename M, typename F>
	MessageHandler& add(F&& handlerFunc) requires IsNoParamsNotificationCallback<M, F>;

	MessageHandler& add(std::string_view method, GenericMessageCallback callback);
	MessageHandler& add(std::string_view method, GenericAsyncMessageCallback callback);

	void remove(std::string_view method);

	/*
	 * sendRequest
	 */

	using ResponseErrorCallback = void(*)(const ResponseError&);

	template<typename M, typename F, typename E = ResponseErrorCallback>
	MessageId sendRequest(typename M::Params&& params, F&& then, E&& error = [](const ResponseError&){}) requires SendRequest<M, F, E>;

	template<typename M, typename F, typename E = ResponseErrorCallback>
	MessageId sendRequest(F&& then, E&& error = [](const ResponseError&){}) requires SendNoParamsRequest<M, F, E>;

	template<typename M>
	[[nodiscard]] FutureResponse<M> sendRequest(typename M::Params&& params) requires message::IsRequest<M> && message::HasParams<M>;

	template<typename M>
	[[nodiscard]] FutureResponse<M> sendRequest() requires message::IsRequest<M> && (!message::HasParams<M>);

	FutureResponse<GenericMessage> sendRequest(std::string_view method, std::optional<json::Value>&& params = std::nullopt);

	MessageId sendRequest(
		std::string_view method,
		std::optional<json::Value>&& params,
		GenericResponseCallback then,
		GenericErrorResponseCallback error);

	/*
	 * sendNotification
	 */

	template<typename M>
	void sendNotification(typename M::Params&& params) requires SendNotification<M>;

	template<typename M>
	void sendNotification() requires SendNoParamsNotification<M>;

	void sendNotification(std::string_view method, std::optional<json::Value>&& params = std::nullopt);

private:
	class ResponseResultBase;
	class RequestResultBase;
	using RequestResultPtr  = std::unique_ptr<RequestResultBase>;
	using ResponseResultPtr = std::unique_ptr<ResponseResultBase>;
	using OptionalResponse  = std::optional<jsonrpc::Response>;

	class RequestRegistration;
	using RequestRegistrationPtr = std::shared_ptr<const RequestRegistration>;

	// What the dispatcher tells a handler wrapper about the request it serves.
	// The registration is what keeps the cancellation token in the table: the
	// async path copies it into the pool task, which outlives processRequest.
	struct RequestContext{
		const MessageId&       id;
		std::stop_token        token;
		RequestRegistrationPtr registration;
	};

	using HandlerWrapper = std::function<OptionalResponse(const RequestContext&, json::Value&&, bool)>;

	// General
	Connection&                                      m_connection;
	// Incoming requests. The wrapper is held by shared_ptr so a dispatch can
	// take a reference out of the table and outlive a concurrent re-register.
	using HandlerWrapperPtr = std::shared_ptr<const HandlerWrapper>;
	StrMap<std::string, HandlerWrapperPtr>           m_requestHandlersByMethod;
	std::mutex                                       m_requestHandlersMutex;
	// Incoming request cancellation. A cancel that arrives before the request
	// it names is remembered, so the handler can start on a stopped token.
	std::mutex                                       m_requestTokensMutex;
	std::unordered_map<MessageId, std::stop_source>  m_requestTokens;
	std::unordered_set<MessageId>                    m_cancelledBeforeDispatch;
	// Outgoing requests
	std::mutex                                       m_pendingRequestsMutex;
	std::unordered_map<MessageId, RequestResultPtr>  m_pendingRequests;
	// DECLARED LAST, SO IT IS DESTROYED FIRST: ~ThreadPool joins the workers,
	// and a task still writing a response reaches the maps above while they
	// are all still alive.
	ThreadPool                                       m_threadPool;

	template<typename T>
	static jsonrpc::Response createResponse(const MessageId& id, T&& result);

	template<typename M>
	static jsonrpc::Response createResponseFromAsyncResult(const MessageId& id, AsyncRequestResult<M>& result);

	// Prepends the request id and appends its cancellation token, each when the
	// callback declared a parameter for it.
	template<typename F, typename... Args>
	static decltype(auto) invokeCallback(F& callback, const RequestContext& context, Args&&... args);

	// Handles $/cancelRequest. Returns true when the request was already
	// running, which is when the notification stops here.
	bool cancelRequest(const std::optional<json::Value>& params);
	[[nodiscard]] RequestRegistrationPtr registerRequestToken(const MessageId& id, std::stop_source& source);
	void eraseRequestToken(const MessageId& id);

	OptionalResponse processRequest(jsonrpc::Request&& request, bool allowAsync);
	void processResponse(jsonrpc::Response&& response);
	void addHandler(std::string_view method, HandlerWrapper&& handlerFunc);
	void sendResponse(jsonrpc::Response&& response);
	MessageId sendRequest(std::string_view method, RequestResultPtr result, std::optional<json::Value>&& params = std::nullopt);

	// Holds a request's entry in m_requestTokens for as long as anyone can still
	// answer that request.
	class RequestRegistration{
	public:
		RequestRegistration(MessageHandler& handler, MessageId id)
			: m_handler{handler}
			, m_id{std::move(id)}
		{
		}

		RequestRegistration(const RequestRegistration&) = delete;
		RequestRegistration& operator=(const RequestRegistration&) = delete;
		~RequestRegistration(){ m_handler.eraseRequestToken(m_id); }

	private:
		MessageHandler& m_handler;
		MessageId       m_id;
	};

	/*
	 * Request result wrapper
	 */

	class RequestResultBase{
	public:
		virtual ~RequestResultBase() = default;
		virtual void setValueFromJson(json::Value&& json) = 0;
		virtual void setError(ResponseError&& error) = 0;
	};

	template<typename T, typename F, typename E>
	class CallbackRequestResult final : public RequestResultBase{
	public:
		CallbackRequestResult(F&& then, E&& error)
			: m_then{std::forward<F>(then)}
			, m_error{std::forward<E>(error)}
		{
		}

		void setValueFromJson(json::Value&& json) override;
		void setError(ResponseError&& error) override;

	private:
		F m_then;
		E m_error;
	};

	template<typename T>
	class FutureRequestResult final : public RequestResultBase{
	public:
		std::future<T> future(){ return m_promise.get_future(); }

		void setValueFromJson(json::Value&& json) override;
		void setError(ResponseError&& error) override;

	private:
		std::promise<T> m_promise;
	};
};

} // namespace lsp

#include "messagehandler.inl"
