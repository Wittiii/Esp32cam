#pragma once

#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

// Bounded, allocation-free parsing for flat camera control fields. Unknown
// nested JSON values are validated and skipped; they are never settings.
namespace controlpayload {

struct Token {
  const char *begin;
  const char *end;
  bool quoted;
  Token(const char *first = nullptr, const char *last = nullptr, bool isQuoted = false)
      : begin(first), end(last), quoted(isQuoted) {}
};

inline bool whitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline void skipWhitespace(const char *&cursor, const char *end) {
  while (cursor != end && whitespace(*cursor)) ++cursor;
}

inline bool parseInteger(const char *token, int &value) {
  char *end = nullptr;
  errno = 0;
  const long parsed = strtol(token, &end, 10);
  if (errno == ERANGE || end == token || parsed < INT_MIN || parsed > INT_MAX) return false;
  while (*end != '\0' && whitespace(*end)) ++end;
  if (*end != '\0') return false;
  value = static_cast<int>(parsed);
  return true;
}

inline bool hexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool readString(const char *&cursor, const char *end, Token &token) {
  if (cursor == end || *cursor++ != '"') return false;
  token.begin = cursor;
  token.quoted = true;
  while (cursor != end) {
    const unsigned char c = static_cast<unsigned char>(*cursor++);
    if (c == '"') {
      token.end = cursor - 1;
      return true;
    }
    if (c < 0x20) return false;
    if (c != '\\') continue;
    if (cursor == end) return false;
    const char escape = *cursor++;
    if (escape == 'u') {
      for (int digit = 0; digit != 4; ++digit)
        if (cursor == end || !hexDigit(*cursor++)) return false;
    } else if (escape != '"' && escape != '\\' && escape != '/' &&
               escape != 'b' && escape != 'f' && escape != 'n' &&
               escape != 'r' && escape != 't') {
      return false;
    }
  }
  return false;
}

inline bool matches(const Token &token, const char *text) {
  const size_t length = strlen(text);
  return static_cast<size_t>(token.end - token.begin) == length &&
         memcmp(token.begin, text, length) == 0;
}

inline bool readValue(const char *&cursor, const char *end, Token &token, unsigned depth = 0) {
  if (cursor == end || depth > 8) return false;
  if (*cursor == '"') return readString(cursor, end, token);
  token = {cursor, nullptr, false};
  if (*cursor == '{' || *cursor == '[') {
    const bool object = *cursor++ == '{';
    const char closing = object ? '}' : ']';
    skipWhitespace(cursor, end);
    if (cursor != end && *cursor == closing) {
      token.end = ++cursor;
      return true;
    }
    while (cursor != end) {
      Token ignored;
      if (object) {
        if (!readString(cursor, end, ignored)) return false;
        skipWhitespace(cursor, end);
        if (cursor == end || *cursor++ != ':') return false;
        skipWhitespace(cursor, end);
      }
      if (!readValue(cursor, end, ignored, depth + 1)) return false;
      skipWhitespace(cursor, end);
      if (cursor == end) return false;
      if (*cursor == closing) {
        token.end = ++cursor;
        return true;
      }
      if (*cursor++ != ',') return false;
      skipWhitespace(cursor, end);
    }
    return false;
  }
  const char *literals[] = {"true", "false", "null"};
  for (const char *literal : literals) {
    const size_t length = strlen(literal);
    if (static_cast<size_t>(end - cursor) >= length && memcmp(cursor, literal, length) == 0) {
      cursor += length;
      token.end = cursor;
      return true;
    }
  }
  if (*cursor == '-') ++cursor;
  if (cursor == end || *cursor < '0' || *cursor > '9') return false;
  if (*cursor++ != '0') while (cursor != end && *cursor >= '0' && *cursor <= '9') ++cursor;
  if (cursor != end && *cursor == '.') {
    const char *firstDigit = ++cursor;
    while (cursor != end && *cursor >= '0' && *cursor <= '9') ++cursor;
    if (cursor == firstDigit) return false;
  }
  if (cursor != end && (*cursor == 'e' || *cursor == 'E')) {
    ++cursor;
    if (cursor != end && (*cursor == '+' || *cursor == '-')) ++cursor;
    const char *firstDigit = cursor;
    while (cursor != end && *cursor >= '0' && *cursor <= '9') ++cursor;
    if (cursor == firstDigit) return false;
  }
  token.end = cursor;
  return true;
}

inline bool find(const char *payload, size_t length, const char *key, Token &result) {
  const char *cursor = payload;
  const char *end = payload + length;
  if (memchr(payload, '\0', length) != nullptr) return false;
  skipWhitespace(cursor, end);
  bool found = false;
  Token candidate;
  if (cursor != end && *cursor == '{') {
    ++cursor;
    skipWhitespace(cursor, end);
    if (cursor != end && *cursor == '}') return false;
    while (cursor != end) {
      Token field, value;
      if (!readString(cursor, end, field)) return false;
      skipWhitespace(cursor, end);
      if (cursor == end || *cursor++ != ':') return false;
      skipWhitespace(cursor, end);
      if (!readValue(cursor, end, value)) return false;
      if (matches(field, key)) {
        if (found) return false; // Ambiguous duplicate settings are rejected.
        candidate = value;
        found = true;
      }
      skipWhitespace(cursor, end);
      if (cursor == end) return false;
      if (*cursor == '}') {
        ++cursor;
        skipWhitespace(cursor, end);
        if (cursor != end || !found) return false;
        result = candidate;
        return true;
      }
      if (*cursor++ != ',') return false;
      skipWhitespace(cursor, end);
    }
    return false;
  }
  // Legacy key=value;key=value payloads: match complete field names, so e.g.
  // awb_gain never accidentally matches awb or gain.
  while (cursor != end) {
    const char *segmentEnd = cursor;
    while (segmentEnd != end && *segmentEnd != ';') ++segmentEnd;
    const char *equals = cursor;
    while (equals != segmentEnd && *equals != '=') ++equals;
    if (equals != segmentEnd) {
      const char *nameEnd = equals;
      while (nameEnd != cursor && whitespace(nameEnd[-1])) --nameEnd;
      if (matches({cursor, nameEnd, false}, key)) {
        if (found) return false;
        const char *valueStart = equals + 1;
        skipWhitespace(valueStart, segmentEnd);
        const char *valueEnd = segmentEnd;
        while (valueEnd != valueStart && whitespace(valueEnd[-1])) --valueEnd;
        if (valueStart == valueEnd) return false;
        candidate = {valueStart, valueEnd, false};
        found = true;
      }
    }
    cursor = segmentEnd == end ? end : segmentEnd + 1;
    skipWhitespace(cursor, end);
  }
  if (found) result = candidate;
  return found;
}

inline unsigned hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  return (c >= 'a' && c <= 'f' ? c - 'a' : c - 'A') + 10;
}

