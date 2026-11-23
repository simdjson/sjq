#ifndef SJQ_BUILTINS_H
#define SJQ_BUILTINS_H

// Shared by the files that define builtins (builtins.cpp, text.cpp).
#include "ast.h"

#include <functional>
#include <string>
#include <vector>

namespace sjq {

// A builtin written in C++: "name/arity", its evaluation and, for those that
// are path expressions (select, recurse, first, getpath, ...), its path mode.
struct builtin_entry {
  const char *key;
  builtin_fn fn;
  builtin_path_fn paths;
};

// The builtins of text.cpp: formats, regular expressions, dates.
const std::vector<builtin_entry> &text_builtins();

// The names (name/arity) of the functions of the prelude.
std::vector<std::string> prelude_names();

void require_kind(const value &v, K k, const char *what);
value tostring_of(const value &v);
value indices_of(const value &in, const value &p);
void append_utf8(std::string &r, uint32_t cp);
// A deep copy of a DOM element as owned values.
value to_owned(dom::element el);

// The source of input and inputs; empty when there is none (sjq installs
// one when the filter reads inputs).
extern std::function<bool(value &)> g_next_input;
// The name of the file being read, for input_filename (null for stdin).
extern const char *g_input_filename;

} // namespace sjq

#endif // SJQ_BUILTINS_H
