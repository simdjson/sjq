#ifndef SJQ_AST_H
#define SJQ_AST_H

#include "value.h"
#include "writer.h"

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace sjq {

#if defined(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
// Fuzzing builds bound the work of generators (range, recurse) so that a
// filter such as range(1e18) does not run for ever.
inline thread_local long long g_fuzz_budget = 0;
inline void fuzz_tick() {
  if (--g_fuzz_budget < 0) { throw jq_error("fuzzing budget exhausted"); }
}
#else
inline void fuzz_tick() {}
#endif

struct node;
struct func_def;

// The environment: a persistent linked list of variables ("x" for $x),
// closures (the filter arguments of a function, "f/0") and functions
// ("f/2"). The parser resolves every name, so a lookup cannot fail.
enum class bind_kind : uint8_t { var, closure, func };
struct env {
  std::string name{};
  value v{};
  bind_kind kind{bind_kind::var};
  const node *body{};                  // closure: the argument
  const func_def *def{};               // func
  std::shared_ptr<const env> cenv{};   // closure: the environment of the caller
  std::shared_ptr<const env> next{};
};
using env_ptr = std::shared_ptr<const env>;

inline env_ptr push_var(const std::string &name, const value &v, const env_ptr &next) {
  auto n = std::make_shared<env>();
  n->name = name;
  n->v = v;
  n->next = next;
  return n;
}

inline const value *lookup(const env_ptr &e, const std::string &name) {
  for (const env *p = e.get(); p; p = p->next.get()) {
    if (p->kind == bind_kind::var && p->name == name) { return &p->v; }
  }
  return nullptr;
}

// The frame of a function or closure, as the shared pointer that holds it.
inline const env_ptr *lookup_fn(const env_ptr &e, const std::string &key) {
  for (const env_ptr *p = &e; *p; p = &(*p)->next) {
    if ((*p)->kind != bind_kind::var && (*p)->name == key) { return p; }
  }
  return nullptr;
}

using emit_fn = std::function<void(const value &)>;

// Lightweight non-owning callable reference to avoid std::function allocations
// on the hot paths.
class emit_ref {
  void *obj{};
  void (*call)(void *, const value &);
public:
  template <class F>
  emit_ref(F &&f) : obj((void *)&f), call([](void *o, const value &v) { (*(std::remove_reference_t<F> *)o)(v); }) {}
  void operator()(const value &v) const { call(obj, v); }
};

// A path: the keys and indexes from the input to a value. Slices appear as
// {"start": a, "end": b} objects, as in jq.
using path_t = std::vector<value>;

[[noreturn]] void invalid_path(const value &v);

struct node {
  virtual ~node() = default;
  virtual void eval(const value &in, const env_ptr &e, emit_ref out) const = 0;
  // Path mode (path(f), assignments, del): calls out(v) for each output v of
  // the filter while p holds the path of v, extended from the path of in.
  // Every node restores p before it returns or throws.
  virtual void paths(const value &in, path_t &, const env_ptr &e, emit_ref) const {
    eval(in, e, [&](const value &v) { invalid_path(v); });
  }
};
using node_ptr = std::unique_ptr<node>;

// Keeps p at its current length when the scope ends, even through an
// exception.
struct path_restore {
  path_t &p;
  size_t n;
  explicit path_restore(path_t &path) : p(path), n(path.size()) {}
  ~path_restore() { p.resize(n); }
  path_restore(const path_restore &) = delete;
  path_restore &operator=(const path_restore &) = delete;
};

// Thrown to stop a generator early (first, limit, ...); id tells which
// consumer threw it, so that an inner one does not catch it.
struct stop_signal { const void *id; };

struct identity_node : node {
  void eval(const value &in, const env_ptr &, emit_ref out) const override { out(in); }
  void paths(const value &in, path_t &, const env_ptr &, emit_ref out) const override { out(in); }
};

// jq 1.8's error for t[idx].
[[noreturn]] inline void cannot_index(const value &t, const value &idx) {
  throw jq_error(std::string("Cannot index ") + kind_name(t.k) + " with " + kind_name(idx.k) + " (" + error_text(idx) + ")");
}
[[noreturn]] inline void cannot_index(const value &t, const std::string &key) { cannot_index(t, value::string(key)); }

// The bounds [a, b) of the slice from:to of a sequence of n elements (null
// bounds are the ends).
inline void slice_bounds(const value &from, const value &to, int64_t n, int64_t &a, int64_t &b) {
  // As jq: negative bounds count from the end, the start is rounded down and
  // the end up.
  auto clamp = [&](const value &x, int64_t dflt, bool up) {
    if (x.k == K::Null) { return dflt; }
    if (x.k != K::Num) { throw jq_error("Start and end indices of an array slice must be numbers"); }
    double d = get_num(x);
    if (std::isnan(d)) { d = 0; }
    if (d < 0) { d += double(n); }
    if (d < 0) { return int64_t(0); }
    if (d > double(n)) { return n; }
    return int64_t(up ? std::ceil(d) : std::floor(d));
  };
  a = clamp(from, 0, false);
  b = clamp(to, n, true);
  if (b < a) { b = a; }
}

// t[from:to]; throws jq's error unless t is an array, a string or null.
inline value slice_value(const value &t, const value &from, const value &to) {
  if (t.k == K::Null) { return value::null(); }
  if (t.k != K::Arr && t.k != K::Str) { cannot_index(t, value::object({{"start", from}, {"end", to}})); }
  // jq counts strings in code points.
  int64_t n = t.k == K::Arr ? int64_t(container_size(t)) : int64_t(utf8_count(get_str(t)));
  int64_t a = 0, b = 0;
  slice_bounds(from, to, n, a, b);
  if (t.k == K::Str) {
    std::string_view str = get_str(t);
    size_t ba = utf8_offset(str, size_t(a));
    size_t bb = ba + utf8_offset(str.substr(ba), size_t(b - a));
    return value::string(std::string(str.substr(ba, bb - ba)));
  }
  auto v = to_vec(t);
  return value::array(std::vector<value>(v.begin() + a, v.begin() + b));
}

inline void index_value(const value &t, const value &idx, emit_ref out, bool opt) {
  if (t.k == K::Null) {
    if (idx.k == K::Str || idx.k == K::Num || idx.k == K::Null) { out(value::null()); return; }
  }
  if (t.k == K::Obj && idx.k == K::Str) {
    value r;
    if (object_get(t, get_str(idx), r)) { out(r); } else { out(value::null()); }
    return;
  }
  if (t.k == K::Arr && idx.k == K::Num) {
    double d = get_num(idx);
    value r;
    if (!std::isnan(d) && array_get(t, to_int64(std::floor(d)), r)) { out(r); } else { out(value::null()); }
    return;
  }
  if (t.k == K::Arr && idx.k == K::Obj) {
    // .[{"start": a, "end": b}]
    value a, b;
    if (!object_get(idx, "start", a)) { a = value::null(); }
    if (!object_get(idx, "end", b)) { b = value::null(); }
    out(slice_value(t, a, b));
    return;
  }
  if (opt) { return; }
  cannot_index(t, idx);
}

struct field_node : node {
  node_ptr target{};  // may be null: applies to input
  std::string name{};
  bool opt{false};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    auto apply = [&](const value &t) {
      if (t.k == K::Obj) {
        value r;
        if (object_get(t, name, r)) { out(r); } else { out(value::null()); }
      } else if (t.k == K::Null) {
        out(value::null());
      } else if (!opt) {
        cannot_index(t, name);
      }
    };
    if (target) { target->eval(in, e, apply); } else { apply(in); }
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    auto apply = [&](const value &t) {
      if (t.k != K::Obj && t.k != K::Null) {
        if (opt) { return; }
        cannot_index(t, name);
      }
      value r;
      if (t.k == K::Null || !object_get(t, name, r)) { r = value::null(); }
      p.push_back(value::string(name));
      out(r);
      p.pop_back();
    };
    if (target) { target->paths(in, p, e, apply); } else { apply(in); }
  }
};

