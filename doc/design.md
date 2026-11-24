# sjq design notes

- Input: regular files, and standard input when it is redirected from a
  regular file (`sjq . < file`), are memory-mapped with
  `simdjson::padded_memory_map` (`--no-mmap` reads files into a
  `padded_string` instead). On Windows the map uses the placeholder APIs
  (Windows 10 version 1803 or later): sjq turns on
  `SIMDJSON_ENABLE_MEMORY_FILE_MAPPING_ON_WINDOWS` for itself and links
  `onecore`; the library is built as configured. A pipe or terminal on standard input is
  read as it comes: each read is processed up to its last newline (a cut there
  cannot split a scalar; an incomplete container waits for more input), and
  output is written as each document is done, so `tail -f log | sjq` works.
  While more input is ready, sjq keeps reading (up to 1 MB, and after a round
  without progress until the buffer has doubled, so a large document arriving
  in pieces is parsed a logarithmic number of times); once the producer is
  idle, it processes what it has. The padding simdjson needs
  is guaranteed, not assumed: the bytes after the end of the file up to the end
  of its last page read as zeros, and when fewer than 64 remain, the file is
  mapped in front of a reserved anonymous zero page. Documents are read as a
  stream (`iterate_many` / `parse_many`), so a single document, NDJSON and
  whitespace-separated document streams all work.
- A file that shrinks while it is read does not crash sjq. Reading a vanished
  page of a mapping raises SIGBUS; sjq's handler, for faults inside the
  mapping only, maps zero pages over the rest of it and returns, so the read is
  retried and sees zeros. The drivers stop at the next document. A shrink that
  causes no fault (within the last page, or macOS serving old pages of a
  private mapping) is caught by an fstat after the file is processed. Either
  way sjq keeps the output already written, reports "changed while being read;
  the output is incomplete" and exits with status 2. Faults elsewhere crash as
  before. Content rewritten in place without changing the size is not
  detected, as with any reader. `SJQ_TEST_TRUNCATE=n` (truncate right after
  mapping) is a test hook.
- Portability: tested on macOS (Apple clang, GCC 15) and Linux x86-64 (GCC 16,
  clang 23), all with the developer warning flags; cross-built for Windows with
  MinGW-w64 (GCC 16). The string scans use NEON on ARM and SSE2 on x86-64
  (part of the base instruction set), SWAR elsewhere. On Windows, a manifest
  makes UTF-8 the process code page (Windows 10 version 1903 and later), so
  filters and file names can hold any character; the console gets escape
  sequences (colors) and UTF-8 output. One document can be at most 4 GiB (a
  simdjson limit, reported as CAPACITY); a stream of documents can be larger.
- On Windows a file with a mapped view cannot be truncated, so the remaining
  risk is a read error (a network share that goes away, a removed drive),
  which raises EXCEPTION_IN_PAGE_ERROR. A vectored exception handler, for
  faults inside the mapping only, reports it and exits with status 2.
- Evaluation: a tree-walking interpreter with jq's generator semantics
  (each node calls a callback once per output). Values are either a
  reference into the simdjson DOM tape (no copy) or an owned
  number/string/array/object. Path expressions, `select`, comparisons and
  output never copy input data.
- Output: our own writer that reproduces jq's formatting: 2-space indent, jq's
  string escaping (including `\u007f`), jq 1.7's number formatting for computed
  values (shortest round-trip, exponent when decpt <= -4 or decpt > digits+15).
