// Builtins on text: @formats and string interpolation, regular expressions
// (test, match, capture, scan, split/2, sub, gsub) and dates.
#include "builtins.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <regex>
#include <unordered_map>

namespace sjq {

#define BI [[maybe_unused]] const call_node &c, [[maybe_unused]] const value &in, [[maybe_unused]] const env_ptr &e, [[maybe_unused]] emit_ref out

// Replaces invalid UTF-8 sequences with U+FFFD, as jq does for decoded data.
static std::string sanitize_utf8(const std::string &s) {
  std::string r;
  r.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    uint8_t ch = uint8_t(s[i]);
    size_t len = ch < 0x80 ? 1 : (ch >= 0xC2 && ch < 0xE0) ? 2 : (ch >= 0xE0 && ch < 0xF0) ? 3 : (ch >= 0xF0 && ch < 0xF5) ? 4 : 0;
    bool ok = len > 0 && i + len <= s.size();
    for (size_t k = 1; ok && k < len; k++) { ok = (uint8_t(s[i + k]) & 0xC0) == 0x80; }
    if (ok && len == 3) {
      uint8_t c1 = uint8_t(s[i + 1]);
      ok = !(ch == 0xE0 && c1 < 0xA0) && !(ch == 0xED && c1 >= 0xA0);
    }
    if (ok && len == 4) {
      uint8_t c1 = uint8_t(s[i + 1]);
      ok = !(ch == 0xF0 && c1 < 0x90) && !(ch == 0xF4 && c1 >= 0x90);
    }
    if (ok) {
      r.append(s, i, len);
      i += len;
    } else {
      r.append("\xEF\xBF\xBD");
      i++;
    }
  }
  return r;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(std::string_view s) {
  std::string r;
  size_t i = 0;
  for (; i + 3 <= s.size(); i += 3) {
    uint32_t v = (uint32_t(uint8_t(s[i])) << 16) | (uint32_t(uint8_t(s[i + 1])) << 8) | uint8_t(s[i + 2]);
    r.push_back(B64[v >> 18]);
    r.push_back(B64[(v >> 12) & 63]);
    r.push_back(B64[(v >> 6) & 63]);
    r.push_back(B64[v & 63]);
  }
  if (i < s.size()) {
    uint32_t v = uint32_t(uint8_t(s[i])) << 16;
    if (i + 1 < s.size()) { v |= uint32_t(uint8_t(s[i + 1])) << 8; }
    r.push_back(B64[v >> 18]);
    r.push_back(B64[(v >> 12) & 63]);
    r.push_back(i + 1 < s.size() ? B64[(v >> 6) & 63] : '=');
    r.push_back('=');
  }
  return r;
}

static std::string base64_decode(std::string_view s, const value &orig) {
  std::string r;
  uint32_t acc = 0;
  int bits = 0;
  size_t end = s.size();
  while (end > 0 && s[end - 1] == '=') { end--; }
  for (size_t i = 0; i < end; i++) {
    const char *p = strchr(B64, s[i]);
    if (!p || !s[i]) { throw jq_error(std::string(kind_name(orig.k)) + " (" + error_text(orig) + ") is not valid base64 data"); }
    acc = (acc << 6) | uint32_t(p - B64);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      r.push_back(char((acc >> bits) & 0xFF));
    }
  }
  return sanitize_utf8(r);
}

static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static std::string base32_encode(std::string_view s) {
  std::string r;
  for (size_t i = 0; i < s.size(); i += 5) {
    uint64_t v = 0;
    size_t n = std::min<size_t>(5, s.size() - i);
    for (size_t k = 0; k < 5; k++) { v = (v << 8) | (k < n ? uint8_t(s[i + k]) : 0); }
    static const size_t chars[] = {0, 2, 4, 5, 7, 8};
    for (size_t k = 0; k < 8; k++) {
      r.push_back(k < chars[n] ? B32[(v >> (35 - 5 * k)) & 31] : '=');
    }
  }
  return r;
}

static std::string base32_decode(std::string_view s, const value &orig) {
  std::string r;
  uint64_t acc = 0;
  int bits = 0;
  size_t end = s.size();
  while (end > 0 && s[end - 1] == '=') { end--; }
  for (size_t i = 0; i < end; i++) {
    const char *p = strchr(B32, s[i]);
    if (!p || !s[i]) { throw jq_error(std::string(kind_name(orig.k)) + " (" + error_text(orig) + ") is not valid base32 data"); }
    acc = (acc << 5) | uint64_t(p - B32);
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      r.push_back(char((acc >> bits) & 0xFF));
    }
  }
  return sanitize_utf8(r);
}

