#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <variant>
#include <test/test.h>
#include <lspgen/meta_model.h>
#include <lspgen/util.h>
#include <lsp/types.h>

int main(int argc, char** argv)
{
	auto app = test::TestApp();
	app.addTest("Generated/Defaults", []()
	{
		alignas(lsp::AuditDefaults) std::array<std::byte, sizeof(lsp::AuditDefaults)> memory;
		memory.fill(std::byte{0xa5});
		auto* value = ::new(memory.data()) lsp::AuditDefaults;
		test::compare(value->count, 0);
		test::check(!value->enabled);
		test::compare(value->ratio, 0.0);
		std::destroy_at(value);
	});
	app.addTest("Generated/AbsentOptionalsClearPreviousValues", []()
	{
		lsp::AuditOptions value;
		lsp::fromJson(lsp::json::parse(R"({"count":4,"label":"old","nested":{"count":1}})"), value);
		test::check(value.count && value.label && value.nested);
		lsp::fromJson(lsp::json::parse("{}"), value);
		test::check(!value.count && !value.label && !value.nested);
	});
	app.addTest("Generated/InheritedLiteral", []()
	{
		lsp::AuditLeaf value;
		test::compare(value.kind, "leaf");
		test::check(&value.kind == &static_cast<lsp::AuditBase&>(value).kind);
		lsp::fromJson(lsp::json::parse(R"({"kind":"leaf"})"), value);
		test::expectException<lsp::json::TypeError>([&]()
		{
			lsp::fromJson(lsp::json::parse(R"({"kind":"wrong"})"), value);
		});
	});
	app.addTest("Generated/DistinctInlineUnionTypes", []()
	{
		lsp::AuditChoice value;
		lsp::fromJson(lsp::json::parse(R"({"payload":7})"), value);
		test::compare(value.index(), 0u);
		test::compare(std::get<0>(value).payload, 7);
		lsp::fromJson(lsp::json::parse(R"({"payload":"text"})"), value);
		test::compare(value.index(), 1u);
		test::compare(std::get<1>(value).payload, "text");
	});
	app.addTest("Generated/OptionalLiterals", []()
	{
		lsp::AuditOptionalLiterals value;
		test::check(!value.kind && !value.flag && !value.number);
		lsp::fromJson(lsp::json::parse(R"({"kind":"audit","flag":true,"number":7})"), value);
		test::check(value.kind && value.flag && value.number);
		lsp::fromJson(lsp::json::parse("{}"), value);
		test::check(!value.kind && !value.flag && !value.number);
		test::expectException<lsp::json::TypeError>([&]()
		{
			lsp::fromJson(lsp::json::parse(R"({"flag":false})"), value);
		});
	});
	app.addTest("Generated/InheritedOptionalLiteralStaysAbsent", []()
	{
		lsp::AuditOptionalLeaf value;
		test::check(!value.kind);
		lsp::fromJson(lsp::json::parse(R"({"kind":"leaf"})"), value);
		test::check(value.kind && *value.kind == "leaf");
		lsp::fromJson(lsp::json::parse("{}"), value);
		test::check(!value.kind);
	});
	app.addTest("MetaModel/IntegerLiteralBounds", []()
	{
		for(const auto* text : {R"({"value":2147483648})", R"({"value":-2147483649})", R"({"value":1.5})"})
			test::expectException<lsp::json::TypeError>([&]()
			{
				lspgen::IntegerLiteralType literal;
				literal.extract(lsp::json::parse(text).object());
			});
		lspgen::IntegerLiteralType literal;
		literal.extract(lsp::json::parse(R"({"value":2147483647.0})").object());
		test::compare(literal.integerValue, 2147483647);
	});
#if defined(__linux__)
	app.addTest("Generator/WriteFailureIsReported", []()
	{
		test::expectException<std::runtime_error>([]()
		{
			lspgen::writeFileContent("/dev/full", "unwritten");
		});
	});
#endif
	return app.main(argc, argv);
}
