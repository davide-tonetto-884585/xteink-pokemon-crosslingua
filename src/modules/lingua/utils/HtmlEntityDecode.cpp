#include "modules/lingua/utils/HtmlEntityDecode.h"

#include <Epub/htmlEntities.h>

#include <cstdint>
#include <cstring>

namespace lingua {

namespace {

// Longest entity the decoder will consider, '&' and ';' included. The named table's longest key is
// "&thetasym;" (10); a numeric one is at most "&#1114111;" / "&#x10FFFF;" (10). Bounding the scan keeps
// a stray '&' in a long paragraph from searching to the end of the text.
constexpr size_t MAX_ENTITY_LEN = 12;

void appendUtf8(const uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Parses the digits of "&#...;" between `begin` and `end` (exclusive of '&#' and ';'). Returns 0 for
// anything that is not a valid, encodable code point; 0 itself is never a valid entity.
uint32_t parseNumericEntity(const char* begin, const char* end) {
  bool hex = false;
  if (begin < end && (*begin == 'x' || *begin == 'X')) {
    hex = true;
    ++begin;
  }
  if (begin == end) return 0;
  uint32_t cp = 0;
  for (const char* p = begin; p < end; ++p) {
    uint32_t digit;
    if (*p >= '0' && *p <= '9') {
      digit = static_cast<uint32_t>(*p - '0');
    } else if (hex && *p >= 'a' && *p <= 'f') {
      digit = static_cast<uint32_t>(*p - 'a' + 10);
    } else if (hex && *p >= 'A' && *p <= 'F') {
      digit = static_cast<uint32_t>(*p - 'A' + 10);
    } else {
      return 0;
    }
    cp = cp * (hex ? 16 : 10) + digit;
    if (cp > 0x10FFFF) return 0;
  }
  if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
  return cp;
}

}  // namespace

void decodeHtmlEntitiesInPlace(std::string& text) {
  const size_t firstAmp = text.find('&');
  if (firstAmp == std::string::npos) return;

  std::string out;
  out.reserve(text.size());
  out.append(text, 0, firstAmp);

  size_t i = firstAmp;
  while (i < text.size()) {
    const char c = text[i];
    if (c != '&') {
      out += c;
      ++i;
      continue;
    }
    const size_t limit = text.size() - i < MAX_ENTITY_LEN ? text.size() - i : MAX_ENTITY_LEN;
    const char* start = text.data() + i;
    const void* semi = std::memchr(start + 1, ';', limit - 1);
    if (semi != nullptr) {
      const char* end = static_cast<const char*>(semi);
      const size_t entityLen = static_cast<size_t>(end - start) + 1;
      if (start[1] == '#') {
        const uint32_t cp = parseNumericEntity(start + 2, end);
        if (cp != 0) {
          appendUtf8(cp, out);
          i += entityLen;
          continue;
        }
      } else if (const char* value = lookupHtmlEntity(start, entityLen)) {
        out += value;
        i += entityLen;
        continue;
      }
    }
    out += c;
    ++i;
  }
  text.swap(out);
}

}  // namespace lingua