static std::string shell_quote(std::string_view s) {
  std::string r = "'";
  for (char ch : s) {
    if (ch == '\'') { r += "'\\''"; } else { r.push_back(ch); }
  }
  r.push_back('\'');
  return r;
}

std::string apply_format(const std::string &name, const value &v) {
  if (name == "text") { return std::string(get_str(tostring_of(v))); }
  if (name == "json") { return to_json(v); }
  if (name == "html") {
    std::string s(get_str(tostring_of(v))), r;
    for (char ch : s) {
      switch (ch) {
      case '<': r += "&lt;"; break;
      case '>': r += "&gt;"; break;
      case '&': r += "&amp;"; break;
      case '\'': r += "&apos;"; break;
      case '"': r += "&quot;"; break;
      default: r.push_back(ch);
      }
    }
    return r;
  }
  if (name == "uri") {
    std::string s(get_str(tostring_of(v))), r;
    for (char ch : s) {
      unsigned char u = (unsigned char)ch;
      if (isalnum(u) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
        r.push_back(ch);
      } else {
        char tmp[4];
        snprintf(tmp, sizeof(tmp), "%%%02X", u);
        r += tmp;
      }
    }
    return r;
  }
  if (name == "csv" || name == "tsv") {
    bool csv = name == "csv";
    if (v.k != K::Arr) { throw jq_error(std::string(kind_name(v.k)) + " (" + error_text(v) + ") cannot be " + name + "-formatted, only an array can be"); }
    std::string r;
    bool first = true;
    for_each_elem(v, [&](const value &x) {
      if (!first) { r.push_back(csv ? ',' : '\t'); }
      first = false;
      switch (x.k) {
      case K::Null: break;
      case K::True: case K::False: r += to_json(x); break;
      case K::Num: if (!std::isnan(get_num(x))) { r += to_json(x); } break;
      case K::Str: {
        std::string_view s = get_str(x);
        if (csv) {
          r.push_back('"');
          for (char ch : s) {
            if (ch == '"') { r += "\"\""; } else { r.push_back(ch); }
          }
          r.push_back('"');
        } else {
          for (char ch : s) {
            switch (ch) {
            case '\\': r += "\\\\"; break;
            case '\t': r += "\\t"; break;
            case '\r': r += "\\r"; break;
            case '\n': r += "\\n"; break;
            default: r.push_back(ch);
            }
          }
        }
        break;
      }
      default: throw jq_error(std::string(kind_name(x.k)) + " (" + error_text(x) + ") is not valid in a csv row");
      }
    });
    return r;
  }
  if (name == "sh") {
    auto one = [&](const value &x) -> std::string {
      if (x.k == K::Arr || x.k == K::Obj) { throw jq_error(std::string(kind_name(x.k)) + " (" + error_text(x) + ") can not be escaped for shell"); }
      if (x.k == K::Str) { return shell_quote(get_str(x)); }
      return to_json(x);
    };
    if (v.k != K::Arr) { return one(v); }
    std::string r;
    bool first = true;
    for_each_elem(v, [&](const value &x) {
      if (!first) { r.push_back(' '); }
      first = false;
      r += one(x);
    });
    return r;
  }
  if (name == "base64") { return base64_encode(get_str(tostring_of(v))); }
  if (name == "base64d") {
    value s = tostring_of(v);
    return base64_decode(get_str(s), s);
  }
  if (name == "base32") { return base32_encode(get_str(tostring_of(v))); }
  if (name == "base32d") {
    value s = tostring_of(v);
    return base32_decode(get_str(s), s);
  }
  throw jq_error(name + " is not a valid format");
}

void string_node::eval(const value &in, const env_ptr &e, emit_ref out) const {
  std::vector<std::string> pieces(exprs.size());
  // The last expression is the outer loop.
  std::function<void(size_t)> rec = [&](size_t k) {
    if (k == 0) {
      std::string s = lits[0];
      for (size_t i = 0; i < pieces.size(); i++) {
        s += pieces[i];
        s += lits[i + 1];
      }
      out(value::string(std::move(s)));
      return;
    }
    exprs[k - 1]->eval(in, e, [&](const value &v) {
      pieces[k - 1] = apply_format(format, v);
      rec(k - 1);
    });
  };
  rec(exprs.size());
}

void format_node::eval(const value &in, const env_ptr &, emit_ref out) const { out(value::string(apply_format(format, in))); }

