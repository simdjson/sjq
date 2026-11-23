#include "ast.h"
#include "builtins.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>

#if defined(_WIN32)
#include <stdlib.h>
#else
extern char **environ;
#endif

namespace sjq {

// As jq 1.8: longer than 28 bytes, the text is cut to at most 25 bytes (at a
// character boundary), followed by "..." and its last character (the
// closing quote or bracket).
std::string error_text(const value &v) {
  std::string s = to_json(v);
  if (s.size() > 28) {
    char last = s.back();
    size_t cut = 25;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) { cut--; }
    s.resize(cut);
    s += "...";
    if (last == '"' || last == ']' || last == '}') { s.push_back(last); }
  }
  return s;
}

void invalid_path(const value &v) {
  throw jq_error("Invalid path expression with result " + error_text(v));
}

value arith(op o, const value &a, const value &b) {
  switch (o) {
  case op::add:
    if (a.k == K::Null) { return b; }
    if (b.k == K::Null) { return a; }
    if (a.k == K::Num && b.k == K::Num) { return value::number(get_num(a) + get_num(b)); }
    if (a.k == K::Str && b.k == K::Str) {
      std::string s(get_str(a));
      s.append(get_str(b));
      return value::string(std::move(s));
    }
    if (a.k == K::Arr && b.k == K::Arr) {
      auto v = to_vec(a);
      for_each_elem(b, [&](const value &x) { v.push_back(x); });
      return value::array(std::move(v));
    }
    if (a.k == K::Obj && b.k == K::Obj) { return merge_objects(a, b, false); }
    break;
  case op::sub:
    if (a.k == K::Num && b.k == K::Num) { return value::number(get_num(a) - get_num(b)); }
    if (a.k == K::Arr && b.k == K::Arr) {
      auto bv = to_vec(b);
      std::vector<value> r;
      for_each_elem(a, [&](const value &x) {
        for (auto &y : bv) { if (compare(x, y) == 0) { return; } }
        r.push_back(x);
      });
      return value::array(std::move(r));
    }
    break;
  case op::mul:
    if (a.k == K::Num && b.k == K::Num) { return value::number(get_num(a) * get_num(b)); }
    if (a.k == K::Obj && b.k == K::Obj) { return merge_objects(a, b, true); }
    if ((a.k == K::Str && b.k == K::Num) || (a.k == K::Num && b.k == K::Str)) {
      const value &s = a.k == K::Str ? a : b;
      double n = get_num(a.k == K::Num ? a : b);
      // As jq 1.8: negative counts give null, others are truncated.
      if (std::isnan(n) || n < 0) { return value::null(); }
      std::string_view piece = get_str(s);
      int64_t times = to_int64(n);
      if (piece.empty() || times == 0) { return value::string(std::string()); }
      // jq keeps string lengths within an int.
      if (!piece.empty() && times > int64_t(INT32_MAX) / int64_t(piece.size())) { throw jq_error("Repeat string result too long"); }
      std::string r;
      r.reserve(piece.size() * size_t(times));
      for (int64_t i = 0; i < times; i++) {
        fuzz_tick();
        r.append(get_str(s));
      }
      return value::string(std::move(r));
    }
    break;
  case op::div:
    if (a.k == K::Num && b.k == K::Num) {
      if (get_num(b) == 0) {
        throw jq_error(std::string(kind_name(a.k)) + " (" + error_text(a) + ") and " + kind_name(b.k) + " (" + error_text(b) + ") cannot be divided because the divisor is zero");
      }
      return value::number(get_num(a) / get_num(b));
    }
    if (a.k == K::Str && b.k == K::Str) {
      std::vector<value> parts;
      std::string_view s = get_str(a), sep = get_str(b);
      if (s.empty()) { return value::array({}); }
      if (sep.empty()) {
        for (size_t i = 0; i < s.size();) {
          size_t len = utf8_offset(s.substr(i), 1);
          parts.push_back(value::string(std::string(s.substr(i, len))));
          i += len;
        }
      } else {
        size_t pos = 0;
        while (true) {
          size_t f = s.find(sep, pos);
          if (f == std::string_view::npos) { parts.push_back(value::string(std::string(s.substr(pos)))); break; }
          parts.push_back(value::string(std::string(s.substr(pos, f - pos))));
          pos = f + sep.size();
        }
      }
      return value::array(std::move(parts));
    }
    break;
  case op::mod:
    if (a.k == K::Num && b.k == K::Num) {
      // As jq 1.8: NaN gives NaN, the operands are converted to integers
      // with saturation, and x % -1 is 0 (INT64_MIN % -1 overflows).
      double da = get_num(a), db = get_num(b);
      if (std::isnan(da) || std::isnan(db)) { return value::number(NAN); }
      auto dtoi = [](double d) {
        if (d < -9223372036854775808.0) { return INT64_MIN; }
        if (d >= 9223372036854775808.0) { return INT64_MAX; }
        return int64_t(d);
      };
      int64_t y = dtoi(db);
      if (y == 0) {
        throw jq_error(std::string(kind_name(a.k)) + " (" + error_text(a) + ") and " + kind_name(b.k) + " (" + error_text(b) + ") cannot be divided (remainder) because the divisor is zero");
      }
      return value::number(y == -1 ? 0.0 : double(dtoi(da) % y));
    }
    break;
  case op::eq: return value::boolean(compare(a, b) == 0);
  case op::ne: return value::boolean(compare(a, b) != 0);
  case op::lt: return value::boolean(compare(a, b) < 0);
  case op::le: return value::boolean(compare(a, b) <= 0);
  case op::gt: return value::boolean(compare(a, b) > 0);
  case op::ge: return value::boolean(compare(a, b) >= 0);
  default: break;
  }
  static const char *names[] = {"added", "subtracted", "multiplied", "divided", "divided"};
  std::string msg = std::string(kind_name(a.k)) + " (" + error_text(a) + ") and " + kind_name(b.k) + " (" + error_text(b) + ") cannot be " + names[int(o)];
  throw jq_error(msg);
}

value merge_objects(const value &a, const value &b, bool deep) {
  auto r = to_fields(a);
  for_each_field(b, [&](std::string_view k, const value &v) {
    auto it = std::find_if(r.begin(), r.end(), [&](const auto &kv) { return kv.first == k; });
    if (it == r.end()) { r.emplace_back(std::string(k), v); return; }
    if (deep && it->second.k == K::Obj && v.k == K::Obj) { it->second = merge_objects(it->second, v, true); }
    else { it->second = v; }
  });
  return value::object(std::move(r));
}

// ---------------------------------------------------------------------------
// Paths: getpath, setpath, delpaths.
// ---------------------------------------------------------------------------

static void slice_of(const value &k, value &from, value &to) {
  if (!object_get(k, "start", from)) { from = value::null(); }
  if (!object_get(k, "end", to)) { to = value::null(); }
}

static void check_path_depth(const path_t &p) {
  if (p.size() > 10000) { throw jq_error("Path too deep"); }
}

value get_path(const value &t, const path_t &p, size_t from) {
  check_path_depth(p);
  value cur = t;
  for (size_t i = from; i < p.size(); i++) {
    if (cur.k == K::Null) { return cur; }
    const value &k = p[i];
    if (k.k == K::Obj) {
      value a, b;
      slice_of(k, a, b);
      cur = slice_value(cur, a, b);
      continue;
    }
    value r;
    index_value(cur, k, [&](const value &x) { r = x; }, false);
    cur = r;
  }
  return cur;
}

// Makes t a container that only t references, copying one level when it is
// shared or in the DOM, and returns its storage.
static heap &own(value &t) {
  if (!t.dom && t.h && t.h.use_count() == 1) { return const_cast<heap &>(*t.h); }
  auto h = std::make_shared<heap>();
  uint32_t d = 0;
  if (t.k == K::Arr) {
    h->a = to_vec(t);
    for (const value &x : h->a) { d = std::max(d, child_depth(x)); }
  } else {
    h->o = to_fields(t);
    for (const auto &kv : h->o) { d = std::max(d, child_depth(kv.second)); }
  }
  h->depth = d + 1;
  t.dom = false;
  t.e = dom::element{};
  t.h = std::move(h);
  return const_cast<heap &>(*t.h);
}

static void grew(heap &h, const value &child) {
  h.depth = std::max(h.depth, child_depth(child) + 1);
  check_depth(h.depth);
}

