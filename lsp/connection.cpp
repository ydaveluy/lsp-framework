#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <optional>
#include <string_view>
#include <system_error>
#include <lsp/connection.h>
#include <lsp/error.h>
#include <lsp/io/stream.h>
#include <lsp/json/json.h>

#ifndef LSP_MESSAGE_DEBUG_LOG
	#ifdef NDEBUG
		#define LSP_MESSAGE_DEBUG_LOG 0
	#else
		#define LSP_MESSAGE_DEBUG_LOG 1
	#endif
#endif

#if LSP_MESSAGE_DEBUG_LOG
	#ifdef __APPLE__
		#include <os/log.h>
	#elif defined(_WIN32)
		#define WIN32_LEAN_AND_MEAN
		#include <Windows.h>
	#else
		#include <cstdio> // stderr expands to a macro, so <cstdio> has to be included here
	#endif
#endif

namespace lsp{
namespace{

/*
 * Message logging
 */

#if LSP_MESSAGE_DEBUG_LOG
void debugLogMessageJson([[maybe_unused]] const std::string& messageType, [[maybe_unused]] const lsp::json::Value& json)
{
#ifdef __APPLE__
	os_log_debug(OS_LOG_DEFAULT, "%{public}s", (messageType + ": " + lsp::json::stringify(json, true)).c_str());
#elif defined(_WIN32)
	OutputDebugStringA((messageType + ": " + lsp::json::stringify(json, true) + '\n').c_str());
#elif defined(__linux__) || defined(__HAIKU__)
	std::fprintf(stderr, "%s\n",  (messageType + ": " + lsp::json::stringify(json, true)).c_str());
#endif
}
#endif

std::string_view trimWhitespace(std::string_view str)
{
	while(!str.empty() && std::isspace(static_cast<unsigned char>(str.front())))
		str.remove_prefix(1);

	while(!str.empty() && std::isspace(static_cast<unsigned char>(str.back())))
		str.remove_suffix(1);

	return str;
}

bool equalCaseInsensitive(std::string_view lhs, std::string_view rhs)
{
	return std::ranges::equal(lhs, rhs, [](char a, char b)
		{
			return std::tolower(static_cast<unsigned char>(a)) ==
			       std::tolower(static_cast<unsigned char>(b));
		});
}

constexpr std::string_view DefaultContentType{"application/vscode-jsonrpc; charset=utf-8"};

void verifyContentType(std::string_view contentType)
{
	if(!contentType.starts_with("application/vscode-jsonrpc"))
		throw ConnectionError{"Protocol: Unsupported or invalid content type: " + std::string(contentType)};

	constexpr std::string_view charsetKey{"charset="};
	if(const auto idx = contentType.find(charsetKey); idx != std::string_view::npos)
	{
		auto charset = contentType.substr(idx + charsetKey.size());
		charset = trimWhitespace(charset.substr(0, charset.find(';')));

		if(charset != "utf-8" && charset != "utf8")
			throw ConnectionError{"Protocol: Unsupported or invalid character encoding: " + std::string{charset}};
	}
}

} // namespace

/*
 * Connection::InputReader
 * Wrapper around io::Stream that allows for peeking and reading single chars
 */

class Connection::InputReader{
public:
	InputReader(io::Stream& stream)
		: m_stream{stream}
	{
	}

	char peek()
	{
		if(!m_peek.has_value())
			m_peek = get();

		return m_peek.value();
	}

	char get()
	{
		if(m_peek.has_value())
		{
			const char c = m_peek.value();
			m_peek.reset();
			return c;
		}

		char c = io::Stream::Eof;
		read(&c, 1);
		return c;
	}

