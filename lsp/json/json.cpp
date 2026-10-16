#include <algorithm>
#include "json.h"
#include "parser.h"
#include "writer.h"

namespace lsp::json{

/*
 * parse/stringify
 */

auto parse(std::string_view text) -> Value
{
	auto parser = Parser(text);
	return parser.parse();
}

auto stringify(const Value& json, std::string_view indent) -> std::string
{
	auto writer = Writer(indent);
	writer.write(json);
	return std::move(writer).text();
}

/*
 * Value
 */

auto Value::number() const -> Decimal
{
	if(isDecimal())
		return get<Decimal>(nullptr);

	if(isInteger())
		return static_cast<Decimal>(get<Integer>(nullptr));

	throwTypeError("number");
}

void Value::throwTypeError(const char* expectedType)
{
	throw TypeError(std::string("JSON value is not ") + expectedType);
}

/*
 * Object
 */

Object::Object(std::initializer_list<KeyValuePair> pairs)
	: m_keyValuePairs(pairs)
{
	// TODO: Check for duplicates?
}

Object::SizeType Object::size() const
{
	return m_keyValuePairs.size();
}

Object::SizeType Object::capacity() const
{
	return m_keyValuePairs.capacity();
}

auto Object::insert(String key, Value value) -> Value&
{
	Value* existingValue = find(key);

	if(existingValue)
	{
		*existingValue = std::move(value);
		return *existingValue;
	}

	return append(std::move(key), std::move(value));
}

auto Object::append(String key, Value value) -> Value&
{
	return m_keyValuePairs.emplace_back(std::move(key), std::move(value)).value();
}

void Object::remove(std::string_view key)
{
	for(auto it = m_keyValuePairs.begin(); it != m_keyValuePairs.end(); ++it)
	{
		if(it->key() == key)
		{
			m_keyValuePairs.erase(it);
			break;
		}
	}
}

void Object::clear()
{
	m_keyValuePairs.clear();
}

void Object::reserve(SizeType size)
{
	m_keyValuePairs.reserve(size);
}

auto Object::find(std::string_view key) -> Value*
{
	for(auto& [objKey, value] : m_keyValuePairs)
	{
		if(objKey == key)
			return &value;
	}

	return nullptr;
}

auto Object::get(std::string_view key) -> Value&
{
	auto* value = find(key);

	if(value)
		return *value;

	throw TypeError("JSON object does not contain key '" + std::string(key) + '\'');
}

auto Object::operator[](std::string_view key) -> Value&
{
	auto* value = find(key);

	if(value)
		return *value;

	return m_keyValuePairs.emplace_back(String(key), Value()).value();
}

auto Object::operator==(const Object& other) const -> bool
{
	if(size() != other.size())
		return false;

	if(std::equal(begin(), end(), other.begin(), [](const auto& lhs, const auto& rhs){
		return lhs.key() == rhs.key() && lhs.value() == rhs.value();
	}))
		return true;

	for(const auto& [key, value] : m_keyValuePairs)
	{
		const auto matches = [&](const auto& pair){ return pair.key() == key && pair.value() == value; };
		const auto count = std::count_if(m_keyValuePairs.begin(), m_keyValuePairs.end(), matches);
		if(count == 0 || count != std::count_if(other.begin(), other.end(), matches))
			return false;
	}

	return true;
}

} // namespace lsp::json