// Iterative: paths can be as deep as values (10,000 levels), more than the
// stack allows with sanitizers when each level is a call.
void set_path(value &t, const path_t &p, size_t from, const value &v) {
  if (from == 0) { check_path_depth(p); }
  check_stack();
  // The containers along the path and their child on the path, to update
  // their recorded depth once the value is in place.
  std::vector<std::pair<heap *, value *>> chain;
  value *cur = &t;
  auto fix_depths = [&]() {
    for (size_t j = chain.size(); j-- > 0;) { grew(*chain[j].first, *chain[j].second); }
  };
  for (size_t i = from; i < p.size(); i++) {
    const value &k = p[i];
    value &c = *cur;
    if (c.k == K::Str && k.k == K::Obj) { throw jq_error("Cannot update string slices"); }
    if (k.k != K::Str && k.k != K::Num && k.k != K::Obj) {
      throw jq_error(std::string("Cannot update field at ") + kind_name(k.k) + " index of " + kind_name(c.k));
    }
    if (c.k == K::Null) {
      if (k.k == K::Str) { c = value::object({}); } else { c = value::array({}); }
    }
    if (k.k == K::Str) {
      if (c.k != K::Obj) { cannot_index(c, k); }
      heap &h = own(c);
      std::string_view key = get_str(k);
      auto it = std::find_if(h.o.begin(), h.o.end(), [&](const auto &kv) { return kv.first == key; });
      if (it == h.o.end()) {
        h.o.emplace_back(std::string(key), value::null());
        it = h.o.end() - 1;
      }
      chain.emplace_back(&h, &it->second);
      cur = &it->second;
      continue;
    }
    if (k.k == K::Num) {
      if (c.k != K::Arr) { cannot_index(c, k); }
      double d = get_num(k);
      if (std::isnan(d)) { throw jq_error("Cannot set array element at NaN index"); }
      heap &h = own(c);
      int64_t idx = to_int64(std::floor(d));
      if (idx < 0) { idx += int64_t(h.a.size()); }
      if (idx < 0) { throw jq_error("Out of bounds negative array index"); }
      if (idx >= (int64_t(1) << 29)) { throw jq_error("Array index too large"); }
      if (size_t(idx) >= h.a.size()) { h.a.resize(size_t(idx) + 1); }
      chain.emplace_back(&h, &h.a[size_t(idx)]);
      cur = &h.a[size_t(idx)];
      continue;
    }
    // A slice: the rest of the path applies to the slice, which then
    // replaces its range.
    if (c.k != K::Arr) { throw jq_error(std::string("Cannot update field at object index of ") + kind_name(c.k)); }
    value a, b;
    slice_of(k, a, b);
    int64_t s = 0, en = 0;
    slice_bounds(a, b, int64_t(container_size(c)), s, en);
    value sub = slice_value(c, a, b);
    set_path(sub, p, i + 1, v);
    if (sub.k != K::Arr) { throw jq_error("A slice of an array can only be assigned another array"); }
    std::vector<value> repl = to_vec(sub);
    heap &h = own(c);
    h.a.erase(h.a.begin() + s, h.a.begin() + en);
    h.a.insert(h.a.begin() + s, repl.begin(), repl.end());
    grew(h, sub);
    fix_depths();
    return;
  }
  *cur = v;
  fix_depths();
}

static void del_path(value &t, const path_t &p, size_t i) {
  check_stack();
  if (t.k == K::Null) { return; }
  const value &k = p[i];
  bool last = i + 1 == p.size();
  if (k.k == K::Str) {
    if (t.k != K::Obj) { throw jq_error(std::string("Cannot delete field at object index of ") + kind_name(t.k)); }
    value child;
    if (!object_get(t, get_str(k), child)) { return; }
    heap &h = own(t);
    std::string_view key = get_str(k);
    auto it = std::find_if(h.o.begin(), h.o.end(), [&](const auto &kv) { return kv.first == key; });
    if (last) { h.o.erase(it); } else { del_path(it->second, p, i + 1); }
    return;
  }
  if (k.k == K::Num) {
    if (t.k != K::Arr) { throw jq_error(std::string("Cannot delete field at index of ") + kind_name(t.k)); }
    int64_t n = int64_t(container_size(t));
    double d = get_num(k);
    if (std::isnan(d)) { return; }
    int64_t idx = to_int64(std::floor(d));
    if (idx < 0) { idx += n; }
    if (idx < 0 || idx >= n) { return; }
    heap &h = own(t);
    if (last) { h.a.erase(h.a.begin() + idx); } else { del_path(h.a[size_t(idx)], p, i + 1); }
    return;
  }
  if (k.k == K::Obj) {
    if (t.k != K::Arr) { throw jq_error(std::string("Cannot delete field at object index of ") + kind_name(t.k)); }
    value a, b;
    slice_of(k, a, b);
    int64_t s = 0, en = 0;
    slice_bounds(a, b, int64_t(container_size(t)), s, en);
    std::vector<value> repl;
    if (!last) {
      value sub = slice_value(t, a, b);
      del_path(sub, p, i + 1);
      repl = to_vec(sub);
    }
    heap &h = own(t);
    h.a.erase(h.a.begin() + s, h.a.begin() + en);
    h.a.insert(h.a.begin() + s, repl.begin(), repl.end());
    return;
  }
  throw jq_error(std::string("Invalid path component (") + kind_name(k.k) + ")");
}

static int compare_paths(const path_t &a, const path_t &b) {
  for (size_t i = 0; i < a.size() && i < b.size(); i++) {
    int c = compare(a[i], b[i]);
    if (c) { return c; }
  }
  return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
}

// Rewrites p as indexes into t: negative array indexes count from the end,
// and a slice at the end of a path becomes its indexes (one path each), so
// that deleting from the last path to the first never moves an element that
// is still to be deleted.
static void normalize_path(const value &t, const path_t &p, std::vector<path_t> &outp) {
  path_t r;
  value cur = t;
  for (size_t i = 0; i < p.size(); i++) {
    value k = p[i];
    bool last = i + 1 == p.size();
    if (cur.k == K::Arr && k.k == K::Num && !std::isnan(get_num(k))) {
      double d = std::floor(get_num(k));
      if (d < 0) { d += double(container_size(cur)); }
      k = value::number(d);
    } else if (cur.k == K::Arr && k.k == K::Obj) {
      value a, b;
      slice_of(k, a, b);
      int64_t s = 0, en = 0;
      slice_bounds(a, b, int64_t(container_size(cur)), s, en);
      if (last) {
        for (int64_t x = s; x < en; x++) {
          path_t q = r;
          q.push_back(value::number(double(x)));
          outp.push_back(std::move(q));
        }
        return;
      }
      k = value::object({{"start", value::number(double(s))}, {"end", value::number(double(en))}});
    }
    r.push_back(k);
    if (!last) {
      try {
        cur = get_path(cur, path_t{p[i]});
      } catch (jq_error &) {
        cur = value::null();
      }
    }
  }
  outp.push_back(std::move(r));
}

value delete_paths(const value &t, std::vector<path_t> raw) {
  std::vector<path_t> ps;
  for (const path_t &p : raw) {
    check_path_depth(p);
    normalize_path(t, p, ps);
  }
  // Last paths first, so that deleting an element does not move the others.
  std::sort(ps.begin(), ps.end(), [](const path_t &a, const path_t &b) { return compare_paths(a, b) > 0; });
  ps.erase(std::unique(ps.begin(), ps.end(), [](const path_t &a, const path_t &b) { return compare_paths(a, b) == 0; }), ps.end());
  value r = t;
  for (const path_t &p : ps) {
    if (p.empty()) { return value::null(); }
    del_path(r, p, 0);
  }
  return r;
}

static path_t path_arg(const value &v) {
  if (v.k != K::Arr) { throw jq_error("Path must be specified as an array"); }
  return to_vec(v);
}

// ---------------------------------------------------------------------------
// Function calls, assignments, patterns.
// ---------------------------------------------------------------------------

// Calls the function of frame: binds the arguments (closures, and the values
// of $params, one binding per combination of their outputs), then runs the
// body.
template <class Run>
static void bind_args(const call_node &c, const func_def &d, size_t i, const env_ptr &acc, const value &in, const env_ptr &caller, Run &run) {
  if (i == c.args.size()) { run(*d.body, acc); return; }
  auto cl = std::make_shared<env>();
  cl->name = d.keys[i];
  cl->kind = bind_kind::closure;
  cl->body = c.args[i].get();
  cl->cenv = caller;
  cl->next = acc;
  if (!d.by_value[i]) {
    bind_args(c, d, i + 1, cl, in, caller, run);
    return;
  }
  env_ptr clp = cl;
  c.args[i]->eval(in, caller, [&](const value &v) { bind_args(c, d, i + 1, push_var(d.params[i], v, clp), in, caller, run); });
}

template <class Run>
static void call_function(const call_node &c, const env_ptr &frame, const value &in, const env_ptr &caller, Run &&run) {
  const func_def &d = *frame->def;
  if (c.args.empty()) { run(*d.body, frame); return; }
  bind_args(c, d, 0, frame, in, caller, run);
}

void call_node::eval(const value &in, const env_ptr &e, emit_ref out) const {
  check_stack();
  auto run = [&](const node &body, const env_ptr &env) { body.eval(in, env, out); };
  switch (k) {
  case kind::builtin: fn(*this, in, e, out); return;
  case kind::prelude: call_function(*this, target, in, e, run); return;
  case kind::dynamic: {
    const env_ptr *f = lookup_fn(e, key);
    if (!f) { throw jq_error(key + " is not defined"); }
    if ((*f)->kind == bind_kind::closure) { (*f)->body->eval(in, (*f)->cenv, out); return; }
    call_function(*this, *f, in, e, run);
    return;
  }
  }
}

void call_node::paths(const value &in, path_t &p, const env_ptr &e, emit_ref out) const {
  check_stack();
  auto run = [&](const node &body, const env_ptr &env) { body.paths(in, p, env, out); };
  switch (k) {
  case kind::builtin:
    if (pfn) { pfn(*this, in, p, e, out); } else { node::paths(in, p, e, out); }
    return;
  case kind::prelude: call_function(*this, target, in, e, run); return;
  case kind::dynamic: {
    const env_ptr *f = lookup_fn(e, key);
    if (!f) { throw jq_error(key + " is not defined"); }
    if ((*f)->kind == bind_kind::closure) { (*f)->body->paths(in, p, (*f)->cenv, out); return; }
    call_function(*this, *f, in, e, run);
    return;
  }
  }
}