	void read(char* buffer, std::size_t size)
	{
		if(size > 0)
		{
			if(m_peek.has_value())
			{
				*buffer = m_peek.value();
				m_peek.reset();
				++buffer;
				--size;
			}

			m_stream.read(buffer, size);
		}
	}

private:
	io::Stream&         m_stream;
	std::optional<char> m_peek;
};

/*
 * Connection
 */

struct Connection::MessageHeader{
	std::size_t contentLength = 0;
	std::string contentType; // empty means DefaultContentType
};

Connection::Connection(io::Stream& stream)
	: m_stream{stream}
{
}

Connection::Message Connection::readMessage()
{
	try
	{
		auto readLock = std::unique_lock(m_readMutex);
		auto reader   = InputReader(m_stream);

		if(reader.peek() == io::Stream::Eof)
			throw ConnectionError{"Connection lost"};

		const auto header = readMessageHeader(reader);

		std::string content;
		content.resize(header.contentLength);
		reader.read(&content[0], header.contentLength);

		readLock.unlock();

		// Verify only after reading the entire message so no partially unread message is left in the stream
		verifyContentType(header.contentType.empty() ? DefaultContentType : std::string_view{header.contentType});

		auto json = json::parse(content);
#if LSP_MESSAGE_DEBUG_LOG
		debugLogMessageJson("incoming", json);
#endif

		if(json.isObject())
			return jsonrpc::messageFromJson(std::move(json.object()));

		if(!json.isArray())
			throw jsonrpc::ProtocolError("Message must be a json object or array");

		return jsonrpc::messageBatchFromJson(std::move(json.array()));
	}
	catch(const json::ParseError& e)
	{
		writeMessage(jsonrpc::createErrorResponse(json::Null(), MessageError::ParseError, e.what()));
		throw; // FIXME: This shouldn't abort the connection
	}
	catch(const jsonrpc::ProtocolError& e)
	{
		writeMessage(jsonrpc::createErrorResponse(json::Null(), MessageError::InvalidRequest, e.what()));
		throw; // FIXME: This shouldn't abort the connection
	}
	catch(const ConnectionError&)
	{
		throw;
	}
	catch(const std::exception& e)
	{
		throw ConnectionError{e.what()};
	}
	catch(...)
	{
		throw ConnectionError{"Unknown error"};
	}
}

void Connection::writeMessage(Message&& message)
{
	try
	{
		auto json = json::Value();

		if(auto* const msg = std::get_if<jsonrpc::Message>(&message))
			json = jsonrpc::messageToJson(std::move(*msg));
		else
			json = jsonrpc::messageBatchToJson(std::move(std::get<jsonrpc::MessageBatch>(message)));

#if LSP_MESSAGE_DEBUG_LOG
		debugLogMessageJson("outgoing", json);
#endif
		writeMessageData(json::stringify(json));
	}
	catch(const std::exception& e)
	{
		throw ConnectionError{e.what()};
	}
	catch(...)
	{
		throw ConnectionError{"Unknown error"};
	}
}

Connection::MessageHeader Connection::readMessageHeader(InputReader& reader)
{
	MessageHeader header;

	while(reader.peek() != '\r')
		readNextMessageHeaderField(header, reader);

	if(reader.get() != '\r' || reader.get() != '\n')
		throw ConnectionError("Protocol: Expected header to be terminated by '\\r\\n'");

	return header;
}

void Connection::parseHeaderValue(MessageHeader& header, std::string_view line)
{
	const auto separatorIdx = line.find(':');

	if(separatorIdx != std::string_view::npos)
	{
		const auto key   = trimWhitespace(line.substr(0, separatorIdx));
		const auto value = trimWhitespace(line.substr(separatorIdx + 1));

		if(equalCaseInsensitive(key, "Content-Length"))
		{
			const auto* first    = value.data();
			const auto* last     = first + value.size();
			const auto [ptr, ec] = std::from_chars(first, last, header.contentLength);

			if(ec != std::errc{} || ptr != last)
				throw ConnectionError("Protocol: Invalid value for Content-Length header field");
		}
		else if(equalCaseInsensitive(key, "Content-Type"))
		{
			header.contentType = std::string{value.data(), value.size()};
		}
	}
}

void Connection::readNextMessageHeaderField(MessageHeader& header, InputReader& reader)
{
	if(reader.peek() == std::char_traits<char>::eof())
		throw ConnectionError{"Connection lost"};

	std::string lineData;
	lineData.reserve(64);

	while(reader.peek() != '\r')
	{
		const auto c = reader.get();

		if(c == '\n')
			throw ConnectionError("Protocol: Unexpected '\\n' in header field, expected '\\r\\n'");

		lineData.push_back(c);
	}

	parseHeaderValue(header, lineData);

	if(reader.get() != '\r' || reader.get() != '\n')
		throw ConnectionError("Protocol: Expected header field to be terminated by '\\r\\n'");
}

void Connection::writeMessageData(const std::string& content)
{
	// The header goes into a stack buffer and the payload is written as it is:
	// concatenating them copied the whole message on every write.
	char        buffer[160];
	std::size_t size = 0;

	// The appends below are unchecked, so the buffer must hold the longest
	// header the fixed parts plus a 20-digit content length can produce.
	static_assert(sizeof(buffer) >=
	              std::string_view{"Content-Length: "}.size() + 20 +
	              std::string_view{"\r\nContent-Type: "}.size() +
	              DefaultContentType.size() +
	              std::string_view{"\r\n\r\n"}.size(),
	              "Message header buffer is too small for DefaultContentType");

	const auto append = [&](std::string_view part)
	{
		std::memcpy(buffer + size, part.data(), part.size());
		size += part.size();
	};

	append("Content-Length: ");
	size = static_cast<std::size_t>(std::to_chars(buffer + size, buffer + sizeof(buffer), content.size()).ptr - buffer);
	append("\r\nContent-Type: ");
	append(DefaultContentType);
	append("\r\n\r\n");

	std::lock_guard lock{m_writeMutex};
	m_stream.write(buffer, size);
	m_stream.write(content.data(), content.size());
}

std::string Connection::messageHeaderString(const MessageHeader& header)
{
	return "Content-Length: " + std::to_string(header.contentLength) + "\r\n" +
	       "Content-Type: " + (header.contentType.empty() ? std::string{DefaultContentType} : header.contentType) + "\r\n\r\n";
}

} // namespace lsp
