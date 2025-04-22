#pragma once

#include <concepts>
#include <functional>
#include <future>
#include <istream>
#include <lsp/error.h>
#include <lsp/jsonrpc/jsonrpc.h>
#include <lsp/messagebase.h>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lsp {

/*
 * The return type of MessageDispatcher::sendRequest.
 * id can be used to send a cancel notification (if the requests supports it).
 * result will contain the result of the request once it is ready.
 * Do not call result.wait() on the same thread that handles incoming messages
 * as that would result in infinite waiting.
 */
template <typename MessageType> struct FutureResponse {
  using ResultFuture = std::future<typename MessageType::Result>;

  FutureResponse(jsonrpc::MessageId _messageId, ResultFuture _result)
      : messageId{std::move(_messageId)}, result(std::move(_result)) {}

  jsonrpc::MessageId messageId;
  ResultFuture result;
};

/*
 * The result returned from a request handler callback that does processing
 * asynchronously
 */
template <typename MessageType>
using AsyncRequestResult = std::future<typename MessageType::Result>;

/*
 * Concepts to verify the type of callback
 */

template <typename MessageType, typename F>
concept IsRequestCallback =
    message::HasParams<MessageType> && message::HasResult<MessageType> &&
    std::invocable<F, const jsonrpc::MessageId &,
                   typename MessageType::Params &&>;

template <typename MessageType, typename F>
concept IsNoParamsRequestCallback =
    !message::HasParams<MessageType> && message::HasResult<MessageType> &&
    std::invocable<F, const jsonrpc::MessageId &>;

template <typename MessageType, typename F>
concept IsNotificationCallback =
    message::HasParams<MessageType> && !message::HasResult<MessageType> &&
    std::invocable<F, typename MessageType::Params &&>;

template <typename MessageType, typename F>
concept IsNoParamsNotificationCallback =
    !message::HasParams<MessageType> && !message::HasResult<MessageType> &&
    std::invocable<F>;

/*
 * Connection between the server and a client.
 * I/O happens via std::istream and std::ostream so the underlying
 * implementation can be anything from stdio to sockets
 */
class Connection {
public:
  Connection(std::istream &in, std::ostream &out);
  ~Connection();

  void sendRequest(jsonrpc::Request &&request);
  void sendResponse(jsonrpc::Response &&response);
  void sendRequestBatch(jsonrpc::RequestBatch &&batch);
  void sendResponseBatch(jsonrpc::ResponseBatch &&batch);

  void processIncomingMessages();

  /*
   * sendRequest
   */
  template <typename MessageType>
    requires message::HasParams<MessageType> && message::HasResult<MessageType>
  [[nodiscard]] FutureResponse<MessageType>
  sendRequest(typename MessageType::Params &&params);

  template <typename MessageType>
    requires message::HasResult<MessageType> &&
             (!message::HasParams<MessageType>)
  [[nodiscard]] FutureResponse<MessageType> sendRequest();

  template <typename MessageType, typename F,
            typename E = void (*)(const Error &)>
    requires message::HasParams<MessageType> &&
             message::HasResult<MessageType> &&
             std::invocable<F, typename MessageType::Result &&> &&
             std::invocable<E, const Error &>
  jsonrpc::MessageId sendRequest(
      typename MessageType::Params &&params, F &&then,
      E &&error = [](const Error &) {});

  template <typename MessageType, typename F,
            typename E = void (*)(const Error &)>
    requires message::HasResult<MessageType> &&
             (!message::HasParams<MessageType>) &&
             std::invocable<F, typename MessageType::Result &&> &&
             std::invocable<E, const Error &>
  jsonrpc::MessageId sendRequest(F &&then, E &&error = [](const Error &) {});

  /*
   * sendNotification
   */

  template <typename MessageType>
    requires(!message::HasParams<MessageType>) &&
            (!message::HasResult<MessageType>)
  void sendNotification() {
    sendNotification(MessageType::Method);
  }

  template <typename MessageType>
    requires message::HasParams<MessageType> &&
             (!message::HasResult<MessageType>)
  void sendNotification(typename MessageType::Params &&params) {
    sendNotification(MessageType::Method, toJson(std::move(params)));
  }

  /*
   * Callback registration
   */

  template <typename MessageType, typename F>
    requires IsRequestCallback<MessageType, F>
  Connection &add(F &&handlerFunc);

  template <typename MessageType, typename F>
    requires IsNoParamsRequestCallback<MessageType, F>
  Connection &add(F &&handlerFunc);

  template <typename MessageType, typename F>
    requires IsNotificationCallback<MessageType, F>
  Connection &add(F &&handlerFunc);

