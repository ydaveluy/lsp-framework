#pragma once

#include <optional>
#include <string_view>
#include <lsp/json/json.h>

namespace lsp::jsonrpc{

	using Id = std::variant<json::String, json::Integer, json::Null>;
	using Parameters = std::variant<json::Object, json::Array>;

	struct Request{
		std::string               jsonrpc;
		std::string               method;
		std::optional<Id>         id;
		std::optional<Parameters> params;
	};

	Request requestFromJson(const json::Any& json);

	enum class ErrorCode{
		ParseError     = -32700,
		InvalidRequest = -32600,
		MethodNotFound = -32601,
		InvalidParams  = -32602,
		InternalError  = -32603
	};

	struct Error{
		int                      code;
		json::String             message;
		std::optional<json::Any> data;
	};

	struct Response{
		const std::string        jsonrpc = "2.0";
		Id                       id;
		std::optional<json::Any> result;
		std::optional<Error>     error;
	};

	json::Any responseToJson(const Response& response);

}
