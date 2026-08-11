#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <iterator>
#include <limits>
#include <string>
#include <vector>
#include <lsp/json/json.h>

namespace lsp::json{
namespace{

// isspace/isdigit/isalnum reach for the locale on every call; the parse loops
// run per byte, so they read a table instead.
enum CharClass : std::uint8_t{
	ClassSpace = 1,
	ClassDigit = 2,
	ClassAlpha = 4
};

constexpr auto CharClasses = []
{
	std::array<std::uint8_t, 256> table{};

	for(std::size_t i = 0; i < table.size(); ++i)
	{
		std::uint8_t flags = 0;

		if(i == ' ' || i == '\t' || i == '\n' || i == '\r' || i == '\v' || i == '\f')
			flags |= ClassSpace;

		if(i >= '0' && i <= '9')
			flags |= ClassDigit;

		if((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z'))
			flags |= ClassAlpha;

		table[i] = flags;
	}

	return table;
}();

constexpr bool isClass(char c, std::uint8_t flags)
{
	return (CharClasses[static_cast<unsigned char>(c)] & flags) != 0;
}


constexpr std::string_view NullValueString{"null"};
constexpr std::string_view TrueValueString{"true"};
constexpr std::string_view FalseValueString{"false"};

/*
 * Parser
 */

class Parser{
public:
	Parser(std::string_view text) :
		m_start{text.data()},
		m_end{text.data() + text.size()},
		m_pos{m_start}
	{
		m_stateStack.reserve(10);
	}

	bool atEnd() const
	{
		return m_pos >= m_end;
	}

	std::size_t textOffset(const char* pos) const
	{
		return static_cast<std::size_t>(std::distance(m_start, pos));
	}

	std::size_t currentTextOffset() const
	{
		return textOffset(m_pos);
	}

	Value parse()
	{
		Value result;

		pushState(State::Value, result);

		while(!m_stateStack.empty())
		{
			skipWhitespace();

			if(atEnd())
				throw ParseError{"Unexpected end of input", currentTextOffset()};

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
			throw ParseError{"Trailing characters in json", currentTextOffset()};

		return result;
	}

private:
	enum class State{
		Value,
		Object,
		ObjectKey,
		Array
	};

	struct StateStackEntry{
		State  context;
		Value* value;
	};

	std::vector<StateStackEntry> m_stateStack;
	const char* const            m_start = nullptr;
	const char* const            m_end   = nullptr;
	const char*                  m_pos   = nullptr;

	void handleValue()
	{
		assert(currentState() == State::Value);

		if(*m_pos == '{')
		{
			++m_pos;
			currentValue() = Object{};
			pushState(State::Object, currentValue());
		}
		else if(*m_pos == '[')
		{
			++m_pos;
			currentValue() = Array{};
			pushState(State::Array, currentValue());
		}
		else
		{
			currentValue() = parseSimpleValue();
			popState();
		}
	}

	void handleObject()
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
			if(!currentValue().object().empty())
			{
				if(*m_pos != ',')
					throw ParseError{"Expected ','", currentTextOffset()};

				const char* pos = m_pos;
				++m_pos;
				skipWhitespace();

				if(!atEnd() && *m_pos == '}')
					throw ParseError{"Trailing ','", textOffset(pos)};
			}

			pushState(State::ObjectKey, currentValue());
		}
	}

	void handleObjectKey()
	{
		assert(currentState() == State::ObjectKey);

		const char* keyPos = m_pos;
		auto&       map    = currentValue().object().keyValueMap();
		auto        key    = parseString();

		if(map.find(key) != map.end())
			throw ParseError{"Duplicate key '" + key + "'", textOffset(keyPos)};

		skipWhitespace();

		if(!atEnd() && *m_pos != ':')
			throw ParseError{"Expected ':'", currentTextOffset()};

		++m_pos;

		popState();
		pushState(State::Value, map.append(std::move(key)));
	}

	void handleArray()
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
					throw ParseError{"Expected ','", currentTextOffset()};

				const char* pos = m_pos;
				++m_pos;
				skipWhitespace();

				if(!atEnd() && *m_pos == ']')
					throw ParseError{"Trailing ','", textOffset(pos)};
			}

			pushState(State::Value, array.emplace_back());
		}
	}

