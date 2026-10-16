#include <cassert>
#include <charconv>
#include <filesystem>
#include <tuple>
#include <lsp/uri.h>

namespace lsp{
namespace{

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

auto parseUriScheme(std::string_view uriSchemeStr) -> std::size_t
{
	if(uriSchemeStr.empty() || !isAlpha(uriSchemeStr[0]))
		return 0;

	std::size_t len = 0;

	for(const char c : uriSchemeStr)
	{
		if(!isAlphanumeric(c) && c != '-' && c != '.' && c != '+')
			break;

		++len;
	}

	return len;
}

auto parseUriAuthority(std::string_view uriAuthorityStr) -> std::size_t
{
	std::size_t len = 0;

	for(const char c : uriAuthorityStr)
	{
		if(c == '/' || c == '?' || c == '#')
			break;

		++len;
	}

	return len;
}

auto parseUriPath(std::string_view uriPathStr) -> std::size_t
{
	std::size_t len = 0;

	for(const char c : uriPathStr)
	{
		if(c == '?' || c == '#')
			break;

		++len;
	}

	return len;
}

auto parseUriQuery(std::string_view uriQueryStr) -> std::size_t
{
	std::size_t len = 0;

	for(const char c : uriQueryStr)
	{
		if(c == '#')
			break;

		++len;
	}

	return len;
}

auto hasCharAt(std::string_view str, std::size_t idx, char c) -> bool
{
	return str.size() > idx && str[idx] == c;
}

auto utf8SequenceLength(std::string_view bytes, std::size_t offset) -> std::size_t
{
	const auto first = static_cast<unsigned char>(bytes[offset]);
	const auto length = first < 0x80 ? 1u : first >= 0xC2 && first <= 0xDF ? 2u :
	                    first >= 0xE0 && first <= 0xEF ? 3u : first >= 0xF0 && first <= 0xF4 ? 4u : 0u;
	if(length == 0 || length > bytes.size() - offset)
		return 0;
	for(std::size_t i = 1; i < length; ++i)
		if((static_cast<unsigned char>(bytes[offset + i]) & 0xC0) != 0x80)
			return 0;
	if(length > 2)
	{
		const auto second = static_cast<unsigned char>(bytes[offset + 1]);
		if((first == 0xE0 && second < 0xA0) || (first == 0xED && second >= 0xA0) ||
		   (first == 0xF0 && second < 0x90) || (first == 0xF4 && second >= 0x90))
			return 0;
	}
	return length;
}

void normalizeEncodedCase(std::string& str, std::size_t first, std::size_t count)
{
	const auto end = std::min(first + count, str.size());

	for(std::size_t i = first; i < end; ++i)
	{
		if(str[i] == '%' && i + 2 < end)
		{
			const auto toUpper = [](char c) -> char{ return c >= 'a' && c <= 'z' ? c - 32 : c; };

			auto j = i + 1;
			str[j] = toUpper(str[j]);
			++j;
			str[j] = toUpper(str[j]);

			i += 2;
		}
	}
}

} // namespace

auto Uri::parse(std::string_view uriStr) -> Uri
{
	auto uri = Uri();

	const auto schemeLen = parseUriScheme(uriStr);

	if(schemeLen == 0 || !hasCharAt(uriStr, schemeLen, ':'))
		return {};

	const auto scheme = uriStr.substr(0, schemeLen);
	auto       idx    = schemeLen;

	uri.insertScheme(scheme);
	idx += 1; // :

	const auto hasAuthority = hasCharAt(uriStr, idx, '/') && hasCharAt(uriStr, idx + 1, '/');

	if(hasAuthority)
	{
		idx += 2; // //
		const auto authorityLen = parseUriAuthority(uriStr.substr(idx));
		const auto authority    = uriStr.substr(idx, authorityLen);
		uri.insertAuthority(authority);
		idx += authorityLen;
	}

	const auto pathIsEmpty = idx >= uriStr.size();

	if(!pathIsEmpty)
	{
		if(hasAuthority && uriStr[idx] != '/' && uriStr[idx] != '?' && uriStr[idx] != '#')
			return {};

		const auto pathLen     = parseUriPath(uriStr.substr(idx));
		const auto path        = uriStr.substr(idx, pathLen);
		const auto decodedPath = decode(path);
		uri.insertPath(decodedPath);
		idx += pathLen;
	}

	const auto hasQuery = hasCharAt(uriStr, idx, '?');

	if(hasQuery)
	{
		idx += 1; // ?
		const auto queryLen = parseUriQuery(uriStr.substr(idx));
		const auto query    = uriStr.substr(idx, queryLen);
		uri.insertQuery(query);
		idx += queryLen;
	}

	const auto hasFragment = hasCharAt(uriStr, idx, '#');

	if(hasFragment)
	{
		idx += 1; // #
		const auto fragment = uriStr.substr(idx);
		uri.insertFragment(fragment);
		idx += fragment.size();
	}

	assert(idx == uriStr.size());

	return uri;
}

auto Uri::fileUriFromPath(std::string_view path) -> Uri
{
	const auto absPath = std::filesystem::absolute(
		std::u8string_view(reinterpret_cast<const char8_t*>(path.data()), path.size()));
	auto u8Path = absPath.generic_u8string();
	auto uriPath = std::string_view(reinterpret_cast<const char*>(u8Path.data()), u8Path.size());
	auto authority = std::string_view();

#ifdef _WIN32
	if(uriPath.starts_with("//"))
#else
	if(path.starts_with("//"))
#endif
	{
#ifndef _WIN32
		uriPath = path;
#endif
		uriPath.remove_prefix(2);
		const auto separator = uriPath.find('/');
		authority = uriPath.substr(0, separator);
		uriPath = separator == std::string_view::npos ? std::string_view("/") : uriPath.substr(separator);
	}
#ifdef _WIN32
	else if(!uriPath.starts_with('/'))
	{
		u8Path.insert(0, 1, '/');
		uriPath = std::string_view(reinterpret_cast<const char*>(u8Path.data()), u8Path.size());
	}
#endif

	auto uri = Uri();
	uri.setScheme(FileScheme);
	uri.setAuthority(encode(authority, ":@[]"));
	uri.setPath(uriPath);

	return uri;
}

auto Uri::fsPath() const -> std::string
{
	assert(isFileUri());
	auto p = path();
	auto result = std::string();
	if(!authority().empty())
	{
		const auto host = decode(authority());
		result.reserve(2 + host.size() + p.size());
		result = "//";
		result += host;
		result += p;
	}
	else
	{

#ifdef _WIN32
		if(p.size() >= 3 && p[0] == '/' && isAlpha(p[1]) && p[2] == ':')
			p.remove_prefix(1);
#endif
		result = std::string(p);
	}

#ifdef _WIN32
	for(auto& c : result)
		if(c == '/')
			c = '\\';
#endif
	return result;
}

auto Uri::isValid() const -> bool
{
	return m_schemeLen > 0;
}

auto Uri::isFileUri() const -> bool
{
	return scheme() == FileScheme;
}

auto Uri::hasAuthority() const -> bool
{
	return !!m_hasAuthority;
}

auto Uri::hasQuery() const -> bool
{
	return !!m_hasQuery;
}

auto Uri::hasFragment() const -> bool
{
	return !!m_hasFragment;
}

auto Uri::scheme() const -> std::string_view
{
	return std::string_view(m_data).substr(0, m_schemeLen);
}

auto Uri::authority() const -> std::string_view
{
	if(!hasAuthority())
		return {};

	const auto authorityIdx = m_schemeLen;
	return std::string_view(m_data).substr(authorityIdx, m_authorityLen);
}

auto Uri::path() const -> std::string_view
{
	const auto pathIdx = m_schemeLen + m_authorityLen;
	return std::string_view(m_data).substr(pathIdx, m_pathLen);
}

auto Uri::query() const -> std::string_view
{
	if(!hasQuery())
		return {};

	const auto queryIdx = m_schemeLen + m_authorityLen + m_pathLen;
	return std::string_view(m_data).substr(queryIdx, m_queryLen);
}

auto Uri::fragment() const -> std::string_view
{
	if(!hasFragment())
		return {};

	const auto fragmentIdx = m_schemeLen + m_authorityLen + m_pathLen + m_queryLen;
	return std::string_view(m_data).substr(fragmentIdx, m_fragmentLen);
}

auto Uri::setScheme(std::string_view scheme) -> bool
{
	const auto len = parseUriScheme(scheme);

	if(len == scheme.size())
	{
		insertScheme(scheme);
		return true;
	}

	return false;
}

auto Uri::setAuthority(std::string_view authority) -> bool
{
	const auto len = parseUriAuthority(authority);

	if(len == authority.size())
	{
		insertAuthority(authority);
		return true;
	}

	return false;
}

auto Uri::setPath(std::string_view path) -> bool
{
	if(!hasAuthority() && path.starts_with("//"))
		return false;

	insertPath(path);
	return true;
}

auto Uri::setQuery(std::string_view query) -> bool
{
	const auto len = parseUriQuery(query);

	if(len == query.size())
	{
		insertQuery(query);
		return true;
	}

	return false;
}

auto Uri::setFragment(std::string_view fragment) -> bool
{
	insertFragment(fragment);
	return true;
}

void Uri::removeAuthority()
{
	insertAuthority({});
	m_hasAuthority = 0;
}

void Uri::removeQuery()
{
	insertQuery({});
	m_hasQuery = 0;
}

void Uri::removeFragment()
{
	insertFragment({});
	m_hasFragment = 0;
}

void Uri::insertScheme(std::string_view scheme)
{
	m_data.replace(0, m_schemeLen, scheme);
	m_schemeLen = scheme.size();

	const auto toLower = [](char c) -> char{ return c >= 'A' && c <= 'Z' ? c + 32 : c; };

	for(std::size_t i = 0; i < m_schemeLen; ++i)
		m_data[i] = toLower(m_data[i]);
}

void Uri::insertAuthority(std::string_view authority)
{
	const auto authorityIdx = m_schemeLen;
	m_data.replace(authorityIdx, m_authorityLen, authority);
	m_authorityLen = authority.size();
	normalizeEncodedCase(m_data, m_schemeLen, m_authorityLen);
	m_hasAuthority = 1;
}

void Uri::insertPath(std::string_view path)
{
	const auto pathIdx = m_schemeLen + m_authorityLen;
	m_data.replace(pathIdx, m_pathLen, path);
	m_pathLen = path.size();
}

void Uri::insertQuery(std::string_view query)
{
	const auto queryIdx = m_schemeLen + m_authorityLen + m_pathLen;
	m_data.replace(queryIdx, m_queryLen, query);
	m_queryLen = query.size();
	normalizeEncodedCase(m_data, queryIdx, m_queryLen);
	m_hasQuery = 1;
}

void Uri::insertFragment(std::string_view fragment)
{
	const auto fragmentIdx = m_schemeLen + m_authorityLen + m_pathLen + m_queryLen;
	m_data.replace(fragmentIdx, m_fragmentLen, fragment);
	m_fragmentLen = fragment.size();
	normalizeEncodedCase(m_data, fragmentIdx, m_fragmentLen);
	m_hasFragment = 1;
}

auto Uri::toString() const -> std::string
{
	if(!isValid())
		return {};

	auto encodedPath = encode(path(), "/");
	if(!hasAuthority() && encodedPath.starts_with("//"))
		encodedPath.replace(0, 1, "%2F");

	auto result = std::string();
	result.reserve(m_data.size() - path().size() + encodedPath.size() + sizeof("://?#"));

	result += scheme();
	result += ':';

	if(hasAuthority())
	{
		result += "//";
		result += authority();

		if(!encodedPath.empty() && !encodedPath.starts_with('/'))
			result += '/';
	}

	result += encodedPath;

	if(hasQuery())
	{
		result += '?';
		result += query();
	}

	if(hasFragment())
	{
		result += '#';
		result += fragment();
	}

	return result;
}

auto Uri::encode(std::string_view decoded, std::string_view exclude) -> std::string
{
	std::string encoded;
	encoded.reserve(decoded.size());

	for(const char c : decoded)
	{
		if(exclude.find(c) != std::string_view::npos ||
		   isAlphanumeric(c) ||
		   c == '_' ||
		   c == '.' ||
		   c == '-')
		{
			encoded += c;
		}
		else
		{
			constexpr auto hexLookup = "0123456789ABCDEF";
			encoded += '%';
			encoded += hexLookup[static_cast<unsigned char>(c) >> 4];
			encoded += hexLookup[static_cast<unsigned char>(c) & 0xF];
		}
	}

	return encoded;
}

auto Uri::decode(std::string_view encoded) -> std::string
{
	const auto firstEscape = encoded.find('%');
	if(firstEscape == std::string_view::npos)
		return std::string(encoded);
	std::string decoded;
	decoded.reserve(encoded.size());
	decoded.append(encoded.substr(0, firstEscape));

	for(std::size_t i = firstEscape; i < encoded.size();)
	{
		if(encoded[i] != '%' || i + 2 >= encoded.size() || !isAlphanumeric(encoded[i + 1]) || !isAlphanumeric(encoded[i + 2]))
		{
			decoded += encoded[i++];
			continue;
		}
		const auto begin = i;
		const auto decodedBegin = decoded.size();
		auto literalPrefix = std::size_t(0);
		while(i + 2 < encoded.size() && encoded[i] == '%' && isAlphanumeric(encoded[i + 1]) && isAlphanumeric(encoded[i + 2]))
		{
			const char* start = encoded.data() + i + 1;
			const char* end   = start + 2;
			unsigned char c = 0;
			const auto [ptr, ec] = std::from_chars(start, end, c, 16);
			decoded += static_cast<char>(c);
			if(ec != std::errc{} || ptr != end)
				literalPrefix = decoded.size() - decodedBegin;
			i += 3;
		}
		const auto bytes = std::string_view(decoded).substr(decodedBegin);
		for(auto offset = literalPrefix; offset < bytes.size();)
		{
			const auto length = utf8SequenceLength(bytes, offset);
			if(length == 0)
				literalPrefix = ++offset;
			else
				offset += length;
		}
		// Failed runs retain their prefix until the remaining suffix can be decoded.
		if(literalPrefix != 0)
			decoded.replace(decodedBegin, literalPrefix, encoded.substr(begin, literalPrefix * 3));
	}

	return decoded;
}

auto Uri::operator==(const Uri& other) const -> bool
{
	if(data().size() != other.data().size())
		return false;

	if(scheme() != other.scheme())
		return false;

	if(hasAuthority() != other.hasAuthority())
		return false;

	if(authority() != other.authority())
		return false;

	if(path() != other.path())
		return false;

	if(hasQuery() != other.hasQuery())
		return false;

	if(query() != other.query())
		return false;

	if(hasFragment() != other.hasFragment())
		return false;

	if(fragment() != other.fragment())
		return false;

	return true;
}

auto Uri::operator<(const Uri& other) const -> bool
{
	return std::tuple(scheme(), hasAuthority(), authority(), path(), hasQuery(), query(), hasFragment(), fragment()) <
	       std::tuple(other.scheme(), other.hasAuthority(), other.authority(), other.path(), other.hasQuery(), other.query(), other.hasFragment(), other.fragment());
}

} // namespace lsp
