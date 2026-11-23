#include "diagnose.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sjq {

namespace {

// A simple, sequential JSON checker. It runs only after simdjson rejected
// the input, to say where and why.
struct checker {
  const char *s;
  size_t n;
  size_t i{0};
  json_problem problem{};

  bool fail(size_t at, std::string m) {
    problem.found = true;
    problem.offset = at;
    problem.message = std::move(m);
    return false;
  }

  // The character at i, for messages: 'x', or "end of input".
  std::string shown(size_t at) const {
    if (at >= n) { return "the end of the input"; }
    unsigned char c = (unsigned char)s[at];
    if (c < 0x20) {
      char tmp[40];
      snprintf(tmp, sizeof(tmp), "control character U+%04X", unsigned(c));
      return tmp;
    }
    size_t len = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2));
    if (len > n - at) { len = n - at; }
    return "'" + std::string(s + at, len) + "'";
  }

  void skip_space() {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) { i++; }
  }

  bool string_value() {
    size_t start = i;
    i++;  // the opening quote
    while (true) {
      if (i >= n) { return fail(start, "unclosed string"); }
      unsigned char c = (unsigned char)s[i];
      if (c == '"') {
        i++;
        return true;
      }
      if (c < 0x20) { return fail(i, "unescaped " + shown(i) + " in a string"); }
      if (c == '\\') {
        if (i + 1 >= n) { return fail(start, "unclosed string"); }
        char e = s[i + 1];
        if (e == 'u') {
          for (size_t k = 2; k < 6; k++) {
            if (i + k >= n || !isxdigit((unsigned char)s[i + k])) { return fail(i, "invalid \\u escape (it needs four hexadecimal digits)"); }
          }
          unsigned v = unsigned(strtoul(std::string(s + i + 2, 4).c_str(), nullptr, 16));
          if (v >= 0xD800 && v < 0xDC00) {
            bool pair = i + 11 < n && s[i + 6] == '\\' && s[i + 7] == 'u';
            unsigned lo = pair ? unsigned(strtoul(std::string(s + i + 8, 4).c_str(), nullptr, 16)) : 0;
            if (!pair || lo < 0xDC00 || lo >= 0xE000) { return fail(i, "unpaired surrogate in a \\u escape"); }
            i += 12;
            continue;
          }
          if (v >= 0xDC00 && v < 0xE000) { return fail(i, "unpaired surrogate in a \\u escape"); }
          i += 6;
          continue;
        }
        if (!(e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't')) {
          return fail(i, "invalid escape \\" + std::string(1, e) + " in a string");
        }
        i += 2;
        continue;
      }
      if (c >= 0x80) {
        size_t len = (c >= 0xC2 && c < 0xE0) ? 2 : (c >= 0xE0 && c < 0xF0) ? 3 : (c >= 0xF0 && c < 0xF5) ? 4 : 0;
        bool ok = len > 0 && i + len <= n;
        for (size_t k = 1; ok && k < len; k++) { ok = ((unsigned char)s[i + k] & 0xC0) == 0x80; }
        if (ok && len == 3) {
          unsigned char c1 = (unsigned char)s[i + 1];
          ok = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0);
        }
        if (ok && len == 4) {
          unsigned char c1 = (unsigned char)s[i + 1];
          ok = !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90);
        }
        if (!ok) {
          char tmp[64];
          snprintf(tmp, sizeof(tmp), "invalid UTF-8 (byte 0x%02X)", unsigned(c));
          return fail(i, tmp);
        }
        i += len;
        continue;
      }
      i++;
    }
  }

  static bool delimiter(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ']' || c == '}' || c == ':';
  }

  bool number_value() {
    size_t start = i;
    if (s[i] == '-') { i++; }
    if (i < n && s[i] == '0') {
      i++;
      if (i < n && isdigit((unsigned char)s[i])) { return fail(start, "invalid number (a leading zero)"); }
    } else if (i < n && isdigit((unsigned char)s[i])) {
      while (i < n && isdigit((unsigned char)s[i])) { i++; }
    } else {
      return fail(start, "invalid number");
    }
    if (i < n && s[i] == '.') {
      i++;
      if (i >= n || !isdigit((unsigned char)s[i])) { return fail(start, "invalid number (no digit after '.')"); }
      while (i < n && isdigit((unsigned char)s[i])) { i++; }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
      i++;
      if (i < n && (s[i] == '+' || s[i] == '-')) { i++; }
      if (i >= n || !isdigit((unsigned char)s[i])) { return fail(start, "invalid number (no digit in the exponent)"); }
      while (i < n && isdigit((unsigned char)s[i])) { i++; }
    }
    if (i < n && !delimiter(s[i]) && s[i] != '"' && s[i] != '[' && s[i] != '{') { return fail(start, "invalid number"); }
    if (std::isinf(strtod(std::string(s + start, i - start).c_str(), nullptr))) { return fail(start, "number out of range"); }
    return true;
  }

  bool literal(const char *word) {
    size_t len = strlen(word);
    if (n - i < len || memcmp(s + i, word, len) != 0 || (i + len < n && isalnum((unsigned char)s[i + len]))) {
      size_t end = i;
      while (end < n && isalnum((unsigned char)s[end])) { end++; }
      return fail(i, "invalid literal '" + std::string(s + i, end - i) + "' (expected true, false or null)");
    }
    i += len;
    return true;
  }

  // One value; the containers are kept on an explicit stack.
  bool value() {
    struct frame {
      char kind;    // '[' or '{'
      size_t open;  // offset of the bracket
      int state;    // 0: first item or close, 1: after an item, 2: after ',', 3: after a key
    };
    std::vector<frame> stack;
    bool want_value = true;
    while (true) {
      skip_space();
      if (want_value) {
        if (i >= n) {
          if (stack.empty()) { return fail(i, "expected a value but found the end of the input"); }
          return fail(stack.back().open, std::string("unclosed ") + (stack.back().kind == '[' ? "array" : "object"));
        }
        char c = s[i];
        if (c == '{' || c == '[') {
          stack.push_back({c, i, 0});
          i++;
          want_value = false;
          continue;
        }
        bool ok;
        if (c == '"') { ok = string_value(); }
        else if (c == '-' || isdigit((unsigned char)c)) { ok = number_value(); }
        else if (c == 't') { ok = literal("true"); }
        else if (c == 'f') { ok = literal("false"); }
        else if (c == 'n') { ok = literal("null"); }
        else {
          std::string after = !stack.empty() && stack.back().state == 2 ? " after ','" : (!stack.empty() && stack.back().state == 3 ? " after ':'" : "");
          return fail(i, "expected a value but found " + shown(i) + after);
        }
        if (!ok) { return false; }
        if (stack.empty()) { return true; }
        stack.back().state = 1;
        want_value = false;
        continue;
      }
      frame &f = stack.back();
      if (i >= n) { return fail(f.open, std::string("unclosed ") + (f.kind == '[' ? "array" : "object")); }
      char c = s[i];
      char close = f.kind == '[' ? ']' : '}';
      if (f.state == 0 && c == close) {
        stack.pop_back();
        i++;
        if (stack.empty()) { return true; }
        stack.back().state = 1;
        continue;
      }
      if (f.state == 1) {
        if (c == close) {
          stack.pop_back();
          i++;
          if (stack.empty()) { return true; }
          stack.back().state = 1;
          continue;
        }
        if (c == ',') {
          i++;
          f.state = 2;
          if (f.kind == '[') { want_value = true; }
          continue;
        }
        return fail(i, std::string("expected ',' or '") + close + "' but found " + shown(i));
      }
      if (f.kind == '{' && (f.state == 0 || f.state == 2)) {
        // A key, then ':'.
        if (c != '"') {
          std::string after = f.state == 2 ? " after ','" : "";
          return fail(i, "expected a string (an object key) but found " + shown(i) + after);
        }
        if (!string_value()) { return false; }
        skip_space();
        if (i >= n || s[i] != ':') { return fail(i, "expected ':' after an object key but found " + shown(i)); }
        i++;
        f.state = 3;
        want_value = true;
        continue;
      }
      // An array after '[' (state 0, not empty): an item.
      want_value = true;
    }
  }

  void run(bool single) {
    // A UTF-8 byte order mark at the start is accepted.
    if (n >= 3 && memcmp(s, "\xEF\xBB\xBF", 3) == 0) { i = 3; }
    bool first = true;
    while (true) {
      skip_space();
      if (i >= n) { return; }
      if (single && !first) {
        fail(i, "unexpected content after the JSON value");
        return;
      }
      if (!value()) { return; }
      first = false;
    }
  }
};

} // namespace

