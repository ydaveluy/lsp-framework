#include <cassert>
#include <charconv>
#include <iterator>
#include "parser.h"

namespace lsp::json{
namespace{

auto isWhitespace(char c) -> bool
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

auto isDigit(char c) -> bool
{
	return c >= '0' && c <= '9';
}

auto isAlpha(char c) -> bool
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

auto isAlphanumeric(char c) -> bool
{
	return isAlpha(c) || isDigit(c);
}

void appendCodePointAsUtf8(std::string& str, unsigned int codepoint)
{
	if(codepoint < 0x80)
	{
		str += static_cast<char>(codepoint);
	}
	else if(codepoint < 0x800)
	{
		str += static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
		str += static_cast<char>(0x80 | (codepoint & 0x3F));
	}
	else if(codepoint < 0x10000)
	{
		str += static_cast<char>(0xE0 | ((codepoint >> 12) & 0xF));
		str += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		str += static_cast<char>(0x80 | (codepoint & 0x3F));
	}
	else if(codepoint < 0x200000)
	{
		str += static_cast<char>(0xF0 | ((codepoint >> 18) & 0x7));
		str += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
		str += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		str += static_cast<char>(0x80 | (codepoint & 0x3F));
	}
	else
	{
		str += "?";
	}
}

} // namespace

/*
 * Parser
 */

enum class Parser::State
{
	Value,
	Object,
	ObjectKey,
	Array
};

struct Parser::StateStackEntry{
	State  context;
	Value* value;
};

Parser::Parser(std::string_view text)
	: m_start(text.empty() ? "" : text.data())
	, m_end(m_start + text.size())
	, m_pos(m_start)
{
	m_stateStack.reserve(10);
}

Parser::~Parser() = default;

auto Parser::atEnd() const -> bool
{
	return m_pos >= m_end;
}

auto Parser::textOffset(const char* pos) const -> int
{
	return static_cast<int>(std::distance(m_start, pos));
}

auto Parser::currentTextOffset() const -> int
{
	return textOffset(m_pos);
}

auto Parser::parse() -> Value
{
	Value result;

	pushState(State::Value, result);

	while(!m_stateStack.empty())
	{
		skipWhitespace();

		if(atEnd())
			throw ParseError("Unexpected end of input", currentTextOffset());

		switch(currentState())
		{
		case State::Value:
			handleValue();
			break;
		case State::Object:
			handleObject();
			break;
		case State::ObjectKey:
			handleObjectKey();
			break;
		case State::Array:
			handleArray();
			break;
		}
	}

	skipWhitespace();

	if(!atEnd())
		throw ParseError("Trailing characters", currentTextOffset());

	return result;
}

void Parser::reset()
{
	m_stateStack.clear();
	m_pos = m_start;
}

void Parser::handleValue()
{
	assert(currentState() == State::Value);

	const char c = *m_pos;
	if(c == '{' || c == '[')
	{
		++m_pos;

		// Value destruction is recursive even though parsing is iterative.
		constexpr std::size_t maxNestingDepth = 512;
		if(m_stateStack.size() / 2 >= maxNestingDepth)
			throw ParseError("Maximum JSON nesting depth exceeded", currentTextOffset());

		if(c == '{')
		{
			currentValue() = Object{};
			pushState(State::Object, currentValue());
		}
		else
		{
			currentValue() = Array{};
			pushState(State::Array, currentValue());
		}
	}
	else
	{
		currentValue() = parseSimpleValue();
		popState();
	}
}

void Parser::handleObject()
{
	assert(currentState() == State::Object);

	if(*m_pos == '}')
	{
		++m_pos;
		popState(); // Object
		popState(); // Value
	}
	else
	{
		if(!currentValue().object().isEmpty())
		{
			if(*m_pos != ',')
				throw ParseError("Expected ','", currentTextOffset());

			const char* pos = m_pos;
			++m_pos;
			skipWhitespace();

			if(!atEnd() && *m_pos == '}')
				throw ParseError("Trailing ','", textOffset(pos));
		}

		pushState(State::ObjectKey, currentValue());
	}
}

void Parser::handleObjectKey()
{
	assert(currentState() == State::ObjectKey);

	const char* keyPos = m_pos;
	auto&       object = currentValue().object();
	const auto  key    = parseString();

	if(object.contains(key))
		throw ParseError("Duplicate key '" + key + "'", textOffset(keyPos));

	skipWhitespace();

	if(atEnd() || *m_pos != ':')
		throw ParseError("Expected ':'", currentTextOffset());

	++m_pos;

	popState();
	pushState(State::Value, object.append(key, {}));
}

void Parser::handleArray()
{
	assert(currentState() == State::Array);

	if(*m_pos == ']')
	{
		++m_pos;
		popState(); // Array
		popState(); // Value
	}
	else
	{
		auto& array = currentValue().array();

		if(!array.empty())
		{
			if(*m_pos != ',')
				throw ParseError("Expected ','", currentTextOffset());

			const char* pos = m_pos;
			++m_pos;
			skipWhitespace();

			if(!atEnd() && *m_pos == ']')
				throw ParseError("Trailing ','", textOffset(pos));
		}

		pushState(State::Value, array.emplace_back());
	}
}

auto Parser::currentState() const -> Parser::State
{
	assert(!m_stateStack.empty());
	return m_stateStack.back().context;
}

auto Parser::currentValue() -> Value&
{
	assert(!m_stateStack.empty());
	return *m_stateStack.back().value;
}

void Parser::pushState(State state, Value& value)
{
	m_stateStack.push_back({state, &value});
}

void Parser::popState()
{
	assert(!m_stateStack.empty());
	m_stateStack.pop_back();
}

void Parser::skipWhitespace()
{
	while(!atEnd() && isWhitespace(*m_pos))
		++m_pos;
}

auto Parser::parseString() -> String
{
	if(atEnd() || *m_pos != '\"')
		throw ParseError("Expected string", currentTextOffset());

	const char* const stringStart = m_pos++;
	auto              hasEscape   = false;

	for(;;)
	{
		if(atEnd() || static_cast<unsigned char>(*m_pos) < 0x20)
		{
			if(atEnd() || *m_pos == '\n')
				throw ParseError("Unmatched '\"'", textOffset(stringStart));

			throw ParseError("Unescaped control character", currentTextOffset());
		}

		if(!hasEscape && *m_pos == '"')
		{
			++m_pos;
			break;
		}

		hasEscape = !hasEscape && *m_pos == '\\';
		++m_pos;
	}

	auto str = std::string_view(stringStart, m_pos);

	if(str.size() > 0 && str.front() == '\"')
		str.remove_prefix(1);

	if(str.size() > 0 && str.back() == '\"')
		str.remove_suffix(1);

	std::string result;
	result.reserve(str.size());

	for(std::size_t i = 0; i < str.size(); ++i)
	{
		if(str[i] == '\\' && i != str.size() - 1)
		{
			++i;
			switch(str[i])
			{
			case 'b':
				result += '\b';
				break;
			case 't':
				result += '\t';
				break;
			case 'n':
				result += '\n';
				break;
			case 'f':
				result += '\f';
				break;
			case 'r':
				result += '\r';
				break;
			case '"':
				result += '"';
				break;
			case '/':
				result += '/';
				break;
			case '\\':
				result += '\\';
				break;
			case 'u':
				{
					if(str.size() - i >= 5)
					{
						const auto* first = str.data() + i + 1;
						const auto* last  = first + 4;
						unsigned int codepoint;
						const auto [ptr, ec] = std::from_chars(first, last, codepoint, 16);

						if(ec == std::errc{} && ptr == last)
						{
							if(codepoint >= 0xD800 && codepoint <= 0xDBFF &&
								str.size() - i >= 11 && str[i + 5] == '\\' && str[i + 6] == 'u')
							{
								unsigned int low;
								const auto [lowPtr, lowEc] = std::from_chars(first + 6, last + 6, low, 16);
								if(lowEc == std::errc{} && lowPtr == last + 6 && low >= 0xDC00 && low <= 0xDFFF)
								{
									codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
									i += 6;
								}
							}
							appendCodePointAsUtf8(result, codepoint);
							i += 4;
							break;
						}
					}
				}
				[[fallthrough]];
			default:
				{
					const char* invalidEscapeStart = stringStart + i;
					const auto remaining = static_cast<std::size_t>(str.data() + str.size() - invalidEscapeStart);
					const auto escapeSize = str[i] == 'u' ? 6u : 2u;
					const char* invalidEscapeEnd = invalidEscapeStart + (remaining < escapeSize ? remaining : escapeSize);

					throw ParseError(
						"Invalid escape sequence '" + std::string(invalidEscapeStart, invalidEscapeEnd) + '\'',
						textOffset(invalidEscapeStart));
				}
			}
		}
		else
		{
			result += str[i];
		}
	}

	return result;
}

auto Parser::parseNumber() -> Value
{
	const char* numberStart = m_pos;
	bool isDecimal = false;

	const auto isNumberCharacter = [](char c){
		return isAlphanumeric(c) || c == '+' || c == '-' || c == '.';
	};
	const auto invalidNumber = [&]() -> Value{
		while(!atEnd() && isNumberCharacter(*m_pos))
			++m_pos;
		throw ParseError("Invalid number value '" + std::string{numberStart, m_pos} + "'", textOffset(numberStart));
	};
	const auto consumeDigits = [&](){
		const auto* begin = m_pos;
		while(!atEnd() && isDigit(*m_pos))
			++m_pos;
		return begin != m_pos;
	};

	if(*m_pos == '-')
		++m_pos;
	if(atEnd() || !isDigit(*m_pos))
		return invalidNumber();

	if(*m_pos == '0')
		++m_pos;
	else
		consumeDigits();

	if(!atEnd() && *m_pos == '.')
	{
		isDecimal = true;
		++m_pos;
		if(!consumeDigits())
			return invalidNumber();
	}

	if(!atEnd() && (*m_pos == 'e' || *m_pos == 'E'))
	{
		isDecimal = true;
		++m_pos;
		if(!atEnd() && (*m_pos == '+' || *m_pos == '-'))
			++m_pos;
		if(!consumeDigits())
			return invalidNumber();
	}

	if(!atEnd() && isNumberCharacter(*m_pos))
		return invalidNumber();

	if(!isDecimal)
	{
		if(std::string_view(numberStart, m_pos) == "-0")
			return -0.0;

		Integer integer;
		const auto [ptr, ec] = std::from_chars(numberStart, m_pos, integer);
		if(ec == std::errc{} && ptr == m_pos)
			return integer;
	}

	Decimal decimal;
	const auto [ptr, ec] = std::from_chars(numberStart, m_pos, decimal);
	if(ec != std::errc{} || ptr != m_pos)
		return invalidNumber();

	return decimal;
}

auto Parser::parseIdentifier() -> Value
{
	const char* idStart = m_pos;

	while(!atEnd() && isAlphanumeric(*m_pos))
		++m_pos;

	auto identifier = std::string_view(idStart, m_pos);

	if(identifier == "true")
		return Boolean(true);

	if(identifier == "false")
		return Boolean(false);

	if(identifier == "null")
		return Null();

	throw ParseError("Unexpected '" + std::string(identifier) + "'", textOffset(idStart));
}

auto Parser::parseSimpleValue() -> Value
{
	if(*m_pos == '\"')
		return parseString();

	if(isDigit(*m_pos) || *m_pos == '-')
		return parseNumber();

	if(isAlpha(*m_pos))
		return parseIdentifier();

	throw ParseError(std::string("Unexpected '") + * m_pos + "'", currentTextOffset());
}

} // namespace lsp::json