void assign_node::eval(const value &in, const env_ptr &e, emit_ref out) const {
  if (o == assign_op::update) {
    // As jq's _modify: the first output of rhs replaces each value; when rhs
    // is empty, the path is deleted (after all the updates).
    value acc = in;
    std::vector<path_t> dels;
    path_t p;
    lhs->paths(in, p, e, [&](const value &) {
      value cur = get_path(acc, p);
      value nv;
      bool got = false;
      try {
        rhs->eval(cur, e, [&](const value &x) {
          nv = x;
          got = true;
          throw stop_signal{&got};
        });
      } catch (stop_signal &s) {
        if (s.id != &got) { throw; }
      }
      if (got) { set_path(acc, p, 0, nv); } else { dels.push_back(p); }
    });
    if (!dels.empty()) { acc = delete_paths(acc, std::move(dels)); }
    out(acc);
    return;
  }
  // The right side is evaluated on the input; each of its outputs gives one
  // result.
  rhs->eval(in, e, [&](const value &x) {
    value acc = in;
    path_t p;
    lhs->paths(in, p, e, [&](const value &) {
      if (o == assign_op::set) { set_path(acc, p, 0, x); return; }
      value cur = get_path(acc, p);
      value nv;
      switch (o) {
      case assign_op::add: nv = arith(op::add, cur, x); break;
      case assign_op::sub: nv = arith(op::sub, cur, x); break;
      case assign_op::mul: nv = arith(op::mul, cur, x); break;
      case assign_op::div: nv = arith(op::div, cur, x); break;
      case assign_op::mod: nv = arith(op::mod, cur, x); break;
      default: nv = truthy(cur) ? cur : x; break;
      }
      set_path(acc, p, 0, nv);
    });
    out(acc);
  });
}

static void bind_fields(const pattern &pat, size_t i, const value &v, const value &in, const env_ptr &e, const bind_cont &k);

void bind_pattern(const pattern &pat, const value &v, const value &in, const env_ptr &e, const bind_cont &k) {
  check_stack();
  switch (pat.k) {
  case pattern::kind::var: k(push_var(pat.name, v, e)); return;
  case pattern::kind::array: {
    if (v.k != K::Arr && v.k != K::Null) { cannot_index(v, value::number(0)); }
    std::function<void(size_t, const env_ptr &)> elems = [&](size_t i, const env_ptr &acc) {
      if (i == pat.elems.size()) { k(acc); return; }
      value x;
      if (v.k != K::Arr || !array_get(v, int64_t(i), x)) { x = value::null(); }
      bind_pattern(pat.elems[i], x, in, acc, [&](const env_ptr &ne) { elems(i + 1, ne); });
    };
    elems(0, e);
    return;
  }
  case pattern::kind::object: bind_fields(pat, 0, v, in, e, k); return;
  }
}

static void bind_fields(const pattern &pat, size_t i, const value &v, const value &in, const env_ptr &e, const bind_cont &k) {
  if (i == pat.fields.size()) { k(e); return; }
  const pattern::field &f = pat.fields[i];
  f.key->eval(in, e, [&](const value &key) {
    if (key.k != K::Str) { cannot_index(v, key); }
    value x;
    if (v.k == K::Null) { x = value::null(); }
    else if (v.k == K::Obj) { if (!object_get(v, get_str(key), x)) { x = value::null(); } }
    else { cannot_index(v, key); }
    env_ptr ne = f.var.empty() ? e : push_var(f.var, x, e);
    if (f.sub) {
      bind_pattern(*f.sub, x, in, ne, [&](const env_ptr &n2) { bind_fields(pat, i + 1, v, in, n2, k); });
    } else {
      bind_fields(pat, i + 1, v, in, ne, k);
    }
  });
}

// ---------------------------------------------------------------------------
// Builtins.
// ---------------------------------------------------------------------------

static value length_of(const value &v) {
  switch (v.k) {
  case K::Null: return value::number(0);
  case K::True: case K::False: throw jq_error("boolean (" + to_json(v) + ") has no length");
  case K::Num: return value::number(std::fabs(get_num(v)));
  case K::Str: return value::number(double(utf8_count(get_str(v))));
  default: return value::number(double(container_size(v)));
  }
}

static value keys_of(const value &v, bool sorted) {
  std::vector<value> r{};
  if (v.k == K::Obj) {
    std::vector<std::string> ks;
    for_each_field(v, [&](std::string_view k, const value &) { ks.emplace_back(k); });
    if (sorted) { std::sort(ks.begin(), ks.end()); }
    for (auto &k : ks) { r.push_back(value::string(std::move(k))); }
  } else if (v.k == K::Arr) {
    size_t n = container_size(v);
    for (size_t i = 0; i < n; i++) { r.push_back(value::number(double(i))); }
  } else {
    throw jq_error(std::string(kind_name(v.k)) + " (" + to_json(v) + ") has no keys");
  }
  return value::array(std::move(r));
}

void require_kind(const value &v, K k, const char *what) {
  if (v.k != k) { throw jq_error(std::string(kind_name(v.k)) + " (" + error_text(v) + ") " + what); }
}

// jq's containment; values of different kinds do not contain each other
// (the contains builtin rejects them at the top level).
static bool contains(const value &a, const value &b) {
  check_stack();
  if (a.k != b.k) { return false; }
  if (a.k == K::Obj && b.k == K::Obj) {
    bool ok = true;
    for_each_field(b, [&](std::string_view k, const value &bv) {
      value av;
      if (!ok) { return; }
      if (!object_get(a, k, av) || !contains(av, bv)) { ok = false; }
    });
    return ok;
  }
  if (a.k == K::Arr && b.k == K::Arr) {
    auto av = to_vec(a);
    bool ok = true;
    for_each_elem(b, [&](const value &bv) {
      if (!ok) { return; }
      bool found = false;
      for (auto &x : av) { if (contains(x, bv)) { found = true; break; } }
      ok = found;
    });
    return ok;
  }
  if (a.k == K::Str && b.k == K::Str) { return get_str(a).find(get_str(b)) != std::string_view::npos; }
  return compare(a, b) == 0;
}

static bool contains_checked(const value &a, const value &b) {
  if (a.k != b.k) {
    throw jq_error(std::string(kind_name(a.k)) + " (" + error_text(a) + ") and " + kind_name(b.k) + " (" + error_text(b) + ") cannot have their containment checked");
  }
  return contains(a, b);
}

value tostring_of(const value &v) {
  if (v.k == K::Str) { return v; }
  return value::string(to_json(v));
}

static value sort_by_keys(std::vector<std::pair<value, value>> &kv) {
  std::stable_sort(kv.begin(), kv.end(), [](const auto &x, const auto &y) { return compare(x.first, y.first) < 0; });
  std::vector<value> r{};
  for (auto &p : kv) { r.push_back(p.second); }
  return value::array(std::move(r));
}

// Evaluates f on x and returns its outputs collected in an array value
// (used for sort_by/group_by keys, as jq does).
static value collect(const node &f, const value &x, const env_ptr &e) {
  std::vector<value> r{};
  f.eval(x, e, [&](const value &y) { r.push_back(y); });
  return value::array(std::move(r));
}

// Calls f for each element of an array or each value of an object (jq's .[]).
template <class F> static void each_value(const value &v, F &&f) {
  if (v.k == K::Arr) { for_each_elem(v, f); return; }
  if (v.k == K::Obj) { for_each_field(v, [&](std::string_view, const value &x) { f(x); }); return; }
  cannot_iterate(v);
}

// The first output of f, if any.
static bool first_output(const node &f, const value &in, const env_ptr &e, value &r) {
  bool got = false;
  try {
    f.eval(in, e, [&](const value &x) {
      r = x;
      got = true;
      throw stop_signal{&got};
    });
  } catch (stop_signal &s) {
    if (s.id != &got) { throw; }
  }
  return got;
}

static double num_arg(const value &v) {
  if (v.k != K::Num) { throw jq_error(std::string(kind_name(v.k)) + " (" + error_text(v) + ") number required"); }
  return get_num(v);
}

static const value &env_object() {
  static const value env = [] {
    std::vector<std::pair<std::string, value>> o;
#if defined(_WIN32)
    char **envp = _environ;
#else
    char **envp = environ;
#endif
    for (char **p = envp; p && *p; p++) {
      std::string kv(*p);
      size_t eq = kv.find('=');
      if (eq == std::string::npos || eq == 0) { continue; }
      std::string k = kv.substr(0, eq);
      if (std::any_of(o.begin(), o.end(), [&](const auto &x) { return x.first == k; })) { continue; }
      o.emplace_back(std::move(k), value::string(kv.substr(eq + 1)));
    }
    return value::object(std::move(o));
  }();
  return env;
}

std::function<bool(value &)> g_next_input{};
const char *g_input_filename = nullptr;

#define BI [[maybe_unused]] const call_node &c, [[maybe_unused]] const value &in, [[maybe_unused]] const env_ptr &e, [[maybe_unused]] emit_ref out
#define BP [[maybe_unused]] const call_node &c, [[maybe_unused]] const value &in, [[maybe_unused]] path_t &p, [[maybe_unused]] const env_ptr &e, [[maybe_unused]] emit_ref out
#define ARG(i, x) c.args[i]->eval(in, e, [&](const value &x)

static void select_type(const value &in, emit_ref out, bool ok) {
  if (ok) { out(in); }
}

static bool is_kind(const value &v, const char *what) {
  switch (what[0]) {
  case 'v': return v.k != K::Null;                              // values
  case 'n': return what[1] == 'u' && what[2] == 'l' ? v.k == K::Null : v.k == K::Num;  // nulls, numbers
  case 'b': return v.k == K::True || v.k == K::False;           // booleans
  case 's': return what[1] == 't' ? v.k == K::Str : (v.k != K::Arr && v.k != K::Obj);  // strings, scalars
  case 'a': return v.k == K::Arr;                               // arrays
  case 'o': return v.k == K::Obj;                               // objects
  case 'i': return v.k == K::Arr || v.k == K::Obj;              // iterables
  }
  return false;
}

