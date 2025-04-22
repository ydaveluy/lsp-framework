#include "connection.h"

#include "error.h"
#include <cassert>
#include <charconv>
#include <lsp/json/json.h>
#include <lsp/jsonrpc/jsonrpc.h>
#include <lsp/str.h>
#include <string_view>

#ifndef LSP_MESSAGE_DEBUG_LOG
#ifdef NDEBUG
#define LSP_MESSAGE_DEBUG_LOG 0
#else
#define LSP_MESSAGE_DEBUG_LOG 1
#endif
#endif

#if LSP_MESSAGE_DEBUG_LOG
#ifdef __APPLE__
#include <os/log.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#endif

namespace lsp {
namespace {

/*
 * Message logging
 */

#if LSP_MESSAGE_DEBUG_LOG
void debugLogMessageJson([[maybe_unused]] const std::string &messageType,
                         [[maybe_unused]] const lsp::json::Any &json) {
#ifdef __APPLE__
  os_log_debug(OS_LOG_DEFAULT, "%{public}s",
               (messageType + ": " + lsp::json::stringify(json, true)).c_str());
#elif defined(_WIN32)
  OutputDebugStringA(
      (messageType + ": " + lsp::json::stringify(json, true) + '\n').c_str());
#endif
}
#endif

json::Integer nextUniqueRequestId() {
  static std::atomic<json::Integer> g_uniqueRequestId = 0;
  return ++g_uniqueRequestId;
}
} // namespace

/*
 * Connection
 */

Connection::Connection(std::istream &in, std::ostream &out)
    : m_in{in}, m_out{out} {

  m_asyncResponseThread = std::thread{[this]() {
    while (true) {
      std::unique_lock lock{m_pendingResponsesMutex};
      std::erase_if(m_pendingResponses, [this](const auto &p) {
        const auto &result = p.second;
        const auto ready = result->isReady();

        if (ready)
          sendResponse(result->createResponse());

        return ready;
      });

      if (!m_running)
        break;

      lock.unlock();
      std::this_thread::sleep_for(
          std::chrono::milliseconds{100}); // TODO: Do this in a better way
    }
  }};
}

void Connection::processIncomingMessages() {
  std::lock_guard lock{m_readMutex};

  if (m_in.peek() == std::char_traits<char>::eof())
    throw ConnectionError{"Connection lost"};

  const auto header = readMessageHeader();

  std::string content;
  content.resize(header.contentLength);
  m_in.read(&content[0], static_cast<std::streamsize>(header.contentLength));

  // Verify only after reading the entire message so no partially unread message
  // is left in the stream

  std::string_view contentType{header.contentType};

  if (!contentType.starts_with("application/vscode-jsonrpc"))
    throw ProtocolError{"Unsupported or invalid content type: " +
                        header.contentType};

  constexpr std::string_view charsetKey{"charset="};
  if (const auto idx = contentType.find(charsetKey);
      idx != std::string_view::npos) {
    auto charset = contentType.substr(idx + charsetKey.size());
    charset = str::trimView(charset.substr(0, charset.find(';')));

    if (charset != "utf-8" && charset != "utf8")
      throw ProtocolError{"Unsupported or invalid character encoding: " +
                          std::string{charset}};
  }

  try {
    auto json = json::parse(content);
#if LSP_MESSAGE_DEBUG_LOG
    debugLogMessageJson("incoming", json);
#endif

    if (json.isObject()) {
      auto message = jsonrpc::messageFromJson(std::move(json.object()));

      if (std::holds_alternative<jsonrpc::Request>(message))
        onRequest(std::move(std::get<jsonrpc::Request>(message)));
      else
        onResponse(std::move(std::get<jsonrpc::Response>(message)));
    } else if (json.isArray()) {
      auto batch = jsonrpc::messageBatchFromJson(std::move(json.array()));

      if (std::holds_alternative<jsonrpc::RequestBatch>(batch))
        onRequestBatch(std::move(std::get<jsonrpc::RequestBatch>(batch)));
      else
        onResponseBatch(std::move(std::get<jsonrpc::ResponseBatch>(batch)));
    }
  } catch (const std::exception &e) {
    throw ConnectionError{e.what()};
  }
}

void Connection::sendRequest(jsonrpc::Request &&request) {
  writeJsonMessage(jsonrpc::requestToJson(std::move(request)));
}

void Connection::sendResponse(jsonrpc::Response &&response) {
  writeJsonMessage(jsonrpc::responseToJson(std::move(response)));
}

void Connection::sendRequestBatch(jsonrpc::RequestBatch &&batch) {
  writeJsonMessage(jsonrpc::requestBatchToJson(std::move(batch)));
}

void Connection::sendResponseBatch(jsonrpc::ResponseBatch &&batch) {
  writeJsonMessage(jsonrpc::responseBatchToJson(std::move(batch)));
}

Connection::MessageHeader Connection::readMessageHeader() {
  MessageHeader header;

  while (m_in.peek() != '\r')
    readNextMessageHeaderField(header);

  m_in.get(); // \r

  if (m_in.peek() != '\n')
    throw ProtocolError{"Invalid message header format"};

  m_in.get(); // \n

  return header;
}

void Connection::readNextMessageHeaderField(MessageHeader &header) {
  if (m_in.peek() == std::char_traits<char>::eof())
    throw ConnectionError{"Connection lost"};

  std::string lineData;
  std::getline(m_in,
               lineData); // This also consumes the newline so it's only
                          // necessary to check for one \r\n before the content

  std::string_view line{lineData};
  const auto separatorIdx = line.find(':');

  if (separatorIdx != std::string_view::npos) {
    const auto key = str::trimView(line.substr(0, separatorIdx));
    const auto value = str::trimView(line.substr(separatorIdx + 1));

    if (key == "Content-Length")
      std::from_chars(value.data(), value.data() + value.size(),
                      header.contentLength);
    else if (key == "Content-Type")
      header.contentType = std::string{value.data(), value.size()};
  }
}

void Connection::writeJsonMessage(const json::Any &content) {
#if LSP_MESSAGE_DEBUG_LOG
  debugLogMessageJson("outgoing", content);
#endif
  writeMessage(json::stringify(content));
}

void Connection::writeMessage(const std::string &content) {
  std::lock_guard lock{m_writeMutex};
  MessageHeader header{content.size()};
  writeMessageHeader(header);
  m_out.write(content.data(), static_cast<std::streamsize>(content.size()));
  m_out.flush();
}

void Connection::writeMessageHeader(const MessageHeader &header) {
  const auto headerStr =
      "Content-Length: " + std::to_string(header.contentLength) + "\r\n\r\n";
  m_out.write(headerStr.data(),
              static_cast<std::streamsize>(headerStr.length()));
}

void Connection::onResponse(jsonrpc::Response &&response) {
  RequestResultPtr result;

  // Find pending request for the response that was received based on the
  // message id.
  {
    std::lock_guard lock{m_pendingRequestsMutex};
    if (auto it = m_pendingRequests.find(response.id);
        it != m_pendingRequests.end()) {
      result = std::move(it->second);
      m_pendingRequests.erase(it);
    }
  }

  if (!result) // If there's no result it means a response was received without
               // a request which makes no sense but just ignore it...
    return;

  if (response.result.has_value()) {
    try {
      result->setValueFromJson(std::move(*response.result));
    } catch (const json::TypeError &e) {
      result->setException(std::make_exception_ptr(e));
    }
  } else // Error response received. Create an exception.
  {
    assert(response.error.has_value());
    const auto &error = *response.error;
    result->setException(std::make_exception_ptr(
        ResponseError{ErrorCodes{error.code}, error.message, error.data}));
  }
}

void Connection::onResponseBatch(jsonrpc::ResponseBatch &&batch) {
  // This should never be called as no batches are ever sent
  for (auto &&r : batch)
    onResponse(std::move(r));
}

jsonrpc::MessageId
Connection::sendRequest(MessageMethod method, RequestResultPtr result,
                        const std::optional<json::Any> &params) {
  std::lock_guard lock{m_pendingRequestsMutex};
  auto messageId = nextUniqueRequestId();
  m_pendingRequests[messageId] = std::move(result);
  auto methodStr = messageMethodToString(method);
  sendRequest(jsonrpc::createRequest(messageId, methodStr, params));
  return messageId;
}

void Connection::sendNotification(MessageMethod method,
                                  const std::optional<json::Any> &params) {
  const auto methodStr = messageMethodToString(method);
  auto notification = jsonrpc::createNotification(methodStr, params);
  sendRequest(std::move(notification));
}

Connection::~Connection() {
  {
    std::lock_guard lock{m_pendingResponsesMutex};
    m_running = false;
  }

  m_asyncResponseThread.join();
}

void Connection::remove(MessageMethod method) {
  const auto idx = static_cast<std::size_t>(method);

  if (idx < m_requestHandlers.size())
    m_requestHandlers[idx] = nullptr;
}

void Connection::onRequest(jsonrpc::Request &&request) {
  auto response = processRequest(std::move(request), true);

  if (response.has_value())
    sendResponse(std::move(*response));
}

void Connection::onRequestBatch(jsonrpc::RequestBatch &&batch) {
  jsonrpc::ResponseBatch responses;
  responses.reserve(batch.size());

  for (auto &&r : batch) {
    auto response = processRequest(std::move(r), false);

    if (response.has_value())
      responses.push_back(std::move(*response));
  }

  sendResponseBatch(std::move(responses));
}

Connection::OptionalResponse
Connection::processRequest(jsonrpc::Request &&request, bool allowAsync) {
  const auto method = messageMethodFromString(request.method);
  const auto index = static_cast<std::size_t>(method);
  OptionalResponse response;

  if (index < m_requestHandlers.size() && m_requestHandlers[index]) {
    try {
      // Call handler for the method type and return optional response
      response = m_requestHandlers[index](
          request.id.has_value() ? *request.id : json::Null{},
          request.params.has_value() ? std::move(*request.params)
                                     : json::Null{},
          allowAsync);
    } catch (const RequestError &e) {
      if (!request.isNotification()) {
        response = jsonrpc::createErrorResponse(*request.id, e.code(), e.what(),
                                                e.data());
      }
    } catch (const json::TypeError &e) {
      if (!request.isNotification()) {
        response = jsonrpc::createErrorResponse(
            *request.id, ErrorCodes{ErrorCodes::InvalidParams}, e.what());
      }
    } catch (const std::exception &e) {
      if (!request.isNotification()) {
        response = jsonrpc::createErrorResponse(
            *request.id, ErrorCodes{ErrorCodes::InternalError}, e.what());
      }
    }
  } else if (!request.isNotification()) {
    response = jsonrpc::createErrorResponse(
        *request.id, ErrorCodes{ErrorCodes::MethodNotFound},
        "Unsupported method: " + request.method);
  }

  return response;
}

void Connection::addHandler(MessageMethod method,
                            HandlerWrapper &&handlerFunc) {
  std::lock_guard lock{m_requestHandlersMutex};
  const auto index = static_cast<std::size_t>(method);

  if (m_requestHandlers.size() <= index)
    m_requestHandlers.resize(index + 1);

  m_requestHandlers[index] = std::move(handlerFunc);
}

void Connection::addResponseResult(const jsonrpc::MessageId &id,
                                   ResponseResultPtr result) {
  std::lock_guard lock{m_pendingResponsesMutex};
  const auto it = m_pendingResponses.find(id);

  if (it != m_pendingResponses.end())
    throw RequestError{ErrorCodes::InvalidRequest, "Request id is not unique"};

  m_pendingResponses.emplace(id, std::move(result));
}

} // namespace lsp
