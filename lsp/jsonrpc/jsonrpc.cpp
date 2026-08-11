#include <cassert>
#include <lsp/jsonrpc/jsonrpc.h>

namespace lsp::jsonrpc{
namespace{

constexpr std::string_view ProtocolVersion{"2.0"};

void verifyProtocolVersion(const json::Object& json)
{
	const auto* const jsonrpc = json.find("jsonrpc");

	if(!jsonrpc)
		throw ProtocolError{"jsonrpc property is missing"};

	if(!jsonrpc->isString())
		throw ProtocolError{"jsonrpc property expected to be a string"};

	if(jsonrpc->string() != ProtocolVersion)
		throw ProtocolError{"Invalid or unsupported jsonrpc version"};
}

MessageId messageIdFromJson(json::Value& json)
{
	if(json.isString())
		return std::move(json.string());

	if(json.isNumber())
		return static_cast<json::Integer>(json.number());

	if(json.isNull())
		return nullptr;

	throw ProtocolError{"Request id type must be string, number or null"};
}

Request requestFromJson(json::Object& json)
{
	verifyProtocolVersion(json);

	Request request;
	request.method = std::move(json.get("method").string());

	if(auto* const id = json.find("id"))
		request.id = messageIdFromJson(*id);

	if(auto* const params = json.find("params"))
	{
		if(params->isObject())
			request.params = std::move(params->object());
		else if(params->isArray())
			request.params = std::move(params->array());
		else if(!params->isNull()) // Be lenient and allow null params even though it is not allowed by jsonrpc 2.0
			throw ProtocolError{"Params type must be object or array"};
	}

	return request;
}

Response responseFromJson(json::Object& json)
{
	verifyProtocolVersion(json);

	Response response;

	if(auto* const id = json.find("id"))
		response.id = messageIdFromJson(*id);

	if(auto* const result = json.find("result"))
		response.result = std::move(*result);

	if(auto* const error = json.find("error"))
	{
		auto& errorObj      = error->object();
		auto& responseError = response.error.emplace();

		const auto* const errorCode = errorObj.find("code");

		if(!errorCode)
			throw ProtocolError{"Response error is missing the error code"};

		if(!errorCode->isNumber())
			throw ProtocolError{"Response error code must be a number"};

		responseError.code = static_cast<json::Integer>(errorCode->number());

		auto* const errorMessage = errorObj.find("message");

		if(!errorMessage)
			throw ProtocolError{"Response error is missing the error message"};

		if(!errorMessage->isString())
			throw ProtocolError{"Response error message must be a string"};

		responseError.message = std::move(errorMessage->string());

		if(auto* const data = errorObj.find("data"))
			responseError.data = std::move(*data);
	}

	if((response.result.has_value() && response.error.has_value()) || (!response.result.has_value() && !response.error.has_value()))
		throw ProtocolError{"Response must have either 'result' or 'error'"};

	return response;
}

} // namespace

Message messageFromJson(json::Object&& json)
{
	if(json.contains("method"))
		return requestFromJson(json);

	return responseFromJson(json);
}

MessageBatch messageBatchFromJson(json::Array&& json)
{
	if(json.empty())
		throw ProtocolError{"Message batch must not be empty"};

	auto batch = MessageBatch();
	batch.reserve(json.size());

	for(auto& jsonMessage : json)
		batch.push_back(messageFromJson(std::move(jsonMessage.object())));

	return batch;
}

json::Object messageToJson(Message&& message)
{
	json::Object json;
	json["jsonrpc"] = std::string{ProtocolVersion};

	if(auto* const request = std::get_if<Request>(&message))
	{
		if(request->id.has_value())
			std::visit([&json](auto& v){ json["id"] = std::move(v); }, *request->id);

		json["method"] = std::move(request->method);

		if(request->params.has_value())
			json["params"] = std::move(*request->params);
	}
	else
	{
		auto& response = std::get<Response>(message);
		assert(response.result.has_value() != response.error.has_value());

		std::visit([&json](auto& v){ json["id"] = std::move(v); }, response.id);

		if(response.result.has_value())
			json["result"] = std::move(*response.result);

		if(response.error.has_value())
		{
			auto& responseError = *response.error;
			auto  errorJson     = json::Object();

			errorJson["code"]    = responseError.code;
			errorJson["message"] = std::move(responseError.message);

			if(responseError.data.has_value())
				errorJson["data"] = std::move(*responseError.data);

			json["error"] = std::move(errorJson);
		}
	}

	return json;
}

json::Array messageBatchToJson(MessageBatch&& batch)
{
	auto json = json::Array();
	json.reserve(batch.size());

	for(auto& message : batch)
		json.push_back(messageToJson(std::move(message)));

	return json;
}

Request createRequest(MessageId id, std::string_view method, std::optional<json::Value> params)
{
	Request request;
	request.id = std::move(id);
	request.method = method;
	request.params = std::move(params);
	return request;
}

Request createNotification(std::string_view method, std::optional<json::Value> params)
{
	Request notification;
	notification.method = method;
	notification.params = std::move(params);
	return notification;
}

Response createResponse(MessageId id, json::Value result)
{
	Response response;
	response.id = std::move(id);
	response.result = std::move(result);
	return response;
}

Response createErrorResponse(MessageId id, json::Integer errorCode, json::String message, std::optional<json::Value> data)
{
	Response response;
	response.id = std::move(id);
	auto& error = response.error.emplace();
	error.code = errorCode;
	error.message = std::move(message);
	error.data = std::move(data);
	return response;
}

} // namespace lsp::jsonrpc