struct index_node : node {
  node_ptr target{};
  node_ptr index{};
  bool opt{false};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    auto apply = [&](const value &t) {
      index->eval(in, e, [&](const value &idx) { index_value(t, idx, out, opt); });
    };
    if (target) { target->eval(in, e, apply); } else { apply(in); }
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    auto apply = [&](const value &t) {
      index->eval(in, e, [&](const value &idx) {
        if (idx.k == K::Obj) {
          // .[{"start": a, "end": b}]: a slice written as a path component.
          index_value(t, idx, out, opt);
          return;
        }
        index_value(t, idx, [&](const value &r) {
          p.push_back(idx);
          out(r);
          p.pop_back();
        }, opt);
      });
    };
    if (target) { target->paths(in, p, e, apply); } else { apply(in); }
  }
};

struct slice_node : node {
  node_ptr target{}, from{}, to{};  // from/to may be null
  bool opt{false};
  template <class F> void bounds(const value &in, const env_ptr &e, F &&f) const {
    auto with_to = [&](const value &a) {
      if (to) { to->eval(in, e, [&](const value &b) { f(a, b); }); } else { f(a, value::null()); }
    };
    if (from) { from->eval(in, e, with_to); } else { with_to(value::null()); }
  }
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    auto apply = [&](const value &t) {
      bounds(in, e, [&](const value &a, const value &b) {
        if (opt && t.k != K::Null && t.k != K::Arr && t.k != K::Str) { return; }
        out(slice_value(t, a, b));
      });
    };
    if (target) { target->eval(in, e, apply); } else { apply(in); }
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    auto apply = [&](const value &t) {
      bounds(in, e, [&](const value &a, const value &b) {
        if (opt && t.k != K::Null && t.k != K::Arr && t.k != K::Str) { return; }
        value r = slice_value(t, a, b);
        p.push_back(value::object({{"start", a}, {"end", b}}));
        out(r);
        p.pop_back();
      });
    };
    if (target) { target->paths(in, p, e, apply); } else { apply(in); }
  }
};

