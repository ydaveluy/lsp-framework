#include <charconv>
#include <cmath>
#include <cstddef>
#include <utility>
#include "writer.h"
#include "json.h"

namespace lsp::json{
namespace{

constexpr auto hexLookup = "0123456789ABCDEF";

struct Utf8Sequence{
	std::ptrdiff_t size;       // The whole sequence, or its maximal ill-formed subpart
	bool           wellFormed;
};

/*
 * Reads the UTF-8 sequence that starts at it with a byte of 0x80 or more.
 * Surrogate code points (ED A0..BF xx) count as well formed: the parser
 * stores lone \u escapes that way.
 */
auto readUtf8Sequence(std::string_view::iterator it, std::string_view::iterator end) -> Utf8Sequence
{
	// Past the end reads as 0, which is never a continuation byte.
	const auto byteAt = [it, left = end - it](std::ptrdiff_t i) -> unsigned int{
		return i < left ? static_cast<unsigned char>(it[i]) : 0u;
	};
	const auto isContinuation = [](unsigned int byte){ return (byte & 0xC0) == 0x80; };

	const auto lead   = byteAt(0);
	const auto second = byteAt(1);

	if(lead >= 0xC2 && lead <= 0xDF)
		return isContinuation(second) ? Utf8Sequence{2, true} : Utf8Sequence{1, false};

	if(lead >= 0xE0 && lead <= 0xEF)
	{
		if(second < (lead == 0xE0 ? 0xA0u : 0x80u) || second > 0xBF)
			return {1, false};

		return isContinuation(byteAt(2)) ? Utf8Sequence{3, true} : Utf8Sequence{2, false};
	}

	if(lead >= 0xF0 && lead <= 0xF4)
	{
		if(second < (lead == 0xF0 ? 0x90u : 0x80u) || second > (lead == 0xF4 ? 0x8Fu : 0xBFu))
			return {1, false};

		if(!isContinuation(byteAt(2)))
			return {2, false};

		return isContinuation(byteAt(3)) ? Utf8Sequence{4, true} : Utf8Sequence{3, false};
	}

	return {1, false};
}

void appendStringLiteral(std::string_view str, std::string& out)
{
	out.reserve(out.size() + str.size() + 2);
	out += '\"';

	const auto end = str.end();
	auto segment = str.begin();
	for(auto it = segment; it != end; ++it)
	{
		const char c = *it;
		const auto byte = static_cast<unsigned char>(c);
		auto sequence = Utf8Sequence{1, false};

		if(byte >= 0x80)
		{
			sequence = readUtf8Sequence(it, end);
			if(sequence.wellFormed && (byte != 0xED || static_cast<unsigned char>(it[1]) < 0xA0))
			{
				it += sequence.size - 1;
				continue;
			}
		}
		else if(byte >= 0x20 && c != '"' && c != '\\')
		{
			continue;
		}

		if(segment != it)
			out.append(segment, it);

		switch(c)
		{
		case '\b':
			out += "\\b";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		default:
			if(byte < 0x20)
			{
				out += "\\u00";
				out += hexLookup[byte >> 4];
				out += hexLookup[byte & 0xF];
			}
			else if(sequence.wellFormed)
			{
				// A surrogate code point has no UTF-8 form: write it as an escape.
				const auto codepoint = 0xD000u | ((static_cast<unsigned char>(it[1]) & 0x3Fu) << 6) |
					(static_cast<unsigned char>(it[2]) & 0x3Fu);
				out += "\\u";
				out += hexLookup[(codepoint >> 12) & 0xF];
				out += hexLookup[(codepoint >> 8) & 0xF];
				out += hexLookup[(codepoint >> 4) & 0xF];
				out += hexLookup[codepoint & 0xF];
			}
			else
			{
				// JSON text is UTF-8: each maximal ill-formed subpart becomes U+FFFD.
				out += "\xEF\xBF\xBD";
			}
		}
		it += sequence.size - 1;
		segment = it + 1;
	}

	if(segment != end)
		out.append(segment, end);

	out += '\"';
}

} // namespace

/*
 * Writer
 */

Writer::Writer(std::string_view indent)
	: m_indent(indent)
	, m_keySep(indent.empty() ? ":" : ": ")
	, m_valueSep(indent.empty() ? "," : ",\n")
	, m_newline(indent.empty() ? "" : "\n")
{
}

auto Writer::text() const& -> const std::string&
{
	return m_text;
}

auto Writer::text() && -> std::string
{
	return std::move(m_text);
}

void Writer::write(std::nullptr_t)
{
	m_text += "null";
}

void Writer::write(bool value)
{
	m_text += value ? "true" : "false";
}

void Writer::write(long long value)
{
	char buffer[32];
	const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
	m_text += std::string_view(buffer, ptr);
}

void Writer::write(unsigned long long value)
{
	char buffer[32];
	const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
	m_text += std::string_view(buffer, ptr);
}

void Writer::write(double value)
{
	if(!std::isfinite(value))
	{
		m_text += "null"; // There's no nan/inf in json
		return;
	}

	const auto absValue     = std::abs(value);
	const auto numberFormat = (absValue != 0.0 && (absValue < 1e-6 || absValue >= 1e21))
	 ? std::chars_format::scientific
	 : std::chars_format::fixed;

	char buffer[32];
	const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, numberFormat);
	const auto numberStr = std::string_view(buffer, ptr);
	m_text += numberStr;

