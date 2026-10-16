#include <lsp/io/socket.h>

#ifndef LSP_SOCKET_UNSUPPORTED

#include <cassert>
#include <algorithm>
#include <limits>

#ifdef LSP_SOCKET_POSIX
#include <cerrno>
#include <netdb.h>
#ifndef __linux__
#include <fcntl.h>
#endif
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#elif defined LSP_SOCKET_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <iphlpapi.h>
#include <mutex> // std::call_once
#endif

namespace lsp::io{

/*
 * Socket::Impl
 */

struct Socket::Impl : Stream{
	static constexpr auto InvalidSocket =
#ifdef LSP_SOCKET_POSIX
	-1;
#elif defined(LSP_SOCKET_WIN32)
	INVALID_SOCKET;
#endif
	using SocketHandle =
#ifdef LSP_SOCKET_POSIX
	int;
#elif defined(LSP_SOCKET_WIN32)
	SOCKET;
#endif

	using SizeType =
#ifdef LSP_SOCKET_POSIX
		socklen_t;
#elif defined(LSP_SOCKET_WIN32)
		int;
#endif

	SocketHandle   m_socketFd = InvalidSocket;
	unsigned short m_port     = 0;

	Impl(SocketHandle socket, unsigned short port)
		: m_socketFd(socket)
		, m_port(port)
	{
	}

	~Impl()
	{
		closeSocketHandle(m_socketFd);
	}

	static void closeSocketHandle(SocketHandle handle)
	{
		if(handle == InvalidSocket)
			return;

#ifdef LSP_SOCKET_POSIX
		const auto error = errno;
		shutdown(handle, SHUT_RDWR);
		::close(handle);
		errno = error;
#elif defined(LSP_SOCKET_WIN32)
		const auto error = WSAGetLastError();
		shutdown(handle, SD_BOTH);
		closesocket(handle);
		WSASetLastError(error);
#endif
	}