struct iterate_node : node {
  node_ptr target{};
  bool opt{false};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    auto apply = [&](const value &t) {
      if (t.k == K::Arr) {
        for_each_elem(t, [&](const value &x) { out(x); });
      } else if (t.k == K::Obj) {
        for_each_field(t, [&](std::string_view, const value &x) { out(x); });
      } else if (!opt) {
        cannot_iterate(t);
      }
    };
    if (target) { target->eval(in, e, apply); } else { apply(in); }
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    auto apply = [&](const value &t) {
      if (t.k == K::Arr) {
        double i = 0;
        for_each_elem(t, [&](const value &x) {
          p.push_back(value::number(i++));
          out(x);
          p.pop_back();
        });
      } else if (t.k == K::Obj) {
        for_each_field(t, [&](std::string_view k, const value &x) {
          p.push_back(value::string(std::string(k)));
          out(x);
          p.pop_back();
        });
      } else if (!opt) {
        cannot_iterate(t);
      }
    };
    if (target) { target->paths(in, p, e, apply); } else { apply(in); }
  }
};

// The value a catch handler receives.
inline value error_value(const jq_error &err) { return err.has_payload ? err.payload : value::string(err.what()); }

struct try_node : node {
  node_ptr body{}, handler{};  // handler may be null
  // Only errors raised by the body are caught, not those raised downstream
  // (by the consumer of its outputs): those pass through, as in jq 1.7+.
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    int downstream = 0;
    try {
      body->eval(in, e, [&](const value &v) {
        downstream++;
        out(v);
        downstream--;
      });
    } catch (jq_error &err) {
      if (downstream) { throw; }
      if (handler) { handler->eval(error_value(err), e, out); }
    }
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    int downstream = 0;
    size_t n = p.size();
    try {
      body->paths(in, p, e, [&](const value &v) {
        downstream++;
        out(v);
        downstream--;
      });
    } catch (jq_error &err) {
      if (downstream) { throw; }
      p.resize(n);
      if (handler) { handler->paths(error_value(err), p, e, out); }
    }
  }
};

struct recurse_node : node {
  static void rec(const value &v, emit_ref out) {
    check_stack();
    out(v);
    if (v.k == K::Arr) { for_each_elem(v, [&](const value &x) { rec(x, out); }); }
    else if (v.k == K::Obj) { for_each_field(v, [&](std::string_view, const value &x) { rec(x, out); }); }
  }
  static void rec_paths(const value &v, path_t &p, emit_ref out) {
    check_stack();
    out(v);
    if (v.k == K::Arr) {
      double i = 0;
      for_each_elem(v, [&](const value &x) {
        p.push_back(value::number(i++));
        rec_paths(x, p, out);
        p.pop_back();
      });
    } else if (v.k == K::Obj) {
      for_each_field(v, [&](std::string_view k, const value &x) {
        p.push_back(value::string(std::string(k)));
        rec_paths(x, p, out);
        p.pop_back();
      });
    }
  }
  void eval(const value &in, const env_ptr &, emit_ref out) const override { rec(in, out); }
  void paths(const value &in, path_t &p, const env_ptr &, emit_ref out) const override { rec_paths(in, p, out); }
};