template <typename Append>
bool decode(const Token &token, Append append) {
  const char *cursor = token.begin;
  while (cursor != token.end) {
    char c = *cursor++;
    if (!token.quoted || c != '\\') {
      if (!append(c)) return false;
      continue;
    }
    if (cursor == token.end) return false;
    c = *cursor++;
    if (c == 'u') {
      unsigned codepoint = 0;
      for (int i = 0; i != 4; ++i) {
        if (cursor == token.end || !hexDigit(*cursor)) return false;
        codepoint = codepoint * 16 + hexValue(*cursor++);
      }
      if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
        if (token.end - cursor < 6 || *cursor++ != '\\' || *cursor++ != 'u') return false;
        unsigned low = 0;
        for (int i = 0; i != 4; ++i) {
          if (!hexDigit(*cursor)) return false;
          low = low * 16 + hexValue(*cursor++);
        }
        if (low < 0xDC00 || low > 0xDFFF) return false;
        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + low - 0xDC00;
      } else if (codepoint == 0 || (codepoint >= 0xDC00 && codepoint <= 0xDFFF)) {
        return false;
      }
      if (codepoint < 0x80) {
        if (!append(static_cast<char>(codepoint))) return false;
      } else if (codepoint < 0x800) {
        if (!append(static_cast<char>(0xC0 | (codepoint >> 6))) ||
            !append(static_cast<char>(0x80 | (codepoint & 0x3F)))) return false;
      } else if (codepoint < 0x10000) {
        if (!append(static_cast<char>(0xE0 | (codepoint >> 12))) ||
            !append(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F))) ||
            !append(static_cast<char>(0x80 | (codepoint & 0x3F)))) return false;
      } else {
        if (!append(static_cast<char>(0xF0 | (codepoint >> 18))) ||
            !append(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F))) ||
            !append(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F))) ||
            !append(static_cast<char>(0x80 | (codepoint & 0x3F)))) return false;
      }
      continue;
    }
    switch (c) {
      case 'b': c = '\b'; break;
      case 'f': c = '\f'; break;
      case 'n': c = '\n'; break;
      case 'r': c = '\r'; break;
      case 't': c = '\t'; break;
      case '"': case '\\': case '/': break;
      default: return false;
    }
    if (!append(c)) return false;
  }
  return true;
}

}  // namespace controlpayload
