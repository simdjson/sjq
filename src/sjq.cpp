// sjq: a jq-compatible JSON processor built on simdjson.
#include "ast.h"
#include "builtins.h"
#include "input.h"
#include "ondemand.h"
#include "writer.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace sjq;

static void usage(FILE *f) {
  fprintf(f,
          "usage: sjq [OPTIONS] FILTER [FILE...]\n"
          "       sjq [OPTIONS] -f FILTER_FILE [FILE...]\n"
          "  -c, --compact-output      one line per output\n"
          "  -r, --raw-output          write strings without quotes\n"
          "  -j, --join-output         like -r, without newlines\n"
          "      --raw-output0         like -r, with a NUL after each output\n"
          "  -a, --ascii-output        escape non-ASCII characters\n"
          "  -n, --null-input          run the filter once on null\n"
          "  -s, --slurp               read all inputs into one array\n"
          "  -R, --raw-input           read lines as strings (with -s: the whole input)\n"
          "  -e, --exit-status         exit status from the last output\n"
          "  -S, --sort-keys           sort object keys\n"
          "  -C, --color-output        colorize output (default when stdout is a terminal)\n"
          "  -M, --monochrome-output\n"
          "      --tab                 indent with tabs\n"
          "      --indent n            indent with n spaces (-1 to 7; -1 means tabs)\n"
          "  -f, --from-file file      read the filter from a file\n"
          "      --arg name value      set $name to the string value\n"
          "      --argjson name json   set $name to the JSON value\n"
          "      --slurpfile name file set $name to an array of the JSON values in file\n"
          "      --rawfile name file   set $name to the contents of file\n"
          "      --args                the remaining arguments are strings ($ARGS.positional)\n"
          "      --jsonargs            the remaining arguments are JSON values\n"
          "      --seq                 write an RS before each output; read RS-separated input\n"
          "      --unbuffered          flush after each input\n"
          "  -b, --binary              on Windows, write LF line ends (no CR)\n"
          "      --threads n           worker threads for large files (1: sequential)\n"
          "      --no-od               do not use the On-Demand fast path\n"
          "      --no-mmap             read files instead of mapping them\n"
          "  -V, --version\n"
          "  -h, --help\n");
}

// Reads a whole file (or standard input for "-"); false when it cannot.
static bool read_file(const std::string &name, std::string &out) {
  out.clear();
  if (name == "-") {
    char buf[65536];
    while (true) {
      long long r = read_stdin(buf, sizeof(buf));
      if (r < 0) { return false; }
      if (r == 0) { return true; }
      out.append(buf, size_t(r));
    }
  }
  input_source src;
  if (!src.load(name, false)) { return false; }
  out.assign(src.data(), src.size());
  return true;
}

// Parses every JSON value of text as owned values; false on a parse error.
static bool parse_all(const std::string &text, std::vector<value> &out, std::string &err) {
  dom::parser parser;
  padded_string copy(text);
  dom::document_stream stream;
  if (auto e = parser.parse_many(copy, std::max<size_t>(copy.size(), 1024)).get(stream)) {
    err = error_message(e);
    return false;
  }
  for (auto it = stream.begin(); it != stream.end(); ++it) {
    dom::element doc;
    auto e = (*it).get(doc);
    if (e == EMPTY) { break; }
    if (e) {
      err = error_message(e);
      return false;
    }
    out.push_back(to_owned(doc));
  }
  if (stream.truncated_bytes()) {
    err = "Unfinished JSON term at EOF";
    return false;
  }
  return true;
}