// ---------------------------------------------------------------------------
// Regular expressions, with std::wregex (ECMAScript syntax): wide strings
// make '.' and the offsets work on characters. Named groups (?<name>...),
// which ECMAScript lacks in C++, are rewritten as plain groups.
// ---------------------------------------------------------------------------

static std::wstring to_wide(std::string_view s) {
  std::wstring w;
  w.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    uint8_t ch = uint8_t(s[i]);
    int len = ch < 0x80 ? 1 : (ch >= 0xF0 ? 4 : (ch >= 0xE0 ? 3 : 2));
    uint32_t cp = len == 1 ? ch : (ch & (len == 4 ? 0x07 : (len == 3 ? 0x0F : 0x1F)));
    for (int k = 1; k < len && i + size_t(k) < s.size(); k++) { cp = (cp << 6) | (uint8_t(s[i + size_t(k)]) & 0x3F); }
    i += size_t(len);
    if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
      cp -= 0x10000;
      w.push_back(wchar_t(0xD800 + (cp >> 10)));
      w.push_back(wchar_t(0xDC00 + (cp & 0x3FF)));
    } else {
      w.push_back(wchar_t(cp));
    }
  }
  return w;
}

static std::string to_utf8(const wchar_t *b, const wchar_t *e) {
  std::string r;
  for (const wchar_t *p = b; p < e; p++) {
    uint32_t cp = uint32_t(*p);
    if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp < 0xDC00 && p + 1 < e) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (uint32_t(p[1]) - 0xDC00);
      p++;
    }
    append_utf8(r, cp);
  }
  return r;
}

struct compiled_regex {
  std::wregex re{};
  std::vector<std::string> names{};  // per group (1-based in matches); "" when unnamed
  bool global{false};
  bool skip_empty{false};
  bool caret{false};  // the pattern has a ^ anchor
};

