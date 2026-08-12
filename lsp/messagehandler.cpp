#include <cassert>
#include <lsp/messagehandler.h>

namespace lsp{
namespace{

constexpr std::string_view CancelRequestMethod{"$/cancelRequest"};

json::Integer nextUniqueRequestId()
{
	static std::atomic<json::Integer> s_uniqueRequestId = 0;
	return ++s_uniqueRequestId;
}

// The request a $/cancelRequest names, when it names one the protocol allows.
std::optional<MessageId> cancelledRequestId(const std::optional<json::Value>& params)
{
	if(!params.has_value() || !params->isObject())
		return std::nullopt;

	const auto* const id = params->object().find("id");

	if(!id)
		return std::nullopt;

	if(id->isInteger())
		return MessageId(id->integer());

	if(id->isString())
		return MessageId(id->string());

	return std::nullopt;
}

}

MessageHandler::MessageHandler(Connection& connection, unsigned int maxResponseThreads)
	: m_connection{connection}
	, m_threadPool(0, maxResponseThreads)
{
}

void MessageHandler::processIncomingMessages()
{
	auto messageOrBatch = m_connection.readMessage();

	if(auto* const message = std::get_if<jsonrpc::Message>(&messageOrBatch))
	{
		if(auto* const request = std::get_if<jsonrpc::Request>(message))
		{
			auto optionalResponse = processRequest(std::move(*request), true);

			if(optionalResponse.has_value())
				m_connection.writeMessage(std::move(*optionalResponse));
		}
		else
		{
			processResponse(std::move(std::get<jsonrpc::Response>(*message)));
		}
	}
	else
	{
		auto& batch         = std::get<jsonrpc::MessageBatch>(messageOrBatch);
		auto  responseBatch = jsonrpc::MessageBatch();

		for(auto& msg : batch)
		{
			if(auto* const request = std::get_if<jsonrpc::Request>(&msg))
			{
				auto optionalResponse = processRequest(std::move(*request), false);

				if(optionalResponse.has_value())
					responseBatch.push_back(std::move(*optionalResponse));
			}
			else
			{
				processResponse(std::move(std::get<jsonrpc::Response>(msg)));
			}
		}

		if(!responseBatch.empty())
			m_connection.writeMessage(std::move(responseBatch));
	}
}

void MessageHandler::remove(std::string_view method)
{
	std::lock_guard lock{m_requestHandlersMutex};

	if(const auto it = m_requestHandlersByMethod.find(method); it != m_requestHandlersByMethod.end())
		m_requestHandlersByMethod.erase(it);
}

// Stops a running request, or remembers the id of one no pump has dispatched
// yet. Only the dispatcher sees a message before the handler table does, which
// is why the cancellation registry belongs here and not in the consumer.
bool MessageHandler::cancelRequest(const std::optional<json::Value>& params)
{
	const auto id = cancelledRequestId(params);

	if(!id.has_value())
		return false;

	const auto lock = std::lock_guard(m_requestTokensMutex);

	if(const auto it = m_requestTokens.find(*id); it != m_requestTokens.end())
	{
		it->second.request_stop();
		return true;
	}

	m_cancelledBeforeDispatch.insert(*id);
	return false;
}

MessageHandler::RequestRegistrationPtr MessageHandler::registerRequestToken(const MessageId& id, std::stop_source& source)
{
	{
		const auto lock = std::lock_guard(m_requestTokensMutex);

		if(m_cancelledBeforeDispatch.erase(id) != 0)
			source.request_stop();

		m_requestTokens.insert_or_assign(id, source);
	}

	return std::make_shared<const RequestRegistration>(*this, id);
}

void MessageHandler::eraseRequestToken(const MessageId& id)
{
	const auto lock = std::lock_guard(m_requestTokensMutex);
	m_requestTokens.erase(id);
}

MessageHandler::OptionalResponse MessageHandler::processRequest(jsonrpc::Request&& request, bool allowAsync)
{
	// $/cancelRequest is answered here, before the handler table is consulted:
	// a request that is already running is stopped and the notification ends.
	if(request.method == CancelRequestMethod && cancelRequest(request.params))
		return std::nullopt;

	// The handler is taken out of the table under the lock and kept alive by the
	// shared_ptr for the whole call: dispatching through the table's iterator
	// after unlocking let add()/remove() destroy the callable mid-call.
	HandlerWrapperPtr handler;
	{
		const auto lock = std::lock_guard(m_requestHandlersMutex);

		if(const auto it = m_requestHandlersByMethod.find(request.method); it != m_requestHandlersByMethod.end())
			handler = it->second;
	}

	OptionalResponse response;

	if(handler && *handler)
	{
		// A notification is a request whose id is null, and the handler is told
		// which one it serves rather than having to ask the current thread.
		// The token is registered here, on the read thread, so a cancel that
		// arrives while the handler works finds it.
		static const MessageId NullMessageId = json::Null();
		const MessageId&       id            = request.id.has_value() ? *request.id : NullMessageId;

		auto       source  = std::stop_source();
		const auto context = RequestContext{
			.id           = id,
			.token        = source.get_token(),
			.registration = request.isNotification() ? RequestRegistrationPtr() : registerRequestToken(id, source)
		};

		try
		{
			// Call handler for the method type and return optional response
			response = (*handler)(
				context,
				request.params.has_value() ? std::move(*request.params) : json::Null{},
				allowAsync);
		}
		catch(const RequestError& e)
		{
			if(!request.isNotification())
			{
				response = jsonrpc::createErrorResponse(
					*request.id, e.code(), e.what(), e.data());
			}
		}
		catch(const json::TypeError& e)
		{
			if(!request.isNotification())
			{
				response = jsonrpc::createErrorResponse(
					*request.id, MessageError::InvalidParams, e.what());
			}
		}
		catch(const std::exception& e)
		{
			if(!request.isNotification())
			{
				response = jsonrpc::createErrorResponse(
					*request.id, MessageError::InternalError, e.what());
			}
		}
	}
	else
	{
		if(!request.isNotification())
			response = jsonrpc::createErrorResponse(*request.id, MessageError::MethodNotFound, "Method not found");
	}

	return response;
}

void MessageHandler::processResponse(jsonrpc::Response&& response)
{
	RequestResultPtr result;

	// Find pending request for the response that was received based on the message id.
	{
		std::lock_guard lock{m_pendingRequestsMutex};
		if(auto it = m_pendingRequests.find(response.id); it != m_pendingRequests.end())
		{
			result = std::move(it->second);
			m_pendingRequests.erase(it);
		}
	}

	if(!result) // If there's no result it means a response was received without a request which makes no sense but just ignore it...
		return;

	if(response.result.has_value())
	{
		result->setValueFromJson(std::move(*response.result));
	}
	else // Error response received.
	{
		assert(response.error.has_value());
		auto& error = *response.error;
		result->setError(ResponseError(error.code, std::move(error.message), std::move(error.data)));
	}
}

void MessageHandler::addHandler(std::string_view method, HandlerWrapper&& handlerFunc)
{
	auto handler = std::make_shared<const HandlerWrapper>(std::move(handlerFunc));

	const auto lock = std::lock_guard(m_requestHandlersMutex);
	m_requestHandlersByMethod[std::string(method)] = std::move(handler);
}

MessageHandler& MessageHandler::add(std::string_view method, GenericMessageCallback callback)
{
	addHandler(method,
		[f = std::move(callback)](const RequestContext& context, json::Value&& params, bool) -> OptionalResponse
		{
			const auto isNotification = std::holds_alternative<json::Null>(context.id);
			auto result = f(context.id, std::move(params), context.token);

			if(!isNotification)
				return jsonrpc::createResponse(context.id, std::move(result));

			return std::nullopt;
		}
	);

	return *this;
}

MessageHandler& MessageHandler::add(std::string_view method, GenericAsyncMessageCallback callback)
{
	addHandler(method,
		[this, f = std::move(callback)](const RequestContext& context, json::Value&& params, bool allowAsync) -> OptionalResponse
		{
			const auto isNotification = std::holds_alternative<json::Null>(context.id);
			auto future = f(context.id, std::move(params), context.token);

			if(allowAsync)
			{
				m_threadPool.addTask(
					[this, future = std::move(future), isNotification = isNotification,
					 requestId = context.id, registration = context.registration]() mutable
					{
						auto response = createResponseFromAsyncResult<GenericMessage>(requestId, future);

						if(!isNotification)
							sendResponse(std::move(response));
					}
				);
				return std::nullopt;
			}

			auto result = future.get();

			if(!isNotification)
				return jsonrpc::createResponse(context.id, std::move(result));

			return std::nullopt;
		}
	);

	return *this;
}

void MessageHandler::sendResponse(jsonrpc::Response&& response)
{
	m_connection.writeMessage(std::move(response));
}

MessageId MessageHandler::sendRequest(std::string_view method, RequestResultPtr result, std::optional<json::Value>&& params)
{
	const auto messageId = nextUniqueRequestId();

	// The pending entry is in place before the request goes out, but the lock is
	// not held across the write: a congested wire would otherwise stall every
	// response the read thread is trying to deliver.
	{
		const auto lock = std::lock_guard(m_pendingRequestsMutex);
		m_pendingRequests[messageId] = std::move(result);
	}

	m_connection.writeMessage(jsonrpc::createRequest(messageId, method, std::move(params)));
	return messageId;
}

MessageId MessageHandler::sendRequest(
	std::string_view method,
	std::optional<json::Value>&& params,
	GenericResponseCallback then,
	GenericErrorResponseCallback error)
{
	auto result = std::make_unique<CallbackRequestResult<json::Value, decltype(then), decltype(error)>>(
		std::move(then), std::move(error));
	return sendRequest(method, std::move(result), std::move(params));
}

FutureResponse<MessageHandler::GenericMessage> MessageHandler::sendRequest(std::string_view method, std::optional<json::Value>&& params)
{
	auto result    = std::make_unique<FutureRequestResult<json::Value>>();
	auto future    = result->future();
	auto messageId = sendRequest(method, std::move(result), std::move(params));

	return {std::move(messageId), std::move(future)};
}

void MessageHandler::sendNotification(std::string_view method, std::optional<json::Value>&& params)
{
	auto notification = jsonrpc::createNotification(method, std::move(params));
	m_connection.writeMessage(std::move(notification));
}

} // namespace lsp
