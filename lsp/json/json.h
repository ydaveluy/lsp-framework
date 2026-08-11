#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <lsp/exception.h>
#include <lsp/strmap.h>

namespace lsp::json{

/*
 * Types
 */

class Value;
class Object;

using Null    = std::nullptr_t;
using Boolean = bool;
using Integer = std::int32_t;
using Decimal = double;
using String  = std::string;
using Array   = std::vector<Value>;

/*
 * Errors
 */

class Error : public Exception{
protected:
	using Exception::Exception;
};

class TypeError : public Error{
public:
	TypeError(const std::string& message = "Unexpected json value") : Error{message}{}
};

class ParseError : public Error{
public:
	ParseError(const std::string& message, std::size_t textPos)
		: Error{message}
	  , m_textPos{textPos}{}

	std::size_t textPos() const noexcept{ return m_textPos; }

private:
	 std::size_t m_textPos = 0;
};

/*
 * Object
 */

class Object{
public:
	class MapType;

	Object();
	Object(const Object& other);
	Object(Object&&) noexcept;
	~Object();

	Object& operator=(const Object& other);
	Object& operator=(Object&& other) noexcept;

	[[nodiscard]] bool operator==(const Object& other) const;
	[[nodiscard]] bool operator!=(const Object& other) const{ return !(*this == other); }

	[[nodiscard]] Value& operator[](std::string_view key);

	[[nodiscard]] std::size_t size() const;
	[[nodiscard]] bool empty() const;
	[[nodiscard]] bool contains(std::string_view key) const;
	void reserve(std::size_t count);
	[[nodiscard]] Value& get(std::string_view key);
	[[nodiscard]] const Value& get(std::string_view key) const;
	[[nodiscard]] Value* find(std::string_view key);
	[[nodiscard]] const Value* find(std::string_view key) const;

	[[nodiscard]] MapType& keyValueMap();
	[[nodiscard]] const MapType& keyValueMap() const;

private:
	std::unique_ptr<MapType> m_map;
};

/*
 * Value
 */

class Value{
public:
	using VariantType = std::variant<Null, Boolean, Integer, Decimal, String, Array, Object>;

	constexpr Value() = default;
	constexpr Value(Null){}
	constexpr Value(Boolean b) : m_variant{b}{}
	constexpr Value(Integer i) : m_variant{i}{}
	constexpr Value(Decimal d) : m_variant{d}{}
	Value(String&& s) : m_variant{std::move(s)}{}
	Value(Array&& a) : m_variant{std::move(a)}{}
	Value(Object&& o) : m_variant{std::move(o)}{}

	[[nodiscard]] constexpr bool isNull()    const{ return std::holds_alternative<Null>(m_variant); }
	[[nodiscard]] constexpr bool isBoolean() const{ return std::holds_alternative<Boolean>(m_variant); }
	[[nodiscard]] constexpr bool isInteger() const{ return std::holds_alternative<Integer>(m_variant); }
	[[nodiscard]] constexpr bool isDecimal() const{ return std::holds_alternative<Decimal>(m_variant); }
	[[nodiscard]] constexpr bool isNumber()  const{ return isInteger() || isDecimal(); }
	[[nodiscard]] constexpr bool isString()  const{ return std::holds_alternative<String>(m_variant); }
	[[nodiscard]] constexpr bool isObject()  const{ return std::holds_alternative<Object>(m_variant); }
	[[nodiscard]] constexpr bool isArray()   const{ return std::holds_alternative<Array>(m_variant); }

	[[nodiscard]] Boolean       boolean() const{ return get<Boolean>(); }
	[[nodiscard]] Integer       integer() const{ return get<Integer>(); }
	[[nodiscard]] Decimal       decimal() const{ return get<Decimal>(); }
	[[nodiscard]] const String& string()  const{ return get<String>(); }
	[[nodiscard]] const Object& object()  const{ return get<Object>(); }
	[[nodiscard]] const Array&  array()   const{ return get<Array>(); }
	[[nodiscard]] String&       string(){ return get<String>(); }
	[[nodiscard]] Object&       object(){ return get<Object>(); }
	[[nodiscard]] Array&        array(){ return get<Array>(); }

	[[nodiscard]] Decimal number() const
	{
		if(isDecimal())
			return get<Decimal>();

		if(isInteger())
			return static_cast<Decimal>(get<Integer>());

		throw TypeError{};
	}

	[[nodiscard]] bool operator==(const Value& other) const = default;
	[[nodiscard]] bool operator!=(const Value& other) const = default;

	[[nodiscard]] const VariantType& variant() const{ return m_variant; }
	[[nodiscard]] VariantType& variant(){ return m_variant; }

private:
	VariantType m_variant;

	template<typename T>
	T& get()
	{
		if(auto* const v = std::get_if<T>(&m_variant))
			return *v;

		throw TypeError{};
	}