json_problem find_json_problem(const char *data, size_t len, bool single) {
  checker c{data, len};
  c.run(single);
  return c.problem;
}

std::string plain_error(simdjson::error_code err) {
  using namespace simdjson;
  switch (err) {
  case CAPACITY: return "the document is too large (more than 4 GiB)";
  case MEMALLOC: return "out of memory";
  case TAPE_ERROR: return "the JSON structure is invalid";
  case DEPTH_ERROR: return "the document is nested too deeply";
  case STRING_ERROR: return "invalid string";
  case T_ATOM_ERROR: case F_ATOM_ERROR: case N_ATOM_ERROR: return "invalid literal (expected true, false or null)";
  case NUMBER_ERROR: return "invalid number";
  case BIGINT_ERROR: return "an integer is too large (more than 64 bits)";
  case UTF8_ERROR: return "invalid UTF-8";
  case UNCLOSED_STRING: return "unclosed string";
  case UNESCAPED_CHARS: return "a string holds an unescaped control character";
  case EMPTY: return "no JSON value";
  case TRAILING_CONTENT: return "unexpected content after the JSON value";
  case IO_ERROR: return "cannot read the input";
  default: return error_message(err);
  }
}

std::string line_and_column(const char *text, size_t offset) {
  size_t line = 1, col = 1;
  for (size_t k = 0; k < offset; k++) {
    if (text[k] == '\n') {
      line++;
      col = 1;
    } else if (((unsigned char)text[k] & 0xC0) != 0x80) {
      col++;
    }
  }
  return "line " + std::to_string(line) + ", column " + std::to_string(col);
}