	static void ensureInitialized()
	{
#ifdef LSP_SOCKET_WIN32
		static std::once_flag initFlag;
		std::call_once(initFlag, []()
		{
				WSADATA wsaData;
				if(WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
					throwError("WSAStartup failed");
		});
#endif
	}

	static void configureSocket([[maybe_unused]] SocketHandle handle)
	{
#ifdef LSP_SOCKET_POSIX
#ifndef __linux__
		if(fcntl(handle, F_SETFD, FD_CLOEXEC) == -1)
			throwError("Failed to configure socket inheritance");
#endif
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
		const int yes = 1;
		if(setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0)
			throwError("Failed to disable socket SIGPIPE");
#endif
#elif defined(LSP_SOCKET_WIN32)
		if(!SetHandleInformation(reinterpret_cast<HANDLE>(handle), HANDLE_FLAG_INHERIT, 0))
		{
			const auto error = GetLastError();
			throw Error("Failed to configure socket inheritance: " + std::to_string(error));
		}
#endif
	}

	static auto createSocket(int family, int type, int protocol) -> SocketHandle
	{
#ifdef LSP_SOCKET_WIN32
		return WSASocketW(family, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
		return ::socket(family, type | SOCK_CLOEXEC, protocol);
#else
		return ::socket(family, type, protocol);
#endif
	}

	[[noreturn]]
	static void throwError(const std::string& msg)
	{
		const auto errorCode =
#ifdef LSP_SOCKET_POSIX
		errno;
#elif defined(LSP_SOCKET_WIN32)
		WSAGetLastError();
#endif
		throw Error(msg + ": " + std::to_string(errorCode));
	}

	static auto setupForListen(unsigned short port, unsigned short backlog) -> std::unique_ptr<Impl>
	{
		ensureInitialized();

		auto impl = std::make_unique<Impl>(InvalidSocket, port);
		const auto socketFd = impl->m_socketFd = createSocket(AF_INET, SOCK_STREAM, 0);

		if(socketFd == InvalidSocket)
			throwError("Failed to create socket");
		configureSocket(socketFd);

		const int yes = 1;
#ifdef LSP_SOCKET_POSIX
		const int optName = SO_REUSEADDR;
		const auto* option = &yes;
#elif defined(LSP_SOCKET_WIN32)
		const int optName = SO_EXCLUSIVEADDRUSE;
		const auto* option = reinterpret_cast<const char*>(&yes);
#endif
		if(setsockopt(socketFd, SOL_SOCKET, optName, option, sizeof(yes)) != 0)
			throwError("Failed to configure socket address reuse");

		auto addr = sockaddr_in{};
		addr.sin_family      = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port        = htons(port);

		if(bind(socketFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
			throwError("Failed to bind socket address");

		SizeType addrLen = sizeof(addr);
		if(getsockname(socketFd, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0)
			throwError("Failed to get listen port");
		if(::listen(socketFd, backlog) != 0)
			throwError("Failed to listen for new socket connections");

		impl->m_port = ntohs(addr.sin_port);
		return impl;
	}

	static auto connect(const std::string& address, unsigned short port) -> std::unique_ptr<Impl>
	{
		if(port == 0)
			throw Error("Cannot connect on port 0");

		ensureInitialized();

		addrinfo hints = {};
		hints.ai_family   = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		addrinfo* addrInfoList = nullptr;

		if(auto status = getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hints, &addrInfoList); status != 0)
			throwError("getaddrinfo: " + std::to_string(status));
		const auto addresses = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>(addrInfoList, &freeaddrinfo);

		for(const auto* addr = addresses.get(); addr; addr = addr->ai_next)
		{
			auto impl = std::make_unique<Impl>(InvalidSocket, port);
			const auto socketFd = impl->m_socketFd = createSocket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);

			if(socketFd == InvalidSocket)
				continue;
			configureSocket(socketFd);

			if(::connect(socketFd, addr->ai_addr, static_cast<SizeType>(addr->ai_addrlen)) == 0)
				return impl;
		}

		throwError("Failed to connect to any resolved address");
	}

	auto accept() -> std::unique_ptr<Impl>
	{
		assert(m_socketFd != InvalidSocket);

		auto impl = std::make_unique<Impl>(InvalidSocket, m_port);
		auto& other = impl->m_socketFd;
		do
		{
#ifdef __linux__
			other = ::accept4(m_socketFd, nullptr, nullptr, SOCK_CLOEXEC);
#else
			other = ::accept(m_socketFd, nullptr, nullptr);
#endif
		}
#ifdef LSP_SOCKET_POSIX
		while(other == InvalidSocket && errno == EINTR);
#else
		while(false);
#endif

		if(other == InvalidSocket)
			throwError("Failed to accept socket connection");
		configureSocket(other);

		return impl;
	}

	void read(char* buffer, std::size_t size) override
	{
		if(size == 0)
			return;

		std::size_t totalBytesRead = 0;

		while(totalBytesRead < size)
		{
#ifdef LSP_SOCKET_WIN32
			const auto remaining = static_cast<int>(std::min(size - totalBytesRead, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#else
			const auto remaining = std::min(size - totalBytesRead, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
#endif
			const auto bytesRead = recv(m_socketFd, buffer + totalBytesRead, remaining, 0);
#ifdef LSP_SOCKET_POSIX
			if(bytesRead < 0 && errno == EINTR)
				continue;
#endif
			if(bytesRead <= 0)
				throwError("Failed to read from socket");

			totalBytesRead += static_cast<std::size_t>(bytesRead);
		}
	}

	void write(const char* buffer, std::size_t size) override
	{
		if(size == 0)
			return;

		std::size_t totalBytesWritten = 0;

		while(totalBytesWritten < size)
		{
#ifdef LSP_SOCKET_WIN32
			const auto remaining = static_cast<int>(std::min(size - totalBytesWritten, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#else
			const auto remaining = std::min(size - totalBytesWritten, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
#endif
#ifdef MSG_NOSIGNAL
			constexpr auto flags = MSG_NOSIGNAL;
#else
			constexpr auto flags = 0;
#endif
			const auto bytesWritten = send(m_socketFd, buffer + totalBytesWritten, remaining, flags);
#ifdef LSP_SOCKET_POSIX
			if(bytesWritten < 0 && errno == EINTR)
				continue;
#endif
			if(bytesWritten <= 0)
				throwError("Failed to write to socket");

			totalBytesWritten += static_cast<std::size_t>(bytesWritten);
		}
	}
};

/*
 * Socket
 */

Socket::~Socket() = default;
Socket::Socket(Socket&&) noexcept = default;
Socket& Socket::operator=(Socket&&) noexcept = default;

Socket::Socket(std::unique_ptr<Impl> impl)
	: m_impl(std::move(impl))
{
}

Socket Socket::connect(const std::string& address, unsigned short port)
{
	return Socket(Impl::connect(address, port));
}

auto Socket::isOpen() const -> bool
{
	return !!m_impl;
}

auto Socket::port() const -> unsigned short
{
	if(m_impl)
		return m_impl->m_port;

	return 0;
}

void Socket::close()
{
	if(m_impl)
		m_impl.reset();
}

void Socket::read(char* buffer, std::size_t size)
{
	if(!m_impl)
		throw Error("Socket is not open for reading");

	m_impl->read(buffer, size);
}

void Socket::write(const char* buffer, std::size_t size)
{
	if(!m_impl)
		throw Error("Socket is not open for writing");

	m_impl->write(buffer, size);
}

auto Socket::stream() -> Stream&
{
	assert(m_impl);
	return *m_impl;
}

/*
 * SocketListener
 */

SocketListener::SocketListener(unsigned short port, unsigned short backlog)
	: m_socket(Socket::Impl::setupForListen(port, backlog))
{
}

auto SocketListener::accept() -> Socket
{
	if(!isOpen())
		throw Error("Server socket is not open for listening");

	return Socket(m_socket.m_impl->accept());
}

} // namespace lsp::io

#endif // LSP_SOCKET_UNSUPPORTED