	template<typename T>
	const T& get() const
	{
		if(auto* const v = std::get_if<T>(&m_variant))
			return *v;

		throw TypeError{};
	}
};

/*
 * Object storage
 *
 * Flat and contiguous: a json object holds a handful of keys, and scanning
 * them beats hashing plus a node allocation each. An index is built once a
 * object grows past the threshold. Iteration order is insertion order.
 */

class Object::MapType{
public:
	using value_type     = std::pair<String, Value>;
	using Storage        = std::vector<value_type>;
	using iterator       = Storage::iterator;
	using const_iterator = Storage::const_iterator;

	MapType() = default;
	MapType(const MapType& other) : m_entries{other.m_entries}{ reindex(); }
	MapType(MapType&&) noexcept = default;
	MapType& operator=(const MapType& other){ m_entries = other.m_entries; reindex(); return *this; }
	MapType& operator=(MapType&&) noexcept = default;

	[[nodiscard]] iterator begin(){ return m_entries.begin(); }
	[[nodiscard]] iterator end(){ return m_entries.end(); }
	[[nodiscard]] const_iterator begin() const{ return m_entries.begin(); }
	[[nodiscard]] const_iterator end() const{ return m_entries.end(); }

	[[nodiscard]] std::size_t size() const{ return m_entries.size(); }
	[[nodiscard]] bool empty() const{ return m_entries.empty(); }
	void reserve(std::size_t count){ m_entries.reserve(count); }

	[[nodiscard]] iterator find(std::string_view key)
	{
		const auto idx = indexOf(key);
		return idx == NoIndex ? m_entries.end() : m_entries.begin() + static_cast<std::ptrdiff_t>(idx);
	}

	[[nodiscard]] const_iterator find(std::string_view key) const
	{
		const auto idx = indexOf(key);
		return idx == NoIndex ? m_entries.end() : m_entries.begin() + static_cast<std::ptrdiff_t>(idx);
	}

	[[nodiscard]] bool contains(std::string_view key) const{ return indexOf(key) != NoIndex; }

	// Appends without looking the key up; the caller has to know it is absent.
	Value& append(String&& key)
	{
		const auto* const oldData = m_entries.data();
		m_entries.emplace_back(std::move(key), Value{});

		if(m_entries.size() > IndexThreshold)
		{
			if(!m_index || m_entries.data() != oldData)
				reindex();
			else
				m_index->emplace(std::string_view{m_entries.back().first}, m_entries.size() - 1);
		}

		return m_entries.back().second;
	}

	// Returns the value for key, default constructing an entry if there is none.
	Value& emplace(std::string_view key)
	{
		if(const auto idx = indexOf(key); idx != NoIndex)
			return m_entries[idx].second;

		const auto* const oldData = m_entries.data();
		m_entries.emplace_back(String{key}, Value{});

		if(m_entries.size() > IndexThreshold)
		{
			if(!m_index || m_entries.data() != oldData)
				reindex();
			else
				m_index->emplace(std::string_view{m_entries.back().first}, m_entries.size() - 1);
		}

		return m_entries.back().second;
	}

	[[nodiscard]] bool operator==(const MapType& other) const
	{
		if(m_entries.size() != other.m_entries.size())
			return false;

		for(const auto& [key, value] : m_entries)
		{
			const auto it = other.find(key);

			if(it == other.end() || it->second != value)
				return false;
		}

		return true;
	}

private:
	static constexpr std::size_t IndexThreshold = 16;
	static constexpr std::size_t NoIndex        = static_cast<std::size_t>(-1);

	Storage                                             m_entries;
	std::unique_ptr<StrMap<std::string_view, std::size_t>> m_index;

	[[nodiscard]] std::size_t indexOf(std::string_view key) const
	{
		if(m_index)
		{
			const auto it = m_index->find(key);
			return it == m_index->end() ? NoIndex : it->second;
		}

		for(std::size_t i = 0; i < m_entries.size(); ++i)
		{
			if(m_entries[i].first == key)
				return i;
		}

		return NoIndex;
	}

	// The views point into m_entries, so this runs again after a reallocation.
	void reindex()
	{
		if(m_entries.size() <= IndexThreshold)
		{
			m_index.reset();
			return;
		}

		m_index = std::make_unique<StrMap<std::string_view, std::size_t>>();
		m_index->reserve(m_entries.size());

		for(std::size_t i = 0; i < m_entries.size(); ++i)
			m_index->emplace(std::string_view{m_entries[i].first}, i);
	}
};

/*
 * parse/stringify
 */

Value       parse(std::string_view text);
std::string stringify(const Value& json, bool format = false);
void        stringify(const Value& json, std::string& str, bool format = false);
std::string toStringLiteral(std::string_view str);
void        appendStringLiteral(std::string& str, std::string_view value);
std::string fromStringLiteral(std::string_view str);

} // namespace lsp::json