struct literal_node : node {
  value v{};
  std::string text{};  // source text of a number literal
  void eval(const value &, const env_ptr &, emit_ref out) const override { out(v); }
  void paths(const value &in, path_t &, const env_ptr &, emit_ref out) const override {
    // As in jq, null is a path expression on null.
    if (v.k == K::Null && in.k == K::Null) { out(in); return; }
    invalid_path(v);
  }
};

struct var_node : node {
  std::string name{};
  void eval(const value &, const env_ptr &e, emit_ref out) const override {
    const value *v = lookup(e, name);
    if (!v) { throw jq_error("$" + name + " is not defined"); }
    out(*v);
  }
};

struct pipe_node : node {
  node_ptr a{}, b{};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    check_stack();
    a->eval(in, e, [&](const value &x) { b->eval(x, e, out); });
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    check_stack();
    a->paths(in, p, e, [&](const value &x) { b->paths(x, p, e, out); });
  }
};

struct comma_node : node {
  node_ptr a{}, b{};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    a->eval(in, e, out);
    b->eval(in, e, out);
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    a->paths(in, p, e, out);
    b->paths(in, p, e, out);
  }
};

// A destructuring pattern: $name, [p1, p2, ...] or {key: p, $name, ...}.
struct pattern {
  enum class kind : uint8_t { var, array, object } k{kind::var};
  std::string name{};                 // var
  std::vector<pattern> elems{};       // array
  struct field {
    node_ptr key{};                   // evaluated with the input of "as"
    std::string var{};                // {$name} or {$name: p}: also binds $name
    std::unique_ptr<pattern> sub{};   // may be null for {$name}
  };
  std::vector<field> fields{};        // object
};

// Binds the variables of pat to the parts of v, then calls k with the new
// environment (once per binding: key expressions can be generators).
using bind_cont = std::function<void(const env_ptr &)>;
void bind_pattern(const pattern &pat, const value &v, const value &in, const env_ptr &e, const bind_cont &k);

struct bind_node : node {  // source as pattern ?// pattern ... | body
  node_ptr source{}, body{};
  pattern pat{};
  std::vector<pattern> alts{};          // ?// alternatives
  std::vector<std::string> all_vars{};  // with alternatives: every variable
  template <class F> void each(const value &in, const env_ptr &e, F &&f) const {
    source->eval(in, e, [&](const value &x) {
      if (alts.empty()) {
        int none = 0;
        if (pat.k == pattern::kind::var) { f(push_var(pat.name, x, e), none); return; }
        bind_pattern(pat, x, in, e, [&](const env_ptr &ne) { f(ne, none); });
        return;
      }
      // Each alternative in turn, until one runs without an error (in the
      // binding or in the body, not downstream); the error of the last one
      // propagates.
      env_ptr base = e;
      for (const auto &name : all_vars) { base = push_var(name, value::null(), base); }
      for (size_t i = 0; i <= alts.size(); i++) {
        const pattern &pt = i == 0 ? pat : alts[i - 1];
        int downstream = 0;
        try {
          bind_pattern(pt, x, in, base, [&](const env_ptr &ne) { f(ne, downstream); });
          return;
        } catch (jq_error &) {
          if (downstream || i == alts.size()) { throw; }
        }
      }
    });
  }
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    if (alts.empty()) {
      each(in, e, [&](const env_ptr &ne, int &) { body->eval(in, ne, out); });
      return;
    }
    each(in, e, [&](const env_ptr &ne, int &downstream) {
      body->eval(in, ne, [&](const value &v) {
        downstream++;
        out(v);
        downstream--;
      });
    });
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    if (alts.empty()) {
      each(in, e, [&](const env_ptr &ne, int &) { body->paths(in, p, ne, out); });
      return;
    }
    each(in, e, [&](const env_ptr &ne, int &downstream) {
      body->paths(in, p, ne, [&](const value &v) {
        downstream++;
        out(v);
        downstream--;
      });
    });
  }
};

struct reduce_node : node {
  node_ptr source{}, init{}, update{};
  pattern pat{};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    init->eval(in, e, [&](const value &start) {
      value acc = start;
      bool have = true;
      auto step = [&](const env_ptr &ne) {
        value last;
        bool got = false;
        update->eval(have ? acc : value::null(), ne, [&](const value &r) { last = r; got = true; });
        acc = got ? std::move(last) : value::null();
        have = got;
      };
      source->eval(in, e, [&](const value &x) {
        if (pat.k == pattern::kind::var) { step(push_var(pat.name, x, e)); return; }
        bind_pattern(pat, x, in, e, step);
      });
      if (have) { out(acc); } else { out(value::null()); }
    });
  }
};

