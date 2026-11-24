#include "ast.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace sjq {

enum class T { End, Dot, DotDot, Field, Ident, Var, Num, Str, Punct, Keyword, Format };

struct token {
  T t;
  std::string s{};
  double num{0};
  // A string with interpolations: literal parts at even indexes, the source
  // of the expressions at odd indexes.
  std::vector<std::string> parts{};
  std::vector<size_t> part_pos{};  // offset of each part in the filter
  int line{1};
  size_t pos{0};  // offset of the token in the filter
};

struct lexer {
  std::string src{};
  size_t i{0};
  std::vector<token> toks{};
  int base_line{1};   // line of src[0] in the whole filter
  size_t base_pos{0};  // offset of src[0] in the whole filter
  size_t tok_start{0}; // start of the token being read

  [[noreturn]] void fail(const std::string &m, std::string hint = std::string()) {
    throw syntax_error("syntax error: " + m, base_pos + tok_start, std::move(hint));
  }
  [[noreturn]] void fail_at(const std::string &m, size_t at, std::string hint = std::string()) {
    throw syntax_error("syntax error: " + m, base_pos + at, std::move(hint));
  }

  static bool ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
  static bool ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

  static int hex_digit(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
  }
  // The four hex digits of a \u escape at src[at]; errors point at the
  // backslash (at - 2).
  unsigned hex4(size_t at) {
    if (at + 4 > src.size()) { fail_at("invalid \\uXXXX escape (it needs four hexadecimal digits)", at - 2); }
    unsigned cp = 0;
    for (size_t k = at; k < at + 4; k++) {
      int d = hex_digit(src[k]);
      if (d < 0) { fail_at("invalid \\uXXXX escape (it needs four hexadecimal digits)", at - 2); }
      cp = cp * 16 + unsigned(d);
    }
    return cp;
  }

