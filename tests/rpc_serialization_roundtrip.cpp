// Round-trip checks for the jsonrpc find() based decode and the serialization.h
// StrMap fromJson/toJson rewrites. Built by -DLSP_BUILD_TESTS=ON, run by ctest.
#include <cassert>
#include <cstdio>
#include <string>

#include <lsp/uri.h>
#include <lsp/json/json.h>
#include <lsp/jsonrpc/jsonrpc.h>
#include <lsp/serialization.h>
#include <lsp/strmap.h>

using namespace lsp;

namespace{

void checkRequestRoundTrip()
{
	json::Object params;
	params["uri"] = json::String{"file:///a.txt"};

	auto request = jsonrpc::createRequest(json::Integer(7), "textDocument/hover", json::Value(std::move(params)));
	auto wire    = json::stringify(jsonrpc::messageToJson(jsonrpc::Message(std::move(request))));

	auto parsedJson = json::parse(wire);
	auto parsed     = jsonrpc::messageFromJson(std::move(parsedJson.object()));
	auto& back      = std::get<jsonrpc::Request>(parsed);

	assert(back.method == "textDocument/hover");
	assert(back.id.has_value());
	assert(std::get<json::Integer>(*back.id) == 7);
	assert(back.params.has_value());
	assert(back.params->object().get("uri").string() == "file:///a.txt");

	std::puts("ok: request round-trip");
}

void checkSuccessResponseRoundTrip()
{
	auto response = jsonrpc::createResponse(json::String("id-1"), json::Value(json::Integer(42)));
	auto wire     = json::stringify(jsonrpc::messageToJson(jsonrpc::Message(std::move(response))));

	auto parsedJson = json::parse(wire);
	auto parsed     = jsonrpc::messageFromJson(std::move(parsedJson.object()));
	auto& back      = std::get<jsonrpc::Response>(parsed);

	assert(std::get<json::String>(back.id) == "id-1");
	assert(back.result.has_value());
	assert(back.result->number() == 42);
	assert(!back.error.has_value());

	std::puts("ok: success response round-trip");
}

void checkErrorResponseRoundTrip()
{
	json::Object data;
	data["retry"] = true;
	data["nested"] = json::Object();

	auto response = jsonrpc::createErrorResponse(json::Integer(3), jsonrpc::Error::InvalidParams,
	                                               "bad params", json::Value(std::move(data)));
	auto wire = json::stringify(jsonrpc::messageToJson(jsonrpc::Message(std::move(response))));

	// error.data must live inside the error object on the wire
	auto onWire = json::parse(wire);
	assert(onWire.object().get("error").object().contains("data"));

	auto parsedJson = json::parse(wire);
	auto parsed     = jsonrpc::messageFromJson(std::move(parsedJson.object()));
	auto& back      = std::get<jsonrpc::Response>(parsed);

	assert(!back.result.has_value());
	assert(back.error.has_value());
	assert(back.error->code == jsonrpc::Error::InvalidParams);
	assert(back.error->message == "bad params");
	assert(back.error->data.has_value());
	assert(back.error->data->object().get("retry").boolean() == true);

	std::puts("ok: error response round-trip (data preserved)");
}

void checkDocumentUriMapRoundTrip()
{
	StrMap<DocumentUri, int> map;
	map[Uri::fileUriFromPath("/tmp/a.txt")] = 1;
	map[Uri::fileUriFromPath("/tmp/b.txt")] = 2;
	map[Uri::fileUriFromPath("/tmp/c.txt")] = 3;

	auto json = toJson(StrMap<DocumentUri, int>(map));

	StrMap<DocumentUri, int> back;
	fromJson(std::move(json), back);

	assert(back.size() == map.size());
	for(auto&& [k, v] : map)
	{
		auto it = back.find(k);
		assert(it != back.end());
		assert(it->second == v);
	}

	std::puts("ok: Map<DocumentUri, int> round-trip (keys preserved)");
}

void checkUriMapRoundTrip()
{
	// Exercises fromJson(StrMap<Uri, T>&) specifically -- the single-parse fix.
	StrMap<Uri, int> map;
	map[Uri::parse("http://example.com/a")] = 1;
	map[Uri::parse("http://example.com/b")] = 2;

	auto json = toJson(StrMap<Uri, int>(map));

	StrMap<Uri, int> back;
	fromJson(std::move(json), back);

	assert(back.size() == map.size());
	for(auto&& [k, v] : map)
	{
		auto it = back.find(k);
		assert(it != back.end());
		assert(it->second == v);
	}

	std::puts("ok: Map<Uri, int> round-trip (keys preserved)");
}

void checkStringMapRoundTrip()
{
	// Exercises the generic StrMap<K,T> fromJson overload (key move out of the entry).
	StrMap<std::string, std::string> map;
	map["alpha"] = "1";
	map["beta"]  = "2";
	map["gamma"] = "3";

	auto json = toJson(StrMap<std::string, std::string>(map));

	StrMap<std::string, std::string> back;
	fromJson(std::move(json), back);

	assert(back.size() == map.size());
	for(auto&& [k, v] : map)
	{
		auto it = back.find(k);
		assert(it != back.end());
		assert(it->second == v);
	}

	std::puts("ok: Map<std::string, std::string> round-trip (keys preserved)");
}

} // namespace

int main()
{
	checkRequestRoundTrip();
	checkSuccessResponseRoundTrip();
	checkErrorResponseRoundTrip();
	checkDocumentUriMapRoundTrip();
	checkUriMapRoundTrip();
	checkStringMapRoundTrip();
	std::puts("all round-trip checks passed");
	return 0;
}