#define TYPE_FILTER(name) \
  {name "/0", [](BI) { select_type(in, out, is_kind(in, name)); }, [](BP) { select_type(in, out, is_kind(in, name)); }}

static void join_values(const value &in, const value &sep, emit_ref out) {
  std::string r;
  bool first = true;
  each_value(in, [&](const value &x) {
    if (!first) {
      // jq adds the separator with +: other kinds raise its error.
      if (sep.k == K::Str) { r.append(get_str(sep)); }
      else { r = get_str(arith(op::add, value::string(r), sep)); }
    }
    first = false;
    if (x.k == K::Null) { return; }
    if (x.k == K::Str) { r.append(get_str(x)); }
    else if (x.k == K::Num || x.k == K::True || x.k == K::False) { r.append(to_json(x)); }
    else { (void)arith(op::add, value::string(r), x); }
  });
  out(value::string(std::move(r)));
}

// Iterative, for the same reason as set_path.
static void flatten_into(const value &v, double depth, std::vector<value> &r) {
  struct frame {
    std::vector<value> items;
    size_t next;
    double depth;
  };
  std::vector<frame> stack;
  std::vector<value> top;
  each_value(v, [&](const value &x) { top.push_back(x); });
  stack.push_back({std::move(top), 0, depth});
  while (!stack.empty()) {
    frame &f = stack.back();
    if (f.next == f.items.size()) {
      stack.pop_back();
      continue;
    }
    value x = f.items[f.next++];
    if (x.k == K::Arr && f.depth > 0) {
      double d = f.depth - 1;
      stack.push_back({to_vec(x), 0, d});
    } else {
      r.push_back(std::move(x));
    }
  }
}

static void range_gen(double from, double upto, double by, emit_ref out) {
  if (by > 0) {
    for (double i = from; i < upto; i += by) { fuzz_tick(); out(value::number(i)); }
  } else if (by < 0) {
    for (double i = from; i > upto; i += by) { fuzz_tick(); out(value::number(i)); }
  }
}

static void entries_of(const value &in, std::vector<value> &r) {
  if (in.k == K::Obj) {
    for_each_field(in, [&](std::string_view k, const value &v) {
      r.push_back(value::object({{"key", value::string(std::string(k))}, {"value", v}}));
    });
  } else if (in.k == K::Arr) {
    double i = 0;
    for_each_elem(in, [&](const value &v) { r.push_back(value::object({{"key", value::number(i++)}, {"value", v}})); });
  } else {
    (void)keys_of(in, false);  // raises jq's error
  }
}

static value from_entries_of(const value &in) {
  std::vector<std::pair<std::string, value>> r;
  each_value(in, [&](const value &x) {
    value k, v;
    if (x.k != K::Obj) { cannot_index(x, std::string("key")); }
    bool found = false;
    for (const char *name : {"key", "k", "name", "Name", "Key", "K"}) {
      if (object_get(x, name, k) && truthy(k)) { found = true; break; }
    }
    if (!found && !object_get(x, "key", k)) { k = value::null(); }
    if (!object_get(x, "value", v) && !object_get(x, "v", v) && !object_get(x, "Value", v) && !object_get(x, "V", v)) { v = value::null(); }
    std::string key;
    if (k.k == K::Str) { key = std::string(get_str(k)); }
    else if (k.k == K::Num || k.k == K::True || k.k == K::False) { key = to_json(k); }
    else if (k.k == K::Null) { key = "null"; }
    else { throw jq_error(std::string("Cannot use ") + kind_name(k.k) + " (" + error_text(k) + ") as object key"); }
    auto it = std::find_if(r.begin(), r.end(), [&](const auto &kv) { return kv.first == key; });
    if (it != r.end()) { it->second = v; } else { r.emplace_back(std::move(key), v); }
  });
  return value::object(std::move(r));
}

static void limit_gen(const call_node &c, const value &in, const env_ptr &e, int64_t l, const std::function<void(const value &)> &each_out,
                      const std::function<void(emit_ref)> &gen) {
  (void)c; (void)in; (void)e;
  if (l < 0) { throw jq_error("limit doesn't support negative count"); }
  if (l == 0) { return; }
  int64_t count = 0;
  try {
    gen([&](const value &x) {
      each_out(x);
      if (++count >= l) { throw stop_signal{&count}; }
    });
  } catch (stop_signal &s) {
    if (s.id != &count) { throw; }
  }
}

static int64_t count_arg(const value &v) { return to_int64(std::floor(num_arg(v))); }

static value from_json(std::string_view s);

// The length of the white space character at s[i] (as jq 1.8: ASCII and
// Unicode white space), or 0.
static size_t space_at(std::string_view s, size_t i) {
  static const char *const spaces[] = {"\xC2\x85", "\xC2\xA0", "\xE1\x9A\x80", "\xE2\x80\xA8", "\xE2\x80\xA9", "\xE2\x80\xAF", "\xE2\x81\x9F", "\xE3\x80\x80"};
  char ch = s[i];
  if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v') { return 1; }
  if (s.substr(i, 2) == "\xE2\x80" && i + 2 < s.size() && uint8_t(s[i + 2]) >= 0x80 && uint8_t(s[i + 2]) <= 0x8A) { return 3; }
  for (const char *sp : spaces) {
    size_t n = strlen(sp);
    if (s.substr(i, n) == sp) { return n; }
  }
  return 0;
}

static std::string trim_ws(std::string_view s, bool left, bool right) {
  size_t a = 0, b = s.size();
  if (left) {
    while (a < b) {
      size_t n = space_at(s, a);
      if (!n) { break; }
      a += n;
    }
  }
  if (right) {
    while (b > a) {
      // Step back to the start of the last character.
      size_t start = b - 1;
      while (start > a && (uint8_t(s[start]) & 0xC0) == 0x80) { start--; }
      size_t n = space_at(s, start);
      if (!n || start + n != b) { break; }
      b = start;
    }
  }
  return std::string(s.substr(a, b - a));
}

// Parses a number as jq 1.8's tonumber: an optional sign, digits with an
// optional fraction and exponent, nothing else.
static bool parse_number_text(std::string_view s, double &d) {
  size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) { i++; }
  size_t digits = 0;
  while (i < s.size() && isdigit((unsigned char)s[i])) { i++; digits++; }
  if (i < s.size() && s[i] == '.') {
    i++;
    while (i < s.size() && isdigit((unsigned char)s[i])) { i++; digits++; }
  }
  if (digits == 0) { return false; }
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { i++; }
    size_t ed = 0;
    while (i < s.size() && isdigit((unsigned char)s[i])) { i++; ed++; }
    if (ed == 0) { return false; }
  }
  if (i != s.size()) { return false; }
  std::string copy(s);
  d = strtod(copy.c_str(), nullptr);
  return true;
}

