#ifndef SJQ_INPUT_H
#define SJQ_INPUT_H

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

namespace sjq {

// Set when reading the mapped input faulted because the file shrank.
extern std::atomic<bool> g_input_changed;

// The bytes of one input file, followed by at least SIMDJSON_PADDING readable
// bytes. Regular files are memory-mapped with simdjson::padded_memory_map
// when possible, else read into a padded_string.
class input_source {
public:
  input_source();
  ~input_source();
  input_source(const input_source &) = delete;
  input_source &operator=(const input_source &) = delete;

  // Returns false if the file cannot be opened.
  bool load(const std::string &fname, bool use_mmap);
  // Standard input redirected from a regular file.
  bool load_stdin_file(bool use_mmap);

  const char *data() const;
  size_t size() const;
  // True when the mapped file shrank while it was read.
  bool changed() const;
  // Replaces the contents (a file converted from UTF-16).
  void adopt(const std::string &text);

private:
  struct impl;
  std::unique_ptr<impl> p_;
};

// -o FILE. Why FILE cannot be written (empty when it can), checked before
// the run without changing FILE.
std::string output_problem(const std::string &fname);
// The output of -o FILE: a temporary file next to the real FILE (symbolic
// links followed), created exclusively with the permissions and owner of
// FILE, then renamed over it (ReplaceFileW on Windows, which keeps the ACLs).
// The replacement is atomic; a hard link to FILE keeps the old content. A
// device or a pipe (/dev/null, NUL) is written directly.
struct output_file {
  FILE *f{nullptr};
#if defined(_WIN32)
  std::wstring wtmp{}, wtarget{};
#else
  std::string tmp{}, target{};
#endif
};
bool open_output(const std::string &fname, output_file &out, std::string &why);
// Puts the output in place; false with the reason in why when it cannot
// (FILE is then unchanged).
bool commit_output(output_file &out, std::string &why);
// Drops the output: FILE is unchanged.
void discard_output(output_file &out);

// Why a file cannot be read ("No such file or directory", ...).
std::string open_failure_reason(const std::string &fname);

// Turns a fault while reading the mapped input into an error instead of a
// crash (see input.cpp).
void install_fault_handler();

// How much of the main thread's stack sjq may use (see check_stack).
size_t main_stack_budget();

bool stdout_is_terminal();
// True when standard input is a non-empty regular file.
bool stdin_is_regular_file();
// Reads standard input: the number of bytes read, 0 at the end, -1 on error.
long long read_stdin(char *dst, size_t n);
// True when more standard input can be read without waiting.
bool stdin_has_more();
// Puts standard input (and standard output with binary_output) in binary
// mode and, on a Windows console, turns on escape sequences. Returns false
// when the terminal cannot show colors.
bool prepare_console(bool binary_output);

} // namespace sjq

#endif // SJQ_INPUT_H