  // The whole UTF-8 character that starts at src[at].
  std::string utf8_char_at(size_t at) const {
    unsigned char c = (unsigned char)src[at];
    size_t len = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : 2));
    return src.substr(at, std::min(len, src.size() - at));
  }

  int line_at(size_t pos) const {
    return base_line + int(std::count(src.begin(), src.begin() + std::ptrdiff_t(std::min(pos, src.size())), '\n'));
  }

  // Skips a string literal; at points after its opening quote. Returns the
  // index after the closing quote.
  size_t skip_string(size_t at) {
    while (at < src.size() && src[at] != '"') {
      if (src[at] == '\\') {
        if (at + 1 < src.size() && src[at + 1] == '(') { at = skip_interp(at + 2) + 1; continue; }
        at += 2;
        continue;
      }
      at++;
    }
    if (at >= src.size()) { fail_at("unterminated string", src.size()); }
    return at + 1;
  }
  // Finds the ')' that closes an interpolation; at points after "\(".
  size_t skip_interp(size_t at) {
    int depth = 0;
    while (at < src.size()) {
      char c = src[at];
      if (c == '"') { at = skip_string(at + 1); continue; }
      if (c == '#') {
        while (at < src.size() && src[at] != '\n') { at++; }
        continue;
      }
      if (c == '(') { depth++; }
      if (c == ')') {
        if (depth == 0) { return at; }
        depth--;
      }
      at++;
    }
    fail("unterminated string interpolation");
  }

  // Reads a string literal; i points after the opening quote. Returns the
  // parts (one literal part when there is no interpolation).
  std::vector<size_t> last_part_pos{};  // set by read_string

  std::vector<std::string> read_string() {
    std::vector<std::string> parts(1);
    last_part_pos.assign(1, 0);
    while (i < src.size() && src[i] != '"') {
      std::string &r = parts.back();
      char c = src[i++];
      if (c != '\\') {
        r.push_back(c);
        continue;
      }
      size_t esc = i - 1;  // the backslash, for errors
      if (i >= src.size()) { fail_at("unterminated string", src.size()); }
      char d = src[i++];
      switch (d) {
      case 'n': r.push_back('\n'); break;
      case 't': r.push_back('\t'); break;
      case 'r': r.push_back('\r'); break;
      case 'b': r.push_back('\b'); break;
      case 'f': r.push_back('\f'); break;
      case '/': r.push_back('/'); break;
      case '\\': r.push_back('\\'); break;
      case '"': r.push_back('"'); break;
      case 'u': {
        unsigned cp = hex4(i);
        i += 4;
        if (cp >= 0xDC00 && cp < 0xE000) { fail_at("invalid \\uXXXX\\uXXXX surrogate pair escape", esc); }
        if (cp >= 0xD800 && cp < 0xDC00) {
          if (i + 2 > src.size() || src[i] != '\\' || src[i + 1] != 'u') { fail_at("invalid \\uXXXX\\uXXXX surrogate pair escape", esc); }
          unsigned lo = hex4(i + 2);
          if (lo < 0xDC00 || lo >= 0xE000) { fail_at("invalid \\uXXXX\\uXXXX surrogate pair escape", esc); }
          i += 6;
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        if (cp < 0x80) { r.push_back(char(cp)); }
        else if (cp < 0x800) { r.push_back(char(0xC0 | (cp >> 6))); r.push_back(char(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { r.push_back(char(0xE0 | (cp >> 12))); r.push_back(char(0x80 | ((cp >> 6) & 0x3F))); r.push_back(char(0x80 | (cp & 0x3F))); }
        else { r.push_back(char(0xF0 | (cp >> 18))); r.push_back(char(0x80 | ((cp >> 12) & 0x3F))); r.push_back(char(0x80 | ((cp >> 6) & 0x3F))); r.push_back(char(0x80 | (cp & 0x3F))); }
        break;
      }
      case '(': {
        size_t close = skip_interp(i);
        parts.push_back(src.substr(i, close - i));
        last_part_pos.push_back(base_pos + i);
        parts.emplace_back();
        last_part_pos.push_back(0);
        i = close + 1;
        break;
      }
      default: fail_at(std::string("bad escape \\") + d, esc);
      }
    }
    if (i >= src.size()) { fail_at("unterminated string", src.size()); }
    i++;
    return parts;
  }

  void push(T t, std::string s, size_t at) {
    token tk{t, std::move(s)};
    tk.line = line_at(at);
    tk.pos = base_pos + at;
    toks.push_back(std::move(tk));
  }

  void run() {
    static const char *keywords[] = {"if", "then", "elif", "else", "end", "as", "reduce", "foreach", "and", "or",
                                     "try", "catch", "label", "def", "import", "include"};
    while (true) {
      while (i < src.size() && isspace((unsigned char)src[i])) { i++; }
      if (i < src.size() && src[i] == '#') { while (i < src.size() && src[i] != '\n') { i++; } continue; }
      if (i >= src.size()) { push(T::End, "", i); return; }
      size_t start = i;
      tok_start = start;
      char c = src[i];
      if (c == '.') {
        if (i + 1 < src.size() && src[i + 1] == '.') { i += 2; push(T::DotDot, "..", start); continue; }
        if (i + 1 < src.size() && (ident_start(src[i + 1]) || (unsigned char)src[i + 1] >= 0x80)) {
          size_t j = i + 1;
          bool ascii = true;
          while (j < src.size() && (ident_char(src[j]) || (unsigned char)src[j] >= 0x80)) {
            ascii = ascii && (unsigned char)src[j] < 0x80;
            j++;
          }
          std::string name = src.substr(i + 1, j - i - 1);
          if (!ascii) {
            // As in jq, .name takes ASCII letters, digits and '_' only. This is
            // also what Windows PowerShell 5 leaves of ."name".
            size_t bad = i + 1;
            while ((unsigned char)src[bad] < 0x80) { bad++; }
            fail_at("unexpected character '" + utf8_char_at(bad) + "'", bad,
                    "a field name with non-ASCII characters needs quotes: write .\"" + name + "\" or .[\"" + name + "\"]");
          }
          push(T::Field, name, start);
          i = j;
          continue;
        }
        if (i + 1 < src.size() && src[i + 1] == '"') {
          i += 2;
          auto parts = read_string();
          if (parts.size() > 1) { fail("string interpolation in a field name is not supported"); }
          push(T::Field, std::move(parts[0]), start);
          continue;
        }
        i++;
        push(T::Dot, ".", start);
        continue;
      }
      if (c == '$') {
        size_t j = i + 1;
        while (j < src.size() && (ident_char(src[j]) || (src[j] == ':' && j + 1 < src.size() && src[j + 1] == ':'))) {
          if (src[j] == ':') { j++; }
          j++;
        }
        if (j == i + 1) { fail("expected a variable name after '$'"); }
        push(T::Var, src.substr(i + 1, j - i - 1), start);
        i = j;
        continue;
      }
      if (c == '@') {
        size_t j = i + 1;
        while (j < src.size() && ident_char(src[j])) { j++; }
        push(T::Format, src.substr(i + 1, j - i - 1), start);
        i = j;
        continue;
      }
      if (isdigit((unsigned char)c)) {
        // Digits, an optional fraction and an optional exponent (strtod
        // alone would also take hexadecimal numbers, "inf" and "nan").
        size_t j = i;
        while (j < src.size() && isdigit((unsigned char)src[j])) { j++; }
        if (j < src.size() && src[j] == '.') {
          j++;
          while (j < src.size() && isdigit((unsigned char)src[j])) { j++; }
        }
        if (j < src.size() && (src[j] == 'e' || src[j] == 'E')) {
          size_t k = j + 1;
          if (k < src.size() && (src[k] == '+' || src[k] == '-')) { k++; }
          if (k < src.size() && isdigit((unsigned char)src[k])) {
            while (k < src.size() && isdigit((unsigned char)src[k])) { k++; }
            j = k;
          }
        }
        std::string text = src.substr(i, j - i);
        push(T::Num, text, start);
        toks.back().num = strtod(text.c_str(), nullptr);
        i = j;
        continue;
      }
      if (c == '"') {
        i++;
        auto parts = read_string();
        if (parts.size() == 1) {
          push(T::Str, std::move(parts[0]), start);
        } else {
          push(T::Str, "", start);
          toks.back().parts = std::move(parts);
          toks.back().part_pos = last_part_pos;
        }
        continue;
      }
      if (ident_start(c)) {
        size_t j = i;
        while (j < src.size() && (ident_char(src[j]) || (src[j] == ':' && j + 1 < src.size() && src[j + 1] == ':'))) {
          if (src[j] == ':') { j++; }
          j++;
        }
        std::string w = src.substr(i, j - i);
        i = j;
        bool kw = false;
        for (auto k : keywords) { if (w == k) { kw = true; } }
        push(kw ? T::Keyword : T::Ident, w, start);
        continue;
      }
      // jq lexes ?// (destructuring alternatives) as one token.
      if (src.compare(i, 3, "?//") == 0) { push(T::Punct, "?//", start); i += 3; continue; }
      if (src.compare(i, 3, "//=") == 0) { push(T::Punct, "//=", start); i += 3; continue; }
      static const char *two[] = {"==", "!=", "<=", ">=", "//", "|=", "+=", "-=", "*=", "/=", "%="};
      bool matched = false;
      for (auto p : two) {
        if (src.compare(i, 2, p) == 0) { push(T::Punct, p, start); i += 2; matched = true; break; }
      }
      if (matched) { continue; }
      if (strchr("|,()[]{}:;<>+-*/%?=", c)) { push(T::Punct, std::string(1, c), start); i++; continue; }
      if (c == '\'') {
        size_t first = src.find_first_not_of(" \t\r\n");
        size_t last = src.find_last_not_of(" \t\r\n");
        bool wrapped = base_pos == 0 && first == i && last != first && src[last] == '\'';
        fail("unexpected character '\''",
             wrapped ? "the filter seems to be wrapped in single quotes: the Windows Command Prompt (cmd) passes them on; "
                       "use double quotes there (with \\\" inside), or put the filter in a file and use -f FILE"
                     : "strings in filters use double quotes");
      }
      fail("unexpected character '" + utf8_char_at(i) + "'");
    }
  }
};

// The functions of the prelude, by name/arity, and their frames.
using prelude_table = std::unordered_map<std::string, env_ptr>;

struct parser_t {
  std::vector<token> toks{};
  size_t p{0};
  int next_label{0};
  std::vector<std::pair<std::string, int>> labels{};
  // Names in scope: variables ("x") and functions or closures ("f/1").
  std::vector<std::string> scope{};
  std::vector<std::string> globals{};
  const prelude_table *prelude{nullptr};
  compile_info info{};

  [[noreturn]] void fail(const std::string &m, std::string hint = std::string()) {
    throw syntax_error("syntax error: " + m, toks[p].pos, std::move(hint));
  }
  // An error that is not about the syntax (an undefined name, a bad key), at
  // the token at.
  [[noreturn]] void fail_plain(const std::string &m, size_t at, std::string hint = std::string()) {
    throw syntax_error(m, toks[at].pos, std::move(hint));
  }
  const token &peek() const { return toks[p]; }
  bool is_punct(const char *s) const { return toks[p].t == T::Punct && toks[p].s == s; }
  bool is_kw(const char *s) const { return toks[p].t == T::Keyword && toks[p].s == s; }
  void expect_punct(const char *s) { if (!is_punct(s)) { fail(std::string("expected '") + s + "'"); } p++; }
  void expect_kw(const char *s) { if (!is_kw(s)) { fail(std::string("expected '") + s + "'"); } p++; }

  bool in_scope(const std::string &key) const {
    for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
      if (*it == key) { return true; }
    }
    return false;
  }
  void check_var(const std::string &name) {
    if (in_scope(name)) { return; }
    for (const auto &g : globals) { if (g == name) { return; } }
    std::vector<std::string> known(globals.begin(), globals.end());
    for (const auto &k : scope) {
      if (k.find('/') == std::string::npos) { known.push_back(k); }
    }
    known.push_back("ENV");
    known.push_back("__loc__");
    std::string best = closest_name(name, known);
    fail_plain("$" + name + " is not defined", p, best.empty() ? std::string() : "did you mean $" + best + "?");
  }

  // Edits (insertions, deletions, substitutions, swaps of neighbors) from a
  // to b.
  static size_t edit_distance(const std::string &a, const std::string &b) {
    std::vector<std::vector<size_t>> d(a.size() + 1, std::vector<size_t>(b.size() + 1));
    for (size_t i = 0; i <= a.size(); i++) { d[i][0] = i; }
    for (size_t j = 0; j <= b.size(); j++) { d[0][j] = j; }
    for (size_t i = 1; i <= a.size(); i++) {
      for (size_t j = 1; j <= b.size(); j++) {
        d[i][j] = std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) { d[i][j] = std::min(d[i][j], d[i - 2][j - 2] + 1); }
      }
    }
    return d[a.size()][b.size()];
  }

  // The known name closest to name (at most 2 edits, fewer than its length),
  // or "".
  static std::string closest_name(const std::string &name, const std::vector<std::string> &known) {
    std::string best;
    size_t best_d = 3;
    for (const auto &k : known) {
      if (k == name) { continue; }
      size_t d = edit_distance(name, k);
      if (d < best_d && d < name.size()) {
        best_d = d;
        best = k;
      }
    }
    return best;
  }

  // The hint for an undefined function: the arities it has, or a close name.
  std::string function_hint(const std::string &name, size_t arity) const {
    std::vector<std::string> keys = builtin_names();
    for (const auto &k : scope) {
      if (k.find('/') != std::string::npos) { keys.push_back(k); }
    }
    std::vector<std::string> arities, names;
    for (const auto &k : keys) {
      size_t slash = k.rfind('/');
      std::string n = k.substr(0, slash);
      if (n == name) { arities.push_back(k.substr(slash + 1)); }
      if (n.empty() || n[0] == '_') { continue; }
      names.push_back(n);
    }
    if (!arities.empty()) {
      std::sort(arities.begin(), arities.end());
      arities.erase(std::unique(arities.begin(), arities.end()), arities.end());
      std::string list;
      for (size_t k = 0; k < arities.size(); k++) {
        if (k) { list += k + 1 == arities.size() ? " or " : ", "; }
        list += arities[k];
      }
      return name + " takes " + list + " argument" + (arities.size() == 1 && arities[0] == "1" ? "" : "s") +
             ", not " + std::to_string(arity);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::string best = closest_name(name, names);
    return best.empty() ? std::string() : "did you mean " + best + "?";
  }

  // Each nested term recurses; deep nesting would overflow the stack.
  int depth{0};
  static constexpr int MAX_DEPTH = 256;
  struct depth_guard {
    parser_t &p;
    explicit depth_guard(parser_t &parser) : p(parser) {
      if (++p.depth > MAX_DEPTH) { p.fail("filter nested too deeply"); }
    }
    ~depth_guard() { p.depth--; }
    depth_guard(const depth_guard &) = delete;
    depth_guard &operator=(const depth_guard &) = delete;
  };

  // def name: body;   def name(f; $x): body;
  std::unique_ptr<def_node> parse_def() {
    expect_kw("def");
    if (peek().t != T::Ident && peek().t != T::Keyword) { fail("expected a function name after def"); }
    auto d = std::make_unique<func_def>();
    d->name = toks[p++].s;
    if (is_punct("(")) {
      p++;
      while (true) {
        if (peek().t == T::Var) { d->params.push_back(toks[p++].s); d->by_value.push_back(true); }
        else if (peek().t == T::Ident || peek().t == T::Keyword) { d->params.push_back(toks[p++].s); d->by_value.push_back(false); }
        else { fail("expected a parameter"); }
        if (is_punct(";")) { p++; continue; }
        expect_punct(")");
        break;
      }
    }
    expect_punct(":");
    auto n = std::make_unique<def_node>();
    n->key = d->name + "/" + std::to_string(d->params.size());
    // The function is in scope in its own body (recursion) and after it.
    scope.push_back(n->key);
    size_t mark = scope.size();
    for (size_t k = 0; k < d->params.size(); k++) {
      d->keys.push_back(d->params[k] + "/0");
      scope.push_back(d->keys.back());
      if (d->by_value[k]) { scope.push_back(d->params[k]); }
    }
    d->body = parse_pipe();
    scope.resize(mark);
    expect_punct(";");
    n->def = std::move(d);
    return n;
  }

  // pipe: lowest precedence; handles "def" and "label".
  node_ptr parse_pipe() {
    depth_guard guard(*this);
    if (is_kw("def")) {
      // parse_def leaves the function in scope for the rest only.
      size_t mark = scope.size();
      auto n = parse_def();
      n->rest = parse_pipe();
      scope.resize(mark);
      return n;
    }
    if (is_kw("label")) {
      p++;
      if (peek().t != T::Var) { fail("expected $label"); }
      std::string name = toks[p++].s;
      expect_punct("|");
      auto n = std::make_unique<label_node>();
      n->id = next_label++;
      labels.emplace_back(name, n->id);
      n->body = parse_pipe();
      labels.pop_back();
      return n;
    }
    node_ptr lhs = parse_comma();
    if (is_punct("|")) {
      p++;
      auto n = std::make_unique<pipe_node>();
      n->a = std::move(lhs);
      n->b = parse_pipe();
      return n;
    }
    return lhs;
  }

  node_ptr parse_comma() {
    node_ptr lhs = parse_alt_as();
    while (is_punct(",")) {
      p++;
      auto n = std::make_unique<comma_node>();
      n->a = std::move(lhs);
      n->b = parse_alt_as();
      lhs = std::move(n);
    }
    return lhs;
  }

  // e as $x | body: as in jq 1.8, the source is everything that binds more
  // tightly than ',' (1 + 2 as $x is (1 + 2) as $x), and the body extends as
  // far right as possible.
  node_ptr parse_alt_as() {
    node_ptr e = parse_alt();
    if (!is_kw("as")) { return e; }
    p++;
    auto n = std::make_unique<bind_node>();
    n->source = std::move(e);
    size_t mark = scope.size();
    parse_pattern(n->pat);
    // Destructuring alternatives: $a ?// [$a] ?// {a: $a}.
    while (is_punct("?//")) {
      p++;
      n->alts.emplace_back();
      parse_pattern(n->alts.back());
    }
    if (!n->alts.empty()) {
      // Every variable of every alternative is bound (null when absent).
      std::vector<std::string> names(scope.begin() + std::ptrdiff_t(mark), scope.end());
      std::sort(names.begin(), names.end());
      names.erase(std::unique(names.begin(), names.end()), names.end());
      n->all_vars = names;
    }
    expect_punct("|");
    n->body = parse_pipe();
    scope.resize(mark);
    return n;
  }

  node_ptr make_bin(op o, node_ptr a, node_ptr b) {
    auto n = std::make_unique<binop_node>();
    n->o = o;
    n->a = std::move(a);
    n->b = std::move(b);
    return n;
  }

  // As in jq's grammar, "//" binds more loosely than the assignment
  // operators: .a |= . // 1 is (.a |= .) // 1.
  node_ptr parse_alt() {
    node_ptr lhs = parse_assign();
    if (is_punct("//")) {
      p++;
      return make_bin(op::alt, std::move(lhs), parse_alt());  // right-associative
    }
    return lhs;
  }
  node_ptr parse_assign() {
    node_ptr lhs = parse_or();
    static const std::pair<const char *, assign_op> ops[] = {
        {"=", assign_op::set}, {"|=", assign_op::update}, {"+=", assign_op::add}, {"-=", assign_op::sub},
        {"*=", assign_op::mul}, {"/=", assign_op::div}, {"%=", assign_op::mod}, {"//=", assign_op::alt}};
    for (auto &o : ops) {
      if (is_punct(o.first)) {
        p++;
        auto n = std::make_unique<assign_node>();
        n->o = o.second;
        n->lhs = std::move(lhs);
        n->rhs = parse_or();
        return n;
      }
    }
    return lhs;
  }
  node_ptr parse_or() {
    node_ptr lhs = parse_and();
    while (is_kw("or")) { p++; lhs = make_bin(op::or_, std::move(lhs), parse_and()); }
    return lhs;
  }
  node_ptr parse_and() {
    node_ptr lhs = parse_cmp();
    while (is_kw("and")) { p++; lhs = make_bin(op::and_, std::move(lhs), parse_cmp()); }
    return lhs;
  }
  node_ptr parse_cmp() {
    node_ptr lhs = parse_add();
    static const std::pair<const char *, op> ops[] = {{"==", op::eq}, {"!=", op::ne}, {"<", op::lt}, {"<=", op::le}, {">", op::gt}, {">=", op::ge}};
    for (auto &o : ops) {
      if (is_punct(o.first)) { p++; return make_bin(o.second, std::move(lhs), parse_add()); }
    }
    return lhs;
  }
  node_ptr parse_add() {
    node_ptr lhs = parse_mul();
    while (is_punct("+") || is_punct("-")) {
      op o = is_punct("+") ? op::add : op::sub;
      p++;
      lhs = make_bin(o, std::move(lhs), parse_mul());
    }
    return lhs;
  }
  node_ptr parse_mul() {
    node_ptr lhs = parse_unary();
    while (is_punct("*") || is_punct("/") || is_punct("%")) {
      op o = is_punct("*") ? op::mul : (is_punct("/") ? op::div : op::mod);
      p++;
      lhs = make_bin(o, std::move(lhs), parse_unary());
    }
    return lhs;
  }
  node_ptr parse_unary() {
    if (is_punct("-")) {
      p++;
      auto n = std::make_unique<neg_node>();
      n->a = parse_postfix();
      return n;
    }
    return parse_postfix();
  }

  // Parses the inside of "[...]" after a term: "]" (iterate), "e]", "e:e]", ":e]", "e:]".
  node_ptr parse_bracket_suffix(node_ptr target) {
    expect_punct("[");
    if (is_punct("]")) {
      p++;
      auto n = std::make_unique<iterate_node>();
      n->target = std::move(target);
      return n;
    }
    node_ptr from;
    if (!is_punct(":")) { from = parse_pipe(); }
    if (is_punct(":")) {
      p++;
      auto n = std::make_unique<slice_node>();
      n->target = std::move(target);
      n->from = std::move(from);
      if (!is_punct("]")) { n->to = parse_pipe(); }
      expect_punct("]");
      return n;
    }
    expect_punct("]");
    auto n = std::make_unique<index_node>();
    n->target = std::move(target);
    n->index = std::move(from);
    return n;
  }

  static void mark_optional(node *n) {
    if (auto f = dynamic_cast<field_node *>(n)) { f->opt = true; }
    else if (auto x = dynamic_cast<index_node *>(n)) { x->opt = true; }
    else if (auto s = dynamic_cast<slice_node *>(n)) { s->opt = true; }
    else if (auto it = dynamic_cast<iterate_node *>(n)) { it->opt = true; }
  }

  // jq rejects constant object keys that are not strings when it compiles.
  void check_const_key(const node *key, size_t key_pos) {
    auto lit = dynamic_cast<const literal_node *>(key);
    if (lit && lit->v.k != K::Str) {
      throw syntax_error(std::string("Cannot use ") + kind_name(lit->v.k) + " (" + error_text(lit->v) + ") as object key", toks[key_pos].pos);
    }
  }

  // Patterns: $name, [p, ...], {key: p, $name, $name: p, "str": p, (e): p}.
  // The variables they bind are pushed on the scope.
  void parse_pattern(pattern &pat) {
    depth_guard guard(*this);
    if (peek().t == T::Var) {
      pat.k = pattern::kind::var;
      pat.name = toks[p++].s;
      scope.push_back(pat.name);
      return;
    }
    if (is_punct("[")) {
      p++;
      pat.k = pattern::kind::array;
      while (true) {
        pat.elems.emplace_back();
        parse_pattern(pat.elems.back());
        if (is_punct(",")) { p++; continue; }
        expect_punct("]");
        return;
      }
    }
    if (is_punct("{")) {
      p++;
      pat.k = pattern::kind::object;
      while (true) {
        pattern::field f;
        const token &k = peek();
        if (k.t == T::Var) {
          f.var = k.s;
          auto lit = std::make_unique<literal_node>();
          lit->v = value::string(k.s);
          f.key = std::move(lit);
          p++;
          scope.push_back(f.var);
        } else if (k.t == T::Ident || k.t == T::Keyword) {
          auto lit = std::make_unique<literal_node>();
          lit->v = value::string(k.s);
          f.key = std::move(lit);
          p++;
        } else if (k.t == T::Str) {
          f.key = parse_term();
        } else if (is_punct("(")) {
          size_t key_at = p + 1;
          p++;
          f.key = parse_pipe();
          expect_punct(")");
          check_const_key(f.key.get(), key_at);
        } else {
          fail("bad object pattern");
        }
        if (is_punct(":")) {
          p++;
          f.sub = std::make_unique<pattern>();
          parse_pattern(*f.sub);
        } else if (f.var.empty()) {
          fail("object pattern key needs a variable");
        }
        pat.fields.push_back(std::move(f));
        if (is_punct(",")) { p++; continue; }
        expect_punct("}");
        return;
      }
    }
    fail("expected a pattern");
  }

  bool paren_term{false};

  node_ptr parse_postfix() {
    paren_term = false;
    node_ptr t = parse_term();
    bool bare_step = !paren_term;
    while (true) {
      if (peek().t == T::Field) {
        auto n = std::make_unique<field_node>();
        n->target = std::move(t);
        n->name = toks[p++].s;
        t = std::move(n);
        bare_step = true;
      } else if (peek().t == T::Dot && toks[p + 1].t == T::Punct && toks[p + 1].s == "[") {
        p++;
        t = parse_bracket_suffix(std::move(t));
        bare_step = true;
      } else if (peek().t == T::Dot && toks[p + 1].t == T::Str && toks[p + 1].parts.empty()) {
        // ."foo" lexed as two tokens after a term: .a."b"
        p++;
        auto n = std::make_unique<field_node>();
        n->target = std::move(t);
        n->name = toks[p++].s;
        t = std::move(n);
        bare_step = true;
      } else if (is_punct("[")) {
        t = parse_bracket_suffix(std::move(t));
        bare_step = true;
      } else if (is_punct("?")) {
        p++;
        // Postfix "?" on a path step suppresses that step's errors only (as
        // jq's INDEX_OPT); elsewhere it is try.
        bool is_step = dynamic_cast<field_node *>(t.get()) || dynamic_cast<index_node *>(t.get()) || dynamic_cast<iterate_node *>(t.get()) || dynamic_cast<slice_node *>(t.get());
        if (is_step && bare_step) {
          mark_optional(t.get());
        } else {
          auto n = std::make_unique<try_node>();
          n->body = std::move(t);
          t = std::move(n);
        }
      } else {
        return t;
      }
    }
  }

  // A string token, with its interpolations parsed in the current scope.
  node_ptr string_term(const token &tk, const std::string &format) {
    if (tk.parts.empty()) {
      auto n = std::make_unique<literal_node>();
      n->v = value::string(tk.s);
      return n;
    }
    auto n = std::make_unique<string_node>();
    n->format = format;
    for (size_t k = 0; k < tk.parts.size(); k++) {
      if (k % 2 == 0) { n->lits.push_back(tk.parts[k]); continue; }
      lexer lx{};
      lx.src = tk.parts[k];
      lx.base_line = tk.line;
      lx.base_pos = tk.part_pos[k];
      lx.run();
      std::vector<token> saved = std::move(toks);
      size_t saved_p = p;
      toks = std::move(lx.toks);
      p = 0;
      node_ptr e = parse_pipe();
      if (peek().t != T::End) { fail("unexpected token in string interpolation"); }
      toks = std::move(saved);
      p = saved_p;
      n->exprs.push_back(std::move(e));
    }
    return n;
  }

  node_ptr make_call(std::string name, std::vector<node_ptr> args, size_t at) {
    auto n = std::make_unique<call_node>();
    n->name = std::move(name);
    n->args = std::move(args);
    n->key = n->name + "/" + std::to_string(n->args.size());
    static const char *const regex_functions[] = {"test", "match", "capture", "scan", "split", "splits", "sub", "gsub"};
    for (const char *r : regex_functions) {
      if (n->name == r && (n->args.size() >= 2 || (n->name != "split" && !n->args.empty()))) { info.uses_regex = true; }
    }
    if (n->key == "input/0" || n->key == "inputs/0") { info.uses_input = true; }
    if (in_scope(n->key)) {
      n->k = call_node::kind::dynamic;
      return n;
    }
    if (prelude) {
      auto it = prelude->find(n->key);
      if (it != prelude->end()) {
        n->k = call_node::kind::prelude;
        n->target = it->second;
        return n;
      }
    }
    n->fn = find_builtin(n->key);
    n->pfn = find_builtin_paths(n->key);
    if (!n->fn) { fail_plain(n->key + " is not defined", at, function_hint(n->name, n->args.size())); }
    return n;
  }

  node_ptr parse_term() {
    depth_guard guard(*this);
    const token &tk = peek();
    switch (tk.t) {
    case T::Dot: {
      p++;
      if (is_punct("[")) { return parse_bracket_suffix(nullptr); }
      if (peek().t == T::Str && peek().parts.empty()) {  // ."foo" when lexed separately
        auto n = std::make_unique<field_node>();
        n->name = toks[p++].s;
        return n;
      }
      return std::make_unique<identity_node>();
    }
    case T::DotDot: p++; return std::make_unique<recurse_node>();
    case T::Field: {
      auto n = std::make_unique<field_node>();
      n->name = toks[p++].s;
      return n;
    }
    case T::Num: {
      auto n = std::make_unique<literal_node>();
      n->v = value::number(tk.num);
      n->text = tk.s;
      p++;
      return n;
    }
    case T::Str: {
      const token &s = toks[p++];
      return string_term(s, "text");
    }
    case T::Var: {
      if (tk.s == "__loc__") {
        auto n = std::make_unique<literal_node>();
        n->v = value::object({{"file", value::string("<top-level>")}, {"line", value::number(tk.line)}});
        p++;
        return n;
      }
      if (tk.s == "ENV" && !in_scope("ENV")) {
        p++;
        return make_call("env", {}, p - 1);
      }
      check_var(tk.s);
      auto n = std::make_unique<var_node>();
      n->name = tk.s;
      p++;
      return n;
    }
    case T::Format: {
      std::string fmt = tk.s;
      p++;
      if (peek().t == T::Str) {
        const token &s = toks[p++];
        if (s.parts.empty()) {
          auto n = std::make_unique<literal_node>();
          n->v = value::string(s.s);
          return n;
        }
        return string_term(s, fmt);
      }
      auto n = std::make_unique<format_node>();
      n->format = fmt;
      return n;
    }
    case T::Keyword: {
      if (tk.s == "if") {
        p++;
        auto n = std::make_unique<if_node>();
        n->cond = parse_pipe();
        expect_kw("then");
        n->then_ = parse_pipe();
        if_node *cur = n.get();
        while (is_kw("elif")) {
          p++;
          auto m = std::make_unique<if_node>();
          m->cond = parse_pipe();
          expect_kw("then");
          m->then_ = parse_pipe();
          if_node *mp = m.get();
          cur->else_ = std::move(m);
          cur = mp;
        }
        if (is_kw("else")) { p++; cur->else_ = parse_pipe(); }
        expect_kw("end");
        return n;
      }
      if (tk.s == "reduce" || tk.s == "foreach") {
        bool is_reduce = tk.s == "reduce";
        p++;
        // As in jq 1.8, the source is an expression: reduce .[] / .[] as $x.
        node_ptr source = parse_alt();
        expect_kw("as");
        pattern pat;
        size_t mark = scope.size();
        parse_pattern(pat);
        // The pattern's variables are not in scope in the initial value.
        std::vector<std::string> bound(scope.begin() + std::ptrdiff_t(mark), scope.end());
        scope.resize(mark);
        expect_punct("(");
        node_ptr init = parse_pipe();
        expect_punct(";");
        scope.insert(scope.end(), bound.begin(), bound.end());
        node_ptr update = parse_pipe();
        node_ptr extract;
        if (!is_reduce && is_punct(";")) { p++; extract = parse_pipe(); }
        scope.resize(mark);
        expect_punct(")");
        if (is_reduce) {
          auto n = std::make_unique<reduce_node>();
          n->source = std::move(source); n->init = std::move(init); n->update = std::move(update); n->pat = std::move(pat);
          return n;
        }
        auto n = std::make_unique<foreach_node>();
        n->source = std::move(source); n->init = std::move(init); n->update = std::move(update); n->extract = std::move(extract); n->pat = std::move(pat);
        return n;
      }
      if (tk.s == "try") {
        p++;
        auto n = std::make_unique<try_node>();
        n->body = parse_postfix();
        if (is_kw("catch")) { p++; n->handler = parse_postfix(); }
        return n;
      }
      if (tk.s == "def") {
        // A definition inside a term: (def f: 1; f).
        size_t mark = scope.size();
        auto n = parse_def();
        n->rest = parse_pipe();
        scope.resize(mark);
        return n;
      }
      if (tk.s == "import" || tk.s == "include") { fail("modules are not supported"); }
      fail("unexpected keyword " + tk.s);
    }
    case T::Ident: {
      std::string name = tk.s;
      size_t name_at = p;
      p++;
      if (name == "break") {
        if (peek().t != T::Var) { fail("expected $label after break"); }
        std::string l = toks[p++].s;
        for (auto it = labels.rbegin(); it != labels.rend(); ++it) {
          if (it->first == l) {
            struct break_node : node {
              int id{0};
              void eval(const value &, const env_ptr &, emit_ref) const override { throw break_signal{id}; }
              void paths(const value &, path_t &, const env_ptr &, emit_ref) const override { throw break_signal{id}; }
            };
            auto n = std::make_unique<break_node>();
            n->id = it->second;
            return n;
          }
        }
        fail_plain("$*label-" + l + " is not defined", p - 1);
      }
      if ((name == "true" || name == "false" || name == "null") && !is_punct("(")) {
        auto lit = std::make_unique<literal_node>();
        lit->v = name == "null" ? value::null() : value::boolean(name == "true");
        return lit;
      }
      std::vector<node_ptr> args;
      if (is_punct("(")) {
        p++;
        args.push_back(parse_pipe());
        while (is_punct(";")) { p++; args.push_back(parse_pipe()); }
        expect_punct(")");
      }
      return make_call(name, std::move(args), name_at);
    }
    case T::Punct: {
      if (tk.s == "(") {
        p++;
        node_ptr e = parse_pipe();
        expect_punct(")");
        paren_term = true;
        return e;
      }
      if (tk.s == "[") {
        p++;
        auto n = std::make_unique<array_node>();
        if (!is_punct("]")) { n->body = parse_pipe(); }
        expect_punct("]");
        return n;
      }
      if (tk.s == "{") {
        p++;
        auto n = std::make_unique<object_node>();
        while (!is_punct("}")) {
          node_ptr key, val;
          const token &k = peek();
          std::string kname;
          bool simple = false;
          if (k.t == T::Ident || k.t == T::Keyword) { kname = k.s; simple = true; p++; }
          else if (k.t == T::Str && k.parts.empty()) { kname = k.s; simple = true; p++; }
          else if (k.t == T::Str) {
            const token &s = toks[p++];
            key = string_term(s, "text");
          } else if (k.t == T::Var && toks[p + 1].t == T::Punct && toks[p + 1].s == ":") {
            // {$x: v}: the key is the value of $x.
            p++;
            if (k.s == "__loc__") {
              auto lk = std::make_unique<literal_node>();
              lk->v = value::object({{"file", value::string("<top-level>")}, {"line", value::number(k.line)}});
              key = std::move(lk);
            } else {
              check_var(k.s);
              auto vv = std::make_unique<var_node>();
              vv->name = k.s;
              key = std::move(vv);
            }
          } else if (k.t == T::Var) {
            p++;
            auto lk = std::make_unique<literal_node>();
            if (k.s == "__loc__") {
              lk->v = value::string("__loc__");
              auto lv = std::make_unique<literal_node>();
              lv->v = value::object({{"file", value::string("<top-level>")}, {"line", value::number(k.line)}});
              val = std::move(lv);
            } else {
              check_var(k.s);
              lk->v = value::string(k.s);
              auto vv = std::make_unique<var_node>();
              vv->name = k.s;
              val = std::move(vv);
            }
            key = std::move(lk);
          } else if (k.t == T::Format && toks[p + 1].t == T::Str) {
            std::string fmt = k.s;
            p++;
            const token &s = toks[p++];
            key = string_term(s, fmt);
          } else if (k.t == T::Num) {
            fail("object keys must be strings");
          } else if (is_punct("(")) {
            size_t key_at = p + 1;
            p++;
            key = parse_pipe();
            expect_punct(")");
            check_const_key(key.get(), key_at);
          } else {
            fail("bad object key");
          }
          if (simple) {
            auto lk = std::make_unique<literal_node>();
            lk->v = value::string(kname);
            key = std::move(lk);
          }
          if (is_punct(":")) {
            p++;
            val = parse_obj_value();
          } else if (!val) {
            if (simple) {
              auto f = std::make_unique<field_node>();
              f->name = kname;
              val = std::move(f);
            } else if (!dynamic_cast<string_node *>(key.get())) {
              fail("object key needs a value");
            }
          }
          n->entries.emplace_back(std::move(key), std::move(val));
          if (is_punct(",")) { p++; continue; }
          if (!is_punct("}")) { fail("expected ',' or '}' in object"); }
        }
        p++;
        return n;
      }
      if (tk.s == "-") {
        // Unary minus on a term, inside a larger expression: 1 - -1.
        return parse_unary();
      }
      fail("unexpected '" + tk.s + "'");
    }
    default: fail("unexpected end of filter");
    }
  }

  // An object value is an expression without top-level ',' (jq: ExpD).
  node_ptr parse_obj_value() {
    node_ptr lhs = parse_alt();
    while (is_punct("|")) {
      p++;
      auto n = std::make_unique<pipe_node>();
      n->a = std::move(lhs);
      n->b = parse_alt();
      lhs = std::move(n);
    }
    return lhs;
  }
};

extern const char *const PRELUDE;

// The prelude: builtins written in jq, parsed once. Each top-level definition
// gets a frame whose parent is the previous one, so that the bodies see the
// definitions before them (and themselves).
static const prelude_table &prelude_functions() {
  static prelude_table table;
  static std::vector<std::unique_ptr<def_node>> defs;
  static std::once_flag once;
  std::call_once(once, [] {
    lexer lx{};
    lx.src = PRELUDE;
    lx.run();
    parser_t ps{};
    ps.toks = std::move(lx.toks);
    ps.prelude = &table;
    env_ptr last{};
    while (ps.peek().t != T::End) {
      auto d = ps.parse_def();
      ps.scope.clear();
      env_ptr f = d->frame(last);
      table[d->key] = f;
      last = f;
      defs.push_back(std::move(d));
    }
  });
  return table;
}

std::string describe_syntax_error(const std::string &src, const syntax_error &err) {
  size_t pos = std::min(err.pos, src.size());
  size_t line_start = src.rfind('\n', pos == 0 ? 0 : pos - 1);
  line_start = (line_start == std::string::npos || (pos == 0 && src[0] != '\n')) ? 0 : line_start + 1;
  if (pos < line_start) { line_start = 0; }
  size_t line_end = src.find('\n', pos);
  if (line_end == std::string::npos) { line_end = src.size(); }
  size_t line = 1 + size_t(std::count(src.begin(), src.begin() + std::ptrdiff_t(line_start), '\n'));
  // Columns count characters, not bytes.
  auto chars = [&](size_t a, size_t b) {
    size_t n = 0;
    for (size_t k = a; k < b; k++) { n += ((unsigned char)src[k] & 0xC0) != 0x80; }
    return n;
  };
  size_t column = chars(line_start, pos) + 1;
  std::string text = src.substr(line_start, line_end - line_start);
  for (char &c : text) {
    if (c == '\t' || c == '\r') { c = ' '; }
  }
  std::string out = std::string(err.what()) + " at line " + std::to_string(line) + ", column " + std::to_string(column) + ":\n";
  out += "    " + text + "\n";
  out += "    " + std::string(column - 1, ' ') + "^\n";
  if (!err.hint.empty()) { out += "  " + err.hint + "\n"; }
  return out;
}

std::vector<std::string> prelude_names() {
  std::vector<std::string> r;
  for (const auto &kv : prelude_functions()) { r.push_back(kv.first); }
  return r;
}

node_ptr compile(const std::string &src, const std::vector<std::string> &globals, compile_info *info) {
  lexer lx{};
  lx.src = src;
  lx.run();
  parser_t ps{};
  ps.toks = std::move(lx.toks);
  ps.globals = globals;
  ps.prelude = &prelude_functions();
  node_ptr n = ps.parse_pipe();
  if (ps.peek().t != T::End) { ps.fail("unexpected token"); }
  if (info) { *info = ps.info; }
  return n;
}

} // namespace sjq
