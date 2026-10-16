#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <variant>
#include <vector>
#include <test/test.h>
#include <lsp/json/json.h>
#include <lsp/json/writer.h>
#include <lsp/nullable.h>
#include <lsp/serialization.h>
#include <lsp/uri.h>

using namespace lsp;

namespace lsp{

struct test_Point{
	int x;
	int y;
};

void writeJson(const test_Point& value, json::ObjectWriter& writer)
{
	lsp::writeJson("x", value.x, writer);
	lsp::writeJson("y", value.y, writer);
}

struct test_Line{
	test_Point start;
	test_Point end;
};

void writeJson(const test_Line& value, json::ObjectWriter& writer)
{
	lsp::writeJson("start", value.start, writer);
	lsp::writeJson("end", value.end, writer);
}

struct test_Polygon{
	std::vector<test_Point> points;
};

void writeJson(const test_Polygon& value, json::ObjectWriter& writer)
{
	lsp::writeJson("points", value.points, writer);
}

enum class test_Color{ Red, Green, MAX_VALUE };

template<>
const std::string_view Enumeration<test_Color, std::string>::s_values[] = { "red", "green" };

struct test_IntPayload{
	std::string consumedBeforePayload;
	int payload = 0;
};

struct test_StringPayload{
	std::string consumedBeforePayload;
	std::string payload;
};

struct test_ReadOnlyTagPayload{
	const int tag = 1;
	int payload = 0;
};

struct test_ImmovablePayload{
	int payload = 0;
	test_ImmovablePayload() = default;
	test_ImmovablePayload(const test_ImmovablePayload&) = delete;
	test_ImmovablePayload(test_ImmovablePayload&&) = delete;
};

void fromJson(json::Value&& json, test_IntPayload& value)
{
	fromJson(std::move(json.object().get("first")), value.consumedBeforePayload);
	fromJson(std::move(json.object().get("payload")), value.payload);
}

void fromJson(json::Value&& json, test_StringPayload& value)
{
	fromJson(std::move(json.object().get("first")), value.consumedBeforePayload);
	fromJson(std::move(json.object().get("payload")), value.payload);
}

void fromJson(json::Value&& json, test_ReadOnlyTagPayload& value)
{
	fromJson(std::move(json.object().get("payload")), value.payload);
}

void fromJson(json::Value&& json, test_ImmovablePayload& value)
{
	fromJson(std::move(json.object().get("payload")), value.payload);
}

} // namespace lsp

namespace{

std::string build(auto&& fn)
{
	auto writer = json::Writer();
	fn(writer);
	return std::move(writer).text();
}

} // namespace