struct foreach_node : node {
  node_ptr source{}, init{}, update{}, extract{};
  pattern pat{};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    init->eval(in, e, [&](const value &start) {
      value acc = start;
      auto step = [&](const env_ptr &ne) {
        update->eval(acc, ne, [&](const value &r) {
          acc = r;
          if (extract) { extract->eval(r, ne, out); } else { out(r); }
        });
      };
      source->eval(in, e, [&](const value &x) {
        if (pat.k == pattern::kind::var) { step(push_var(pat.name, x, e)); return; }
        bind_pattern(pat, x, in, e, step);
      });
    });
  }
};

struct if_node : node {
  node_ptr cond{}, then_{}, else_{};  // else_ may be null (identity)
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    cond->eval(in, e, [&](const value &c) {
      if (truthy(c)) { then_->eval(in, e, out); }
      else if (else_) { else_->eval(in, e, out); }
      else { out(in); }
    });
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    cond->eval(in, e, [&](const value &c) {
      if (truthy(c)) { then_->paths(in, p, e, out); }
      else if (else_) { else_->paths(in, p, e, out); }
      else { out(in); }
    });
  }
};

struct label_node : node {
  node_ptr body{};
  int id{};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    try { body->eval(in, e, out); } catch (break_signal &b) { if (b.label != id) { throw; } }
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    size_t n = p.size();
    try { body->paths(in, p, e, out); } catch (break_signal &b) { if (b.label != id) { throw; } p.resize(n); }
  }
};

struct array_node : node {
  node_ptr body{};  // may be null
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    std::vector<value> r;
    if (body) { body->eval(in, e, [&](const value &x) { r.push_back(x); }); }
    out(value::array(std::move(r)));
  }
};

struct object_node : node {
  std::vector<std::pair<node_ptr, node_ptr>> entries{};
  void build(size_t i, std::vector<std::pair<std::string, value>> &acc, const value &in, const env_ptr &e, emit_ref out) const {
    if (i == entries.size()) {
      out(value::object(acc));
      return;
    }
    entries[i].first->eval(in, e, [&](const value &k) {
      if (k.k != K::Str) { throw jq_error(std::string("Cannot use ") + kind_name(k.k) + " (" + error_text(k) + ") as object key"); }
      // {"\(e)"}: no value node; the value is the field of that name.
      auto values = [&](const auto &f) {
        if (entries[i].second) { entries[i].second->eval(in, e, f); return; }
        index_value(in, k, f, false);
      };
      values([&](const value &v) {
        std::string key(get_str(k));
        size_t saved = acc.size();
        auto it = std::find_if(acc.begin(), acc.end(), [&](const auto &kv) { return kv.first == key; });
        value prev;
        bool replaced = false;
        if (it != acc.end()) { prev = it->second; it->second = v; replaced = true; }
        else { acc.emplace_back(std::move(key), v); }
        build(i + 1, acc, in, e, out);
        if (replaced) { it->second = prev; } else { acc.resize(saved); }
      });
    });
  }
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    std::vector<std::pair<std::string, value>> acc;
    build(0, acc, in, e, out);
  }
};

enum class op { add, sub, mul, div, mod, eq, ne, lt, le, gt, ge, and_, or_, alt };

value merge_objects(const value &a, const value &b, bool deep);
value arith(op o, const value &a, const value &b);

struct binop_node : node {
  op o{};
  node_ptr a{}, b{};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    check_stack();
    if (o == op::and_ || o == op::or_) {
      a->eval(in, e, [&](const value &x) {
        bool tx = truthy(x);
        if (o == op::and_ && !tx) { out(value::boolean(false)); return; }
        if (o == op::or_ && tx) { out(value::boolean(true)); return; }
        b->eval(in, e, [&](const value &y) { out(value::boolean(truthy(y))); });
      });
      return;
    }
    if (o == op::alt) {
      // As in jq 1.8, an error on the left propagates.
      bool any = false;
      a->eval(in, e, [&](const value &x) { if (truthy(x)) { any = true; out(x); } });
      if (!any) { b->eval(in, e, out); }
      return;
    }
    // jq evaluates the right operand in the outer loop.
    b->eval(in, e, [&](const value &y) {
      a->eval(in, e, [&](const value &x) { out(arith(o, x, y)); });
    });
  }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override {
    if (o != op::alt) { node::paths(in, p, e, out); return; }
    bool any = false;
    a->paths(in, p, e, [&](const value &x) { if (truthy(x)) { any = true; out(x); } });
    if (!any) { b->paths(in, p, e, out); }
  }
};

