#include <algorithm>
#include <cassert>
#include "message_handler.h"

namespace lsp{
namespace{

thread_local const MessageHandler::RequestContext* t_requestContext = nullptr;

} // namespace

/*
 * MessageHandler::RequestContext
 */

MessageHandler::RequestContext::RequestContext(
	MessageHandler& messageHandler,
	std::string_view method,
	const RequestId& requestId,
	RequestTimestamp requestTimestamp)
	: m_messageHandler(&messageHandler)
	, m_method(method)
	, m_requestId(requestId)
	, m_requestTimestamp(requestTimestamp)
{
	assert(!t_requestContext);
	t_requestContext = this;
}

MessageHandler::RequestContext::~RequestContext()
{
	if(t_requestContext == this)
		t_requestContext = nullptr;
}

auto MessageHandler::RequestContext::get() -> const RequestContext&
{
	if(!t_requestContext)
		throw std::logic_error("RequestContext::get called outside of a request context");

	return *t_requestContext;
}

auto MessageHandler::RequestContext::tryGet() -> const RequestContext*
{
	return t_requestContext;
}

void MessageHandler::RequestContext::throwIfCanceled() const
{
	if(isCanceled())
		throw RequestError(MessageError::RequestCancelled, "Canceled");
}

/*
 * MessageHandler
 */

MessageHandler::MessageHandler(Connection connection, unsigned int maxResponseThreads)
	: m_connection(std::move(connection))
	, m_threadPool(0, maxResponseThreads)
{
}

MessageHandler::~MessageHandler()
{
	// Joining the response threads waits for every request still computing:
	// stopping them first lets a request that observes its token end early.
	auto sources = std::vector<std::stop_source>();

	{
		const auto lock = std::lock_guard(m_activeRequestMutex);
		sources.reserve(m_activeRequests.size());

		for(const auto& request : m_activeRequests)
			sources.push_back(request.source);
	}

	for(auto& source : sources)
		source.request_stop();
}

void MessageHandler::processNextMessage()
{
	auto invalidBatch = std::vector<jsonrpc::Error>();
	auto messageOrBatch = m_connection.readMessage(&invalidBatch);

	if(auto* const message = std::get_if<jsonrpc::Message>(&messageOrBatch))
	{
		if(auto* const request = std::get_if<jsonrpc::Request>(message))
			processRequest(std::move(*request), nullptr);
		else
			processResponse(std::move(std::get<jsonrpc::Response>(*message)));
	}
	else
	{
		auto& batch       = std::get<jsonrpc::MessageBatch>(messageOrBatch);
		auto  batchSender = m_connection.messageBatch();

		for(const auto& error : invalidBatch)
		{
			auto writer = batchSender.writeError(json::Null(), error.code, error.message);
		}

		for(auto& msg : batch)
		{
			if(auto* const request = std::get_if<jsonrpc::Request>(&msg))
				processRequest(std::move(*request), &batchSender);
			else
				processResponse(std::move(std::get<jsonrpc::Response>(msg)));
		}

		if(batchSender.batchIsEmpty())
			batchSender.discard();
		else
			batchSender.submit();
	}
}

void MessageHandler::setConnection(Connection connection)
{
	m_connection = std::move(connection);
}

void MessageHandler::setMaxResponseThreads(unsigned int maxResponseThreads)
{
	m_threadPool.setMaxThreads(maxResponseThreads);
}

void MessageHandler::remove(const std::string& method)
{
	decltype(m_requestHandlersByMethod)::node_type request;
	decltype(m_notificationHandlersByMethod)::node_type notification;

	{
		const auto lock = std::lock_guard(m_requestHandlersMutex);
		request = m_requestHandlersByMethod.extract(method);
		notification = m_notificationHandlersByMethod.extract(method);
	}
}

void MessageHandler::cancel(const RequestId& id)
{
	auto source = std::optional<std::stop_source>();

	{
		const auto lock = std::lock_guard(m_activeRequestMutex);
		const auto it = std::ranges::find_if(m_activeRequests, [&id](const auto& r){ return r.id == id; });

		if(it != m_activeRequests.end())
			source = it->source;
	}

	if(source)
		source->request_stop();
}

auto MessageHandler::isCanceled(const RequestId& id) -> bool
{
	const auto lock = std::lock_guard(m_activeRequestMutex);
	const auto it   = std::ranges::find_if(m_activeRequests, [&id](const auto& r){ return r.id == id; });
	return it != m_activeRequests.end() && it->source.stop_requested();
}

auto MessageHandler::stopToken(const RequestId& id) -> std::stop_token
{
	const auto lock = std::lock_guard(m_activeRequestMutex);
	const auto it   = std::ranges::find_if(m_activeRequests, [&id](const auto& r){ return r.id == id; });
	return it != m_activeRequests.end() ? it->source.get_token() : std::stop_token();
}

void MessageHandler::setMessageLogLevel(MessageLogLevel msgLogLevel)
{
	m_msgLogLevel.store(msgLogLevel);
}

void MessageHandler::addMessageLogCallback(MessageLogCallback callback)
{
	if(callback)
	{
		const auto lock = std::lock_guard(m_msgLogCallbacksMutex);
		m_msgLogCallbacks.push_back(std::make_shared<MessageLogCallback>(std::move(callback)));
	}
}

auto MessageHandler::shouldLog() const -> bool
{
	if(m_msgLogLevel.load() == MessageLogLevel::Off)
		return false;

	const auto lock = std::lock_guard(m_msgLogCallbacksMutex);
	return !m_msgLogCallbacks.empty();
}

void MessageHandler::dispatchMessageLog(const MessageLog& msgLog)
{
	auto callbacks = std::vector<std::shared_ptr<MessageLogCallback>>();

	{
		const auto lock = std::lock_guard(m_msgLogCallbacksMutex);
		callbacks = m_msgLogCallbacks;
	}

	for(const auto& callback : callbacks)
		(*callback)(msgLog);
}

void MessageHandler::processRequest(jsonrpc::Request&& request, Connection::BatchSender* batchSender)
{
	const auto requestTimestamp = std::chrono::steady_clock::now();

	if(shouldLog())
	{
		auto msgLog = MessageLog{
			.incoming = true,
			.method   = request.method,
			.id       = request.id,
		};

		if(request.params.has_value())
			dispatchMessageLog(msgLog, *request.params);
		else
			dispatchMessageLog(msgLog);
	}

	auto handler = std::shared_ptr<HandlerWrapper>();

	{
		const auto lock = std::lock_guard(m_requestHandlersMutex);

		const auto& handlers = request.isNotification() ? m_notificationHandlersByMethod : m_requestHandlersByMethod;

		if(const auto it = handlers.find(request.method); it != handlers.end())
			handler = it->second;
	}

	if(handler && *handler)
	{
		try
		{
			if(request.isNotification())
			{
				(*handler)(
					request.params.has_value() ? std::move(*request.params) : json::Null{},
					nullptr);
			}
			else
			{
				addActive(*request.id); // Removed again in sendResponse or on error
				// Instantiate request context for request handler
				auto context = RequestContext(*this, request.method, *request.id, requestTimestamp);

				(*handler)(
					request.params.has_value() ? std::move(*request.params) : json::Null{},
					batchSender);
			}
		}
		catch(const ConnectionError&)
		{
			// Not the request's failure: answering it would write to the same dead connection
			throw;
		}
		catch(const RequestError& e)
		{
			if(!request.isNotification())
			{
				removeActive(*request.id);
				sendErrorResponse(
					request.method,
					requestTimestamp,
					*request.id,
					e.code(),
					e.what(),
					e.data(), batchSender);
			}
		}
		catch(const std::exception& e)
		{
			if(!request.isNotification())
			{
				removeActive(*request.id);
				sendErrorResponse(
					request.method,
					requestTimestamp,
					*request.id,
					MessageError::InternalError,
					e.what(),
					{},
					batchSender);
			}
		}
	}
	else
	{
		if(!request.isNotification())
		{
			sendErrorResponse(
				request.method,
				requestTimestamp,
				*request.id,
				MessageError::MethodNotFound,
				"Method not found",
				{},
				batchSender);
		}
	}
}

void MessageHandler::processResponse(jsonrpc::Response&& response)
{
	auto pendingRequest = PendingRequestPtr();

	// Find pending request for the response that was received based on the message id.
	{
		const auto lock = std::lock_guard(m_pendingRequestsMutex);
		const auto it   = std::ranges::find_if(m_pendingRequests,
			[&id = response.id](const PendingRequestPtr& result)
			{
				return result->requestId() == id;
			});

		if(it != m_pendingRequests.end())
		{
			pendingRequest = std::move(*it);
			m_pendingRequests.erase(it);
		}
	}

	if(!pendingRequest)
		return;

	if(shouldLog())
	{
		auto msgLog = MessageLog{
			.incoming        = true,
			.method          = pendingRequest->method(),
			.requestDuration = std::chrono::steady_clock::now() - pendingRequest->requestTimestamp(),
			.id              = pendingRequest->requestId(),
		};

		if(response.error.has_value())
			msgLog.error = {{response.error->code, response.error->message}};

		if(response.error.has_value())
		{
			if(response.error->data.has_value())
				dispatchMessageLog(msgLog, response.error->data);
			else
				dispatchMessageLog(msgLog);
		}
		else
		{
			dispatchMessageLog(msgLog, response.result);
		}
	}

	// Instantiate request context for response handler
	const auto requestContext = RequestContext(
		*this,
		pendingRequest->method(),
		response.id,
		pendingRequest->requestTimestamp());

	if(response.result.has_value())
	{
		pendingRequest->setValue(std::move(*response.result));
	}
	else
	{
		assert(response.error.has_value());
		auto& error = *response.error;
		pendingRequest->setError(ResponseError(error.code, std::move(error.message), std::move(error.data)));
	}
}

void MessageHandler::addHandler(MessageKind kind, std::string_view method, HandlerWrapper&& handlerFunc)
{
	auto handler = std::make_shared<HandlerWrapper>(std::move(handlerFunc));

	{
		const auto lock = std::lock_guard(m_requestHandlersMutex);
		auto& handlers = kind == MessageKind::Request ? m_requestHandlersByMethod : m_notificationHandlersByMethod;
		handlers[std::string(method)].swap(handler);
	}
}

void MessageHandler::addActive(const RequestId& id)
{
	const auto lock = std::lock_guard(m_activeRequestMutex);
	assert(std::ranges::find_if(m_activeRequests, [&id](const auto& r){ return r.id == id; }) == m_activeRequests.end());
	m_activeRequests.push_back({id, std::stop_source()});
}

void MessageHandler::removeActive(const RequestId& id)
{
	const auto lock = std::lock_guard(m_activeRequestMutex);
	const auto it   = std::ranges::find_if(m_activeRequests, [&id](const auto& r){ return r.id == id; });
	if(it != m_activeRequests.end())
		m_activeRequests.erase(it);
}

void MessageHandler::submitPendingRequest(PendingRequestPtr pendingRequest, Connection::RequestSender& sender)
{
	const auto id = pendingRequest->requestId();

	{
		const auto lock = std::lock_guard(m_pendingRequestsMutex);
		m_pendingRequests.emplace_back(std::move(pendingRequest));
	}

	try
	{
		sender.submit();
	}
	catch(...)
	{
		removePendingRequest(id);
		throw;
	}
}

void MessageHandler::removePendingRequest(const RequestId& id)
{
	auto removed = PendingRequestPtr();

	{
		const auto lock = std::lock_guard(m_pendingRequestsMutex);
		const auto it = std::ranges::find_if(m_pendingRequests, [&id](const auto& request){ return request->requestId() == id; });

		if(it != m_pendingRequests.end())
		{
			removed = std::move(*it);
			m_pendingRequests.erase(it);
		}
	}
}

void MessageHandler::sendErrorResponse(
	std::string_view method,
	RequestTimestamp requestTimestamp,
	const RequestId& requestId,
	int errorCode,
	std::string_view errorMessage,
	const std::optional<json::Value>& errorData,
	Connection::BatchSender* batchSender)
{
	if(shouldLog())
	{
		auto msgLog = MessageLog{
			.incoming        = false,
			.method          = method,
			.requestDuration = std::chrono::steady_clock::now() - requestTimestamp,
			.error           = {{errorCode, errorMessage}},
			.id              = requestId
		};

		if(errorData.has_value())
			dispatchMessageLog(msgLog, *errorData);
		else
			dispatchMessageLog(msgLog);
	}

	if(batchSender)
	{
		auto responseWriter = batchSender->writeError(requestId, errorCode, errorMessage);

		if(errorData.has_value())
		{
			responseWriter.writeData(
				[](std::string_view key, const json::Value& value, json::ObjectWriter& objectWriter)
				{
					objectWriter.write(key, value);
				}, *errorData);
		}
	}
	else
	{
		auto errorResponse = m_connection.errorResponse(requestId, errorCode, errorMessage);

		if(errorData.has_value())
			errorResponse.writeData(*errorData);

		errorResponse.submit();
	}
}

auto MessageHandler::nextUniqueRequestId() -> json::Integer
{
	static std::atomic<json::Integer> s_uniqueRequestId = 0;
	return ++s_uniqueRequestId;
}

MessageHandler::PendingRequestBase::PendingRequestBase(std::string method, RequestTimestamp timestamp, RequestId id)
	: m_method(std::move(method))
	, m_requestTimestamp(timestamp)
	, m_requestId(id)
{
}

} // namespace lsp