// Default colors and JQ_COLORS, as jq 1.8: null, false, true, numbers,
// strings, arrays, objects, object keys.
static void setup_colors(std::string *colors) {
  static const char *defaults[C_COUNT] = {"0;90", "0;39", "0;39", "0;39", "0;32", "1;39", "1;39", "1;34"};
  std::string specs[C_COUNT];
  for (int i = 0; i < C_COUNT; i++) { specs[i] = defaults[i]; }
  if (const char *env = getenv("JQ_COLORS")) {
    std::string e(env);
    std::string parsed[C_COUNT];
    for (int i = 0; i < C_COUNT; i++) { parsed[i] = specs[i]; }
    bool ok = true;
    size_t pos = 0;
    for (int i = 0; i < C_COUNT && pos <= e.size(); i++) {
      size_t colon = e.find(':', pos);
      std::string field = e.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
      if (field.empty() || field.size() > 16 || field.find_first_not_of("0123456789;") != std::string::npos) { ok = false; break; }
      parsed[i] = field;
      if (colon == std::string::npos) { break; }
      pos = colon + 1;
    }
    if (ok) {
      for (int i = 0; i < C_COUNT; i++) { specs[i] = parsed[i]; }
    } else {
      fprintf(stderr, "Failed to set $JQ_COLORS\n");
    }
  }
  for (int i = 0; i < C_COUNT; i++) { colors[i] = "\x1b[" + specs[i] + "m"; }
}

// Compiles the filter, or reports the syntax error and returns null. With
// MinGW GCC 16 on Windows, catching the parser's exception in main itself
// crashed about half the runs (access violation); a separate function does
// not.
simdjson_never_inline node_ptr compile_or_report(const std::string &filter, const std::vector<std::string> &globals, compile_info &info) {
  try {
    return compile(filter, globals, &info);
  } catch (std::exception &ex) {
    fprintf(stderr, "sjq: error: %s\n", ex.what());
    return nullptr;
  }
}