	State currentState() const
	{
		assert(!m_stateStack.empty());
		return m_stateStack.back().context;
	}

	Value& currentValue()
	{
		assert(!m_stateStack.empty());
		return *m_stateStack.back().value;
	}

	void pushState(State state, Value& value)
	{
		m_stateStack.push_back({state, &value});
	}

	void popState()
	{
		assert(!m_stateStack.empty());
		m_stateStack.pop_back();
	}

	void skipWhitespace()
	{
		while(!atEnd() && isClass(*m_pos, ClassSpace))
			++m_pos;
	}

	String parseString()
	{
		if(atEnd() || *m_pos != '\"')
			throw ParseError{"String expected", currentTextOffset()};

		++m_pos;
		const char* const contentStart = m_pos;
		auto              hasEscape    = false;

		for(;;)
		{
			if(atEnd() || *m_pos == '\n')
				throw ParseError("Unmatched '\"'", currentTextOffset());

			if(*m_pos == '"')
				break;

			if(*m_pos == '\\')
			{
				hasEscape = true;
				++m_pos;

				if(atEnd() || *m_pos == '\n')
					throw ParseError("Unmatched '\"'", currentTextOffset());
			}

			++m_pos;
		}

		const auto content = std::string_view(contentStart, m_pos);
		++m_pos; // closing quote

		// Most strings carry no escape: take the bytes as they are.
		if(!hasEscape)
			return String{content};

		return fromStringLiteral(content);
	}

	Value parseNumber()
	{
		const char* numberStart = m_pos;
		bool isDecimal = false;

		while(!atEnd() && (
		      isClass(*m_pos, ClassDigit | ClassAlpha) ||
		      *m_pos == '-' ||
		      *m_pos == '+' ||
		      *m_pos == '.')
		)
		{
			if(!isDecimal && (*m_pos == '.' || *m_pos == 'e' || *m_pos == 'E'))
				isDecimal = true;

			++m_pos;
		}

		if(isDecimal)
		{
			std::size_t   idx     = 0;
			const Decimal decimal = std::stod(std::string{numberStart, m_pos}, &idx);

			if(idx < static_cast<std::size_t>(std::distance(numberStart, m_pos)))
				throw ParseError{"Invalid number value: '" + std::string{numberStart, m_pos} + "'", textOffset(numberStart)};

			return decimal;
		}

		std::int64_t intValue;
		const auto [ptr, ec] = std::from_chars(numberStart, m_pos, intValue);

		if(ec != std::errc{} || ptr != m_pos)
			throw ParseError{"Invalid number value: '" + std::string{numberStart, m_pos} + "'", textOffset(numberStart)};

		if(intValue < std::numeric_limits<json::Integer>::min() || intValue > std::numeric_limits<json::Integer>::max())
			return static_cast<json::Decimal>(intValue);

		return static_cast<json::Integer>(intValue);
	}

	Value parseIdentifier()
	{
		const char* idStart = m_pos;

		while(!atEnd() && isClass(*m_pos, ClassDigit | ClassAlpha))
			++m_pos;

		auto identifier = std::string_view(idStart, m_pos);

		if(identifier == TrueValueString)
			return Boolean(true);

		if(identifier == FalseValueString)
			return Boolean(false);

		if(identifier == NullValueString)
			return Null();

		throw ParseError{"Unexpected '" + std::string(identifier) + "'", currentTextOffset()};
	}

	Value parseSimpleValue()
	{
		if(*m_pos == '\"')
			return parseString();

		if(isClass(*m_pos, ClassDigit) || *m_pos == '-')
			return parseNumber();

		if(isClass(*m_pos, ClassAlpha))
			return parseIdentifier();

		throw ParseError{"Unexpected token", currentTextOffset()};
	}
};