static std::shared_ptr<const compiled_regex> compile_regex(const value &pattern, const value &flags) {
  if (pattern.k != K::Str) { throw jq_error(std::string(kind_name(pattern.k)) + " (" + error_text(pattern) + ") cannot be matched, as it is not a string"); }
  std::string fl;
  if (flags.k == K::Str) { fl = std::string(get_str(flags)); }
  else if (flags.k != K::Null) { throw jq_error(std::string(kind_name(flags.k)) + " (" + error_text(flags) + ") is not a string"); }
  std::string key = fl + '\x01' + std::string(get_str(pattern));
  thread_local std::unordered_map<std::string, std::shared_ptr<const compiled_regex>> cache;
  auto it = cache.find(key);
  if (it != cache.end()) { return it->second; }
  auto cr = std::make_shared<compiled_regex>();
  bool icase = false, extended = false;
  for (char ch : fl) {
    switch (ch) {
    case 'g': cr->global = true; break;
    case 'i': icase = true; break;
    case 'x': extended = true; break;
    case 'n': cr->skip_empty = true; break;
    case 's': case 'l': case 'p': break;
    default: throw jq_error(fl + " is not a valid modifier string");
    }
  }
  std::string src(get_str(pattern)), out;
#if defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
  // std::regex backtracks and recurses: keep fuzzing inputs small.
  if (src.size() > 64) { throw jq_error("regex too long for fuzzing"); }
#endif
  bool in_class = false;
  for (size_t i = 0; i < src.size(); i++) {
    char ch = src[i];
    if (ch == '\\' && i + 1 < src.size()) {
      char d = src[i + 1];
      i++;
      // Oniguruma escapes that ECMAScript lacks.
      if (!in_class && d == 'A') { out += "^"; continue; }
      if (!in_class && (d == 'z' || d == 'Z')) { out += "$"; continue; }
      if (d == 'h') { out += in_class ? "0-9a-fA-F" : "[0-9a-fA-F]"; continue; }
      out.push_back('\\');
      out.push_back(d);
      continue;
    }
    if (in_class) {
      if (ch == ']') { in_class = false; }
      out.push_back(ch);
      continue;
    }
    if (ch == '[') {
      in_class = true;
      out.push_back(ch);
      if (i + 1 < src.size() && src[i + 1] == '^') { out.push_back('^'); i++; }
      if (i + 1 < src.size() && src[i + 1] == ']') { out.push_back(']'); i++; }
      continue;
    }
    if (ch == '^') { cr->caret = true; }
    if (extended && (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')) { continue; }
    if (extended && ch == '#') {
      while (i < src.size() && src[i] != '\n') { i++; }
      continue;
    }
    if (ch == '(') {
      if (i + 1 < src.size() && src[i + 1] == '?') {
        if (i + 2 < src.size() && (src[i + 2] == '<' || src[i + 2] == 'P') && i + 3 < src.size() && src[i + 3] != '=' && src[i + 3] != '!') {
          size_t start = i + (src[i + 2] == 'P' ? 4 : 3);
          size_t close = src.find('>', start);
          if (close == std::string::npos) { throw jq_error("Regex failure: invalid group name"); }
          cr->names.push_back(src.substr(start, close - start));
          out.push_back('(');
          i = close;
          continue;
        }
        if (i + 2 < src.size() && src[i + 2] == '<') { throw jq_error("Regex failure: lookbehind is not supported"); }
      } else {
        cr->names.emplace_back();
      }
    }
    out.push_back(ch);
  }
  try {
    auto opts = std::regex_constants::ECMAScript;
    if (icase) { opts |= std::regex_constants::icase; }
    cr->re = std::wregex(to_wide(out), opts);
  } catch (std::regex_error &err) {
    throw jq_error(std::string("Regex failure: ") + err.what());
  }
  if (cache.size() > 64) { cache.clear(); }
  cache.emplace(std::move(key), cr);
  return cr;
}

// The code point offset of a position of a wide string (UTF-16 on Windows
// counts surrogate pairs as one).
static double cp_offset(const std::wstring &w, size_t pos) {
  if (sizeof(wchar_t) != 2) { return double(pos); }
  size_t n = 0;
  for (size_t i = 0; i < pos; i++) { n += !(uint32_t(w[i]) >= 0xDC00 && uint32_t(w[i]) < 0xE000); }
  return double(n);
}

// Calls f(match object) for each match of re in s (all of them with g).
template <class F> static void each_match(const value &s, const compiled_regex &cr, bool force_global, F &&f) {
  if (s.k != K::Str) { throw jq_error(std::string(kind_name(s.k)) + " (" + error_text(s) + ") cannot be matched, as it is not a string"); }
#if defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
  if (get_str(s).size() > 256) { throw jq_error("string too long for regular expressions while fuzzing"); }
#endif
  check_stack();
  std::wstring w = to_wide(get_str(s));
  bool global = cr.global || force_global;
  size_t pos = 0;
  while (pos <= w.size()) {
    std::wsmatch m;
    // After the first match the search starts inside the string. libc++
    // ignores match_prev_avail for ^, so patterns with ^ use match_not_bol
    // (both libraries honor it for ^).
    auto flags = std::regex_constants::match_default;
    if (pos > 0) { flags = cr.caret ? std::regex_constants::match_not_bol : std::regex_constants::match_prev_avail; }
    bool found = false;
    try {
      found = std::regex_search(w.cbegin() + std::ptrdiff_t(pos), w.cend(), m, cr.re, flags);
    } catch (std::regex_error &err) {
      throw jq_error(std::string("Regex failure: ") + err.what());
    }
    if (!found) { break; }
    size_t mpos = pos + size_t(m.position(0)), mlen = size_t(m.length(0));
    if (!(cr.skip_empty && mlen == 0)) {
      std::vector<value> caps;
      for (size_t g = 1; g < m.size(); g++) {
        value name = g - 1 < cr.names.size() && !cr.names[g - 1].empty() ? value::string(cr.names[g - 1]) : value::null();
        if (!m[g].matched) {
          caps.push_back(value::object({{"offset", value::number(-1)}, {"length", value::number(0)}, {"string", value::null()}, {"name", name}}));
          continue;
        }
        size_t gpos = pos + size_t(m.position(g)), glen = size_t(m.length(g));
        double off = cp_offset(w, gpos);
        caps.push_back(value::object({{"offset", value::number(off)},
                                      {"length", value::number(cp_offset(w, gpos + glen) - off)},
                                      {"string", value::string(to_utf8(w.data() + gpos, w.data() + gpos + glen))},
                                      {"name", name}}));
      }
      double off = cp_offset(w, mpos);
      f(value::object({{"offset", value::number(off)},
                       {"length", value::number(cp_offset(w, mpos + mlen) - off)},
                       {"string", value::string(to_utf8(w.data() + mpos, w.data() + mpos + mlen))},
                       {"captures", value::array(std::move(caps))}}));
    }
    if (!global) { break; }
    pos = mlen == 0 ? mpos + 1 : mpos + mlen;
  }
}

// The regex and flags arguments of c (flags null when absent).
template <class F> static void regex_args(const call_node &c, size_t nre, const value &in, const env_ptr &e, F &&f) {
  auto with_flags = [&](const value &re) {
    if (c.args.size() > nre + 1) {
      c.args[nre + 1]->eval(in, e, [&](const value &fl) { f(re, fl); });
    } else {
      f(re, value::null());
    }
  };
  c.args[nre]->eval(in, e, with_flags);
}

static value capture_object(const value &m) {
  std::vector<std::pair<std::string, value>> r;
  value caps;
  object_get(m, "captures", caps);
  for_each_elem(caps, [&](const value &cap) {
    value name, str;
    object_get(cap, "name", name);
    object_get(cap, "string", str);
    if (name.k == K::Str) { r.emplace_back(std::string(get_str(name)), str); }
  });
  return value::object(std::move(r));
}

static void test_fn(BI) {
  regex_args(c, 0, in, e, [&](const value &re, const value &fl) {
    auto cr = compile_regex(re, fl);
    bool any = false;
    compiled_regex once = *cr;
    once.global = false;
    each_match(in, once, false, [&](const value &) { any = true; });
    out(value::boolean(any));
  });
}

static void match_fn(BI) {
  regex_args(c, 0, in, e, [&](const value &re, const value &fl) { each_match(in, *compile_regex(re, fl), false, out); });
}

static void capture_fn(BI) {
  regex_args(c, 0, in, e, [&](const value &re, const value &fl) {
    each_match(in, *compile_regex(re, fl), false, [&](const value &m) { out(capture_object(m)); });
  });
}

static void scan_fn(BI) {
  regex_args(c, 0, in, e, [&](const value &re, const value &fl) {
    each_match(in, *compile_regex(re, fl), true, [&](const value &m) {
      value caps, str;
      object_get(m, "captures", caps);
      if (container_size(caps) == 0) {
        object_get(m, "string", str);
        out(str);
        return;
      }
      std::vector<value> r;
      for_each_elem(caps, [&](const value &cap) {
        value s;
        object_get(cap, "string", s);
        r.push_back(s);
      });
      out(value::array(std::move(r)));
    });
  });
}

// The pieces of in between the matches (split/2).
static void split2_fn(BI) {
  regex_args(c, 0, in, e, [&](const value &re, const value &fl) {
    std::string_view s = get_str(in);
    std::vector<value> parts;
    size_t prev = 0;  // in code points
    each_match(in, *compile_regex(re, fl), true, [&](const value &m) {
      value off, len;
      object_get(m, "offset", off);
      object_get(m, "length", len);
      size_t a = size_t(get_num(off)), b = a + size_t(get_num(len));
      size_t ba = utf8_offset(s, prev), bb = utf8_offset(s, a);
      parts.push_back(value::string(std::string(s.substr(ba, bb - ba))));
      prev = b;
    });
    size_t ba = utf8_offset(s, prev);
    parts.push_back(value::string(std::string(s.substr(ba))));
    out(value::array(std::move(parts)));
  });
}

static void sub_impl(const call_node &c, const value &in, const env_ptr &e, emit_ref out, bool global) {
  if (in.k != K::Str) { throw jq_error(std::string(kind_name(in.k)) + " (" + error_text(in) + ") cannot be matched, as it is not a string"); }
  auto with = [&](const value &re, const value &fl) {
    std::string flags = fl.k == K::Str ? std::string(get_str(fl)) : std::string();
    if (global) { flags += "g"; }
    auto cr = compile_regex(re, value::string(flags));
    std::string_view s = get_str(in);
    std::vector<value> ms;
    each_match(in, *cr, false, [&](const value &m) { ms.push_back(m); });
    // The replacement is a filter on the captures; each of its outputs gives
    // a result (all combinations for several matches).
    std::function<void(size_t, size_t, std::string)> rec = [&](size_t i, size_t prev, std::string acc) {
      if (i == ms.size()) {
        acc.append(s.substr(utf8_offset(s, prev)));
        out(value::string(std::move(acc)));
        return;
      }
      value off, len;
      object_get(ms[i], "offset", off);
      object_get(ms[i], "length", len);
      size_t a = size_t(get_num(off));
      size_t ba = utf8_offset(s, prev), bb = utf8_offset(s, a);
      std::string gap(s.substr(ba, bb - ba));
      c.args[1]->eval(capture_object(ms[i]), e, [&](const value &rep) {
        if (rep.k != K::Str) { throw jq_error(std::string("string (") + error_text(value::string(acc + gap)) + ") and " + kind_name(rep.k) + " (" + error_text(rep) + ") cannot be added"); }
        rec(i + 1, a + size_t(get_num(len)), acc + gap + std::string(get_str(rep)));
      });
    };
    rec(0, 0, std::string());
  };
  c.args[0]->eval(in, e, [&](const value &re) {
    if (c.args.size() > 2) {
      c.args[2]->eval(in, e, [&](const value &fl) { with(re, fl); });
    } else {
      with(re, value::null());
    }
  });
}

// ---------------------------------------------------------------------------
// Dates. Broken-down times are arrays: [year, month (0-11), day of the month,
// hours, minutes, seconds (with a fraction), day of the week, day of the
// year], as in jq. Conversions use the proleptic Gregorian calendar in UTC.
// ---------------------------------------------------------------------------

// Days since 1970-01-01 of a date (month 1-12), and the reverse (H. Hinnant).
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}
static void civil_from_days(int64_t z, int64_t &y, int64_t &m, int64_t &d) {
  z += 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = yoe + era * 400;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y += m <= 2;
}

static value broken_down(double t) {
  if (std::isnan(t) || std::fabs(t) > 1e17) { throw jq_error("gmtime() requires a valid time"); }
  double secs = std::floor(t);
  int64_t s = int64_t(secs);
  int64_t days = (s >= 0 ? s : s - 86399) / 86400;
  int64_t rem = s - days * 86400;
  int64_t y = 0, m = 0, d = 0;
  civil_from_days(days, y, m, d);
  int64_t wday = ((days % 7) + 11) % 7;  // 1970-01-01 was a Thursday
  int64_t yday = days - days_from_civil(y, 1, 1);
  return value::array({value::number(double(y)), value::number(double(m - 1)), value::number(double(d)),
                       value::number(double(rem / 3600)), value::number(double((rem / 60) % 60)),
                       value::number(double(rem % 60) + (t - secs)), value::number(double(wday)), value::number(double(yday))});
}

static std::vector<double> tm_fields(const value &v, const char *who) {
  if (v.k != K::Arr) { throw jq_error(std::string(who) + " requires array of 6 numbers"); }
  std::vector<double> f;
  for_each_elem(v, [&](const value &x) {
    if (x.k != K::Num) { throw jq_error(std::string(who) + " requires parsed datetime inputs"); }
    f.push_back(get_num(x));
  });
  if (f.size() < 6) { throw jq_error(std::string(who) + " requires array of 6 numbers"); }
  while (f.size() < 8) { f.push_back(0); }
  return f;
}

static double timegm_of(const std::vector<double> &f) {
  int64_t y = int64_t(f[0]), mon = int64_t(f[1]);
  y += mon >= 0 ? mon / 12 : -((11 - mon) / 12);
  mon = ((mon % 12) + 12) % 12;
  int64_t days = days_from_civil(y, mon + 1, 1) + int64_t(f[2]) - 1;
  return double(days) * 86400 + f[3] * 3600 + f[4] * 60 + std::floor(f[5]);
}

static struct tm to_tm(const std::vector<double> &f) {
  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_year = int(f[0]) - 1900;
  t.tm_mon = int(f[1]);
  t.tm_mday = int(f[2]);
  t.tm_hour = int(f[3]);
  t.tm_min = int(f[4]);
  t.tm_sec = int(f[5]);
  t.tm_wday = int(f[6]);
  t.tm_yday = int(f[7]);
  return t;
}

static std::string format_tm(const struct tm &t, std::string fmt) {
  if (fmt.empty()) { return std::string(); }
  std::vector<char> buf(256);
  while (true) {
    size_t n = strftime(buf.data(), buf.size(), fmt.c_str(), &t);
    if (n > 0) { return std::string(buf.data(), n); }
    if (buf.size() > 65536) { return std::string(); }
    buf.resize(buf.size() * 4);
  }
}

static void strftime_fn(BI) {
  c.args[0]->eval(in, e, [&](const value &fmt) {
    if (fmt.k != K::Str) { throw jq_error("strftime/1 requires a string format"); }
    value bd = in.k == K::Num ? broken_down(get_num(in)) : in;
    if (bd.k != K::Arr) { throw jq_error("strftime/1 requires parsed datetime inputs"); }
    // UTC: %Z and %z do not depend on the time zone of the machine.
    std::string f(get_str(fmt)), g;
    for (size_t i = 0; i < f.size(); i++) {
      if (f[i] == '%' && i + 1 < f.size()) {
        if (f[i + 1] == 'Z') { g += "UTC"; i++; continue; }
        if (f[i + 1] == 'z') { g += "+0000"; i++; continue; }
        g += f.substr(i, 2);
        i++;
        continue;
      }
      g.push_back(f[i]);
    }
    out(value::string(format_tm(to_tm(tm_fields(bd, "strftime/1")), g)));
  });
}

static bool local_tm(double t, struct tm &out_tm) {
  time_t tt = time_t(std::floor(t));
#if defined(_WIN32)
  return localtime_s(&out_tm, &tt) == 0;
#else
  return localtime_r(&tt, &out_tm) != nullptr;
#endif
}

static value tm_to_value(const struct tm &t, double frac) {
  return value::array({value::number(t.tm_year + 1900.0), value::number(t.tm_mon), value::number(t.tm_mday), value::number(t.tm_hour),
                       value::number(t.tm_min), value::number(t.tm_sec + frac), value::number(t.tm_wday), value::number(t.tm_yday)});
}

static void localtime_fn(BI) {
  double t = in.k == K::Num ? get_num(in) : NAN;
  struct tm lt;
  if (std::isnan(t) || !local_tm(t, lt)) { throw jq_error("localtime() requires a number"); }
  out(tm_to_value(lt, t - std::floor(t)));
}

static void strflocaltime_fn(BI) {
  c.args[0]->eval(in, e, [&](const value &fmt) {
    if (fmt.k != K::Str) { throw jq_error("strflocaltime/1 requires a string format"); }
    struct tm lt;
    if (in.k == K::Num) {
      if (!local_tm(get_num(in), lt)) { throw jq_error("strflocaltime/1 requires parsed datetime inputs"); }
    } else {
      lt = to_tm(tm_fields(in, "strflocaltime/1"));
    }
    out(value::string(format_tm(lt, std::string(get_str(fmt)))));
  });
}

// strptime for the common directives (the C library has none on Windows).
static bool parse_time(const std::string &s, const std::string &f, std::vector<double> &tmv) {
  static const char *months[] = {"january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"};
  static const char *day_names[] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
  int64_t year = 1900, mon = 0, mday = 1, hour = 0, min = 0, yday = -1;
  double sec = 0;
  bool pm = false, have_pm = false;
  size_t i = 0;
  auto num = [&](size_t maxlen, int64_t &v) {
    size_t start = i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    int64_t r = 0;
    while (i < s.size() && i - start < maxlen && isdigit((unsigned char)s[i])) { r = r * 10 + (s[i] - '0'); i++; }
    if (i == start || (i == start + 1 && (s[start] == '+' || s[start] == '-'))) { return false; }
    v = neg ? -r : r;
    return true;
  };
  auto name = [&](const char *const *names, int count, int64_t &v) {
    for (int k = 0; k < count; k++) {
      size_t len = strlen(names[k]);
      for (size_t l : {len, size_t(3)}) {
        if (i + l > s.size()) { continue; }
        bool ok = true;
        for (size_t q = 0; q < l && ok; q++) { ok = tolower((unsigned char)s[i + q]) == names[k][q]; }
        if (ok) { v = k; i += l; return true; }
      }
    }
    return false;
  };
  for (size_t k = 0; k < f.size(); k++) {
    char fc = f[k];
    if (isspace((unsigned char)fc)) {
      while (i < s.size() && isspace((unsigned char)s[i])) { i++; }
      continue;
    }
    if (fc != '%' || k + 1 >= f.size()) {
      if (i >= s.size() || s[i] != fc) { return false; }
      i++;
      continue;
    }
    char d = f[++k];
    int64_t v = 0;
    switch (d) {
    case 'Y': if (!num(4, year)) { return false; } break;
    case 'm': if (!num(2, v)) { return false; } mon = v - 1; break;
    case 'd': case 'e':
      while (d == 'e' && i < s.size() && s[i] == ' ') { i++; }
      if (!num(2, mday)) { return false; }
      break;
    case 'H': if (!num(2, hour)) { return false; } break;
    case 'I': if (!num(2, hour)) { return false; } break;
    case 'M': if (!num(2, min)) { return false; } break;
    case 'S': if (!num(2, v)) { return false; } sec = double(v); break;
    case 'j': if (!num(3, v)) { return false; } yday = v - 1; break;
    case 'y': if (!num(2, v)) { return false; } year = v < 69 ? 2000 + v : 1900 + v; break;
    case 'b': case 'B': case 'h': if (!name(months, 12, mon)) { return false; } break;
    case 'a': case 'A': if (!name(day_names, 7, v)) { return false; } break;
    case 'p':
      if (i + 2 <= s.size() && (tolower((unsigned char)s[i]) == 'a' || tolower((unsigned char)s[i]) == 'p') && tolower((unsigned char)s[i + 1]) == 'm') {
        pm = tolower((unsigned char)s[i]) == 'p';
        have_pm = true;
        i += 2;
      } else {
        return false;
      }
      break;
    case 'Z': while (i < s.size() && isalpha((unsigned char)s[i])) { i++; } break;
    case 'z':
      if (i < s.size() && (s[i] == 'Z' || s[i] == 'z')) { i++; break; }
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        i++;
        size_t start = i;
        while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == ':') && i - start < 5) { i++; }
        if (i == start) { return false; }
      } else {
        return false;
      }
      break;
    case 'T':
      if (!num(2, hour) || i >= s.size() || s[i++] != ':' || !num(2, min) || i >= s.size() || s[i++] != ':' || !num(2, v)) { return false; }
      sec = double(v);
      break;
    case 'F':
      if (!num(4, year) || i >= s.size() || s[i++] != '-' || !num(2, v) || i >= s.size() || s[i++] != '-' || !num(2, mday)) { return false; }
      mon = v - 1;
      break;
    case 'R':
      if (!num(2, hour) || i >= s.size() || s[i++] != ':' || !num(2, min)) { return false; }
      break;
    case 's': {
      if (!num(20, v)) { return false; }
      value bd = broken_down(double(v));
      tmv.clear();
      for_each_elem(bd, [&](const value &x) { tmv.push_back(get_num(x)); });
      year = int64_t(tmv[0]); mon = int64_t(tmv[1]); mday = int64_t(tmv[2]); hour = int64_t(tmv[3]); min = int64_t(tmv[4]); sec = tmv[5];
      break;
    }
    case 'n': case 't': while (i < s.size() && isspace((unsigned char)s[i])) { i++; } break;
    case '%': if (i >= s.size() || s[i] != '%') { return false; } i++; break;
    default: return false;
    }
  }
  if (i != s.size()) { return false; }
  if (have_pm) {
    if (hour == 12) { hour = 0; }
    if (pm) { hour += 12; }
  }
  if (yday >= 0 && mon == 0 && mday == 1) {
    int64_t days = days_from_civil(year, 1, 1) + yday;
    int64_t y = 0, m = 0, d = 0;
    civil_from_days(days, y, m, d);
    mon = m - 1;
    mday = d;
  }
  int64_t days = days_from_civil(year, mon + 1, mday);
  tmv = {double(year), double(mon), double(mday), double(hour), double(min), sec,
         double(((days % 7) + 11) % 7), double(days - days_from_civil(year, 1, 1))};
  return true;
}