int main(int argc, char **argv) {
  stack_guard_init(main_stack_budget());
  writer w{};
  bool raw = false, join = false, null_input = false, seq = false, unbuffered = false, binary = false;
  bool slurp = false, raw_input = false, exit_status = false, raw0 = false;
  int color_mode = -1;  // -1: when stdout is a terminal
  int threads = int(std::max(1u, std::thread::hardware_concurrency()));
  bool use_od = true, use_mmap = true;
  std::string filter{};
  bool have_filter = false;
  std::string filter_file{};
  std::vector<std::string> files{};
  // --arg and friends: the variables, in order, and $ARGS.
  std::vector<std::pair<std::string, value>> named{};
  std::vector<value> positional{};
  int rest_mode = 0;  // after --args (1) or --jsonargs (2): positional values
  bool options_done = false;
  auto need = [&](int i, int count, const std::string &opt) {
    if (i + count >= argc) {
      fprintf(stderr, "sjq: %s takes %d parameter%s\n", opt.c_str(), count, count > 1 ? "s" : "");
      exit(2);
    }
  };
  auto set_named = [&](const std::string &name, const value &v) {
    for (auto &kv : named) {
      if (kv.first == name) { kv.second = v; return; }
    }
    named.emplace_back(name, v);
  };
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    bool is_option = !options_done && a.size() > 1 && a[0] == '-';
    if (!is_option) {
      if (!have_filter && filter_file.empty()) { filter = a; have_filter = true; continue; }
      if (rest_mode == 1) { positional.push_back(value::string(a)); continue; }
      if (rest_mode == 2) {
        std::vector<value> vs;
        std::string err;
        if (!parse_all(a, vs, err) || vs.size() != 1) {
          fprintf(stderr, "sjq: invalid JSON text passed to --jsonargs\n");
          return 2;
        }
        positional.push_back(vs[0]);
        continue;
      }
      files.push_back(a);
      continue;
    }
    if (a == "--") { options_done = true; continue; }
    if (a == "-c" || a == "--compact-output") { w.pretty = false; }
    else if (a == "-r" || a == "--raw-output") { raw = true; }
    else if (a == "-j" || a == "--join-output") { raw = true; join = true; }
    else if (a == "--raw-output0") { raw = true; raw0 = true; }
    else if (a == "-a" || a == "--ascii-output") { w.ascii = true; }
    else if (a == "-n" || a == "--null-input") { null_input = true; }
    else if (a == "-s" || a == "--slurp") { slurp = true; }
    else if (a == "-R" || a == "--raw-input") { raw_input = true; }
    else if (a == "-e" || a == "--exit-status") { exit_status = true; }
    else if (a == "-S" || a == "--sort-keys") { w.sort_keys = true; }
    else if (a == "-C" || a == "--color-output") { color_mode = 1; }
    else if (a == "-M" || a == "--monochrome-output") { color_mode = 0; }
    else if (a == "-V" || a == "--version") { printf("sjq %s (simdjson %s)\n", SJQ_VERSION, SIMDJSON_VERSION); return 0; }
    else if (a == "-f" || a == "--from-file") {
      need(i, 1, a);
      filter_file = argv[++i];
      if (have_filter) {
        // The first positional argument was an input file, not the filter.
        files.insert(files.begin(), filter);
        have_filter = false;
      }
    }
    else if (a == "--arg") { need(i, 2, a); set_named(argv[i + 1], value::string(argv[i + 2])); i += 2; }
    else if (a == "--argjson") {
      need(i, 2, a);
      std::vector<value> vs;
      std::string err;
      if (!parse_all(argv[i + 2], vs, err) || vs.size() != 1) {
        fprintf(stderr, "sjq: invalid JSON text passed to --argjson\n");
        return 2;
      }
      set_named(argv[i + 1], vs[0]);
      i += 2;
    }
    else if (a == "--slurpfile" || a == "--rawfile") {
      need(i, 2, a);
      std::string text;
      if (!read_file(argv[i + 2], text)) {
        fprintf(stderr, "sjq: error: could not open %s\n", argv[i + 2]);
        return 2;
      }
      if (a == "--rawfile") {
        set_named(argv[i + 1], value::string(text));
      } else {
        std::vector<value> vs;
        std::string err;
        if (!parse_all(text, vs, err)) {
          fprintf(stderr, "sjq: error: %s: %s\n", argv[i + 2], err.c_str());
          return 2;
        }
        set_named(argv[i + 1], value::array(std::move(vs)));
      }
      i += 2;
    }
    else if (a == "--args") { rest_mode = 1; }
    else if (a == "--jsonargs") { rest_mode = 2; }
    else if (a == "--tab") { w.tab = true; w.pretty = true; }
    else if (a == "--indent") {
      if (i + 1 >= argc) { fprintf(stderr, "sjq: --indent takes one parameter\n"); return 2; }
      char *endp = nullptr;
      long n = strtol(argv[++i], &endp, 10);
      if (*endp || n < -1 || n > 7) { fprintf(stderr, "sjq: --indent takes a number between -1 and 7\n"); return 2; }
      w.pretty = true;
      w.tab = n == -1;
      w.indent = n < 0 ? 0 : int(n);
    }
    else if (a == "--seq") { seq = true; }
    else if (a == "--unbuffered") { unbuffered = true; }
    else if (a == "-b" || a == "--binary") { binary = true; }
    else if (a == "--no-od") { use_od = false; }
    else if (a == "--no-mmap") { use_mmap = false; }
    else if (a == "--threads") {
      if (i + 1 >= argc) { fprintf(stderr, "sjq: --threads takes one parameter\n"); return 2; }
      threads = std::max(1, atoi(argv[++i]));
    }
    else if (a == "-h" || a == "--help") { usage(stdout); return 0; }
    else if (a[1] != '-' && a.find_first_not_of("crjnSMCbsRea", 1) == std::string::npos) {
      // Combined short flags such as -rc.
      for (size_t j = 1; j < a.size(); j++) {
        switch (a[j]) {
        case 'c': w.pretty = false; break;
        case 'r': raw = true; break;
        case 'j': raw = join = true; break;
        case 'n': null_input = true; break;
        case 'S': w.sort_keys = true; break;
        case 'C': color_mode = 1; break;
        case 'M': color_mode = 0; break;
        case 'b': binary = true; break;
        case 's': slurp = true; break;
        case 'R': raw_input = true; break;
        case 'e': exit_status = true; break;
        case 'a': w.ascii = true; break;
        }
      }
    }
    else {
      fprintf(stderr, "sjq: unknown option %s (see sjq --help)\n", a.c_str());
      return 2;
    }
  }
  if (!filter_file.empty()) {
    if (!read_file(filter_file, filter)) {
      fprintf(stderr, "sjq: error: could not open %s\n", filter_file.c_str());
      return 2;
    }
    have_filter = true;
  }
  if (!have_filter) { usage(stderr); return 2; }

  static std::string colors[C_COUNT];
  const char *no_color = getenv("NO_COLOR");
  const bool terminal = stdout_is_terminal();
  const bool can_color = prepare_console(binary);
  w.color = color_mode == 1 || (color_mode == -1 && terminal && can_color && !(no_color && *no_color));
  if (w.color) {
    setup_colors(colors);
    w.colors = colors;
  }
  // Output reaches a terminal (or the user asked for it) as each input is done.
  const bool flush_each = unbuffered || terminal;

  std::vector<std::string> globals{"ARGS"};
  for (auto &kv : named) { globals.push_back(kv.first); }
  compile_info info{};
  node_ptr prog = compile_or_report(filter, globals, info);
  if (!prog) { return 3; }
  // The variables of --arg and friends, and $ARGS.
  env_ptr root_env{};
  root_env = push_var("ARGS", value::object({{"positional", value::array(positional)}, {"named", value::object(named)}}), root_env);
  for (auto &kv : named) { root_env = push_var(kv.first, kv.second, root_env); }

  // With -e, the exit status depends on the last output: the outputs must be
  // seen in order, by the main thread. Regular expressions recurse deeply in
  // std::regex: they stay on the main thread with its large stack.
  if (exit_status || info.uses_regex) { threads = 1; }
  od_ptr plan{};
  if (use_od && !w.sort_keys && !w.ascii && !raw0 && !exit_status) { plan = od_compile(prog.get()); }
  if (getenv("SJQ_DEBUG")) { fprintf(stderr, "sjq: %s path\n", plan ? "On-Demand" : "DOM"); }

  int status = 0;
  int last_output = -1;  // -e: -1 no output, 0 false or null, 1 other
  auto emit = [&](const value &v) {
    if (seq) { w.put('\x1e'); }
    if (raw && v.k == K::Str) { w.put(get_str(v)); }
    else { w.write_value(v, 0); }
    if (raw0) { w.put('\0'); }
    else if (!join) { w.put('\n'); }
    last_output = truthy(v) ? 1 : 0;
    w.maybe_flush();
  };
  auto final_status = [&](int st) {
    if (!exit_status || st != 0) { return st; }
    return last_output < 0 ? 4 : (last_output == 0 ? 1 : 0);
  };
  // jq reports the number of lines consumed when an error happens. line_count
  // counts the newlines of the current buffer before line_pos.
  const char *line_base = nullptr;
  size_t line_pos = 0, line_count = 0, doc_end = 0;
  auto count_lines_to = [&](size_t end) {
    if (end <= line_pos) { return; }
    line_count += size_t(std::count(line_base + line_pos, line_base + end, '\n'));
    line_pos = end;
  };
  auto location = [&](const char *name) {
    std::string loc(name);
    if (line_base) {
      count_lines_to(doc_end);
      loc += ":" + std::to_string(line_count);
    }
    return loc;
  };
  auto run = [&](const value &in, const char *name) {
    try {
      prog->eval(in, root_env, emit);
      status = 0;
    } catch (jq_error &err) {
      w.flush();
      if (err.has_payload && err.payload.k != K::Str) {
        fprintf(stderr, "sjq: error (at %s) (not a string): %s\n", location(name).c_str(), to_json(err.payload).c_str());
      } else {
        fprintf(stderr, "sjq: error (at %s): %s\n", location(name).c_str(), err.what());
      }
      status = 5;
    } catch (halt_signal &h) {
      w.flush();
      fputs(h.message.c_str(), stderr);
      fflush(stderr);
      exit(h.code);
    } catch (break_signal &) {
      w.flush();
      fprintf(stderr, "sjq: error (at %s): break\n", name);
      status = 5;
    } catch (std::exception &ex) {
      w.flush();
      fprintf(stderr, "sjq: error (at %s): %s\n", location(name).c_str(), ex.what());
      status = 5;
    }
  };

  if (files.empty()) { files.push_back("-"); }

  // -s, -R and filters that call input/inputs read the inputs in order, as
  // owned values (a value must outlive the reading of the next one).
  if (slurp || raw_input || info.uses_input) {
    size_t next_file = 0;
    std::vector<value> pending;
    size_t pending_at = 0;
    bool slurped = false, failed = false;
    std::string current_name;
    // Loads the values of the next file into pending; false at the end.
    auto load_next = [&]() -> bool {
      while (next_file < files.size()) {
        const std::string &fname = files[next_file++];
        current_name = fname;
        g_input_filename = fname == "-" ? nullptr : fname.c_str();
        std::string text;
        if (!read_file(fname, text)) {
          fprintf(stderr, "sjq: error: could not open %s\n", fname.c_str());
          status = 2;
          continue;
        }
        pending.clear();
        pending_at = 0;
        if (raw_input) {
          size_t pos = 0;
          while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) { pending.push_back(value::string(text.substr(pos))); break; }
            pending.push_back(value::string(text.substr(pos, nl - pos)));
            pos = nl + 1;
          }
        } else {
          std::string err;
          if (!parse_all(text, pending, err)) {
            w.flush();
            fprintf(stderr, "sjq: error (at %s): %s\n", fname == "-" ? "<stdin>" : fname.c_str(), err.c_str());
            status = 2;
            failed = true;
          }
        }
        if (!pending.empty()) { return true; }
        if (failed) { return false; }
      }
      return false;
    };
    // With -s, all inputs make one value (a string with -R).
    std::function<bool(value &)> next_value;
    if (slurp) {
      next_value = [&](value &v) -> bool {
        if (slurped) { return false; }
        slurped = true;
        if (raw_input) {
          std::string all;
          for (const auto &fname : files) {
            std::string text;
            if (!read_file(fname, text)) {
              fprintf(stderr, "sjq: error: could not open %s\n", fname.c_str());
              status = 2;
              continue;
            }
            all += text;
          }
          v = value::string(std::move(all));
          return true;
        }
        std::vector<value> all;
        while (load_next()) {
          for (auto &x : pending) { all.push_back(std::move(x)); }
          pending.clear();
        }
        v = value::array(std::move(all));
        return true;
      };
    } else {
      next_value = [&](value &v) -> bool {
        while (pending_at >= pending.size()) {
          if (failed || !load_next()) { return false; }
        }
        v = std::move(pending[pending_at++]);
        return true;
      };
    }
    g_next_input = next_value;
    if (null_input) {
      run(value::null(), "<unknown>");
    } else {
      value v;
      while (next_value(v)) {
        value in = std::move(v);
        run(in, current_name == "-" ? "<stdin>" : current_name.c_str());
      }
    }
    if (failed) { status = 2; }
    w.flush();
    return final_status(status);
  }

  if (null_input) {
    run(value::null(), "<unknown>");
    w.flush();
    return final_status(status);
  }
  if (use_mmap) { install_fault_handler(); }
  dom::parser parser{};
  ondemand::parser od_parser{};
  // The mapped input being read, to tell errors caused by a file changing
  // under sjq from errors in the input.
  const input_source *current = nullptr;
  auto input_changed = [&]() { return current && current->changed(); };
  bool fatal = false;  // a parse error ends the input, as in jq

  // Filters the documents of data[offset, len). When at_eof is false, an
  // incomplete last document is left for later. Returns the offset where
  // processing stopped: len, or the start of the incomplete document.
  auto run_sequential = [&](const char *data, size_t len, size_t offset, bool at_eof, stream_format fmt, const char *name) -> size_t {
    // If a document does not fit in a batch, the stream restarts there with a
    // larger batch.
    size_t batch = std::max<size_t>(size_t(1) << 20, std::min<size_t>(len - offset, size_t(64) << 20));
    while (offset < len) {
      size_t slen = len - offset;
      const uint8_t *sbuf = reinterpret_cast<const uint8_t *>(data + offset);
      size_t done = slen;
      bool retry = false, failed = false;
      size_t truncated = 0;
      // Handles one parsed document; returns false to stop.
      auto stream_error = [&](error_code err, size_t index) {
        if (err == CAPACITY && batch < slen) {
          done = index;
          retry = true;
          return false;
        }
        if (err == EMPTY) { return false; }
        w.flush();
        if (!input_changed()) { fprintf(stderr, "sjq: error (at %s): %s\n", name, error_message(err)); }
        status = 5;
        failed = fatal = true;
        return false;
      };
      if (plan) {
        ondemand::document_stream stream;
        if (od_parser.iterate_many(sbuf, slen, batch, fmt).get(stream)) {
          fprintf(stderr, "sjq: error: cannot parse %s\n", name);
          status = 2;
          fatal = true;
          return len;
        }
        for (auto it = stream.begin(); it != stream.end(); ++it) {
          ondemand::document_reference doc;
          auto err = (*it).get(doc);
          if (err) {
            if (!stream_error(err, it.current_index())) { break; }
          }
          if (g_input_changed.load()) { failed = true; break; }
          std::string_view src = it.source();
          size_t end = offset + it.current_index() + src.size();
          if (end < len && data[end] == '\n') { end++; }
          doc_end = end;
          if (od_run_doc(*plan, doc, w, raw, join, seq)) {
            status = 0;
          } else {
            dom::element el;
            auto derr = parser.parse(src.data(), src.size(), true).get(el);
            if (derr) {
              w.flush();
              if (!input_changed()) { fprintf(stderr, "sjq: error (at %s): %s\n", name, error_message(derr)); }
              status = 5;
              failed = fatal = true;
              break;
            }
            run(value::of(el), name);
          }
          if (flush_each) { w.flush(); } else { w.maybe_flush(); }
        }
        if (!retry && !failed) { truncated = stream.truncated_bytes(); }
      } else {
        dom::document_stream stream;
        if (parser.parse_many(sbuf, slen, batch, fmt).get(stream)) {
          fprintf(stderr, "sjq: error: cannot parse %s\n", name);
          status = 2;
          fatal = true;
          return len;
        }
        for (auto it = stream.begin(); it != stream.end(); ++it) {
          dom::element doc;
          auto err = (*it).get(doc);
          if (err) {
            if (!stream_error(err, it.current_index())) { break; }
          }
          if (g_input_changed.load()) { failed = true; break; }
          // End of this document plus the newline jq consumes after it.
          size_t end = offset + it.current_index() + it.source().size();
          if (end < len && data[end] == '\n') { end++; }
          doc_end = end;
          run(value::of(doc), name);
          if (flush_each) { w.flush(); } else { w.maybe_flush(); }
        }
        if (!retry && !failed) { truncated = stream.truncated_bytes(); }
      }
      if (truncated) {
        if (!at_eof) { return offset + slen - truncated; }
        w.flush();
        if (!input_changed()) { fprintf(stderr, "sjq: error (at %s): Unfinished JSON term at EOF\n", name); }
        status = 5;
        fatal = true;
      }
      if (failed) { return len; }
      offset += done;
      if (retry) { batch = len - offset + 1; }
      else { return len; }
    }
    return len;
  };

  // Parallel processing: the input is cut into chunks right after newlines
  // and each worker filters whole chunks into its own buffer; the main thread
  // writes the buffers in order. A cut inside a document makes the chunk
  // before it end with an incomplete document (or an error), and so does the
  // first bad cut, so any failed chunk sends the rest of the input, from that
  // chunk on, to the sequential path, which also reports errors like jq.
  // Returns the offset where sequential processing must resume.
  auto run_parallel = [&](const input_source &input) -> size_t {
    const size_t chunk_target = size_t(1) << 20;
    std::vector<size_t> starts;
    for (size_t pos = 0; pos < input.size();) {
      starts.push_back(pos);
      size_t cut = pos + chunk_target;
      if (cut >= input.size()) { break; }
      const void *nl = memchr(input.data() + cut, '\n', input.size() - cut);
      if (!nl) { break; }
      pos = size_t(static_cast<const char *>(nl) - input.data()) + 1;
    }
    const size_t n = starts.size();
    starts.push_back(input.size());
    struct slot {
      std::string out{};
      bool ok{false};
      bool done{false};
    };
    std::vector<slot> slots(n);
    std::mutex mu;
    std::condition_variable cv_done, cv_window;
    std::atomic<size_t> next{0};
    std::atomic<bool> abort{false};
    size_t flushed = 0;
    const size_t window = size_t(threads) * 4;
    auto worker = [&]() {
      // Worker stacks are small (512 KiB on macOS): deeper values make the
      // chunk fail, and the main thread redoes it.
      g_max_depth = 512;
      stack_guard_init(size_t(384) << 10);
      dom::parser local_parser;
      ondemand::parser local_od;
      writer lw;
      lw.copy_format(w);
      lw.to_memory = true;
      auto lemit = [&](const value &v) {
        if (seq) { lw.put('\x1e'); }
        if (raw && v.k == K::Str) { lw.put(get_str(v)); }
        else { lw.write_value(v, 0); }
        if (!join) { lw.put('\n'); }
      };
      while (true) {
        size_t k = next.fetch_add(1);
        if (k >= n) { return; }
        {
          std::unique_lock<std::mutex> lock(mu);
          cv_window.wait(lock, [&] { return k < flushed + window || abort.load(); });
        }
        bool ok = !abort.load();
        size_t len = starts[k + 1] - starts[k];
        const uint8_t *cbuf = reinterpret_cast<const uint8_t *>(input.data() + starts[k]);
        try {
          if (ok && plan) {
            ondemand::document_stream ostream;
            if (local_od.iterate_many(cbuf, len, std::max<size_t>(len, 4096)).get(ostream)) { ok = false; }
            if (ok) {
              for (auto it = ostream.begin(); it != ostream.end(); ++it) {
                ondemand::document_reference doc;
                auto err = (*it).get(doc);
                if (err == EMPTY) { break; }
                if (err || g_input_changed.load()) { ok = false; break; }
                if (od_run_doc(*plan, doc, lw, raw, join, seq)) { continue; }
                std::string_view src = it.source();
                dom::element el;
                if (local_parser.parse(src.data(), src.size(), true).get(el)) { ok = false; break; }
                prog->eval(value::of(el), root_env, lemit);
              }
              if (ok && ostream.truncated_bytes()) { ok = false; }
            }
          } else if (ok) {
            dom::document_stream stream;
            if (local_parser.parse_many(cbuf, len, std::max<size_t>(len, 4096)).get(stream)) { ok = false; }
            if (ok) {
              for (auto it = stream.begin(); it != stream.end(); ++it) {
                dom::element doc;
                auto err = (*it).get(doc);
                if (err == EMPTY) { break; }
                if (err || g_input_changed.load()) { ok = false; break; }
                prog->eval(value::of(doc), root_env, lemit);
              }
              if (ok && stream.truncated_bytes()) { ok = false; }
            }
          }
        } catch (...) {
          ok = false;
        }
        std::string out;
        if (ok) { out.swap(lw.buf); } else { lw.buf.clear(); }
        {
          std::lock_guard<std::mutex> lock(mu);
          slots[k].out = std::move(out);
          slots[k].ok = ok;
          slots[k].done = true;
        }
        cv_done.notify_all();
      }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++) { pool.emplace_back(worker); }
    size_t resume = input.size();
    for (size_t k = 0; k < n; k++) {
      std::string out;
      bool ok;
      {
        std::unique_lock<std::mutex> lock(mu);
        cv_done.wait(lock, [&] { return slots[k].done; });
        out.swap(slots[k].out);
        ok = slots[k].ok;
        if (!ok) { abort = true; }
        flushed = k + 1;
      }
      cv_window.notify_all();
      if (!ok) { resume = starts[k]; break; }
      fwrite(out.data(), 1, out.size(), stdout);
      if (flush_each) { fflush(stdout); }
    }
    for (auto &t : pool) { t.join(); }
    return resume;
  };

  // --seq input: RS-separated when the input starts with RS.
  auto input_format = [&](const char *data, size_t len) {
    if (seq) {
      for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { continue; }
        if (c == '\x1e') { return stream_format::json_sequence; }
        break;
      }
    }
    return stream_format::whitespace_delimited;
  };

  auto run_input = [&](const input_source &input, const char *name) {
    current = &input;
    line_base = input.data();
    line_pos = line_count = doc_end = 0;
    stream_format fmt = input_format(input.data(), input.size());
    size_t offset = 0;
    if (threads > 1 && fmt == stream_format::whitespace_delimited && input.size() >= (size_t(4) << 20)) {
      w.flush();
      offset = run_parallel(input);
      if (offset == input.size()) { status = 0; }
    }
    if (offset < input.size() && !g_input_changed.load()) {
      run_sequential(input.data(), input.size(), offset, true, fmt, name);
    }
    if (input.changed()) {
      w.flush();
      fprintf(stderr, "sjq: error: %s changed while being read; the output is incomplete\n", name);
      status = 2;
    }
    current = nullptr;
  };

  // Reads a pipe or terminal as it comes: each read is processed up to its
  // last newline (a cut there cannot split a scalar, and an incomplete
  // container is left for the next round). After a round that makes no
  // progress, processing waits until the buffer has doubled, so a large
  // document arriving in pieces is parsed a logarithmic number of times.
  auto run_stream = [&](const char *name) {
    // A round that makes progress lifts the doubling rule.
    const size_t block = size_t(1) << 20;
    std::vector<char> buf(block + SIMDJSON_PADDING);
    size_t filled = 0, retry_at = 0;
    bool eof = false, fmt_known = false;
    stream_format fmt = stream_format::whitespace_delimited;
    line_pos = line_count = doc_end = 0;
    while (!fatal) {
      if (!eof) {
        if (buf.size() < filled + block + SIMDJSON_PADDING) { buf.resize(std::max(buf.size() * 2, filled + block + SIMDJSON_PADDING)); }
        long long r = read_stdin(buf.data() + filled, block);
        if (r < 0) {
          w.flush();
          fprintf(stderr, "sjq: error: cannot read %s\n", name);
          status = 2;
          fatal = true;
          break;
        }
        if (r == 0) { eof = true; } else { filled += size_t(r); }
      }
      size_t cut = filled;
      if (!eof) {
        // While more input is ready, keep reading up to a block, and after a
        // round without progress, until the buffer has doubled. Once the
        // producer is idle, process what is there.
        bool more_ready = stdin_has_more();
        if (more_ready && (filled < block || filled < retry_at)) { continue; }
        const char *nl = nullptr;
        for (size_t i = filled; i > 0; i--) {
          if (buf[i - 1] == '\n') { nl = buf.data() + i - 1; break; }
        }
        if (!nl) { continue; }
        cut = size_t(nl - buf.data()) + 1;
      } else {
        memset(buf.data() + filled, 0, SIMDJSON_PADDING);
      }
      if (!fmt_known && cut > 0) {
        fmt = input_format(buf.data(), cut);
        fmt_known = true;
      }
      line_base = buf.data();
      size_t consumed = cut == 0 ? 0 : run_sequential(buf.data(), cut, 0, eof, fmt, name);
      if (eof || fatal) { break; }
      retry_at = consumed == 0 ? filled * 2 : 0;
      count_lines_to(consumed);
      memmove(buf.data(), buf.data() + consumed, filled - consumed);
      filled -= consumed;
      line_pos -= consumed;
      if (doc_end >= consumed) { doc_end -= consumed; } else { doc_end = 0; }
    }
    line_base = nullptr;
  };

  for (const std::string &fname : files) {
    fatal = false;
    g_input_filename = fname == "-" ? nullptr : fname.c_str();
    if (fname == "-") {
      if (stdin_is_regular_file()) {
        input_source input;
        if (input.load_stdin_file(use_mmap)) {
          run_input(input, "<stdin>");
          continue;
        }
      }
      run_stream("<stdin>");
      continue;
    }
    input_source input;
    if (!input.load(fname, use_mmap)) {
      fprintf(stderr, "sjq: error: could not open %s\n", fname.c_str());
      status = 2;
      continue;
    }
    run_input(input, fname.c_str());
  }
  w.flush();
  return final_status(status);
}
