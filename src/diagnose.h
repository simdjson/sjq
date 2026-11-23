#ifndef SJQ_DIAGNOSE_H
#define SJQ_DIAGNOSE_H

// Plain error messages for invalid input: simdjson reports an error code;
// these functions find where the problem is and say what it is.
#include "simdjson.h"

#include <cstddef>
#include <string>

namespace sjq {

struct json_problem {
  bool found{false};
  size_t offset{0};  // of the problem, from the start of the scanned text
  std::string message{};
};

// Scans a sequence of JSON values separated by white space and returns its
// first problem; with single, more than one value is a problem.
json_problem find_json_problem(const char *data, size_t len, bool single = false);

// simdjson's error code in words, without its internal name.
std::string plain_error(simdjson::error_code err);

// "line L, column C" of an offset of text (columns count characters).
std::string line_and_column(const char *text, size_t offset);

// The message for invalid JSON text: where the problem is and what it is,
// falling back on the error code when the scan finds nothing.
std::string describe_json_error(const char *text, size_t len, simdjson::error_code err, bool single = false);

// UTF-16 input (Windows PowerShell 5 writes it with >): true when text
// starts with a UTF-16 byte order mark; out then holds the text in UTF-8.
bool utf16_with_bom_to_utf8(const char *text, size_t len, std::string &out);
// True when text looks like UTF-16 without a byte order mark (NUL bytes
// between ASCII characters).
bool looks_like_utf16(const char *text, size_t len);

} // namespace sjq

#endif // SJQ_DIAGNOSE_H
