#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <lsp/io/stream.h>
#include <lsp/io/sigpipe_guard.h>
#include <lsp/io/standard_io.h>

namespace lsp::io{
namespace{

class StandardIOStream : public Stream{
public:
	StandardIOStream()
	{
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
		_setmode(_fileno(stdout), _O_BINARY);
#endif
	}

	void read(char* buffer, std::size_t size) override
	{
		if(std::fread(buffer, 1, size, stdin) != size)
		{
			if(std::ferror(stdin) != 0)
				throw Error(std::strerror(errno));
			throw Error("Reached EOF");
		}
	}

	void write(const char* buffer, std::size_t size) override
	{
		if(size == 0)
			return;

#if defined(__APPLE__) || defined(__linux__) || defined(__HAIKU__)
		auto signalGuard = detail::SigpipeGuard();
		if(signalGuard.error() != 0)
			throw Error(std::strerror(signalGuard.error()));
#endif
		if(std::fwrite(buffer, 1, size, stdout) != size)
		{
#if defined(__APPLE__) || defined(__linux__) || defined(__HAIKU__)
			signalGuard.failedWith(errno);
#endif
			throw Error(std::strerror(errno));
		}

		if(std::fflush(stdout) != 0)
		{
#if defined(__APPLE__) || defined(__linux__) || defined(__HAIKU__)
			signalGuard.failedWith(errno);
#endif
			throw Error(std::strerror(errno));
		}
	}
};

}

auto standardIO() -> Stream&
{
	static auto stream = StandardIOStream();
	return stream;
}

} // namespace lsp::io