	if(numberStr.find_first_of(".eE") == std::string::npos)
		m_text += ".0";
}

void Writer::write(const char* value)
{
	appendStringLiteral(value, m_text);
}

void Writer::write(const std::string& value)
{
	appendStringLiteral(value, m_text);
}

void Writer::write(std::string_view value)
{
	appendStringLiteral(value, m_text);
}

void Writer::write(const Value& value)
{
	std::visit([this](const auto& v){ write(v); }, value.variant());
}

void Writer::write(const Object& value)
{
	auto objectWriter = beginObject();

	for(const auto& [k, v] : value)
		objectWriter.write(k, v);
}

void Writer::write(const Array& value)
{
	auto arrayWriter = beginArray();

	for(const auto& v : value)
		arrayWriter.write(v);
}

auto Writer::beginObject() -> ObjectWriter
{
	return ObjectWriter(*this);
}

auto Writer::beginArray() -> ArrayWriter
{
	return ArrayWriter(*this);
}

void Writer::writeIndent()
{
	if(!m_indent.empty())
	{
		for(int i = 0; i < m_nestingLevel; ++i)
			m_text += m_indent;
	}
}

void Writer::writeObjectStart()
{
	m_text += '{';
	++m_nestingLevel;
}

void Writer::writeObjectEnd(bool hasItems)
{
	--m_nestingLevel;

	if(hasItems)
	{
		m_text += m_newline;
		writeIndent();
	}

	m_text += '}';
}

void Writer::writeArrayStart()
{
	m_text += '[';
	++m_nestingLevel;
}

void Writer::writeArrayEnd(bool hasItems)
{
	--m_nestingLevel;

	if(hasItems)
	{
		m_text += m_newline;
		writeIndent();
	}

	m_text += ']';
}

void Writer::writeObjectKey(std::string_view key)
{
	appendStringLiteral(key, m_text);
	m_text += m_keySep;
}

void Writer::writePreValue(bool first)
{
	if(first)
		m_text += m_newline;
	else
		m_text += m_valueSep;

	writeIndent();
}

/*
 * ObjectWriter
 */

ObjectWriter::ObjectWriter(Writer& writer)
	: m_writer(&writer)
{
	m_writer->writeObjectStart();
}

ObjectWriter::~ObjectWriter()
{
	finalize();
}

ObjectWriter::ObjectWriter(ObjectWriter&& other) noexcept
	: m_writer(std::exchange(other.m_writer, nullptr))
	, m_first(other.m_first)
{
}

ObjectWriter& ObjectWriter::operator=(ObjectWriter&& other) noexcept
{
	finalize();

	m_writer = std::exchange(other.m_writer, nullptr);
	m_first  = other.m_first;

	return *this;
}

void ObjectWriter::finalize()
{
	if(m_writer)
	{
		m_writer->writeObjectEnd(!m_first);
		m_writer = nullptr;
	}
}

auto ObjectWriter::beginObject(std::string_view key) -> ObjectWriter
{
	m_writer->writePreValue(m_first);
	m_first = false;
	m_writer->writeObjectKey(key);
	return m_writer->beginObject();
}

auto ObjectWriter::beginArray(std::string_view key) -> ArrayWriter
{
	m_writer->writePreValue(m_first);
	m_first = false;
	m_writer->writeObjectKey(key);
	return m_writer->beginArray();
}

/*
 * ArrayWriter
 */

ArrayWriter::ArrayWriter(Writer& writer)
	: m_writer(&writer)
{
	m_writer->writeArrayStart();
}

ArrayWriter::~ArrayWriter()
{
	finalize();
}

ArrayWriter::ArrayWriter(ArrayWriter&& other) noexcept
	: m_writer(std::exchange(other.m_writer, nullptr))
	, m_first(other.m_first)
{
}

ArrayWriter& ArrayWriter::operator=(ArrayWriter&& other) noexcept
{
	finalize();

	m_writer = std::exchange(other.m_writer, nullptr);
	m_first  = other.m_first;

	return *this;
}

void ArrayWriter::finalize()
{
	if(m_writer)
	{
		m_writer->writeArrayEnd(!m_first);
		m_writer = nullptr;
	}
}

ObjectWriter ArrayWriter::beginObject()
{
	m_writer->writePreValue(m_first);
	m_first = false;
	return m_writer->beginObject();
}

ArrayWriter ArrayWriter::beginArray()
{
	m_writer->writePreValue(m_first);
	m_first = false;
	return m_writer->beginArray();
}

} // namespace lsp::json