struct neg_node : node {
  node_ptr a{};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override {
    a->eval(in, e, [&](const value &x) {
      if (x.k != K::Num) { throw jq_error(std::string(kind_name(x.k)) + " (" + error_text(x) + ") cannot be negated"); }
      out(value::number(0 - get_num(x)));  // as jq 1.8: -(0) is 0
    });
  }
};

struct call_node;
using builtin_fn = void (*)(const call_node &c, const value &in, const env_ptr &e, emit_ref out);
using builtin_path_fn = void (*)(const call_node &c, const value &in, path_t &p, const env_ptr &e, emit_ref out);

// The functions written in C++, by "name/arity"; null when there is none.
builtin_fn find_builtin(const std::string &key);
builtin_path_fn find_builtin_paths(const std::string &key);
// The names of all functions, as builtins lists them.
std::vector<std::string> builtin_names();

// def name(params): body;
struct func_def {
  std::string name{};
  std::vector<std::string> params{};  // without the '$'
  std::vector<bool> by_value{};       // $param
  std::vector<std::string> keys{};    // the closures: "param/0"
  node_ptr body{};
};

// def ...; rest
struct def_node : node {
  std::unique_ptr<func_def> def{};
  std::string key{};  // name/arity
  node_ptr rest{};
  env_ptr frame(const env_ptr &e) const {
    auto f = std::make_shared<env>();
    f->name = key;
    f->kind = bind_kind::func;
    f->def = def.get();
    f->next = e;
    return f;
  }
  void eval(const value &in, const env_ptr &e, emit_ref out) const override { rest->eval(in, frame(e), out); }
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override { rest->paths(in, p, frame(e), out); }
};

// A call: of a builtin written in C++, of a function defined in the filter
// or of a closure (looked up in the environment), or of a function of the
// prelude (builtins written in jq, whose frames never change).
struct call_node : node {
  enum class kind : uint8_t { builtin, dynamic, prelude };
  std::string name{};
  std::vector<node_ptr> args{};
  kind k{kind::builtin};
  builtin_fn fn{};
  builtin_path_fn pfn{};
  std::string key{};  // name/arity
  env_ptr target{};   // prelude
  bool is_builtin(const char *n, size_t arity) const { return k == kind::builtin && args.size() == arity && name == n; }
  void eval(const value &in, const env_ptr &e, emit_ref out) const override;
  void paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const override;
};

// "...\(e)..." and @format "...": the outputs of the expressions, formatted,
// between the literal parts. The last expression varies slowest, as in jq.
struct string_node : node {
  std::vector<std::string> lits{};  // exprs.size() + 1 parts
  std::vector<node_ptr> exprs{};
  std::string format{"text"};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override;
};

// @format applied to the input.
struct format_node : node {
  std::string format{};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override;
};

// lhs = rhs, lhs |= rhs, lhs += rhs, ..., lhs //= rhs.
enum class assign_op : uint8_t { set, update, add, sub, mul, div, mod, alt };
struct assign_node : node {
  assign_op o{};
  node_ptr lhs{}, rhs{};
  void eval(const value &in, const env_ptr &e, emit_ref out) const override;
};

// Formats a value as @name does ("text", "json", "csv", "tsv", "html", "uri",
// "sh", "base64", "base64d"); throws jq's error for an unknown format.
std::string apply_format(const std::string &name, const value &v);

// Path helpers (as getpath, setpath and delpaths).
value get_path(const value &t, const path_t &p, size_t from = 0);
// Sets t at p[from..] to v, in place: containers referenced elsewhere are
// copied first.
void set_path(value &t, const path_t &p, size_t from, const value &v);
value delete_paths(const value &t, std::vector<path_t> ps);

// What the driver needs to know about a filter.
struct compile_info {
  bool uses_input{false};  // calls input or inputs
  bool uses_regex{false};  // calls a regular expression builtin
};

// Parses a filter; throws std::runtime_error on a syntax error. globals are
// the names of the variables that the caller defines ($name for --arg name).
node_ptr compile(const std::string &src, const std::vector<std::string> &globals = {}, compile_info *info = nullptr);

} // namespace sjq

#endif // SJQ_AST_H
