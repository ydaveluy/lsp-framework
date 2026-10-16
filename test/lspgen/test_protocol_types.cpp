#include <array>
#include <cstddef>
#include <memory>
#include <new>
#include <test/test.h>
#include <lsp/json/json.h>
#include <lsp/protocol_version.h>
#include <lsp/types.h>

int main(int argc, char** argv)
{
	auto app = test::TestApp();
	app.addTest("Protocol/CompletionDefaults", []()
	{
		alignas(lsp::CompletionList) std::array<std::byte, sizeof(lsp::CompletionList)> memory;
		memory.fill(std::byte{0xa5});
		auto* value = ::new(memory.data()) lsp::CompletionList;
		test::check(!value->isIncomplete);
		test::check(value->items.empty());
		std::destroy_at(value);
	});
	app.addTest("Protocol/ResourceOperationLiteral", []()
	{
		lsp::CreateFile value;
		test::expectException<lsp::json::TypeError>([&]()
		{
			lsp::fromJson(lsp::json::parse(R"({"kind":"rename","uri":"file:/x"})"), value);
		});
		lsp::fromJson(lsp::json::parse(R"({"kind":"create","uri":"file:/x"})"), value);
		test::compare(value.kind, "create");
	});
#if LSP_PROTOCOL_VERSION >= LSP_INT_VERSION(3, 18, 0)
	app.addTest("Protocol/InlayHintPartialResult", []()
	{
		lsp::InlayHintParams value;
		lsp::fromJson(lsp::json::parse(R"({"textDocument":{"uri":"file:/x"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"partialResultToken":"p"})"), value);
		test::check(value.partialResultToken.has_value());
		test::compare(std::get<lsp::String>(*value.partialResultToken), "p");
	});
#endif
	return app.main(argc, argv);
}