void stringifyImplementation(const Value& json, std::string& str, std::size_t indentLevel, bool format)
{
	const auto getIndent = [&indentLevel, format]()
	{
		if(!format)
			return std::string_view{};

		static constexpr std::string_view Tabs{"\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t"};
		return Tabs.substr(0, std::min(indentLevel, Tabs.size()));
	};

	std::string_view keySep{":"};
	std::string_view valueSep{","};
	std::string_view listStart;
	std::string_view listEnd;

	if(format)
	{
		keySep = ": ";
		valueSep = ",\n";
		listStart = "\n";
		listEnd = "\n";
	}

	if(json.isNull())
	{
		str += NullValueString;
	}
	else if(json.isBoolean())
	{
		str += json.boolean() ? TrueValueString : FalseValueString;
	}
	else if(json.isInteger())
	{
		str += std::to_string(json.integer());
	}
	else if(json.isDecimal())
	{
		// to_string is printf %f: it fixes six decimals, so 1e-9 serializes as 0.
		char       buffer[32];
		const auto end       = std::to_chars(buffer, buffer + sizeof(buffer), json.decimal(), std::chars_format::general).ptr;
		const auto numberStr = std::string_view(buffer, end);

		str += numberStr;

		// general format drops the fraction of an integral value; keep it a decimal.
		if(numberStr.find_first_not_of("-0123456789") == std::string_view::npos)
			str += ".0";
	}
	else if(json.isString())
	{
		appendStringLiteral(str, json.string());
	}
	else if(json.isObject())
	{
		const auto& objMap = json.object().keyValueMap();

		str += '{';

		if(auto it = objMap.begin(); it != objMap.end())
		{
			str += listStart;
			++indentLevel;
			str += getIndent();
			appendStringLiteral(str, it->first);
			str += keySep;
			stringifyImplementation(it->second, str, indentLevel, format);
			++it;

			while(it != objMap.end())
			{
				str += valueSep;
				str += getIndent();
				appendStringLiteral(str, it->first);
				str += keySep;
				stringifyImplementation(it->second, str, indentLevel, format);
				++it;
			}

			str += listEnd;
			--indentLevel;
			str += getIndent();
		}

		str += '}';
	}
	else if(json.isArray())
	{
		const auto& array = json.array();

		str += '[';

		if(auto it = array.begin(); it != array.end())
		{
			str += listStart;
			++indentLevel;
			str += getIndent();
			stringifyImplementation(*it, str, indentLevel, format);
			++it;

			while(it != array.end())
			{
				str += valueSep;
				str += getIndent();
				stringifyImplementation(*it, str, indentLevel, format);
				++it;
			}

			str += listEnd;
			--indentLevel;
			str += getIndent();
		}

		str += ']';
	}
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

Object::Object()
	: m_map{std::make_unique<MapType>()}
{
}

Object::Object(const Object& other)
	: m_map{std::make_unique<MapType>(*other.m_map)}
{
}

Object::Object(Object&&) noexcept = default;
Object& Object::operator=(Object&&) noexcept = default;
// Defined here, not in the class: the defaulted move members destroy the old
// pimpl, which needs MapType complete -- it is not, at the declaration.
Object::~Object() = default;

Object& Object::operator=(const Object& other)
{
	*this->m_map = *other.m_map;
	return *this;
}

bool Object::operator==(const Object& other) const
{
	return *this->m_map == *other.m_map;
}

std::size_t Object::size() const
{
	return m_map->size();
}

bool Object::empty() const
{
	return m_map->empty();
}

bool Object::contains(std::string_view key) const
{
	return m_map->contains(key);
}

void Object::reserve(std::size_t count)
{
	m_map->reserve(count);
}

Value& Object::operator[](std::string_view key)
{
	return m_map->emplace(key);
}

Value& Object::get(std::string_view key)
{
	if(const auto it = m_map->find(key); it != m_map->end())
		return it->second;

	throw TypeError("Missing key '" + std::string{key} + '\'');
}

const Value& Object::get(std::string_view key) const
{
	if(const auto it = m_map->find(key); it != m_map->end())
		return it->second;

	throw TypeError("Missing key '" + std::string{key} + '\'');
}

Value* Object::find(std::string_view key)
{
	if(const auto it = m_map->find(key); it != m_map->end())
		return &it->second;

	return nullptr;
}

const Value* Object::find(std::string_view key) const
{
	if(const auto it = m_map->find(key); it != m_map->end())
		return &it->second;

	return nullptr;
}

Object::MapType& Object::keyValueMap()
{
	return *m_map;
}

const Object::MapType& Object::keyValueMap() const
{
	return *m_map;
}

Value parse(std::string_view text)
{
	Parser parser{text};

	return parser.parse();
}

std::string stringify(const Value& json, bool format)
{
	std::string str;
	stringifyImplementation(json, str, 0, format);
	return str;
}

void stringify(const Value& json, std::string& str, bool format)
{
	stringifyImplementation(json, str, 0, format);
}

void appendStringLiteral(std::string& str, std::string_view value)
{
	str += '\"';

	// Copy the runs between escapes whole instead of a byte at a time.
	std::size_t runStart = 0;

	for(std::size_t i = 0; i < value.size(); ++i)
	{
		const char        c  = value[i];
		const char* const escape = [c]() -> const char*
		{
			switch(c)
			{
			case '\b': return "\\b";
			case '\t': return "\\t";
			case '\n': return "\\n";
			case '\f': return "\\f";
			case '\r': return "\\r";
			case '\"': return "\\\"";
			case '\\': return "\\\\";
			default:   return nullptr;
			}
		}();

		if(escape)
		{
			str += value.substr(runStart, i - runStart);
			str += escape;
			runStart = i + 1;
		}
		else if(static_cast<unsigned char>(c) < 0x20)
		{
			constexpr auto hexLookup = "0123456789ABCDEF";
			str += value.substr(runStart, i - runStart);
			str += "\\u00";
			str += hexLookup[(static_cast<unsigned char>(c) >> 4) & 0xF];
			str += hexLookup[c & 0xF];
			runStart = i + 1;
		}
	}

	str += value.substr(runStart);
	str += '\"';
}

std::string toStringLiteral(std::string_view str)
{
	std::string result;
	result.reserve(str.size() + 2);
	appendStringLiteral(result, str);
	return result;
}

std::string fromStringLiteral(std::string_view str)
{
	if(str.size() > 0 && str.front() == '\"')
		str.remove_prefix(1);

	if(str.size() > 0 && str.back() == '\"')
		str.remove_suffix(1);

	std::string result;
	result.reserve(str.size());

	// Copy the runs between escapes whole instead of a byte at a time.
	std::size_t runStart = 0;

	for(std::size_t i = 0; i < str.size(); ++i)
	{
		if(str[i] == '\\' && i != str.size() - 1)
		{
			result += str.substr(runStart, i - runStart);
			++i;
			switch(str[i])
			{
			case '0':
				result += '\0';
				break;
			case 'a':
				result += '\a';
				break;
			case 'b':
				result += '\b';
				break;
			case 't':
				result += '\t';
				break;
			case 'n':
				result += '\n';
				break;
			case 'v':
				result += '\v';
				break;
			case 'f':
				result += '\f';
				break;
			case 'r':
				result += '\r';
				break;
			case 'u':
				{
					const auto* first = str.data() + i + 1;
					const auto* last  = first + 4;

					if(last <= str.data() + str.size())
					{
						unsigned int codepoint;
						const auto [ptr, ec] = std::from_chars(first, last, codepoint, 16);

						if(ec == std::errc{} && ptr == last)
						{
							appendCodePointAsUtf8(result, codepoint);
							i += 4;
						}
						else
						{
							const auto len = static_cast<std::size_t>(std::distance(first, ptr));
							result += "\\u";
							result += std::string_view(first, len);
							i += len;
						}
					}
					else
					{
						const auto len = static_cast<std::size_t>(std::distance(first, str.data() + str.size()));
						result += "\\u";
						result += std::string_view(first, len);
						i += len;
					}
					break;
				}
			default:
				result += str[i];
			}

			runStart = i + 1;
		}
	}

	result += str.substr(runStart);

	return result;
}

} // namespace lsp::json
