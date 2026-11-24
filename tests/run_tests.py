#!/usr/bin/env python3
"""Tests for sjq.

Groups (all run by default):
  golden    cases/*.test: exact output and exit status, recorded from jq 1.8.2
  targeted  behavior that needs files or processes (mapping, streaming, ...)
  jqsuite   jq's own jq.test and man.test; tests listed in
            jq/expected_pass.txt must pass, the others are reported
  diff      differential runs against jq (only with --jq)

Usage: run_tests.py --sjq PATH [--data DIR] [--jq PATH] [--group NAME]...
       run_tests.py --sjq PATH --update-expected   (rewrite expected_pass.txt)
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TIMEOUT = 60

failures = []


def fail(name, detail):
    failures.append(name)
    print(f"FAIL {name}\n{detail}", flush=True)


def norm(b):
    # Windows writes CRLF line ends unless -b is given.
    return b.replace(b"\r\n", b"\n") if sys.platform == "win32" else b


def run(argv, stdin=b"", env=None, timeout=TIMEOUT):
    full_env = dict(os.environ)
    full_env.pop("NO_COLOR", None)
    full_env.pop("JQ_COLORS", None)
    # jq's tests read $ENV.PAGER; jq's own test harness sets it so.
    full_env["PAGER"] = "less"
    if env:
        full_env.update(env)
    try:
        p = subprocess.run(argv, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           env=full_env, timeout=timeout)
        return p.returncode, norm(p.stdout), norm(p.stderr)
    except subprocess.TimeoutExpired:
        return None, b"", b"timeout"


# ---------------------------------------------------------------------------
# Golden cases. Format, blocks separated by blank lines:
#   # comment
#   %%FLAGS -c -r        (optional)
#   %%FILE twitter.json  (optional: input file from --data instead of a line)
#   %%EXIT 5             (optional, default 0)
#   program
#   input                (one line, absent with %%FILE)
#   expected output lines (exact text)
# ---------------------------------------------------------------------------

def parse_blocks(path):
    blocks, cur, start = [], [], 0
    with open(path, "rb") as f:
        lines = f.read().decode("utf-8").split("\n")
    for i, line in enumerate(lines + [""]):
        if line.startswith("#") and not cur:
            continue
        if line.strip() == "":
            if cur:
                blocks.append((start, cur))
                cur = []
            continue
        if not cur:
            start = i + 1
        cur.append(line)
    return blocks


def golden(sjq, data):
    count = 0
    for name in sorted(os.listdir(os.path.join(HERE, "cases"))):
        if not name.endswith(".test"):
            continue
        path = os.path.join(HERE, "cases", name)
        for line_no, block in parse_blocks(path):
            flags, file_arg, exit_code = [], None, 0
            while block and block[0].startswith("%%"):
                key, _, rest = block[0].partition(" ")
                if key == "%%FLAGS":
                    flags = rest.split()
                elif key == "%%FILE":
                    file_arg = os.path.join(data, rest.strip())
                elif key == "%%EXIT":
                    exit_code = int(rest)
                block = block[1:]
            program = block[0]
            if file_arg:
                stdin, expected = b"", block[1:]
                argv = [sjq] + flags + [program, file_arg]
            else:
                stdin, expected = (block[1] + "\n").encode("utf-8"), block[2:]
                argv = [sjq] + flags + [program]
            code, out, err = run(argv, stdin)
            want = "".join(e + "\n" for e in expected).encode("utf-8")
            count += 1
            if code != exit_code or out != want:
                fail(f"golden {name}:{line_no} {program}",
                     f"  exit {code} (want {exit_code})\n  got:  {out[:400]!r}\n  want: {want[:400]!r}\n  stderr: {err[:200]!r}")
    print(f"golden: {count} cases", flush=True)


# ---------------------------------------------------------------------------
# jq's test suite
# ---------------------------------------------------------------------------

def parse_jq_tests(path):
    """Yields (id, kind, program, input, outputs); kind is 'ok' or 'fail'."""
    with open(path, "rb") as f:
        lines = f.read().decode("utf-8").split("\n")
    i, n = 0, len(lines)
    base = os.path.basename(path)
    while i < n:
        line = lines[i]
        if line.strip() == "" or line.startswith("#"):
            i += 1
            continue
        if line.startswith("%%FAIL"):
            program = lines[i + 1]
            tid = f"{base}:{i + 2}"
            i += 2
            while i < n and lines[i].strip() != "":
                i += 1
            yield tid, "fail", program, None, None
            continue
        tid = f"{base}:{i + 1}"
        program = line
        inp = lines[i + 1] if i + 1 < n else ""
        i += 2
        outs = []
        while i < n and lines[i].strip() != "":
            outs.append(lines[i])
            i += 1
        yield tid, "ok", program, inp, outs


def json_values(text):
    dec = json.JSONDecoder()
    vals, i, s = [], 0, text.strip()
    while i < len(s):
        v, j = dec.raw_decode(s, i)
        vals.append(v)
        i = j
        while i < len(s) and s[i].isspace():
            i += 1
    return vals


def jq_test_passes(sjq, kind, program, inp, outs):
    if kind == "fail":
        # %%FAIL programs must be rejected at compile time (exit status 3),
        # not crash or time out.
        code, _, _ = run([sjq, "-n", "--", program], timeout=10)
        return code == 3
    code, out, _ = run([sjq, "-c", "--", program], (inp + "\n").encode("utf-8"), timeout=10)
    if code != 0:
        return False
    try:
        got = json_values(out.decode("utf-8"))
        want = [json.loads(o) for o in outs]
    except (ValueError, UnicodeDecodeError):
        return False
    return got == want


def jqsuite(sjq, update):
    expected_path = os.path.join(HERE, "jq", "expected_pass.txt")
    expected = set()
    if os.path.exists(expected_path) and not update:
        with open(expected_path) as f:
            expected = {l.strip() for l in f if l.strip() and not l.startswith("#")}
    passed, total, tests = [], 0, {}
    for name in ("jq.test", "man.test"):
        for tid, kind, program, inp, outs in parse_jq_tests(os.path.join(HERE, "jq", name)):
            total += 1
            tests[tid] = (kind, program, inp)
            if jq_test_passes(sjq, kind, program, inp, outs):
                passed.append(tid)
    passed_set = set(passed)
    print(f"jqsuite: {len(passed)} of {total} jq tests pass", flush=True)
    if update:
        with open(expected_path, "w") as f:
            f.write("# jq tests (file:line of the program) that sjq passes; see run_tests.py\n")
            for tid in passed:
                f.write(tid + "\n")
        print(f"jqsuite: wrote {expected_path}")
        return
    lost = sorted(expected - passed_set)
    gained = sorted(passed_set - expected)
    if lost:
        details = []
        for tid in lost:
            if tid not in tests:
                details.append(f"  {tid}: no such test")
                continue
            kind, program, inp = tests[tid]
            argv = [sjq, "-n", "--", program] if kind == "fail" else [sjq, "-c", "--", program]
            start = time.time()
            code, out, err = run(argv, b"" if kind == "fail" else (inp + "\n").encode("utf-8"), timeout=10)
            details.append(f"  {tid}: {program[:60]!r} exit {code} in {time.time() - start:.1f} s, "
                           f"stdout {out[:80]!r}, stderr {err[:120]!r}")
        fail("jqsuite", "  no longer passing: " + " ".join(lost) + "\n" + "\n".join(details))
    if gained:
        print("jqsuite: newly passing (update expected_pass.txt): " + " ".join(gained), flush=True)


# ---------------------------------------------------------------------------
# Targeted tests
# ---------------------------------------------------------------------------

def check(name, cond, detail=""):
    if not cond:
        fail(name, "  " + detail)


def targeted(sjq, data, tmp):
    # Exit status.
    check("exit: no filter", run([sjq])[0] == 2)
    code, _, err = run([sjq, ".["], b"1")
    check("exit: syntax error", code == 3, f"exit {code} {err[:200]!r}")
    check("exit: --indent 8", run([sjq, "--indent", "8", "."], b"1")[0] == 2)
    check("exit: missing file", run([sjq, ".", os.path.join(tmp, "nope.json")])[0] == 2)
    ok_path = os.path.join(tmp, "ok.json")
    with open(ok_path, "wb") as f:
        f.write(b"{}")
    code, out, _ = run([sjq, ".", os.path.join(tmp, "nope.json"), ok_path])
    check("exit: missing file then a good one", code == 2 and out == b"{}\n", f"exit {code} {out!r}")
    code, _, _ = run([sjq, "-s", ".", os.path.join(tmp, "nope.json"), ok_path])
    check("exit: missing file with -s", code == 2, f"exit {code}")
    check("exit: runtime error of the last input", run([sjq, ".a"], b"[]")[0] == 5)
    check("exit: error then success", run([sjq, ".a"], b'[] {"a":1}')[0] == 0)
    check("exit: -b accepted", run([sjq, "-b", "-c", "."], b"[1]")[1] == b"[1]\n")
    check("-j", run([sjq, "-j", ".[]"], b'["x", 1, "y"]')[1] == b"x1y")
    check("empty input", run([sjq, "."], b"") == (0, b"", b""))
    check("blank input", run([sjq, "."], b"  \n ") == (0, b"", b""))

    # Errors in filters: the position, a caret, hints.
    code, _, err = run([sjq, "select(.prix > )"], b"{}")
    check("filter error position", code == 3 and b"line 1, column 16" in err and b"    select(.prix > )\n" in err
          and b"\n                   ^\n" in err, repr(err))
    code, _, err = run([sjq, ".ann\u00e9e"], b"{}")
    check("filter error non-ASCII field", code == 3 and "unexpected character '\u00e9'".encode() in err
          and '."ann\u00e9e"'.encode() in err, repr(err))
    code, _, err = run([sjq, "'.a'"], b"{}")
    check("filter error single quotes", code == 3 and b"single quotes" in err and b"-f" in err, repr(err))
    code, _, err = run([sjq, ".a | lenght"], b"{}")
    check("filter error typo", code == 3 and b"did you mean length?" in err and b"column 6" in err, repr(err))
    code, _, err = run([sjq, "split"], b"{}")
    check("filter error arity", code == 3 and b"split takes 1 or 2 arguments, not 0" in err, repr(err))
    code, _, err = run([sjq, "1 as $count | $cuont"], b"{}")
    check("filter error variable typo", code == 3 and b"did you mean $count?" in err, repr(err))

    # Errors in JSON input: plain words, line and column.
    code, out, err = run([sjq, "-c", "."], b'{"a":1}\n{"b":[1,2}\n')
    check("JSON error position", code == 5 and out == b'{"a":1}\n' and b"invalid JSON at line 2, column 10: expected ',' or ']' but found '}'" in err,
          repr(err))
    code, _, err = run([sjq, "."], b'{"a":1,}')
    check("JSON error trailing comma", code == 5 and b"found '}' after ','" in err and b"TAPE_ERROR" not in err, repr(err))
    code, _, err = run([sjq, ".", os.path.join(tmp, "nope.json")])
    check("missing file reason", code == 2 and (b"No such file or directory" in err), repr(err))

    # Windows: a byte order mark in a filter file, UTF-16 input, -o.
    fpath = os.path.join(tmp, "bom.jq")
    with open(fpath, "wb") as f:
        f.write(b"\xef\xbb\xbf.a")
    code, out, err = run([sjq, "-f", fpath], b'{"a": 7}')
    check("-f with a byte order mark", code == 0 and out == b"7\n", f"exit {code} {out!r} {err!r}")
    u16 = os.path.join(tmp, "u16.json")
    with open(u16, "wb") as f:
        f.write('{"a": "\u00e9t\u00e9"}\r\n'.encode("utf-16"))  # with a byte order mark
    code, out, err = run([sjq, "-r", ".a", u16])
    check("UTF-16 input with a byte order mark", code == 0 and out == "\u00e9t\u00e9\n".encode(), f"exit {code} {out!r} {err!r}")
    with open(u16, "wb") as f:
        f.write('{"a": 1}'.encode("utf-16-le"))
    code, out, err = run([sjq, ".a", u16])
    check("UTF-16 input without a byte order mark", code == 2 and b"UTF-16" in err and b"-o" in err, f"exit {code} {err!r}")
    opath = os.path.join(tmp, "out.json")
    code, out, err = run([sjq, "-o", opath, ".a"], '{"a": "\u00e9"}'.encode())
    with open(opath, "rb") as f:
        written = f.read()
    check("-o FILE", code == 0 and out == b"" and written == '"\u00e9"\n'.encode(), f"exit {code} {written!r} {err!r}")

    # -o: the file is replaced only after a successful run.
    dpath = os.path.join(tmp, "same.json")
    with open(dpath, "wb") as f:
        f.write(b'{"b": {"c": 1}, "z": 2}')
    code, out, err = run([sjq, "-c", "-o", dpath, ".b", dpath])
    with open(dpath, "rb") as f:
        written = f.read()
    check("-o onto the input file", code == 0 and written == b'{"c":1}\n', f"exit {code} {written!r} {err!r}")
    for label, argv in (("filter error", [".a | lenght", dpath]), ("missing input", [".", os.path.join(tmp, "nope.json")]),
                        ("runtime error", ["error(\"x\")", dpath])):
        code, _, err = run([sjq, "-o", dpath] + argv)
        with open(dpath, "rb") as f:
            kept = f.read()
        check(f"-o keeps the file after a {label}", code != 0 and kept == b'{"c":1}\n', f"exit {code} {kept!r} {err!r}")
    leftovers = [n for n in os.listdir(tmp) if ".sjq-" in n]
    check("-o leaves no temporary file", not leftovers, repr(leftovers))
    # An error on one input, then success on the next, is still a failed run.
    two = os.path.join(tmp, "two.ndjson")
    with open(two, "wb") as f:
        f.write(b"1\n2\n")
    code, _, err = run([sjq, "-o", dpath, 'if .==1 then error("x") else . end', two])
    with open(dpath, "rb") as f:
        kept = f.read()
    check("-o keeps the file after an error on an earlier input, exit 5", code == 5 and kept == b'{"c":1}\n',
          f"exit {code} {kept!r} {err!r}")
    code, _, err = run([sjq, "-o", dpath, 'if .==1 then error("x") else halt end', two])
    with open(dpath, "rb") as f:
        kept = f.read()
    check("-o keeps the file after an error, then halt", code == 5 and kept == b'{"c":1}\n', f"exit {code} {kept!r}")
    badj = os.path.join(tmp, "bad-array.json")
    with open(badj, "wb") as f:
        f.write(b"[1,\n")
    code, _, err = run([sjq, "-o", dpath, ".", badj, two])
    with open(dpath, "rb") as f:
        kept = f.read()
    check("invalid input, then valid input: exit 5 and -o unchanged", code == 5 and kept == b'{"c":1}\n', f"exit {code} {kept!r}")
    code, _, _ = run([sjq, "-o", dpath, "-n", "1, halt, 2"])
    with open(dpath, "rb") as f:
        written = f.read()
    check("-o with halt keeps the output so far", code == 0 and written == b"1\n", f"exit {code} {written!r}")
    if sys.platform != "win32":
        real = os.path.join(tmp, "real.json")
        link = os.path.join(tmp, "link.json")
        with open(real, "wb") as f:
            f.write(b'{"a": 1}')
        os.chmod(real, 0o600)
        os.symlink("real.json", link)
        code, _, err = run([sjq, "-c", "-o", link, ".a = 2", link])
        with open(real, "rb") as f:
            written = f.read()
        check("-o writes through a symbolic link", code == 0 and os.path.islink(link) and written == b'{"a":2}\n',
              f"exit {code} {written!r} {err!r}")
        check("-o keeps the permissions", (os.stat(real).st_mode & 0o777) == 0o600, oct(os.stat(real).st_mode))

    # Errors in strings of filters point at the escape, or at the end.
    code, _, err = run([sjq, "-n", '"abc\\q"'])
    check("filter error escape position", code == 3 and b"column 5" in err, repr(err))
    code, _, err = run([sjq, "-n", '"abc'])
    check("filter error unterminated string", code == 3 and b"unterminated string at line 1, column 5" in err, repr(err))
    code, out, _ = run([sjq, "-n", '"  {,}", "1 2" | try fromjson catch .'])
    check("fromjson error positions", code == 0 and b"column 4: expected a string" in out and b"unexpected content after the JSON value" in out,
          repr(out))

    # UTF-16 through a pipe (cmd's type), with -R.
    with open(u16, "wb") as f:
        f.write('{"a": "\u00e9"}'.encode("utf-16"))
    with open(u16, "rb") as f:
        p = subprocess.run([sjq, "-r", ".a"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, input=f.read(), timeout=TIMEOUT)
    check("UTF-16 through a pipe", p.returncode == 0 and norm(p.stdout) == "\u00e9\n".encode(), f"exit {p.returncode} {p.stdout!r} {p.stderr!r}")
    with open(u16, "wb") as f:
        f.write("line\n".encode("utf-16-le"))
    code, _, err = run([sjq, "-R", ".", u16])
    check("UTF-16 with -R", code == 2 and b"UTF-16" in err, f"exit {code} {err!r}")
    p = subprocess.run([sjq, "."], stdout=subprocess.PIPE, stderr=subprocess.PIPE, input=b"1\x00\n\x00", timeout=TIMEOUT)
    check("short UTF-16 through a pipe", p.returncode == 2 and b"UTF-16" in p.stderr, f"exit {p.returncode} {p.stderr!r}")

    # A stream given by name (process substitution, a FIFO).
    if sys.platform != "win32" and hasattr(os, "mkfifo"):
        fifo = os.path.join(tmp, "fifo")
        os.mkfifo(fifo)
        writer = threading.Thread(target=lambda: open(fifo, "wb").write(b'{"k": 5}'))
        writer.start()
        code, out, err = run([sjq, ".k", fifo])
        writer.join(10)
        check("FIFO given by name", code == 0 and out == b"5\n", f"exit {code} {out!r} {err!r}")

    # --fractured (simdjson's FracturedJson layout).
    rows = b'[{"id":1,"name":"a","n":10},{"id":22,"name":"bbb","n":3}]'
    code, out, err = run([sjq, "--fractured", "--width", "40", "."], rows)
    check("--fractured table", code == 0 and out == b'[\n  {"id": 1 , "name": "a",   "n": 10},\n  {"id": 22, "name": "bbb", "n": 3 }\n]\n',
          f"exit {code} {out!r} {err!r}")
    code, out, _ = run([sjq, "--fractured", "--width", "30", "[range(20)]", "-n"])
    check("--fractured --width", code == 0 and max(len(l) for l in out.split(b"\n")) <= 30 and out.count(b"\n") > 2, f"exit {code} {out!r}")
    code, out, _ = run([sjq, "--fractured", "-S", "-a", "--indent", "4", "{b: \"\u00e9\", a: [1, {c: null}], d: {e: [1, 2]}}", "-n"])
    check("--fractured -S -a --indent", code == 0 and out == b'{\n    "a": [ 1, {"c": null} ],\n    "b": "\\u00e9",\n    "d": { "e": [1, 2] }\n}\n',
          f"exit {code} {out!r}")
    code, out, _ = run([sjq, "--fractured", "-r", ".[]"], b'["x", {"a": 1}]')
    check("--fractured -r", code == 0 and out == b'x\n{"a": 1}\n', f"exit {code} {out!r}")
    check("--fractured -c rejected", run([sjq, "--fractured", "-c", "."], b"1")[0] == 2)
    check("--fractured --tab rejected", run([sjq, "--fractured", "--tab", "."], b"1")[0] == 2)
    check("--width 0 rejected", run([sjq, "--fractured", "--width", "0", "."], b"1")[0] == 2)

    # A stack too small for the On-Demand writer: the DOM path runs the
    # document again, and either succeeds or reports the error, never aborts.
    if os.name == "posix":
        import resource
        deep = b"[" * 1000 + b"]" * 1000
        def small_stack():
            resource.setrlimit(resource.RLIMIT_STACK, (256 << 10, resource.getrlimit(resource.RLIMIT_STACK)[1]))
        p = subprocess.run([sjq, "-c", ".[0]"], input=deep, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           preexec_fn=small_stack, timeout=TIMEOUT)
        ok = (p.returncode == 0 and p.stdout == deep[1:-1] + b"\n") or (p.returncode == 5 and b"nested too deeply" in p.stderr)
        check("stack guard on the On-Demand path", ok, f"exit {p.returncode} {p.stderr[:200]!r}")

    # Input sizes around page boundaries: the mapped padding must hold.
    for page in (4096, 16384, 65536):
        for d in (0, 1, 63, 64, 65):
            size = page - d
            # An array, then a bare number in the very last byte.
            text = b"[" + b"1," * ((size - 3) // 2 - 4) + b"0]"
            text = text + b" " * (size - len(text) - 2) + b" 7"
            path = os.path.join(tmp, f"pad_{size}.json")
            with open(path, "wb") as f:
                f.write(text[:size])
            code, out, err = run([sjq, "-c", "length", path])
            check(f"padding {size}", code == 0 and out.endswith(b"7\n"), f"exit {code} {out[-40:]!r} {err[:200]!r}")

    # A large NDJSON file, filtered in parallel, sequentially, redirected and piped.
    docs = [{"i": i, "s": "x" * (i % 50), "a": [i, i * 2], "o": {"k": str(i)}} for i in range(120000)]
    nd = "".join(json.dumps(d, separators=(",", ":")) + "\n" for d in docs).encode()
    nd_path = os.path.join(tmp, "big.ndjson")
    with open(nd_path, "wb") as f:
        f.write(nd)
    want = "".join(json.dumps(d["o"], separators=(",", ":")) + "\n" for d in docs).encode()
    for label, argv, stdin in (
        ("parallel", [sjq, "-c", ".o", nd_path], b""),
        ("sequential", [sjq, "--threads", "1", "-c", ".o", nd_path], b""),
        ("no-mmap", [sjq, "--no-mmap", "-c", ".o", nd_path], b""),
        ("pipe", [sjq, "-c", ".o"], nd),
    ):
        code, out, _ = run(argv, stdin)
        check(f"ndjson {label}", code == 0 and out == want, f"exit {code}, {len(out)} bytes")
    # --raw-output0 on the parallel path and sequentially.
    want0 = "".join(str(d["i"]) + "\0" for d in docs).encode()
    for label, extra in (("parallel", []), ("sequential", ["--threads", "1"])):
        code, out, _ = run([sjq] + extra + ["--raw-output0", ".i | tostring", nd_path])
        check(f"--raw-output0 {label}", code == 0 and out == want0, f"exit {code}, {out.count(b'\0')} NULs, {out.count(b'\n')} newlines")
    with open(nd_path, "rb") as f:
        p = subprocess.run([sjq, "-c", ".o"], stdin=f, stdout=subprocess.PIPE, timeout=TIMEOUT)
    check("ndjson redirected stdin", p.returncode == 0 and norm(p.stdout) == want)

    # Pretty-printed documents: the parallel cuts land inside documents.
    pretty = "".join(json.dumps(d, indent=2) + "\n" for d in docs[:40000]).encode()
    pretty_path = os.path.join(tmp, "pretty.json")
    with open(pretty_path, "wb") as f:
        f.write(pretty)
    want_c = "".join(json.dumps(d, separators=(",", ":")) + "\n" for d in docs[:40000]).encode()
    code, out, _ = run([sjq, "-c", ".", pretty_path])
    check("pretty stream parallel fallback", code == 0 and out == want_c, f"exit {code}")

    # --fractured: the same output from the worker threads and sequentially.
    code1, out1, _ = run([sjq, "--fractured", "{i, a, o}", nd_path])
    code2, out2, _ = run([sjq, "--threads", "1", "--fractured", "{i, a, o}", nd_path])
    check("--fractured parallel and sequential", code1 == 0 and code2 == 0 and out1 == out2 and out1.startswith(b'{ "i": 0, "a": [0, 0], "o": {"k": "0"} }\n'),
          f"exit {code1} {code2} {out1[:80]!r} {out2[:80]!r}")

    # halt in a file larger than a parse batch: sjq used to read the state of
    # the stream after stopping early. SJQ_TEST_BATCH makes the batch 1 MB.
    for threads in ("1", "4"):
        p = subprocess.run([sjq, "--threads", threads, "-c", 'if .i == 100000 then halt_error(7) else empty end', nd_path],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=TIMEOUT,
                           env=dict(os.environ, SJQ_TEST_BATCH=str(1 << 20)))
        check(f"halt_error in a file larger than a batch (--threads {threads})", p.returncode == 7,
              f"exit {p.returncode} {p.stderr[-300:]!r}")
    opath2 = os.path.join(tmp, "parallel-out.json")
    code, _, err = run([sjq, "-c", "-o", opath2, ".o", nd_path])
    with open(opath2, "rb") as f:
        written = f.read()
    check("-o on the parallel path", code == 0 and written == want, f"exit {code}, {len(written)} bytes {err!r}")

    # Only newline-delimited input goes to the worker threads: one large
    # pretty-printed document, or pretty-printed documents, stay sequential.
    code, _, err = run([sjq, "-c", ".i", nd_path], env={"SJQ_DEBUG": "1"})
    check("parallel path for NDJSON", code == 0 and b"sjq: parallel" in err, f"exit {code} {err!r}")
    one_doc = os.path.join(tmp, "one.json")
    with open(one_doc, "w") as f:
        json.dump({"docs": docs[:40000]}, f, indent=2)
    code, out, err = run([sjq, ".docs | length", one_doc], env={"SJQ_DEBUG": "1"})
    check("no parallel path for one large document", code == 0 and out == b"40000\n" and b"sjq: parallel" not in err,
          f"exit {code} {out!r} {err!r}")
    code, _, err = run([sjq, "-c", ".", pretty_path], env={"SJQ_DEBUG": "1"})
    check("no parallel path for pretty-printed documents", code == 0 and b"sjq: parallel" not in err, f"exit {code} {err!r}")

    # An error in the middle stops the output there.
    bad = nd.split(b"\n")
    bad_doc = b"\n".join(bad[:60000]) + b'\n{"i": [1 2]}\n' + b"\n".join(bad[60000:])
    bad_path = os.path.join(tmp, "bad.ndjson")
    with open(bad_path, "wb") as f:
        f.write(bad_doc)
    code, out, _ = run([sjq, "-c", ".i", bad_path])
    check("error mid-stream", code == 5 and out.count(b"\n") == 60000, f"exit {code}, {out.count(chr(10).encode())} lines")
    # The parallel path keeps the output of the failing chunk's documents
    # before the error, and the sequential path resumes at the bad document:
    # the same output, status and message as with one thread.
    bad_at = len(b"\n".join(bad[:60000])) + 1
    for filt in (".i", "{i, n: (.a | add)}"):
        seq_p = subprocess.run([sjq, "--threads", "1", "-c", filt, bad_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=TIMEOUT)
        par_p = subprocess.run([sjq, "--threads", "4", "-c", filt, bad_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               timeout=TIMEOUT, env=dict(os.environ, SJQ_DEBUG="1"))
        # Windows ends the lines of standard error with CRLF.
        seq_err = seq_p.stderr.replace(b"\r\n", b"\n")
        par_all = par_p.stderr.replace(b"\r\n", b"\n")
        par_err = b"\n".join(l for l in par_all.split(b"\n") if not l.startswith(b"sjq: parallel")
                             and not l.startswith(b"sjq: sequential") and not l.endswith(b" path"))
        check(f"error mid-stream, parallel as sequential ({filt})",
              par_p.returncode == seq_p.returncode == 5 and par_p.stdout == seq_p.stdout and par_err == seq_err,
              f"exit {par_p.returncode}/{seq_p.returncode} {par_err[-200:]!r} {seq_err[-200:]!r}")
        check(f"error mid-stream resumes at the bad document ({filt})",
              f"sjq: sequential from byte {bad_at}\n".encode() in par_all, repr(par_all[-300:]))
    # A runtime error in the middle: the same as with one thread, resuming at
    # the document that raised it.
    filt = 'if .i == 70000 then error("stop") else .i end'
    seq_p = subprocess.run([sjq, "--threads", "1", "-c", filt, nd_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=TIMEOUT)
    par_p = subprocess.run([sjq, "--threads", "4", "-c", filt, nd_path], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=TIMEOUT, env=dict(os.environ, SJQ_DEBUG="1"))
    err_at = len(b"\n".join(nd.split(b"\n")[:70000])) + 1
    seq_err = seq_p.stderr.replace(b"\r\n", b"\n")
    par_all = par_p.stderr.replace(b"\r\n", b"\n")
    check("runtime error mid-stream, parallel as sequential",
          par_p.returncode == seq_p.returncode and par_p.stdout == seq_p.stdout and seq_err in par_all
          and f"sjq: sequential from byte {err_at}\n".encode() in par_all,
          f"exit {par_p.returncode}/{seq_p.returncode} {par_all[-300:]!r}")

    # Medium files go parallel too (from 1 MiB), with at most one thread per
    # 512 KiB; smaller files stay sequential.
    medium = os.path.join(tmp, "medium.ndjson")
    with open(medium, "wb") as f:
        f.write(nd[: nd.index(b"\n", 1536 << 10) + 1])
    code, out, err = run([sjq, "--threads", "64", "-c", ".i", medium], env={"SJQ_DEBUG": "1"})
    seq_code, seq_out, _ = run([sjq, "--threads", "1", "-c", ".i", medium])
    check("medium file goes parallel, with few threads", code == 0 and out == seq_out and b"sjq: parallel (3 threads," in err,
          f"exit {code} {err!r}")
    small = os.path.join(tmp, "small.ndjson")
    with open(small, "wb") as f:
        f.write(nd[: nd.index(b"\n", 900 << 10) + 1])
    code, _, err = run([sjq, "-c", ".i", small], env={"SJQ_DEBUG": "1"})
    check("small file stays sequential", code == 0 and b"sjq: parallel" not in err, f"exit {code} {err!r}")
    code, out, err = run([sjq, "-c", "."], b'{"a":1}\n{"a": [1,')
    check("truncated input", code == 5 and out == b'{"a":1}\n' and b"unclosed array" in err, f"exit {code} {out!r} {err!r}")

    # A file that shrinks while it is read (POSIX test hook).
    if sys.platform != "win32":
        victim = os.path.join(tmp, "victim.ndjson")
        with open(victim, "wb") as f:
            f.write(nd)
        code, _, err = run([sjq, "-c", ".i", victim], env={"SJQ_TEST_TRUNCATE": "1000"})
        check("file truncated while read", code == 2 and b"changed while being read" in err, f"exit {code} {err!r}")

    # Streaming: each document is written before the next one arrives.
    p = subprocess.Popen([sjq, "--unbuffered", "-c", ".a"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    got = []

    def reader():
        got.append(p.stdout.readline())

    p.stdin.write(b'{"a": 1}\n')
    p.stdin.flush()
    t = threading.Thread(target=reader)
    t.start()
    t.join(20)
    streaming_ok = bool(got) and norm(got[0]) == b"1\n"
    p.stdin.write(b'{"a": 2}\n')
    p.stdin.close()
    rest = norm(p.stdout.read())
    p.wait(timeout=TIMEOUT)
    check("streaming stdin", streaming_ok and rest == b"2\n", f"first {got!r} rest {rest!r}")

    # Colors (jq 1.8 layout and defaults), JQ_COLORS.
    code, out, _ = run([sjq, "-C", "-c", "."], b'{"a":[1,"x",null,true]}')
    want_color = (b'\x1b[1;39m{\x1b[0m\x1b[1;34m"a"\x1b[0m\x1b[1;39m:\x1b[0m\x1b[1;39m[\x1b[0m\x1b[0;39m1\x1b[0m'
                  b'\x1b[1;39m,\x1b[0m\x1b[0;32m"x"\x1b[0m\x1b[1;39m,\x1b[0m\x1b[0;90mnull\x1b[0m\x1b[1;39m,\x1b[0m'
                  b'\x1b[0;39mtrue\x1b[0m\x1b[1;39m]\x1b[0m\x1b[1;39m}\x1b[0m\n')
    check("colors default", out == want_color, repr(out))
    code, out, _ = run([sjq, "-C", "-c", "."], b'{"a":1}', env={"JQ_COLORS": "0;31:0;32:0;33:4;34:0;35:0;36:0;37:4;31"})
    check("colors JQ_COLORS", out == b'\x1b[0;37m{\x1b[0m\x1b[4;31m"a"\x1b[0m\x1b[0;37m:\x1b[0m\x1b[4;34m1\x1b[0m\x1b[0;37m}\x1b[0m\n', repr(out))
    code, out, err = run([sjq, "-C", "-c", "."], b'1', env={"JQ_COLORS": "bad"})
    check("colors invalid JQ_COLORS", out == b"\x1b[0;39m1\x1b[0m\n" and b"Failed to set $JQ_COLORS" in err, repr(out))
    code, out, _ = run([sjq, "-c", "."], b'1')
    check("no colors on a pipe", out == b"1\n")

    # --seq: RS before each output, RS-separated input.
    code, out, _ = run([sjq, "--seq", "-c", ".a"], b'\x1e{"a":1}\n\x1e{"a":2}\n')
    check("--seq", out == b"\x1e1\n\x1e2\n", repr(out))

    # Non-ASCII file names and filters (UTF-8 code page on Windows).
    uname = os.path.join(tmp, "donn\u00e9es-\u00e9t\u00e9.json")
    with open(uname, "wb") as f:
        f.write('{"cl\u00e9": "valeur \u00e9t\u00e9"}'.encode("utf-8"))
    code, out, err = run([sjq, "-r", '.["cl\u00e9"]', uname])
    check("utf-8 file name and filter", code == 0 and out == "valeur \u00e9t\u00e9\n".encode("utf-8"), f"exit {code} {out!r} {err!r}")

    # Deep values: capped with an error, never a crash.
    code, _, err = run([sjq, "-n", "reduce range(20000) as $i (null; [.]) | length"])
    check("deep value cap", code == 5 and b"nested too deeply" in err, f"exit {code} {err!r}")
    code, _, err = run([sjq, "-n", "1 | recurse(.)"])
    check("recurse cap", code == 5 and b"nested too deeply" in err, f"exit {code} {err!r}")
    code, _, _ = run([sjq, "(" * 2000 + "." + ")" * 2000], b"1")
    check("deeply nested filter", code == 3, f"exit {code}")


# ---------------------------------------------------------------------------
# Differential runs against jq (stdout and exit status; error wording and the
# documented differences are left out).
# ---------------------------------------------------------------------------

DIFF_FILTERS = [
    ".", ".statuses[0]", ".statuses[].user.screen_name", ".statuses | length",
    "[.statuses[] | select(.retweet_count > 0) | .id]",
    ".statuses[] | {id, name: .user.name, n: .user.followers_count}",
    ".statuses | group_by(.user.lang) | map({lang: .[0].user.lang, n: length})",
    ".statuses[] | .entities.urls[]?.expanded_url", ".statuses[0].user | to_entries | .[0:3]",
    "reduce .statuses[] as $s (0; . + $s.favorite_count)", "[paths] | length",
    ".statuses[] | select(.user.lang == \"ja\" and .retweet_count > 0) | .id",
    ".search_metadata", "[..|numbers] | length", ".statuses | map(.text | length) | max",
    ".statuses[0].user | to_entries | .[0:3]", "to_entries", "map(.)", "join(\",\")", "flatten", "any", "add",
    ".a", ".n", ".[]?", "select(.b == true) | .n", "{x: .a, y: .b}", ".a | length",
]


# Known differences, reported but not failures; step by step they go away.
DIFF_KNOWN = {
    # Number literals are lost on the DOM path (map, add over values, ...).
    ("tricky.ndjson", "map(.)"), ("tricky.ndjson", "add"), ("tricky.ndjson", "to_entries"),
    ("tricky.ndjson", "flatten"), ("tricky.ndjson", "join(\",\")"),
    # jq iterates objects in flatten, any and join; sjq requires arrays.
    ("twitter.json", "flatten"), ("twitter.json", "any"), ("tricky.ndjson", "any"),
    ("twitter.json", "join(\",\")"),
}


def differential(sjq, jq, data):
    count = 0
    known = 0
    files = [os.path.join(data, "twitter.json"), os.path.join(HERE, "data", "tricky.ndjson")]
    for path in files:
        for program in DIFF_FILTERS:
            for flags in ([], ["-c"], ["-r"], ["--tab"], ["--indent", "1"]):
                a = run([jq] + flags + [program, path])
                b = run([sjq] + flags + [program, path])
                count += 1
                if a[0] != b[0] or a[1] != b[1]:
                    if (os.path.basename(path), program) in DIFF_KNOWN:
                        known += 1
                    else:
                        fail(f"diff {os.path.basename(path)} {flags} {program}", f"  jq exit {a[0]}, sjq exit {b[0]}")
    print(f"diff: {count} runs against {jq}, {known} known differences", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sjq", required=True)
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    ap.add_argument("--jq")
    ap.add_argument("--group", action="append")
    ap.add_argument("--update-expected", action="store_true")
    args = ap.parse_args()
    sjq = os.path.abspath(args.sjq)
    groups = args.group or ["golden", "targeted", "jqsuite", "diff"]
    start = time.time()
    if args.update_expected:
        jqsuite(sjq, True)
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        if "golden" in groups:
            golden(sjq, args.data)
        if "targeted" in groups:
            targeted(sjq, args.data, tmp)
        if "jqsuite" in groups:
            jqsuite(sjq, False)
        if "diff" in groups and args.jq:
            differential(sjq, args.jq, args.data)
    print(f"{len(failures)} failure(s) in {time.time() - start:.1f} s")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
