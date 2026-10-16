#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <lsp/connection.h>
#include <lsp/error.h>
#include <lsp/io/stream.h>
#include <lsp/json/json.h>

namespace lsp{
namespace{

auto trimWhitespace(std::string_view str) -> std::string_view
{
	while(!str.empty() && str.front() <= 0x20)
		str.remove_prefix(1);

	while(!str.empty() && str.back() <= 0x20)
		str.remove_suffix(1);

	return str;
}

auto equalCaseInsensitive(std::string_view lhs, std::string_view rhs) -> bool
{
	return std::ranges::equal(lhs, rhs, [](char a, char b)
		{
			const auto toLower = [](char c){ return c >= 'A' && c <= 'Z' ? c + 32 : c; };
			return toLower(a) == toLower(b);
		});
}

void verifyContentType(std::string_view contentType)
{
	const auto separator = contentType.find(';');
	const auto mediaType = trimWhitespace(contentType.substr(0, separator));

	if(!equalCaseInsensitive(mediaType, "application/vscode-jsonrpc"))
		throw ConnectionError("Protocol: Unsupported or invalid content type: " + std::string(contentType));

	auto parameters = separator == std::string_view::npos ? std::string_view() : contentType.substr(separator + 1);

	while(!parameters.empty())
	{
		const auto end = parameters.find(';');
		const auto parameter = trimWhitespace(parameters.substr(0, end));
		const auto equals = parameter.find('=');

		if(equals != std::string_view::npos && equalCaseInsensitive(trimWhitespace(parameter.substr(0, equals)), "charset"))
		{
			auto charset = trimWhitespace(parameter.substr(equals + 1));

			if(charset.size() >= 2 && charset.front() == '"' && charset.back() == '"')
				charset = charset.substr(1, charset.size() - 2);

			if(!equalCaseInsensitive(charset, "utf-8") && !equalCaseInsensitive(charset, "utf8"))
				throw ConnectionError("Protocol: Unsupported or invalid character encoding: " + std::string(charset));
		}

		if(end == std::string_view::npos)
			break;

		parameters.remove_prefix(end + 1);
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
		: m_stream(stream)
	{
	}

	auto peek() -> char
	{
		if(!m_peek.has_value())
			m_peek = get();

		return m_peek.value();
	}

	auto get() -> char
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

struct Connection::Internal{
	io::Stream& stream;
	std::mutex  readMutex;
	std::mutex  writeMutex;

	Internal(io::Stream& _stream) : stream(_stream){}
};

struct Connection::MessageHeader{
	std::size_t contentLength = 0;
	bool hasContentLength = false;
	std::optional<std::string> contentType = std::nullopt;
};

Connection::Connection(io::Stream& stream)
	: m(std::make_unique<Internal>(stream))
{
}

Connection::~Connection()                                = default;
Connection::Connection(Connection&&) noexcept            = default;
Connection& Connection::operator=(Connection&&) noexcept = default;

auto Connection::readMessage() -> Connection::Message
{
	return readMessage(nullptr);
}

auto Connection::readMessage(std::vector<jsonrpc::Error>* invalidBatch) -> Connection::Message
{
	for(;;) // Keep trying until a message was read or an unrecoverable error happened
	{
		try
		{
			auto readLock = std::unique_lock(m->readMutex);
			auto reader   = InputReader(m->stream);

			if(reader.peek() == io::Stream::Eof)
				throw ConnectionError("Connection lost");

			const auto header = readMessageHeader(reader);

			std::string content;
			content.resize(header.contentLength);
			reader.read(&content[0], header.contentLength);

			readLock.unlock();

			// Verify only after reading the entire message so no partially unread message is left in the stream
			if(header.contentType)
				verifyContentType(*header.contentType);

			auto json = json::parse(content);

			if(json.isObject())
				return jsonrpc::messageFromJson(std::move(json.object()));

			if(!json.isArray())
				throw jsonrpc::ProtocolError("Message must be a json object or array");

			if(!invalidBatch || json.array().empty())
				return jsonrpc::messageBatchFromJson(std::move(json.array()));

			auto batch = jsonrpc::MessageBatch();
			batch.reserve(json.array().size());

			for(auto& entry : json.array())
			{
				try
				{
					if(!entry.isObject())
						throw jsonrpc::ProtocolError("Message batch entry must be an object");

					batch.push_back(jsonrpc::messageFromJson(std::move(entry.object())));
				}
				catch(const jsonrpc::ProtocolError& e)
				{
					invalidBatch->push_back({jsonrpc::Error::InvalidRequest, e.what()});
				}
			}

			return batch;
		}
		catch(const json::ParseError& e)
		{
			auto responseSender = errorResponse(json::Null(), MessageError::ParseError, e.what());
			responseSender.submit();
			continue;
		}
		catch(const jsonrpc::ProtocolError& e)
		{
			auto responseSender = errorResponse(json::Null(), MessageError::InvalidRequest, e.what());
			responseSender.submit();
			continue;
		}
		catch(const ConnectionError&)
		{
			throw;
		}
		catch(const std::exception& e)
		{
			throw ConnectionError(e.what());
		}
	}
}

auto Connection::readMessageHeader(InputReader& reader) -> Connection::MessageHeader
{
	MessageHeader header;

	while(reader.peek() != '\r')
		readNextMessageHeaderField(header, reader);

	if(!header.hasContentLength)
		throw ConnectionError("Protocol: Missing Content-Length header field");

	if(reader.get() != '\r' || reader.get() != '\n')
		throw ConnectionError("Protocol: Expected header to be terminated by '\\r\\n'");

	return header;
}

void Connection::parseHeaderValue(MessageHeader& header, std::string_view line)
{
	const auto separatorIdx = line.find(':');

	if(separatorIdx == std::string_view::npos)
		throw ConnectionError("Protocol: Header field must contain ':'");

	const auto key   = trimWhitespace(line.substr(0, separatorIdx));
	const auto value = trimWhitespace(line.substr(separatorIdx + 1));

	if(equalCaseInsensitive(key, "Content-Length"))
	{
		const auto* first    = value.data();
		const auto* last     = first + value.size();
		const auto [ptr, ec] = std::from_chars(first, last, header.contentLength);

		if(ec != std::errc{} || ptr != last)
			throw ConnectionError("Protocol: Invalid value for Content-Length header field");

		header.hasContentLength = true;
	}
	else if(equalCaseInsensitive(key, "Content-Type"))
	{
		header.contentType = std::string{value.data(), value.size()};
	}
}

void Connection::readNextMessageHeaderField(MessageHeader& header, InputReader& reader)
{
	if(reader.peek() == io::Stream::Eof)
		throw ConnectionError{"Connection lost"};

	std::string lineData;

	while(reader.peek() != '\r')
	{
		const auto c = reader.get();

		if(c == io::Stream::Eof)
			throw ConnectionError("Connection lost");

		if(c == '\n')
			throw ConnectionError("Protocol: Unexpected '\\n' in header field, expected '\\r\\n'");

		lineData.push_back(c);
	}

	parseHeaderValue(header, lineData);

	if(reader.get() != '\r' || reader.get() != '\n')
		throw ConnectionError("Protocol: Expected header field to be terminated by '\\r\\n'");
}

void Connection::writeMessageData(std::string_view content)
{
	const auto lock      = std::lock_guard(m->writeMutex);
	const auto header    = MessageHeader{ .contentLength = content.size() };
	const auto headerStr = messageHeaderString(header);

	try
	{
		m->stream.write(headerStr.data(), headerStr.size());
		m->stream.write(content.data(), content.size());
	}
	catch(const std::exception& e)
	{
		throw ConnectionError(e.what());
	}
}

auto Connection::messageHeaderString(const MessageHeader& header) -> std::string
{
	auto text = "Content-Length: " + std::to_string(header.contentLength) + "\r\nContent-Type: ";
	text += header.contentType ? std::string_view(*header.contentType) : "application/vscode-jsonrpc; charset=utf-8";
	text += "\r\n\r\n";
	return text;
}

auto Connection::request(std::string_view method, const jsonrpc::MessageId& id) -> Connection::RequestSender
{
	return RequestSender(*this, method, id);
}

auto Connection::notification(std::string_view method) -> Connection::RequestSender
{
	return RequestSender(*this, method);
}

auto Connection::response(const jsonrpc::MessageId& id) -> Connection::ResponseSender
{
	return ResponseSender(*this, id);
}

auto Connection::errorResponse(const jsonrpc::MessageId& id, int code, std::string_view message) -> Connection::ResponseSender
{
	return ResponseSender(*this, id, code, message);
}

auto Connection::messageBatch() -> Connection::BatchSender
{
	return BatchSender(*this);
}

/*
 * Connection::MessageSender
 */

Connection::MessageSender::MessageSender(Connection& connection)
	: m_connection(&connection)
{
}

Connection::MessageSender::~MessageSender()
{
	// Message was not submitted if connection is not nullptr
	assert(!m_connection || std::uncaught_exceptions() > 0);
}

auto Connection::MessageSender::writer() -> json::Writer&
{
	return m_writer;
}

void Connection::MessageSender::submit()
{
	if(m_connection)
	{
		m_connection->writeMessageData(m_writer.text());
		m_connection = nullptr;
	}
}

void Connection::MessageSender::discard()
{
	m_connection = nullptr;
}

/*
 * Connection::RequestSender
 */

Connection::RequestSender::RequestSender(
	Connection& connection, std::string_view method, const jsonrpc::MessageId& id)
	: MessageSender(connection)
	, jsonrpc::RequestWriter(jsonrpc::RequestWriter::writeRequest(writer().beginObject(), id, method))
{
}

Connection::RequestSender::RequestSender(Connection& connection, std::string_view method)
	: MessageSender(connection)
	, jsonrpc::RequestWriter(jsonrpc::RequestWriter::writeNotification(writer().beginObject(), method))
{
}

void Connection::RequestSender::submit()
{
	finalize();
	MessageSender::submit();
}

/*
 * Connection::ResponseSender
 */

Connection::ResponseSender::ResponseSender(Connection& connection, const jsonrpc::MessageId& id)
	: MessageSender(connection)
	, jsonrpc::ResponseWriter(jsonrpc::ResponseWriter::writeResponse(writer().beginObject(), id))
{
}

Connection::ResponseSender::ResponseSender(
	Connection& connection, const jsonrpc::MessageId& id, int code, std::string_view message)
	: MessageSender(connection)
	, jsonrpc::ResponseWriter(jsonrpc::ResponseWriter::writeError(writer().beginObject(), id, code, message))
{
}

void Connection::ResponseSender::submit()
{
	finalize();
	MessageSender::submit();
}

/*
 * Connection::BatchSender
 */

Connection::BatchSender::BatchSender(Connection& connection)
	: MessageSender(connection)
	, jsonrpc::BatchWriter(writer().beginArray())
{
}

void Connection::BatchSender::submit()
{
	finalize();
	MessageSender::submit();
}

} // namespace lsp