std::string describe_json_error(const char *text, size_t len, simdjson::error_code err, bool single) {
  json_problem p = find_json_problem(text, len, single);
  if (p.found) { return "invalid JSON at " + line_and_column(text, p.offset) + ": " + p.message; }
  return "invalid JSON: " + plain_error(err);
}

static void append_utf8(std::string &r, uint32_t cp) {
  if (cp < 0x80) { r.push_back(char(cp)); }
  else if (cp < 0x800) { r.push_back(char(0xC0 | (cp >> 6))); r.push_back(char(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { r.push_back(char(0xE0 | (cp >> 12))); r.push_back(char(0x80 | ((cp >> 6) & 0x3F))); r.push_back(char(0x80 | (cp & 0x3F))); }
  else { r.push_back(char(0xF0 | (cp >> 18))); r.push_back(char(0x80 | ((cp >> 12) & 0x3F))); r.push_back(char(0x80 | ((cp >> 6) & 0x3F))); r.push_back(char(0x80 | (cp & 0x3F))); }
}

bool utf16_with_bom_to_utf8(const char *text, size_t len, std::string &out) {
  if (len < 2) { return false; }
  bool le = (unsigned char)text[0] == 0xFF && (unsigned char)text[1] == 0xFE;
  bool be = (unsigned char)text[0] == 0xFE && (unsigned char)text[1] == 0xFF;
  if (!le && !be) { return false; }
  out.clear();
  out.reserve(len);
  auto unit = [&](size_t k) -> uint32_t {
    unsigned char a = (unsigned char)text[k], b = (unsigned char)text[k + 1];
    return le ? uint32_t(a | (b << 8)) : uint32_t((a << 8) | b);
  };
  for (size_t k = 2; k + 1 < len; k += 2) {
    uint32_t u = unit(k);
    if (u >= 0xD800 && u < 0xDC00 && k + 3 < len) {
      uint32_t lo = unit(k + 2);
      if (lo >= 0xDC00 && lo < 0xE000) {
        append_utf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
        k += 2;
        continue;
      }
    }
    append_utf8(out, (u >= 0xD800 && u < 0xE000) ? 0xFFFD : u);
  }
  // A last, lone byte is not a character.
  if (len % 2) { append_utf8(out, 0xFFFD); }
  return true;
}

bool looks_like_utf16(const char *text, size_t len) {
  // ASCII text in UTF-16 has a NUL byte next to each character.
  size_t probe = len < 64 ? len : 64, nul = 0;
  for (size_t k = 0; k < probe; k++) { nul += text[k] == 0; }
  // JSON text has no NUL byte: a few bytes are enough.
  return probe >= 2 && nul * 3 >= probe;
}

} // namespace sjq