static const builtin_entry core_builtins[] = {
    {"empty/0", [](BI) {}, [](BP) {}},
    {"not/0", [](BI) { out(value::boolean(!truthy(in))); }, nullptr},
    {"length/0", [](BI) { out(length_of(in)); }, nullptr},
    {"utf8bytelength/0", [](BI) { require_kind(in, K::Str, "only strings have UTF-8 byte length"); out(value::number(double(get_str(in).size()))); }, nullptr},
    {"keys/0", [](BI) { out(keys_of(in, true)); }, nullptr},
    {"keys_unsorted/0", [](BI) { out(keys_of(in, false)); }, nullptr},
    {"type/0", [](BI) { out(value::string(kind_name(in.k))); }, nullptr},
    {"true/0", [](BI) { out(value::boolean(true)); }, nullptr},
    {"false/0", [](BI) { out(value::boolean(false)); }, nullptr},
    {"null/0", [](BI) { out(value::null()); }, nullptr},
    TYPE_FILTER("values"), TYPE_FILTER("nulls"), TYPE_FILTER("booleans"), TYPE_FILTER("numbers"), TYPE_FILTER("strings"),
    TYPE_FILTER("arrays"), TYPE_FILTER("objects"), TYPE_FILTER("iterables"), TYPE_FILTER("scalars"),
    {"add/0", [](BI) {
       if (in.k != K::Arr && in.k != K::Obj) { cannot_iterate(in); }
       value acc;
       bool first = true;
       each_value(in, [&](const value &x) { acc = first ? x : arith(op::add, acc, x); first = false; });
       out(acc);
     }, nullptr},
    {"first/0", [](BI) { index_value(in, value::number(0), out, false); },
     [](BP) {
       index_value(in, value::number(0), [&](const value &r) { p.push_back(value::number(0)); out(r); p.pop_back(); }, false);
     }},
    {"last/0", [](BI) { index_value(in, value::number(-1), out, false); },
     [](BP) {
       index_value(in, value::number(-1), [&](const value &r) { p.push_back(value::number(-1)); out(r); p.pop_back(); }, false);
     }},
    {"reverse/0", [](BI) {
       if (in.k == K::Str) {
         // jq 1.8 defines reverse by indexing, which fails on strings.
         size_t cps = utf8_count(get_str(in));
         if (cps == 0) { out(value::array({})); return; }
         throw jq_error("Cannot index string with number (" + std::to_string(cps - 1) + ")");
       }
       if (in.k == K::Null) { out(value::array({})); return; }
       auto v = to_vec(in);
       std::reverse(v.begin(), v.end());
       out(value::array(std::move(v)));
     }, nullptr},
    {"sort/0", [](BI) {
       require_kind(in, K::Arr, "cannot be sorted, as it is not an array");
       auto v = to_vec(in);
       std::stable_sort(v.begin(), v.end(), [](const value &a, const value &b) { return compare(a, b) < 0; });
       out(value::array(std::move(v)));
     }, nullptr},
    {"unique/0", [](BI) {
       require_kind(in, K::Arr, "cannot be sorted, as it is not an array");
       auto v = to_vec(in);
       std::stable_sort(v.begin(), v.end(), [](const value &a, const value &b) { return compare(a, b) < 0; });
       v.erase(std::unique(v.begin(), v.end(), [](const value &a, const value &b) { return compare(a, b) == 0; }), v.end());
       out(value::array(std::move(v)));
     }, nullptr},
    {"min/0", [](BI) {
       require_kind(in, K::Arr, "cannot be iterated over");
       value best;
       bool first = true;
       for_each_elem(in, [&](const value &x) {
         if (first || compare(x, best) < 0) { best = x; }
         first = false;
       });
       out(best);
     }, nullptr},
    {"max/0", [](BI) {
       require_kind(in, K::Arr, "cannot be iterated over");
       value best;
       bool first = true;
       for_each_elem(in, [&](const value &x) {
         if (first || compare(x, best) >= 0) { best = x; }
         first = false;
       });
       out(best);
     }, nullptr},
    {"tostring/0", [](BI) { out(tostring_of(in)); }, nullptr},
    {"tonumber/0", [](BI) {
       if (in.k == K::Num) { out(in); return; }
       if (in.k != K::Str) { require_kind(in, K::Str, "cannot be parsed as a number"); }
       std::string_view s = get_str(in);
       double d = 0;
       if (!parse_number_text(s, d)) { throw jq_error("string (" + error_text(in) + ") cannot be parsed as a number"); }
       out(value::number(d));
     }, nullptr},
    {"toboolean/0", [](BI) {
       if (in.k == K::True || in.k == K::False) { out(in); return; }
       if (in.k == K::Str && get_str(in) == "true") { out(value::boolean(true)); return; }
       if (in.k == K::Str && get_str(in) == "false") { out(value::boolean(false)); return; }
       throw jq_error(std::string(kind_name(in.k)) + " (" + error_text(in) + ") cannot be parsed as a boolean");
     }, nullptr},
    {"ascii_downcase/0", [](BI) {
       require_kind(in, K::Str, "cannot be case-converted");
       std::string s(get_str(in));
       for (char &ch : s) { if (ch >= 'A' && ch <= 'Z') { ch = char(ch + 32); } }
       out(value::string(std::move(s)));
     }, nullptr},
    {"ascii_upcase/0", [](BI) {
       require_kind(in, K::Str, "cannot be case-converted");
       std::string s(get_str(in));
       for (char &ch : s) { if (ch >= 'a' && ch <= 'z') { ch = char(ch - 32); } }
       out(value::string(std::move(s)));
     }, nullptr},
    {"abs/0", [](BI) {
       if (in.k == K::Num && get_num(in) < 0) { out(value::number(-get_num(in))); } else { out(in); }
     }, nullptr},
    {"to_entries/0", [](BI) {
       std::vector<value> r;
       entries_of(in, r);
       out(value::array(std::move(r)));
     }, nullptr},
    {"from_entries/0", [](BI) { out(from_entries_of(in)); }, nullptr},
    {"tojson/0", [](BI) { out(value::string(to_json(in))); }, nullptr},
    {"fromjson/0", [](BI) {
       require_kind(in, K::Str, "only strings can be parsed");
       out(from_json(get_str(in)));
     }, nullptr},
    {"input_line_number/0", [](BI) { out(value::number(0)); }, nullptr},
    {"any/0", [](BI) {
       bool r = false;
       each_value(in, [&](const value &x) { r = r || truthy(x); });
       out(value::boolean(r));
     }, nullptr},
    {"all/0", [](BI) {
       bool r = true;
       each_value(in, [&](const value &x) { r = r && truthy(x); });
       out(value::boolean(r));
     }, nullptr},
    {"flatten/0", [](BI) {
       std::vector<value> r;
       flatten_into(in, 1e18, r);
       out(value::array(std::move(r)));
     }, nullptr},
    {"error/0", [](BI) { throw jq_error(in.k == K::Str ? std::string(get_str(in)) : to_json(in), in); }, [](BP) { throw jq_error(in.k == K::Str ? std::string(get_str(in)) : to_json(in), in); }},
    {"recurse/0", [](BI) { recurse_node::rec(in, out); }, [](BP) { recurse_node::rec_paths(in, p, out); }},
    {"paths/0", [](BI) {
       path_t p;
       recurse_node::rec_paths(in, p, [&](const value &) {
         if (!p.empty()) { out(value::array(p)); }
       });
     }, nullptr},
    {"leaf_paths/0", [](BI) {
       path_t p;
       recurse_node::rec_paths(in, p, [&](const value &v) {
         if (!p.empty() && v.k != K::Arr && v.k != K::Obj) { out(value::array(p)); }
       });
     }, nullptr},
    {"env/0", [](BI) { out(env_object()); }, nullptr},
    {"builtins/0", [](BI) {
       std::vector<value> r;
       for (auto &n : builtin_names()) { r.push_back(value::string(n)); }
       out(value::array(std::move(r)));
     }, nullptr},
    {"input/0", [](BI) {
       value v;
       if (!g_next_input || !g_next_input(v)) { throw jq_error("No more inputs"); }
       out(v);
     }, nullptr},
    {"inputs/0", [](BI) {
       value v;
       while (g_next_input && g_next_input(v)) { out(v); }
     }, nullptr},
    {"input_filename/0", [](BI) { out(g_input_filename ? value::string(g_input_filename) : value::null()); }, nullptr},
    {"now/0", [](BI) {
       auto t = std::chrono::system_clock::now().time_since_epoch();
       out(value::number(double(std::chrono::duration_cast<std::chrono::microseconds>(t).count()) / 1e6));
     }, nullptr},
    {"halt/0", [](BI) { throw halt_signal{0, std::string()}; }, nullptr},
    {"halt_error/1", [](BI) {
       ARG(0, code) {
         std::string msg = in.k == K::Str ? std::string(get_str(in)) : to_json(in) + "\n";
         throw halt_signal{int(count_arg(code)), msg};
       });
     }, nullptr},
    {"debug/0", [](BI) {
       fprintf(stderr, "[\"DEBUG:\",%s]\n", to_json(in).c_str());
       out(in);
     }, nullptr},
    {"stderr/0", [](BI) {
       fputs(to_json(in).c_str(), stderr);
       out(in);
     }, nullptr},
    {"infinite/0", [](BI) { out(value::number(INFINITY)); }, nullptr},
    {"nan/0", [](BI) { out(value::number(NAN)); }, nullptr},
    {"isinfinite/0", [](BI) { out(value::boolean(std::isinf(num_arg(in)))); }, nullptr},
    {"isnan/0", [](BI) { out(value::boolean(std::isnan(num_arg(in)))); }, nullptr},
    {"isnormal/0", [](BI) { out(value::boolean(std::isnormal(num_arg(in)))); }, nullptr},
    {"have_decnum/0", [](BI) { out(value::boolean(false)); }, nullptr},
    {"have_literal_numbers/0", [](BI) { out(value::boolean(true)); }, nullptr},
    {"trim/0", [](BI) { if (in.k != K::Str) { throw jq_error("trim input must be a string"); } out(value::string(trim_ws(get_str(in), true, true))); }, nullptr},
    {"ltrim/0", [](BI) { if (in.k != K::Str) { throw jq_error("trim input must be a string"); } out(value::string(trim_ws(get_str(in), true, false))); }, nullptr},
    {"rtrim/0", [](BI) { if (in.k != K::Str) { throw jq_error("trim input must be a string"); } out(value::string(trim_ws(get_str(in), false, true))); }, nullptr},
    {"explode/0", [](BI) {
       require_kind(in, K::Str, "cannot be exploded");
       std::string_view s = get_str(in);
       std::vector<value> r;
       for (size_t i = 0; i < s.size();) {
         uint8_t ch = uint8_t(s[i]);
         int len = ch < 0x80 ? 1 : (ch >= 0xF0 ? 4 : (ch >= 0xE0 ? 3 : 2));
         uint32_t cp = len == 1 ? ch : (ch & (len == 4 ? 0x07 : (len == 3 ? 0x0F : 0x1F)));
         for (int k = 1; k < len && i + size_t(k) < s.size(); k++) { cp = (cp << 6) | (uint8_t(s[i + size_t(k)]) & 0x3F); }
         i += size_t(len);
         r.push_back(value::number(double(cp)));
       }
       out(value::array(std::move(r)));
     }, nullptr},
    {"implode/0", [](BI) {
       if (in.k != K::Arr) { throw jq_error("Implode input must be an array"); }
       std::string r;
       for_each_elem(in, [&](const value &x) {
         if (x.k != K::Num) { throw jq_error("Unicode codepoint must be numeric"); }
         double d = get_num(x);
         uint32_t cp = (d < 0 || d > 0x10FFFF || (d >= 0xD800 && d < 0xE000)) ? 0xFFFD : uint32_t(d);
         append_utf8(r, cp);
       });
       out(value::string(std::move(r)));
     }, nullptr},

    {"select/1", [](BI) { ARG(0, x) { if (truthy(x)) { out(in); } }); },
     [](BP) { ARG(0, x) { if (truthy(x)) { out(in); } }); }},
    {"map/1", [](BI) {
       if (in.k != K::Arr && in.k != K::Obj) { cannot_iterate(in); }
       std::vector<value> r;
       each_value(in, [&](const value &x) { c.args[0]->eval(x, e, [&](const value &y) { r.push_back(y); }); });
       out(value::array(std::move(r)));
     }, nullptr},
    {"map_values/1", [](BI) {
       if (in.k == K::Arr) {
         std::vector<value> r;
         for_each_elem(in, [&](const value &x) {
           value y;
           if (first_output(*c.args[0], x, e, y)) { r.push_back(y); }
         });
         out(value::array(std::move(r)));
         return;
       }
       std::vector<std::pair<std::string, value>> r;
       for_each_field(in, [&](std::string_view k, const value &x) {
         value y;
         if (first_output(*c.args[0], x, e, y)) { r.emplace_back(std::string(k), y); }
       });
       out(value::object(std::move(r)));
     }, nullptr},
    {"has/1", [](BI) {
       ARG(0, k) {
         if (in.k == K::Obj && k.k == K::Str) { value tmp; out(value::boolean(object_get(in, get_str(k), tmp))); return; }
         if (in.k == K::Arr && k.k == K::Num) { double d = get_num(k); out(value::boolean(d >= 0 && d < double(container_size(in)))); return; }
         throw jq_error(std::string("Cannot check whether ") + kind_name(in.k) + " has a " + kind_name(k.k) + " key");
       });
     }, nullptr},
    {"contains/1", [](BI) { ARG(0, b) { out(value::boolean(contains_checked(in, b))); }); }, nullptr},
    {"inside/1", [](BI) { ARG(0, b) { out(value::boolean(contains_checked(b, in))); }); }, nullptr},
    {"startswith/1", [](BI) {
       ARG(0, b) {
         if (in.k != K::Str || b.k != K::Str) { throw jq_error("startswith() requires string inputs"); }
         std::string_view s = get_str(in), q = get_str(b);
         out(value::boolean(q.size() <= s.size() && s.substr(0, q.size()) == q));
       });
     }, nullptr},
    {"endswith/1", [](BI) {
       ARG(0, b) {
         if (in.k != K::Str || b.k != K::Str) { throw jq_error("endswith() requires string inputs"); }
         std::string_view s = get_str(in), q = get_str(b);
         out(value::boolean(q.size() <= s.size() && s.substr(s.size() - q.size()) == q));
       });
     }, nullptr},
    {"ltrimstr/1", [](BI) {
       ARG(0, b) {
         if (in.k != K::Str || b.k != K::Str) { throw jq_error("startswith() requires string inputs"); }
         std::string_view s = get_str(in), q = get_str(b);
         if (s.substr(0, q.size()) == q) { out(value::string(std::string(s.substr(q.size())))); } else { out(in); }
       });
     }, nullptr},
    {"rtrimstr/1", [](BI) {
       ARG(0, b) {
         if (in.k != K::Str || b.k != K::Str) { throw jq_error("endswith() requires string inputs"); }
         std::string_view s = get_str(in), q = get_str(b);
         if (q.size() <= s.size() && s.substr(s.size() - q.size()) == q) { out(value::string(std::string(s.substr(0, s.size() - q.size())))); } else { out(in); }
       });
     }, nullptr},
    {"split/1", [](BI) {
       ARG(0, b) {
         if (in.k != K::Str || b.k != K::Str) { throw jq_error("split input and separator must be strings"); }
         out(arith(op::div, in, b));
       });
     }, nullptr},
    {"join/1", [](BI) { ARG(0, sep) { join_values(in, sep, out); }); }, nullptr},
    {"flatten/1", [](BI) {
       ARG(0, d) {
         double depth = num_arg(d);
         if (depth < 0) { throw jq_error("flatten depth must not be negative"); }
         std::vector<value> r;
         flatten_into(in, depth, r);
         out(value::array(std::move(r)));
       });
     }, nullptr},
    {"sort_by/1", [](BI) {
       require_kind(in, K::Arr, "cannot be sorted, as it is not an array");
       std::vector<std::pair<value, value>> kv;
       for_each_elem(in, [&](const value &x) { kv.emplace_back(collect(*c.args[0], x, e), x); });
       out(sort_by_keys(kv));
     }, nullptr},
    {"group_by/1", [](BI) {
       require_kind(in, K::Arr, "cannot be grouped, as it is not an array");
       std::vector<std::pair<value, value>> kv;
       for_each_elem(in, [&](const value &x) { kv.emplace_back(collect(*c.args[0], x, e), x); });
       std::stable_sort(kv.begin(), kv.end(), [](const auto &x, const auto &y) { return compare(x.first, y.first) < 0; });
       std::vector<value> groups, cur;
       for (size_t i = 0; i < kv.size(); i++) {
         if (i > 0 && compare(kv[i].first, kv[i - 1].first) != 0) { groups.push_back(value::array(std::move(cur))); cur.clear(); }
         cur.push_back(kv[i].second);
       }
       if (!cur.empty()) { groups.push_back(value::array(std::move(cur))); }
       out(value::array(std::move(groups)));
     }, nullptr},
    {"unique_by/1", [](BI) {
       require_kind(in, K::Arr, "cannot be sorted, as it is not an array");
       std::vector<std::pair<value, value>> kv;
       for_each_elem(in, [&](const value &x) { kv.emplace_back(collect(*c.args[0], x, e), x); });
       std::stable_sort(kv.begin(), kv.end(), [](const auto &x, const auto &y) { return compare(x.first, y.first) < 0; });
       std::vector<value> r;
       for (size_t i = 0; i < kv.size(); i++) {
         if (i == 0 || compare(kv[i].first, kv[i - 1].first) != 0) { r.push_back(kv[i].second); }
       }
       out(value::array(std::move(r)));
     }, nullptr},
    {"min_by/1", [](BI) {
       require_kind(in, K::Arr, "cannot be iterated over");
       value best, bestk;
       bool first = true;
       for_each_elem(in, [&](const value &x) {
         value k = collect(*c.args[0], x, e);
         if (first || compare(k, bestk) < 0) { best = x; bestk = k; }
         first = false;
       });
       out(best);
     }, nullptr},
    {"max_by/1", [](BI) {
       require_kind(in, K::Arr, "cannot be iterated over");
       value best, bestk;
       bool first = true;
       for_each_elem(in, [&](const value &x) {
         value k = collect(*c.args[0], x, e);
         if (first || compare(k, bestk) >= 0) { best = x; bestk = k; }
         first = false;
       });
       out(best);
     }, nullptr},
    {"any/1", [](BI) {
       bool r = false;
       each_value(in, [&](const value &x) { if (!r) { value y; r = first_output(*c.args[0], x, e, y) && truthy(y); } });
       out(value::boolean(r));
     }, nullptr},
    {"all/1", [](BI) {
       bool r = true;
       each_value(in, [&](const value &x) { if (r) { value y; r = !first_output(*c.args[0], x, e, y) || truthy(y); } });
       out(value::boolean(r));
     }, nullptr},
    {"range/1", [](BI) { ARG(0, upto) { range_gen(0, num_arg(upto), 1, out); }); }, nullptr},
    {"recurse/1", [](BI) {
       // Each level recurses: bound it like nested values.
       uint32_t level = 0;
       std::function<void(const value &)> rec = [&](const value &v) {
         fuzz_tick();
         check_depth(++level);
         out(v);
         c.args[0]->eval(v, e, [&](const value &x) { rec(x); });
         level--;
       };
       rec(in);
     },
     [](BP) {
       uint32_t level = 0;
       std::function<void(const value &)> rec = [&](const value &v) {
         fuzz_tick();
         check_depth(++level);
         out(v);
         c.args[0]->paths(v, p, e, [&](const value &x) { rec(x); });
         level--;
       };
       rec(in);
     }},
    {"first/1", [](BI) {
       value r;
       if (first_output(*c.args[0], in, e, r)) { out(r); }
     },
     [](BP) {
       bool done = false;
       size_t n = p.size();
       try {
         c.args[0]->paths(in, p, e, [&](const value &x) {
           out(x);
           done = true;
           throw stop_signal{&done};
         });
       } catch (stop_signal &s) {
         if (s.id != &done) { throw; }
         p.resize(n);
       }
     }},
    {"last/1", [](BI) {
       value r;
       bool got = false;
       c.args[0]->eval(in, e, [&](const value &x) { r = x; got = true; });
       if (got) { out(r); }
     },
     [](BP) {
       path_t lastp;
       value r;
       bool got = false;
       size_t n = p.size();
       c.args[0]->paths(in, p, e, [&](const value &x) { lastp = p; r = x; got = true; });
       if (got) {
         path_t saved(p.begin(), p.begin() + std::ptrdiff_t(n));
         p = lastp;
         out(r);
         p = saved;
       }
     }},
    {"isempty/1", [](BI) {
       value r;
       out(value::boolean(!first_output(*c.args[0], in, e, r)));
     }, nullptr},
    {"error/1", [](BI) { ARG(0, m) { throw jq_error(m.k == K::Str ? std::string(get_str(m)) : to_json(m), m); }); },
     [](BP) { ARG(0, m) { throw jq_error(m.k == K::Str ? std::string(get_str(m)) : to_json(m), m); }); }},
    {"indices/1", [](BI) { ARG(0, q) { out(indices_of(in, q)); }); }, nullptr},
    {"index/1", [](BI) {
       ARG(0, q) {
         value r = indices_of(in, q);
         if (r.k != K::Arr || container_size(r) == 0) { out(value::null()); return; }
         value x;
         array_get(r, 0, x);
         out(x);
       });
     }, nullptr},
    {"rindex/1", [](BI) {
       ARG(0, q) {
         value r = indices_of(in, q);
         if (r.k != K::Arr || container_size(r) == 0) { out(value::null()); return; }
         value x;
         array_get(r, -1, x);
         out(x);
       });
     }, nullptr},
    {"path/1", [](BI) {
       path_t p;
       c.args[0]->paths(in, p, e, [&](const value &) { out(value::array(p)); });
     }, nullptr},
    {"getpath/1", [](BI) {
       ARG(0, pv) {
         out(get_path(in, path_arg(pv)));
       });
     },
     [](BP) {
       ARG(0, pv) {
         path_t pa = path_arg(pv);
         value r = get_path(in, pa);
         size_t n = p.size();
         p.insert(p.end(), pa.begin(), pa.end());
         out(r);
         p.resize(n);
       });
     }},
    {"delpaths/1", [](BI) {
       ARG(0, ps) {
         if (ps.k != K::Arr) { throw jq_error("Paths must be specified as an array"); }
         std::vector<path_t> all;
         for_each_elem(ps, [&](const value &x) { all.push_back(path_arg(x)); });
         out(delete_paths(in, std::move(all)));
       });
     }, nullptr},
    {"limit/2", [](BI) {
       ARG(0, lim) {
         limit_gen(c, in, e, count_arg(lim), [&](const value &x) { out(x); }, [&](emit_ref o) { c.args[1]->eval(in, e, o); });
       });
     },
     [](BP) {
       ARG(0, lim) {
         size_t n = p.size();
         try {
           limit_gen(c, in, e, count_arg(lim), [&](const value &x) { out(x); }, [&](emit_ref o) { c.args[1]->paths(in, p, e, o); });
         } catch (...) {
           p.resize(n);
           throw;
         }
         p.resize(n);
       });
     }},
    {"skip/2", [](BI) {
       ARG(0, cnt) {
         int64_t n = count_arg(cnt);
         if (n < 0) { throw jq_error("skip doesn't support negative count"); }
         int64_t seen = 0;
         c.args[1]->eval(in, e, [&](const value &x) { if (seen++ >= n) { out(x); } });
       });
     },
     [](BP) {
       ARG(0, cnt) {
         int64_t n = count_arg(cnt);
         if (n < 0) { throw jq_error("skip doesn't support negative count"); }
         int64_t seen = 0;
         c.args[1]->paths(in, p, e, [&](const value &x) { if (seen++ >= n) { out(x); } });
       });
     }},
    {"range/2", [](BI) {
       ARG(0, from) {
         c.args[1]->eval(in, e, [&](const value &upto) { range_gen(num_arg(from), num_arg(upto), 1, out); });
       });
     }, nullptr},
    {"range/3", [](BI) {
       ARG(0, from) {
         c.args[1]->eval(in, e, [&](const value &upto) {
           c.args[2]->eval(in, e, [&](const value &by) { range_gen(num_arg(from), num_arg(upto), num_arg(by), out); });
         });
       });
     }, nullptr},
    {"setpath/2", [](BI) {
       ARG(1, v) {
         c.args[0]->eval(in, e, [&](const value &pv) {
           path_t pa = path_arg(pv);
           value acc = in;
           set_path(acc, pa, 0, v);
           out(acc);
         });
       });
     }, nullptr},
    {"bsearch/1", [](BI) {
       ARG(0, target) {
         if (in.k != K::Arr) { throw jq_error(std::string(kind_name(in.k)) + " (" + error_text(in) + ") cannot be searched from"); }
         std::vector<value> v = to_vec(in);
         size_t lo = 0, hi = v.size();
         while (lo < hi) {
           size_t mid = lo + (hi - lo) / 2;
           int cmp = compare(v[mid], target);
           if (cmp == 0) { out(value::number(double(mid))); return; }
           if (cmp < 0) { lo = mid + 1; } else { hi = mid; }
         }
         out(value::number(-1.0 - double(lo)));
       });
     }, nullptr},
    {"any/2", [](BI) {
       bool r = false;
       try {
         c.args[0]->eval(in, e, [&](const value &x) {
           c.args[1]->eval(x, e, [&](const value &y) {
             if (truthy(y)) { r = true; throw stop_signal{&r}; }
           });
         });
       } catch (stop_signal &s) {
         if (s.id != &r) { throw; }
       }
       out(value::boolean(r));
     }, nullptr},
    {"all/2", [](BI) {
       bool r = true;
       try {
         c.args[0]->eval(in, e, [&](const value &x) {
           c.args[1]->eval(x, e, [&](const value &y) {
             if (!truthy(y)) { r = false; throw stop_signal{&r}; }
           });
         });
       } catch (stop_signal &s) {
         if (s.id != &r) { throw; }
       }
       out(value::boolean(r));
     }, nullptr},
};