static void strptime_fn(BI) {
  c.args[0]->eval(in, e, [&](const value &fmt) {
    if (in.k != K::Str) { throw jq_error("strptime/1 requires string inputs and arguments"); }
    if (fmt.k != K::Str) { throw jq_error("strptime/1 requires string inputs and arguments"); }
    std::vector<double> tmv;
    std::string s(get_str(in)), f(get_str(fmt));
    if (!parse_time(s, f, tmv)) { throw jq_error("date \"" + s + "\" does not match format \"" + f + "\""); }
    std::vector<value> r;
    for (double x : tmv) { r.push_back(value::number(x)); }
    out(value::array(std::move(r)));
  });
}

const std::vector<builtin_entry> &text_builtins() {
  static const std::vector<builtin_entry> table = {
      {"test/1", test_fn, nullptr},
      {"test/2", test_fn, nullptr},
      {"match/1", match_fn, nullptr},
      {"match/2", match_fn, nullptr},
      {"capture/1", capture_fn, nullptr},
      {"capture/2", capture_fn, nullptr},
      {"scan/1", scan_fn, nullptr},
      {"scan/2", scan_fn, nullptr},
      {"split/2", split2_fn, nullptr},
      {"sub/2", [](BI) { sub_impl(c, in, e, out, false); }, nullptr},
      {"sub/3", [](BI) { sub_impl(c, in, e, out, false); }, nullptr},
      {"gsub/2", [](BI) { sub_impl(c, in, e, out, true); }, nullptr},
      {"gsub/3", [](BI) { sub_impl(c, in, e, out, true); }, nullptr},
      {"gmtime/0", [](BI) {
         if (in.k != K::Num) { throw jq_error("gmtime() requires a number"); }
         out(broken_down(get_num(in)));
       }, nullptr},
      {"localtime/0", localtime_fn, nullptr},
      {"mktime/0", [](BI) {
         if (in.k != K::Arr) { throw jq_error("mktime requires array of 6 numbers"); }
         out(value::number(timegm_of(tm_fields(in, "mktime"))));
       }, nullptr},
      {"strftime/1", strftime_fn, nullptr},
      {"strflocaltime/1", strflocaltime_fn, nullptr},
      {"strptime/1", strptime_fn, nullptr},
  };
  return table;
}

} // namespace sjq
