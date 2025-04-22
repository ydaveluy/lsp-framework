#pragma once

#include <string>
#include <string_view>

namespace lsp {

/*
 * Really simple uri class that only supports the file:// scheme
 */
class URI {
public:
  inline static const std::string Scheme{"file://"};

  URI() = default;
  URI(std::string_view in) : m_path{fromString(in)} {}
  URI(const std::string &in) : m_path{fromString(in)} {}
  URI(const char *in) : m_path{fromString(in)} {}

  bool operator==(const URI &other) const { return path() == other.path(); }
  bool operator!=(const URI &other) const { return path() != other.path(); }
  bool operator==(std::string_view other) const { return path() == other; }
  bool operator!=(std::string_view other) const { return path() != other; }
  bool operator<(const URI &other) const { return path() < other.path(); }

  const std::string &path() const { return m_path; }
  std::string toString() const;
  bool isValid() const { return !m_path.empty(); }
  void clear() { m_path.clear(); }

private:
  std::string m_path;

  static std::string fromString(std::string_view str);
  static std::string encode(std::string_view decoded);
  static std::string decode(std::string_view encoded);
};

} // namespace lsp