// Math functions of one argument, as jq (C library names).
using math1 = double (*)(double);
struct math1_entry { const char *name; math1 f; };
static double significand_of(double x) {
  if (x == 0 || std::isinf(x) || std::isnan(x)) { return x; }
  int ex = 0;
  double m = std::frexp(x, &ex);
  return m * 2;
}
static double logb_of(double x) { return std::logb(x); }
static double exp10_of(double x) { return std::pow(10.0, x); }
static double gamma_of(double x) { return std::lgamma(x); }
static double drem_of2(double x, double y) { return std::remainder(x, y); }
static const math1_entry math1_table[] = {
    {"floor", [](double x) { return std::floor(x); }}, {"ceil", [](double x) { return std::ceil(x); }},
    {"round", [](double x) { return std::round(x); }}, {"sqrt", [](double x) { return std::sqrt(x); }},
    {"fabs", [](double x) { return std::fabs(x); }}, {"trunc", [](double x) { return std::trunc(x); }},
    {"nearbyint", [](double x) { return std::nearbyint(x); }}, {"rint", [](double x) { return std::rint(x); }},
    {"exp", [](double x) { return std::exp(x); }}, {"exp2", [](double x) { return std::exp2(x); }},
    {"exp10", exp10_of}, {"expm1", [](double x) { return std::expm1(x); }},
    {"log", [](double x) { return std::log(x); }}, {"log2", [](double x) { return std::log2(x); }},
    {"log10", [](double x) { return std::log10(x); }}, {"log1p", [](double x) { return std::log1p(x); }},
    {"sin", [](double x) { return std::sin(x); }}, {"cos", [](double x) { return std::cos(x); }},
    {"tan", [](double x) { return std::tan(x); }}, {"asin", [](double x) { return std::asin(x); }},
    {"acos", [](double x) { return std::acos(x); }}, {"atan", [](double x) { return std::atan(x); }},
    {"sinh", [](double x) { return std::sinh(x); }}, {"cosh", [](double x) { return std::cosh(x); }},
    {"tanh", [](double x) { return std::tanh(x); }}, {"asinh", [](double x) { return std::asinh(x); }},
    {"acosh", [](double x) { return std::acosh(x); }}, {"atanh", [](double x) { return std::atanh(x); }},
    {"cbrt", [](double x) { return std::cbrt(x); }}, {"significand", significand_of},
    {"logb", logb_of}, {"gamma", gamma_of}, {"lgamma", [](double x) { return std::lgamma(x); }},
    {"tgamma", [](double x) { return std::tgamma(x); }}, {"lgamma_r", [](double x) { return std::lgamma(x); }},
};
using math2 = double (*)(double, double);
struct math2_entry { const char *name; math2 f; };
static const math2_entry math2_table[] = {
    {"pow", [](double x, double y) { return std::pow(x, y); }}, {"atan2", [](double x, double y) { return std::atan2(x, y); }},
    {"fmod", [](double x, double y) { return std::fmod(x, y); }}, {"copysign", [](double x, double y) { return std::copysign(x, y); }},
    {"drem", drem_of2}, {"fdim", [](double x, double y) { return std::fdim(x, y); }},
    {"fmax", [](double x, double y) { return std::fmax(x, y); }}, {"fmin", [](double x, double y) { return std::fmin(x, y); }},
    {"hypot", [](double x, double y) { return std::hypot(x, y); }}, {"nextafter", [](double x, double y) { return std::nextafter(x, y); }},
    {"ldexp", [](double x, double y) { return std::ldexp(x, int(std::max(-100000.0, std::min(100000.0, y)))); }},
    {"scalb", [](double x, double y) { return std::ldexp(x, int(std::max(-100000.0, std::min(100000.0, y)))); }},
    {"scalbln", [](double x, double y) { return std::scalbln(x, long(std::max(-100000.0, std::min(100000.0, y)))); }},
    {"remainder", [](double x, double y) { return std::remainder(x, y); }},
};