int main(int argc, char** argv)
{
	auto app = test::TestApp();

	/*
	 * Primitives
	 */

	app.addTest("ToJson/Primitive/TopLevel", [](){
		test::compare(build([](json::Writer& writer){ writeJson(42, writer); }), std::string_view("42"));
	});

	app.addTest("ToJson/Primitive/ArrayElement", [](){
		const auto out = build([](json::Writer& writer){
			auto aw = writer.beginArray();
			writeJson(1, aw);
			writeJson(2, aw);
		});

		test::compare(out, std::string_view("[1,2]"));
	});

	app.addTest("ToJson/Primitive/ObjectProperty", [](){
		const auto out = build([](json::Writer& writer){
			auto ow = writer.beginObject();
			writeJson("a", 1, ow);
		});

		test::compare(out, std::string_view(R"({"a":1})"));
	});

	/*
	 * std::vector
	 */

	app.addTest("ToJson/Vector/TopLevel", [](){
		const auto out = build([](json::Writer& writer){
			writeJson(std::vector<int>{1, 2, 3}, writer);
		});

		test::compare(out, std::string_view("[1,2,3]"));
	});

	app.addTest("ToJson/Vector/Empty", [](){
		const auto out = build([](json::Writer& writer){
			writeJson(std::vector<int>{}, writer);
		});

		test::compare(out, std::string_view("[]"));
	});

	app.addTest("ToJson/Vector/NestedInArray", [](){
		const auto out = build([](json::Writer& writer){
			auto aw = writer.beginArray();
			writeJson(std::vector<int>{1, 2}, aw);
			writeJson(std::vector<int>{3, 4}, aw);
		});

		test::compare(out, std::string_view("[[1,2],[3,4]]"));
	});

	app.addTest("ToJson/Vector/AsObjectProperty", [](){
		const auto out = build([](json::Writer& writer){
			auto ow = writer.beginObject();
			writeJson("items", std::vector<int>{1, 2}, ow);
		});

		test::compare(out, std::string_view(R"({"items":[1,2]})"));
	});

	/*
	 * std::tuple
	 */

	app.addTest("ToJson/Tuple/TopLevel", [](){
		const auto out = build([](json::Writer& writer){
			writeJson(std::tuple<int, std::string>{1, "a"}, writer);
		});

		test::compare(out, std::string_view(R"([1,"a"])"));
	});

	/*
	 * std::unordered_map
	 */

	app.addTest("ToJson/Map/TopLevel", [](){
		const auto out = build([](json::Writer& writer){
			writeJson(std::unordered_map<std::string, int>{{"key", 1}}, writer);
		});

		test::compare(out, std::string_view(R"({"key":1})"));
	});

	app.addTest("ToJson/Map/AsArrayElement", [](){
		const auto out = build([](json::Writer& writer){
			auto aw = writer.beginArray();
			writeJson(std::unordered_map<std::string, int>{{"key", 1}}, aw);
		});

		test::compare(out, std::string_view(R"([{"key":1}])"));
	});

	app.addTest("ToJson/Map/AsObjectProperty", [](){
		const auto out = build([](json::Writer& writer){
			auto ow = writer.beginObject();
			writeJson("m", std::unordered_map<std::string, int>{{"key", 1}}, ow);
		});

		test::compare(out, std::string_view(R"({"m":{"key":1}})"));
	});

	app.addTest("ToJson/Map/UriKey", [](){
		const auto uri = Uri::parse("file:///a/b");
		const auto out = build([&uri](json::Writer& writer){
			writeJson(std::unordered_map<Uri, int>{{uri, 1}}, writer);
		});

		test::compare(out, R"({")" + uri.toString() + R"(":1})");
	});

	/*
	 * std::variant
	 */

	app.addTest("ToJson/Variant", [](std::variant<int, std::string> value, std::string_view expected){
		const auto out = build([&value](json::Writer& writer){
			writeJson(value, writer);
		});

		test::compare(out, expected);
	})({
		{"Int",    {42,  "42"}},
		{"String", {"a", R"("a")"}},
	});

	/*
	 * std::optional
	 */

	app.addTest("ToJson/Optional", [](bool hasValue, std::string_view expected){
		const auto out = build([hasValue](json::Writer& writer){
			writeJson(hasValue ? std::optional<int>(42) : std::optional<int>(), writer);
		});

		test::compare(out, expected);
	})({
		{"HasValue", {true,  "42"}},
		{"Empty",    {false, "null"}},
	});

	/*
	 * std::unique_ptr
	 */

	app.addTest("ToJson/UniquePtr/HasValue", [](){
		const auto out = build([](json::Writer& writer){
			writeJson(std::make_unique<int>(42), writer);
		});

		test::compare(out, std::string_view("42"));
	});

	app.addTest("ToJson/UniquePtr/Null", [](){
		const auto out = build([](json::Writer& writer){
			writeJson(std::unique_ptr<int>(), writer);
		});

		test::compare(out, std::string_view("null"));
	});

	/*
	 * Nullable
	 */

	app.addTest("ToJson/Nullable", [](bool hasValue, std::string_view expected){
		const auto out = build([hasValue](json::Writer& writer){
			writeJson(hasValue ? Nullable<int>(42) : Nullable<int>(nullptr), writer);
		});

		test::compare(out, expected);
	})({
		{"HasValue", {true,  "42"}},
		{"Null",     {false, "null"}},
	});

	/*
	 * NullableVariant
	 */

	app.addTest("ToJson/NullableVariant", [](bool hasValue, std::string_view expected){
		const auto out = build([hasValue](json::Writer& writer){
			writeJson(hasValue ? NullableVariant<int, std::string>(42) : NullableVariant<int, std::string>(nullptr), writer);
		});

		test::compare(out, expected);
	})({
		{"HasValue", {true,  "42"}},
		{"Null",     {false, "null"}},
	});

	/*
	 * Enumeration
	 */

	app.addTest("ToJson/Enumeration", [](std::string value, std::string_view expected){
		const auto out = build([&value](json::Writer& writer){
			writeJson(Enumeration<test_Color, std::string>(std::move(value)), writer);
		});

		test::compare(out, expected);
	})({
		{"Red",    {"red",    R"("red")"}},
		{"Green",  {"green",  R"("green")"}},
		{"Custom", {"custom", R"("custom")"}},
	});

	/*
	 * Uri
	 */

	app.addTest("ToJson/Uri", [](){
		const auto uri = Uri::parse("file:///a/b");
		const auto out = build([&uri](json::Writer& writer){
			writeJson(uri, writer);
		});

		test::compare(out, "\"" + uri.toString() + "\"");
	});

	/*
	 * Struct
	 */

	app.addTest("ToJson/Struct/TopLevel", [](){
		const auto out = build([](json::Writer& writer){
			writeJson(test_Point{1, 2}, writer);
		});

		test::compare(out, std::string_view(R"({"x":1,"y":2})"));
	});

	app.addTest("ToJson/Struct/AsArrayElement", [](){
		const auto out = build([](json::Writer& writer){
			writeJson(std::vector<test_Point>{{1, 2}, {3, 4}}, writer);
		});

		test::compare(out, std::string_view(R"([{"x":1,"y":2},{"x":3,"y":4}])"));
	});

	app.addTest("ToJson/Struct/AsObjectProperty", [](){
		const auto out = build([](json::Writer& writer){
			auto ow = writer.beginObject();
			writeJson("point", test_Point{1, 2}, ow);
		});

		test::compare(out, std::string_view(R"({"point":{"x":1,"y":2}})"));
	});

	app.addTest("ToJson/Struct/NestedObject", [](){
		const auto out = build([](json::Writer& writer){
			writeJson(test_Line{{1, 2}, {3, 4}}, writer);
		});

		test::compare(out, std::string_view(R"({"start":{"x":1,"y":2},"end":{"x":3,"y":4}})"));
	});

	app.addTest("ToJson/Struct/ContainingArray", [](){
		const auto out = build([](json::Writer& writer){
			writeJson(test_Polygon{{{1, 2}, {3, 4}, {5, 6}}}, writer);
		});

		test::compare(out, std::string_view(R"({"points":[{"x":1,"y":2},{"x":3,"y":4},{"x":5,"y":6}]})"));
	});

	app.addTest("FromJson/IntegerBounds", [](){
		int value = 9;
		fromJson(json::Value(-2147483648.0), value);
		test::compare(value, std::numeric_limits<int>::min());
		fromJson(json::Value(2147483647.0), value);
		test::compare(value, std::numeric_limits<int>::max());
		test::expectException<json::TypeError>([&](){ fromJson(json::Value("text"), value); }, "JSON value is not number");
		for(const auto number : {-2147483649.0, 2147483648.0, 0.5,
			std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
		{
			test::expectException<json::TypeError>([&](){ fromJson(json::Value(number), value); });
			test::compare(value, std::numeric_limits<int>::max());
		}
		unsigned long long wide = 0;
		fromJson(json::Value(4294967295.0), wide);
		test::compare(wide, 4294967295ULL);
		test::expectException<json::TypeError>([&](){
			fromJson(json::Value(std::ldexp(1.0, std::numeric_limits<unsigned long long>::digits)), wide);
		});
		long signedWide = 0;
		test::expectException<json::TypeError>([&](){
			fromJson(json::Value(std::ldexp(1.0, std::numeric_limits<long>::digits)), signedWide);
		});
	});

	app.addTest("FromJson/UnsignedLspBounds", [](){
		unsigned int value = 0;
		fromJson(json::Value(2147483647.0), value);
		test::compare(value, 2147483647u);
		for(const auto number : {-1.0, 2147483648.0, 4294967295.0, 0.5})
			test::expectException<json::TypeError>([&](){ fromJson(json::Value(number), value); });
	});

	app.addTest("ToJson/UnsignedLspBounds", [](){
		test::compare(build([](auto& writer){ writeJson(2147483647u, writer); }), "2147483647");
		test::expectException<json::TypeError>([](){
			(void)build([](auto& writer){ writeJson(2147483648u, writer); });
		});
		test::expectException<json::TypeError>([](){
			(void)build([](auto& writer){ auto object = writer.beginObject(); writeJson("n", 2147483648u, object); });
		});
		test::expectException<json::TypeError>([](){
			(void)build([](auto& writer){ writeJson(std::vector<unsigned int>{2147483648u}, writer); });
		});
		test::compare(json::stringify(json::Value(4294967295u)), "4294967295.0");
		test::compare(build([](auto& writer){ writeJson(4294967295ULL, writer); }), "4294967295");
	});

	app.addTest("FromJson/FloatBounds", [](){
		float value = 1;
		fromJson(json::Value(2.5), value);
		test::compare(value, 2.5f);
		test::expectException<json::TypeError>([&](){ fromJson(json::Value(1e300), value); });
		test::compare(value, 2.5f);
	});

	app.addTest("FromJson/NullType", [](){
		fromJson(json::Value(nullptr), nullptr);
		test::expectException<json::TypeError>([](){ fromJson(json::Value(1), nullptr); });
	});

	app.addTest("FromJson/ReplacesExistingContainers", [](){
		auto vector = std::vector<int>{9};
		fromJson(json::parse("[1,2]"), vector);
		test::compare(vector, std::vector<int>{1, 2});
		fromJson(json::parse("[]"), vector);
		test::check(vector.empty());
		auto map = std::unordered_map<std::string, int>{{"old", 9}};
		fromJson(json::parse(R"({"new":1})"), map);
		test::compare(map.size(), 1);
		test::compare(map.at("new"), 1);
		fromJson(json::parse("{}"), map);
		test::check(map.empty());
		auto uriMap = std::unordered_map<Uri, int>{{Uri::parse("file:///old"), 9}};
		fromJson(json::parse(R"({"file:///new":1})"), uriMap);
		test::compare(uriMap.size(), 1);
		test::compare(uriMap.at(Uri::parse("file:///new")), 1);
	});

	app.addTest("FromJson/VectorBool", [](){
		auto vector = std::vector<bool>{true};
		fromJson(json::parse("[false,true]"), vector);
		test::compare(vector.size(), 2);
		test::check(!vector[0] && vector[1]);
	});

	app.addTest("FromJson/VariantsCheckEveryArrayElement", [](){
		using Mixed = std::vector<std::variant<int, std::string>>;
		auto value = std::variant<std::vector<int>, Mixed>{};
		fromJson(json::parse(R"([1,"a"])"), value);
		test::compare(value.index(), 1);
		test::compare(std::get<std::string>(std::get<1>(value)[1]), "a");
	});

	app.addTest("FromJson/VariantsCheckEveryTupleElement", [](){
		auto value = std::variant<std::tuple<int, std::string>, std::tuple<int, int>>{};
		fromJson(json::parse("[1,2]"), value);
		test::compare(value.index(), 1);
		test::compare(std::get<1>(std::get<1>(value)), 2);
		auto empty = std::variant<std::tuple<>, std::tuple<int>>{};
		fromJson(json::parse("[]"), empty);
		test::compare(empty.index(), 0);
	});

	app.addTest("FromJson/AnyAndUriVariants", [](){
		auto any = std::variant<json::Value, int>{};
		fromJson(json::Value("text"), any);
		test::compare(std::get<json::Value>(any).string(), "text");
		auto uri = std::variant<Uri, int>{};
		fromJson(json::Value("file:///a"), uri);
		test::compare(std::get<Uri>(uri).toString(), "file:///a");
		auto optionalAny = std::optional<json::Value>{};
		fromJson(json::Value(nullptr), optionalAny);
		test::check(optionalAny.has_value() && optionalAny->isNull());
	});

	app.addTest("ToJson/NestedOptionalAndUniquePointer", [](){
		const auto optional = build([](auto& writer){
			writeJson(std::vector<std::optional<int>>{1, std::nullopt}, writer);
		});
		test::compare(optional, "[1,null]");
		const auto unique = build([](auto& writer){
			auto values = std::vector<std::unique_ptr<int>>{};
			values.push_back(std::make_unique<int>(2));
			values.push_back(nullptr);
			writeJson(values, writer);
		});
		test::compare(unique, "[2,null]");
	});

	app.addTest("FromJson/VariantTriesAnotherStructuralCandidate", [](){
		auto value = std::variant<test_IntPayload, test_StringPayload>{};
		fromJson(json::parse(R"({"first":"preserved","payload":"text"})"), value);
		test::compare(value.index(), 1);
		test::compare(std::get<1>(value).consumedBeforePayload, "preserved");
		test::compare(std::get<1>(value).payload, "text");
		test::expectException<json::TypeError>([&](){
			fromJson(json::parse(R"({"first":"consumed","payload":true})"), value);
		});
		test::compare(value.index(), 1);
		test::compare(std::get<1>(value).consumedBeforePayload, "preserved");
		test::compare(std::get<1>(value).payload, "text");
	});

	app.addTest("FromJson/InvalidUriIsRejectedWithoutLosingAnEntry", [](){
		auto uri = Uri::parse("file:///existing");
		for(const auto* text : {"", "not-uri"})
		{
			test::expectException<json::TypeError>([&](){ fromJson(json::Value(text), uri); });
			test::compare(uri.toString(), "file:///existing");
		}
		auto uriMap = std::unordered_map<Uri, int>{};
		test::expectException<json::TypeError>([&](){ fromJson(json::parse(R"({"not-uri":1})"), uriMap); });
		test::expectException<json::TypeError>([&](){ fromJson(json::parse(R"({"":1})"), uriMap); });
		test::compare(build([](auto& writer){ writeJson(Uri{}, writer); }), R"("")");
	});

	app.addTest("FromJson/VariantDoesNotRequireAssignment", [](){
		auto value = std::variant<test_ReadOnlyTagPayload, std::string>{};
		fromJson(json::parse(R"({"payload":42})"), value);
		test::compare(std::get<0>(value).tag, 1);
		test::compare(std::get<0>(value).payload, 42);
	});

	app.addTest("FromJson/ImmovableVariantRetainsInPlaceConversion", [](){
		auto value = std::variant<test_ImmovablePayload, std::string>(std::in_place_index<1>, "old");
		fromJson(json::parse(R"({"payload":42})"), value);
		test::compare(std::get<0>(value).payload, 42);
	});

	return app.main(argc, argv);
}
