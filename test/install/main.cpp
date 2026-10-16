#include <future>
#include <lsp/client_endpoint.h>
#include <lsp/thread_pool.h>
#include <lsp/uri.h>

static_assert(__cpp_concepts >= 201907L);

int main()
{
	auto completed = std::promise<void>();
	auto ready = completed.get_future();
	auto pool = lsp::ThreadPool(1);
	pool.addTask([&completed](){ completed.set_value(); });
	ready.get();
	return lsp::Uri::parse("file:/consumer").isValid() ? 0 : 1;
}