struct registry {
  std::unordered_map<std::string, const builtin_entry *> map{};
};

static const registry &builtins_registry() {
  static const registry r = [] {
    registry reg;
    for (const auto &b : core_builtins) {
      if (b.fn) { reg.map[b.key] = &b; }
    }
    for (const auto &b : text_builtins()) { reg.map[b.key] = &b; }
    return reg;
  }();
  return r;
}

// Math functions dispatch on the name of the call.
static void math1_call(const call_node &c, const value &in, const env_ptr &, emit_ref out) {
  for (const auto &m : math1_table) {
    if (c.name == m.name) { out(value::number(m.f(num_arg(in)))); return; }
  }
  if (c.name == "frexp") {
    int ex = 0;
    double m = std::frexp(num_arg(in), &ex);
    out(value::array({value::number(m), value::number(ex)}));
    return;
  }
  if (c.name == "modf") {
    double ip = 0;
    double f = std::modf(num_arg(in), &ip);
    out(value::array({value::number(f), value::number(ip)}));
    return;
  }
  throw jq_error(c.key + " is not defined");
}
static void math2_call(const call_node &c, const value &in, const env_ptr &e, emit_ref out) {
  for (const auto &m : math2_table) {
    if (c.name == m.name) {
      c.args[1]->eval(in, e, [&](const value &b) {
        c.args[0]->eval(in, e, [&](const value &a) { out(value::number(m.f(num_arg(a), num_arg(b)))); });
      });
      return;
    }
  }
  throw jq_error(c.key + " is not defined");
}
static void fma_call(const call_node &c, const value &in, const env_ptr &e, emit_ref out) {
  c.args[2]->eval(in, e, [&](const value &z) {
    c.args[1]->eval(in, e, [&](const value &y) {
      c.args[0]->eval(in, e, [&](const value &x) { out(value::number(std::fma(num_arg(x), num_arg(y), num_arg(z)))); });
    });
  });
}