  template <typename MessageType, typename F>
    requires IsNoParamsNotificationCallback<MessageType, F>
  Connection &add(F &&handlerFunc);

  void remove(MessageMethod method);

private:
  std::istream &m_in;
  std::ostream &m_out;
  std::mutex m_readMutex;
  std::mutex m_writeMutex;


  struct MessageHeader {
    std::size_t contentLength = 0;
    std::string contentType = "application/vscode-jsonrpc; charset=utf-8";
  };

  MessageHeader readMessageHeader();
  void readNextMessageHeaderField(MessageHeader &header);
  void writeJsonMessage(const json::Any &content);
  void writeMessage(const std::string &content);
  void writeMessageHeader(const MessageHeader &header);

  class RequestResultBase;
  using RequestResultPtr = std::unique_ptr<RequestResultBase>;

  std::mutex m_pendingRequestsMutex;
  std::unordered_map<jsonrpc::MessageId, RequestResultPtr> m_pendingRequests;

  void onResponse(jsonrpc::Response &&response);
  void onResponseBatch(jsonrpc::ResponseBatch &&batch);

  jsonrpc::MessageId
  sendRequest(MessageMethod method, RequestResultPtr result,
              const std::optional<json::Any> &params = std::nullopt);
  void sendNotification(MessageMethod method,
                        const std::optional<json::Any> &params = std::nullopt);

  /*
   * Request result wrapper
   */

  class RequestResultBase {
  public:
    virtual ~RequestResultBase() = default;
    virtual void setValueFromJson(json::Any &&json) = 0;
    virtual void setException(std::exception_ptr e) = 0;
  };

  template <typename T> class FutureRequestResult : public RequestResultBase {
  public:
    std::future<T> future() { return m_promise.get_future(); }

    void setValueFromJson(json::Any &&json) override {
      T value;
      fromJson(std::move(json), value);
      m_promise.set_value(std::move(value));
    }

    void setException(std::exception_ptr e) override {
      m_promise.set_exception(e);
    }

  private:
    std::promise<T> m_promise;
  };

  template <typename T, typename F, typename E>
  class CallbackRequestResult : public RequestResultBase {
  public:
    CallbackRequestResult(F &&then, E &&error)
        : m_then{std::forward<F>(then)}, m_error{std::forward<E>(error)} {}

    void setValueFromJson(json::Any &&json) override {
      T value;
      fromJson(std::move(json), value);
      m_then(std::move(value));
    }

    void setException(std::exception_ptr e) override {
      try {
        std::rethrow_exception(e);
      } catch (Error &error) {
        m_error(error);
      }
    }

  private:
    F m_then;
    E m_error;
  };

  class ResponseResultBase;
  using ResponseResultPtr = std::unique_ptr<ResponseResultBase>;
  using OptionalResponse = std::optional<jsonrpc::Response>;
  using HandlerWrapper = std::function<OptionalResponse(
      const jsonrpc::MessageId &, json::Any &&, bool)>;

  std::vector<HandlerWrapper> m_requestHandlers;
  std::mutex m_requestHandlersMutex;

  bool m_running = true;
  std::thread m_asyncResponseThread;
  std::mutex m_pendingResponsesMutex;
  std::unordered_map<jsonrpc::MessageId, ResponseResultPtr> m_pendingResponses;

  void onRequest(jsonrpc::Request &&request);
  void onRequestBatch(jsonrpc::RequestBatch &&batch);

  OptionalResponse processRequest(jsonrpc::Request &&request, bool allowAsync);
  void addHandler(MessageMethod method, HandlerWrapper &&handlerFunc);
  void addResponseResult(const jsonrpc::MessageId &id,
                         ResponseResultPtr result);

  template <typename T>
  static jsonrpc::Response createResponse(const jsonrpc::MessageId &id,
                                          T &&result) {
    return jsonrpc::createResponse(id, toJson(std::forward<T>(result)));
  }

  /*
   * Response result wrapper
   */

  class ResponseResultBase {
  public:
    virtual ~ResponseResultBase() = default;
    virtual bool isReady() const = 0;
    virtual jsonrpc::Response createResponse() = 0;
  };

  template <typename T> class ResponseResult : public ResponseResultBase {
  public:
    ResponseResult(jsonrpc::MessageId id, std::future<T> future);

    bool isReady() const override;
    jsonrpc::Response createResponse() override;

  private:
    jsonrpc::MessageId m_id;
    std::future<T> m_future;
  };
};

/*
 * Error thrown when then connection to a client is unexpectedly lost
 */
class ConnectionError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/*
 * Error thrown when there was a problem parsing the incoming message
 */
class ProtocolError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

} // namespace lsp
