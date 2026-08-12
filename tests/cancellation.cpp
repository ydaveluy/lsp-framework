// $/cancelRequest: the framework stops the request's token and never answers in
// the handler's place. Covers the three arrival orders -- cancel before the
// request is read, while it is in flight, and for something that never comes --
// plus the two invariants: a pre-cancelled request still runs its handler, and
// a notification is never cancellable.

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

#include <lsp/connection.h>
#include <lsp/io/stream.h>
#include <lsp/messagehandler.h>
#include <lsp/messages.h>

using namespace std::chrono_literals;

namespace{

// Blocking in-memory stream. Reads wait for bytes, writes are recorded.
class MemoryStream final : public lsp::io::Stream{
public:
	void push(const std::string& json)
	{
		{
			const auto lock  = std::lock_guard(m_mutex);
			const auto frame = "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json;
			m_in.insert(m_in.end(), frame.begin(), frame.end());
		}

		m_readable.notify_all();
	}

	[[nodiscard]] std::string written()
	{
		const auto lock = std::lock_guard(m_mutex);
		return m_out;
	}

	void read(char* buffer, std::size_t size) override
	{
		auto lock = std::unique_lock(m_mutex);

		for(std::size_t i = 0; i < size; ++i)
		{
			m_readable.wait(lock, [this]{ return !m_in.empty(); });
			buffer[i] = m_in.front();
			m_in.pop_front();
		}
	}

	void write(const char* buffer, std::size_t size) override
	{
		const auto lock = std::lock_guard(m_mutex);
		m_out.append(buffer, size);
	}

private:
	std::mutex              m_mutex;
	std::condition_variable m_readable;
	std::deque<char>        m_in;
	std::string             m_out;
};

std::string requestFrame(int id, std::string_view method)
{
	return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":")" + std::string(method) + R"(","params":{}})";
}

std::string cancelFrame(int id)
{
	return R"({"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":)" + std::to_string(id) + "}}";
}

std::string notificationFrame(std::string_view method)
{
	return R"({"jsonrpc":"2.0","method":")" + std::string(method) + R"("})";
}

// A latch, so each arrival order is deterministic rather than timed.
class Gate{
public:
	void open()
	{
		{
			const auto lock = std::lock_guard(m_mutex);
			m_open = true;
		}

		m_condition.notify_all();
	}

	void wait()
	{
		auto lock = std::unique_lock(m_mutex);
		m_condition.wait(lock, [this]{ return m_open; });
	}

private:
	std::mutex              m_mutex;
	std::condition_variable m_condition;
	bool                    m_open = false;
};

}

int main()
{
	MemoryStream        stream;
	lsp::Connection     connection{stream};
	lsp::MessageHandler handler{connection};

	std::atomic<int>  shutdownCalls{0};
	std::atomic<bool> shutdownTokenStopped{false};
	std::atomic<int>  exitCalls{0};
	std::atomic<bool> exitTokenStopped{false};
	Gate              working;
	Gate              release;

	// A typed request callback taking only the token: the last parameter.
	handler.add<lsp::requests::Shutdown>(
		[&](std::stop_token token) -> lsp::requests::Shutdown::Result
		{
			shutdownCalls.fetch_add(1);
			shutdownTokenStopped.store(token.stop_requested());
			return {};
		});

	// A notification callback taking a token it must never see stopped.
	handler.add<lsp::notifications::Exit>(
		[&](std::stop_token token)
		{
			exitCalls.fetch_add(1);
			exitTokenStopped.store(token.stop_requested());
		});

	// Dispatched, then parked before doing any work: the cancel lands while the
	// request is queued behind the gate.
	handler.add("queued/probe", lsp::MessageHandler::GenericAsyncMessageCallback(
		[&](const lsp::MessageId&, lsp::json::Value&&, std::stop_token token) -> std::future<lsp::json::Value>
		{
			return std::async(std::launch::deferred, [&, token]() -> lsp::json::Value
			{
				working.open();
				release.wait();

				// The handler decides what a cancelled request answers. The
				// framework never synthesizes this response.
				if(token.stop_requested())
					throw lsp::RequestError(lsp::MessageError::RequestCancelled, "Request cancelled");

				return lsp::json::Value{};
			});
		}));

	// Already working, polling its token: the cancel lands mid-flight.
	handler.add("inflight/probe", lsp::MessageHandler::GenericAsyncMessageCallback(
		[&](const lsp::MessageId&, lsp::json::Value&&, std::stop_token token) -> std::future<lsp::json::Value>
		{
			return std::async(std::launch::deferred, [&, token]() -> lsp::json::Value
			{
				working.open();

				for(int i = 0; i < 1000 && !token.stop_requested(); ++i)
					std::this_thread::sleep_for(10ms);

				if(token.stop_requested())
					throw lsp::RequestError(lsp::MessageError::RequestCancelled, "Request cancelled");

				return lsp::json::Value{};
			});
		}));

	// --- Cancelled before the request was ever read --------------------------
	// The stop is remembered and the handler runs anyway, on a stopped token.
	{
		stream.push(cancelFrame(1));
		handler.processIncomingMessages();          // remembers the pre-cancel
		stream.push(requestFrame(1, lsp::requests::Shutdown::Method));
		handler.processIncomingMessages();          // dispatches it all the same

		assert(shutdownCalls.load() == 1 && "a pre-cancelled request still runs its handler");
		assert(shutdownTokenStopped.load() && "a pre-cancelled request starts on a stopped token");
		assert(stream.written().find(R"("id":1)") != std::string::npos && "the handler's own answer went out");
	}

	// --- Cancelled after dispatch, before the work started -------------------
	{
		std::thread pump([&]{ handler.processIncomingMessages(); });
		stream.push(requestFrame(2, "queued/probe"));
		working.wait();                             // registered, parked at the gate
		pump.join();

		stream.push(cancelFrame(2));
		handler.processIncomingMessages();          // stops the live token
		release.open();

		for(int i = 0; i < 500 && stream.written().find(R"("id":2)") == std::string::npos; ++i)
			std::this_thread::sleep_for(10ms);

		assert(stream.written().find(R"("id":2)") != std::string::npos && "the queued request answered");
		assert(stream.written().find("-32800") != std::string::npos &&
		       "the handler saw the stop on the token it was given and answered RequestCancelled");
	}

	// --- Cancelled in flight, while the handler polls its token --------------
	{
		std::thread pump([&]{ handler.processIncomingMessages(); });
		stream.push(requestFrame(3, "inflight/probe"));
		pump.join();

		stream.push(cancelFrame(3));
		handler.processIncomingMessages();          // stops a token being polled

		for(int i = 0; i < 500 && stream.written().find(R"("id":3)") == std::string::npos; ++i)
			std::this_thread::sleep_for(10ms);

		assert(stream.written().find(R"("id":3)") != std::string::npos &&
		       "the running handler observed the stop and answered");
	}

	// --- A notification is never cancellable ---------------------------------
	// Nothing registers a token for it, so no cancel can ever reach one.
	{
		stream.push(cancelFrame(4));
		handler.processIncomingMessages();          // no such request: remembered
		stream.push(notificationFrame(lsp::notifications::Exit::Method));
		handler.processIncomingMessages();

		assert(exitCalls.load() == 1 && "the notification ran");
		assert(!exitTokenStopped.load() && "a notification is never cancelled");
	}

	// --- A cancel for a request that never comes writes nothing --------------
	{
		const auto before = stream.written().size();
		stream.push(cancelFrame(9999));
		handler.processIncomingMessages();

		assert(stream.written().size() == before && "$/cancelRequest answers nothing on its own");
	}

	return 0;
}