builtin_fn find_builtin(const std::string &key) {
  const auto &reg = builtins_registry();
  auto it = reg.map.find(key);
  if (it != reg.map.end()) { return it->second->fn; }
  size_t slash = key.rfind('/');
  std::string name = key.substr(0, slash), arity = key.substr(slash + 1);
  if (arity == "0") {
    for (const auto &m : math1_table) { if (name == m.name) { return math1_call; } }
    if (name == "frexp" || name == "modf") { return math1_call; }
  }
  if (arity == "2") {
    for (const auto &m : math2_table) { if (name == m.name) { return math2_call; } }
  }
  if (key == "fma/3") { return fma_call; }
  return nullptr;
}

builtin_path_fn find_builtin_paths(const std::string &key) {
  const auto &reg = builtins_registry();
  auto it = reg.map.find(key);
  return it == reg.map.end() ? nullptr : it->second->paths;
}

std::vector<std::string> prelude_names();

std::vector<std::string> builtin_names() {
  std::vector<std::string> r;
  for (const auto &kv : builtins_registry().map) { r.push_back(kv.first); }
  for (const auto &m : math1_table) { r.push_back(std::string(m.name) + "/0"); }
  r.push_back("frexp/0");
  r.push_back("modf/0");
  for (const auto &m : math2_table) { r.push_back(std::string(m.name) + "/2"); }
  r.push_back("fma/3");
  for (auto &n : prelude_names()) { r.push_back(n); }
  std::sort(r.begin(), r.end());
  r.erase(std::unique(r.begin(), r.end()), r.end());
  return r;
}

value indices_of(const value &in, const value &p) {
  if (in.k == K::Null) { return value::null(); }
  std::vector<value> r;
  if (in.k == K::Str) {
    if (p.k != K::Str) { throw jq_error(std::string("Cannot determine the indices of ") + kind_name(p.k) + " in a string"); }
    std::string_view s = get_str(in), q = get_str(p);
    if (!q.empty()) {
      // Offsets are in code points.
      size_t cp = 0, counted = 0;
      for (size_t pos = s.find(q); pos != std::string_view::npos; pos = s.find(q, pos + 1)) {
        cp += utf8_count(s.substr(counted, pos - counted));
        counted = pos;
        r.push_back(value::number(double(cp)));
      }
    }
  } else if (in.k == K::Arr) {
    std::vector<value> v = to_vec(in);
    if (p.k == K::Arr) {
      // A subarray: where its elements occur in sequence.
      std::vector<value> q = to_vec(p);
      if (!q.empty()) {
        for (size_t i = 0; i + q.size() <= v.size(); i++) {
          size_t j = 0;
          while (j < q.size() && compare(v[i + j], q[j]) == 0) { j++; }
          if (j == q.size()) { r.push_back(value::number(double(i))); }
        }
      }
    } else {
      for (size_t i = 0; i < v.size(); i++) {
        if (compare(v[i], p) == 0) { r.push_back(value::number(double(i))); }
      }
    }
  } else {
    throw jq_error(std::string("Cannot determine the indices of ") + kind_name(p.k) + " in " + kind_name(in.k));
  }
  return value::array(std::move(r));
}

void append_utf8(std::string &r, uint32_t cp) {
  if (cp < 0x80) { r.push_back(char(cp)); }
  else if (cp < 0x800) { r.push_back(char(0xC0 | (cp >> 6))); r.push_back(char(0x80 | (cp & 0x3F))); }
  else if (cp < 0x10000) { r.push_back(char(0xE0 | (cp >> 12))); r.push_back(char(0x80 | ((cp >> 6) & 0x3F))); r.push_back(char(0x80 | (cp & 0x3F))); }
  else { r.push_back(char(0xF0 | (cp >> 18))); r.push_back(char(0x80 | ((cp >> 12) & 0x3F))); r.push_back(char(0x80 | ((cp >> 6) & 0x3F))); r.push_back(char(0x80 | (cp & 0x3F))); }
}

// A copy of a DOM element as owned values (independent of the parser).
value to_owned(dom::element el) {
  check_stack();
  switch (el.type()) {
  case dom::element_type::ARRAY: {
    dom::array a;
    must_get(el.get_array(), a);
    std::vector<value> r;
    for (dom::element x : a) { r.push_back(to_owned(x)); }
    return value::array(std::move(r));
  }
  case dom::element_type::OBJECT: {
    dom::object o;
    must_get(el.get_object(), o);
    std::vector<std::pair<std::string, value>> r;
    for (dom::key_value_pair kv : o) {
      std::string k(kv.key);
      value v = to_owned(kv.value);
      // As jq, the last of duplicate keys wins.
      auto it = std::find_if(r.begin(), r.end(), [&](const auto &x) { return x.first == k; });
      if (it != r.end()) { it->second = std::move(v); } else { r.emplace_back(std::move(k), std::move(v)); }
    }
    return value::object(std::move(r));
  }
  case dom::element_type::STRING: {
    std::string_view s;
    must_get(el.get_string(), s);
    return value::string(std::string(s));
  }
  case dom::element_type::NULL_VALUE: return value::null();
  case dom::element_type::BOOL: {
    bool b = false;
    must_get(el.get_bool(), b);
    return value::boolean(b);
  }
  default: {
    double d = 0;
    must_get(el.get_double(), d);
    return value::number(d);
  }
  }
}

static value from_json(std::string_view s) {
  thread_local dom::parser parser;
  std::string_view t = s;
  while (!t.empty() && (t.front() == ' ' || t.front() == '\t' || t.front() == '\n' || t.front() == '\r')) { t.remove_prefix(1); }
  while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\n' || t.back() == '\r')) { t.remove_suffix(1); }
  if (t == "nan" || t == "NaN") { return value::number(NAN); }
  padded_string copy(t);
  dom::element el;
  auto err = parser.parse(copy).get(el);
  if (err) { throw jq_error(std::string(error_message(err)) + " (while parsing '" + std::string(s) + "')"); }
  return to_owned(el);
}

// The builtins written in jq. Most definitions follow jq 1.8's builtin.jq
// (MIT license).
extern const char *const PRELUDE;
const char *const PRELUDE = R"JQ(
def in(xs): . as $x | xs | has($x);
def recurse(f; cond): def r: ., (f | select(cond) | r); r;
def while(cond; update): def _w: if cond then ., (update | _w) else empty end; _w;
def until(cond; update): def _u: if cond then . else (update | _u) end; _u;
def repeat(f): def _r: f, _r; _r;
def add(f): reduce f as $x (null; . + $x);
def del(f): delpaths([path(f)]);
def paths(node_filter): . as $dot | paths | select(. as $p | $dot | getpath($p) | node_filter);
def with_entries(f): to_entries | map(f) | from_entries;
def pick(pathexps): . as $top | reduce path(pathexps) as $p (null; setpath($p; $top | getpath($p)));
def nth($n): .[$n];
def nth($n; f): if $n < 0 then error("nth doesn't support negative indices") else first(skip($n; f)) end;
def combinations: if length == 0 then [] else .[0][] as $x | (.[1:] | combinations) as $w | [$x] + $w end;
def combinations(n): . as $dot | [range(n)] | map($dot) | combinations;
def walk(f): def w: if type == "object" then map_values(w) elif type == "array" then map(w) else . end | f; w;
def transpose: [range(0; map(length) | max // 0) as $i | [.[][$i]]];
def isvalid(f): try (f | true) catch false;
def IN(s): any(s == .; .);
def IN(src; s): any(src == s; .);
def INDEX(stream; idx_expr): reduce stream as $row ({}; .[$row | idx_expr | tostring] |= $row);
def INDEX(idx_expr): INDEX(.[]; idx_expr);
def toarray: if type == "array" then . else [.] end;
def JOIN($idx; idx_expr): [.[] | [., $idx[idx_expr]]];
def JOIN($idx; stream; idx_expr): stream | [., $idx[idx_expr]];
def JOIN($idx; stream; idx_expr; join_expr): stream | [., $idx[idx_expr]] | join_expr;
def trimstr($x): ltrimstr($x) | rtrimstr($x);
def finites: select(isinfinite or isnan | not);
def normals: select(isnormal);
def todate: strftime("%Y-%m-%dT%H:%M:%SZ");
def todateiso8601: strftime("%Y-%m-%dT%H:%M:%SZ");
def fromdateiso8601: strptime("%Y-%m-%dT%H:%M:%SZ") | mktime;
def fromdate: fromdateiso8601;
def date: todate;
def dateadd(u; n): . + n;
def datesub(u; n): . - n;
def splits($re; flags): split($re; flags) | .[];
def splits($re): splits($re; null);
def ascii: [.] | implode;
def debug(msg): (msg | debug | empty), .;
def halt_error: halt_error(5);
def tostream: path(def r: (.[]? | r), .; r) as $p | getpath($p) | reduce path(.[]?) as $q ([$p, .]; [$p + $q]);
def fromstream(f): { x: null, e: false } as $init
  | foreach f as $i ($init;
      if .e then $init else . end
      | if $i | length == 2
        then setpath(["e"]; $i[0] | length == 0) | setpath(["x"] + $i[0]; $i[1])
        else setpath(["e"]; $i[0] | length == 1) end;
      if .e then .x else empty end);
def truncate_stream(stream): . as $n | null | stream | . as $input | if (.[0] | length) > $n then setpath([0]; .[0][$n:]) else empty end;
)JQ";

} // namespace sjq