- Parallel NDJSON: inputs of 1 MiB or more are cut into chunks right after
  newlines (about eight per thread, from 256 KiB to 1 MiB); worker threads
  filter whole chunks, the main thread writes them in order. sjq starts at
  most one worker per 512 KiB of input (starting one costs about as much as
  filtering that much), and no more than there are chunks. A worker waits for
  room in the output window before it takes a chunk, so a written chunk wakes
  one worker, not all of them. A cut inside a multi-line document always makes
  some chunk end with an incomplete document or fail to parse, at the first
  bad cut. The first chunk that fails therefore starts on a document boundary
  (the chunks before it ended cleanly), so the documents before its failure
  are real: their output is kept, and the sequential path resumes at the
  document that failed (an invalid document, a runtime error, or the
  incomplete document at a bad cut). That path also produces the error
  messages. `SJQ_DEBUG=1` prints the threads, the chunks and where the
  sequential path resumes. `--threads 1` disables it, and RS-separated
  (`--seq`) input is always read sequentially.

- On-Demand fast path (default when the filter allows it): paths, `.[]`,
  `.[n]`, `?`, `select()` over comparisons with literals, `and`/`or`/`not`,
  `has("k")`, `length`, object/array construction with literal keys, literals,
  `,` and `|`. The program is compiled to a plan run on `ondemand` documents
  from `iterate_many`; output is written from the raw input, so strings without
  escapes are copied and number literals are printed as jq 1.7+ does
  (decNumber's to-scientific-string: `17.0`, `1E+2`, `1E-7`). Only what the
  filter reads is parsed and checked: syntax errors in parts of a document the
  filter skips go unnoticed (stage 1 still rejects invalid UTF-8, unclosed
  strings and unbalanced brackets). Any type error or surprise
  throws inside the plan; that document's output is discarded and it is re-run
  through the DOM evaluator, which reports jq's error. `--no-od` forces the
  DOM path. `SJQ_DEBUG=1` prints the chosen path.
- The parallel cut check does not depend on validation: the chunk before the
  first bad cut starts outside any document and ends inside one, so stage 1
  reports it truncated.
- Output: as jq 1.8. Colors are on when stdout is a terminal and `NO_COLOR`
  is unset or empty; `-C` forces them, `-M` turns them off; `JQ_COLORS` sets
  them (null:false:true:numbers:strings:arrays:objects:object keys; an invalid
  value prints "Failed to set $JQ_COLORS" and keeps the defaults). Every token
  is wrapped in its color and a reset, as jq 1.8 does. `--indent n` takes -1
  (tabs) to 7; `--indent 0` keeps newlines without indentation (it is not
  `-c`). `--seq` writes an RS before each output and reads RS-separated input
  when the input starts with RS. Output is flushed after each input when
  stdout is a terminal or with `--unbuffered`.

## Known differences from jq 1.7/1.8

- On the DOM path (filters the On-Demand plan cannot run, such as arithmetic,
  `map`, `sort_by`), number literals from the input are not preserved: jq 1.7+
  prints `17.0`, `1E+2`; the DOM path prints the parsed value (`17`, `100`).
  Integers wider than 64 bits are rejected by the DOM (`BIGINT_ERROR`); the
  On-Demand path prints them as written.
- No `def`, string interpolation, regular expressions, assignment operators
  (`=`, `|=`, `+=`), `del`, `path`, `getpath/setpath`, `input/inputs`, `-s`,
  `--arg`, `@csv/@tsv/@base64`, dates, SQL-style builtins.
- Parse errors are reported with simdjson's messages, without line/column.
  Exit status follows jq 1.7.1 (status of the last input; 5 on parse errors).
- Error wording mostly follows jq 1.7.1 (`with string "a"`); iteration and
  arithmetic errors already use jq 1.8's form (`string ("a very long...")`).
- Values nested more than 10000 levels deep, and recursion that would exhaust
  the stack (`1 | recurse(.)`), end with an error; jq loops or keeps going.
- `-S` turns off the On-Demand path, so with it the DOM path prints parsed
  numbers instead of the literals (to be fixed).
- With `--seq`, jq 1.8 ignores input that does not start with RS; sjq reads it
  as ordinary input.
- Integers beyond 2^53 are exact when printed unchanged, compared as doubles.
- Objects with duplicate keys are read as simdjson reads them: `.a` finds the
  first `"a"`, and `.` prints every pair. jq keeps only the last value.
