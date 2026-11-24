#!/usr/bin/env python3
"""Writes cases/*.test from the specs below, with outputs recorded from jq.

Usage: make_golden.py --jq PATH_TO_JQ_1.8 [--data DIR]

Each spec is (flags, program, input); an input of the form "@name" reads the
file name from --data instead of a line of standard input. Only stdout and the
exit status are recorded: sjq follows jq 1.7 for error wording.
"""

import argparse
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))

LIT = '{"a": 1.50, "b": 1E2, "c": -0, "d": 0.0000001, "e": 12.5e1, "f": 100000000000000000000000000001, "g": [1.000, 2.0]}'
STR = '{"s": "tab\\there \\"q\\" \\\\ \\u0001 \\u007f \\u00e9 \\ud83d\\ude00 / \\/", "k\\u00e9y": "v"}'

LIB = '{"nom":"Médiathèque","livres":[{"id":"b001","titre":"1984","auteur":"George Orwell","annee":1949,"genres":["dystopie","politique"],"empruntable":true,"emprunts":[{"lecteur":"Alice"},{"lecteur":"Bob"}]},{"id":"b002","titre":"Le Petit Prince","auteur":"Saint-Exupéry","annee":1943,"genres":["conte"],"empruntable":false},{"id":"b003","titre":"Fondation","auteur":"Isaac Asimov","annee":1951,"genres":["science-fiction"],"empruntable":true,"prix":19}]}'

SPECS = {
    "options": [
        ([], ".", '{"a":[1,{"b":2}],"c":{},"d":[]}'),
        (["-c"], ".", '{"a":[1,{"b":2}],"c":{},"d":[]}'),
        (["--tab"], ".", '{"a":[1,{"b":2}]}'),
        (["--indent", "0"], ".", '{"a":[1,{"b":2}]}'),
        (["--indent", "3"], ".", '{"a":[1,{"b":2}]}'),
        (["--indent", "7"], ".", '{"a":[1]}'),
        (["--indent", "-1"], ".", '{"a":[1]}'),
        (["-r"], ".[]", '["x", 1, "y\\nz", null]'),
        (["-S", "-c"], ".", '{"b":1,"a":{"d":2,"c":3}}'),
        (["-n"], "1, 2", "null"),
        (["-nc"], "[1, {\"a\": null}]", "null"),
        (["-rc"], ".a", '{"a": "s"} {"a": [1]}'),
        (["--seq", "-c"], ".a", '\x1e{"a":1}'),
        (["-c"], ".", '1 "two" [3] {"four":4} null true'),
    ],
    "literals": [
        (["-c"], ".", LIT),
        ([], ".", LIT),
        (["-c"], ".a, .b, .c, .d, .e, .f, .g", LIT),
        (["-c"], "{x: .a, y: [.b, .f]}", LIT),
        (["-c"], "select(.a > 1) | .g", LIT),
        (["-c"], ".", "1.50"),
        (["-c"], ".", "100000000000000000000000000001"),
        (["-c"], ".", STR),
        (["-r"], ".s", STR),
        (["-c"], "[1.50, 1e3, 0.1]", "null"),
        (["-c"], "0.1 + 0.2, 1 / 3, 1e300 * 10, 1e17 * 10, 2.5 * 2, 1e-7 * 1, -0 * 1", "null"),
        (["-c"], "[.[] * 1]", "[3.5, 1234.5678, 0.0001, 0.00012, 123456789012]"),
    ],
    "ondemand": [
        (["-c"], ".a.b.c", '{"a":{"b":{"c":[1,2]}}}'),
        (["-c"], ".a[1]", '{"a":[10,20,30]}'),
        (["-c"], ".a[]", '{"a":[10,{"x":1},"s"]}'),
        (["-c"], ".[]", '{"a":1,"b":[2]}'),
        (["-c"], ".a?", '[1]'),
        (["-c"], ".a[1]?", '{"a":"str"}'),
        (["-c"], '.["a"]', '{"a":1}'),
        (["-c"], ".missing", '{"a":1}'),
        (["-c"], ".a.b", '{"a":null}'),
        (["-c"], "select(.n > 1) | .n", '{"n":1} {"n":2} {"n":3}'),
        (["-c"], 'select(.s == "x") | .id', '{"s":"x","id":1} {"s":"y","id":2}'),
        (["-c"], "select(.n >= 2 and .n < 3)", '{"n":1} {"n":2} {"n":3}'),
        (["-c"], "select(.n == 1 or .m)", '{"n":1} {"n":2,"m":true} {"n":3}'),
        (["-c"], "select(.n | not)", '{"n":false} {"n":1} {"m":2}'),
        (["-c"], 'select(has("m"))', '{"n":1} {"m":2}'),
        (["-c"], "select(2 < .n)", '{"n":1} {"n":3}'),
        (["-c"], "{a, b: .x.y, c: [.a, .x]}", '{"a":1,"x":{"y":2}}'),
        (["-c"], "[.[] | .k]", '[{"k":1},{"k":2},{}]'),
        (["-c"], ".a, .b", '{"b":2,"a":1}'),
        (["-c"], ".a | length", '{"a":[1,2,3]} {"a":"\\u00e9t\\u00e9"} {"a":{"x":1}} {"a":null} {"a":-5}'),
        (["-c"], '"lit", 1.50, null', '{}'),
        (["-c"], "empty", '{"a":1}'),
        (["-c"], ".", '"root string" 42 true null'),
    ],
    "builtins": [
        (["-c"], "keys, keys_unsorted, length", '{"b":1,"a":2}'),
        (["-c"], "map(. * 2), add, min, max, sort, unique, reverse", "[3,1,2,3]"),
        (["-c"], "sort_by(.n), group_by(.g), unique_by(.g), min_by(.n), max_by(.n)", '[{"n":2,"g":"x"},{"n":1,"g":"y"},{"n":3,"g":"x"}]'),
        (["-c"], "to_entries, with_entries({key, value: (.value + 1)}), (to_entries | from_entries)", '{"a":1,"b":2}'),
        (["-c"], '[.[] | type]', '[null, true, 1, "s", [], {}]'),
        (["-c"], 'has("a"), has("z"), (.a | has(1))', '{"a":[1,2]}'),
        (["-c"], 'contains({a:[1]}), inside({a:[1,2],b:3}), contains("bar")', '{"a":[1,2]}'),
        (["-c"], '.[] | startswith("ab"), endswith("yz"), ltrimstr("a"), rtrimstr("z")', '["abxyz"]'),
        (["-c"], 'split(","), (split(",") | join("-")), ascii_downcase, ascii_upcase', '"A,b,C"'),
        (["-c"], "[limit(3; range(10))], first(range(5;10)), [range(2;8)]", "null"),
        (["-c"], "reduce .[] as $x (0; . + $x), [foreach .[] as $x (0; . + $x)]", "[1,2,3]"),
        (["-c"], ". as $o | $o.a + 1", '{"a":1}'),
        (["-c"], 'if . > 1 then "big" elif . == 1 then "one" else "small" end', "0 1 2"),
        (["-c"], 'try error("x") catch ., (.a // "default"), [.[]?]', '{"b":1}'),
        (["-c"], "[paths], [..]", '{"a":[1,{"b":2}]}'),
        (["-c"], "flatten, any, all, (map(select(. != null)) | length)", "[[1,[2]],null,3]"),
        (["-c"], "tostring, tojson, (tostring | tonumber)", "[1,2]"),
        (["-c"], ".[2:4], .[:2], .[-2:]", '[0,1,2,3,4,5]'),
        (["-c"], ".[2:4]", '"abcdef"'),
        (["-c"], 'indices(1), index(1), rindex(1), (.[0:2] | tojson)', "[0,1,2,1]"),
        (["-c"], 'label $out | .[] | if . > 2 then ., break $out else . end', "[1,2,3,4]"),
        (["-c"], "floor, ceil, round, sqrt, fabs", "6.25"),
        (["-c"], ". + {c:3}, . * {a:{y:2}}, ([1,2,3] - [2])", '{"a":{"x":1}}'),
        (["-c"], '"x" * 3, ("a,b" / ","), (10 % 3)', "null"),
        (["-c"], "[.[] | numbers], [.[] | strings], [.[] | scalars], [.[] | iterables]", '[1,"s",null,[],{}]'),
        (["-c"], ".[1:3], .[-2:], indices(\"\u20ac\"), index(\"x\"), rindex(\"\u20ac\"), split(\"\")", '"a\u00e9\u20acx\u20ac"'),
        (["-c"], "indices([1,2]), index([1,2]), rindex([1,2]), indices(1), indices([])", "[0,1,2,1,2,3]"),
        (["-c"], "\"\" | reverse", "null"),
        (["-c"], ".a?/2, (.b // .a), (.[] // 0)", '{"a":4,"b":null}'),
        (["-c"], "5.5 % 2, 5 % -2, -5 % 2", "null"),
    ],
    "language": [
        # Tutorial-style programs (jq's tutorial, a course activity).
        (["-c"], ".livres[] | {titre, annee}", LIB),
        (["-c"], "{a, a: .x.y, c: [.a, .x], d: 1, e: 2, f: 3, g: 4}", '{"a":1,"x":{"y":2,"x":{"y":2}}}'),
        (["-c"], "{a: (1,2), a: (3,4), b: (5,6), c: 7, d: 8, e: 9}", "null"),
        (["-r"], '.livres[] | .titre as $t | .prix // "non vendu" | "\\($t) : \\(.)"', LIB),
        (["-r"], '.livres[] | "| \\(.titre) | \\(.genres | join(", ")) | \\((.emprunts // []) | length) |"', LIB),
        (["-c"], ".livres | map({key: .id, value: {titre}}) | from_entries", LIB),
        (["-c"], ".livres | group_by(.titre[0:1]) | map({lettre: .[0].titre[0:1], count: length})", LIB),
        (["-c"], '[.livres[] | select(.genres[] | test("fiction"))] | length', LIB),
        (["-r"], '.livres[] | [.id, .titre, .annee] | @tsv', LIB),
        (["-r"], '.livres[] | [.id, .titre] | @csv', LIB),
        # def, closures, $params, recursion.
        (["-c"], "def f: reduce .[] as $x (0; . + $x); f", "[1,2,3]"),
        (["-c"], "def addvalue(f): f as $x | map(. + $x); addvalue(.[0])", "[[1,2],[10,20]]"),
        (["-c"], "def f($a; $b): $a + $b; f(.[0]; .[1])", "[1,2]"),
        (["-c"], "def fac: if . <= 1 then 1 else . * (. - 1 | fac) end; map(fac)", "[1,5,10]"),
        (["-c"], "def g: .+1; def f: g; def g: .+100; f", "1"),
        # Assignment, paths, del.
        (["-c"], ".a.b |= . + 1, .c = 5, .d += 1, .e //= 3", '{"a":{"b":1}}'),
        (["-c"], ".[] |= empty, (.[1:] = [\"x\"]), (.[2] = 9)", "[1,2,3]"),
        (["-c"], "del(.[1,2]), del(.[] | select(. == 3)), to_entries", "[1,2,3,4]"),
        (["-c"], "[paths], [paths(type == \"number\")], getpath([\"a\",1,\"b\"])", '{"a":[1,{"b":2}]}'),
        (["-c"], "path(..), [tostream], (tostream | select(length == 2))", '{"a":[1]}'),
        (["-c"], "pick(.b.c), with_entries(.value |= tostring), walk(if type == \"number\" then . + 1 else . end)", '{"a":1,"b":{"c":2,"d":3}}'),
        (["-c"], "reduce range(5) as $i ([]; .[$i] = $i * 2)", "null"),
        # Destructuring and string interpolation.
        (["-c"], ". as [$a, [$b]] | {$a, $b}", "[1,[2]]"),
        (["-c"], ". as {a: $x, $b, c: [$y]} | [$x, $b, $y]", '{"a":1,"b":2,"c":[3]}'),
        (["-c"], ".[] as [$a, $b] | {a: $a, b: $b}", "[[1,2],[3,4]]"),
        (["-c"], '"\\(1,2) \\(3,4)", "a\\(.x + 1)b", {"k\\(.x)": .x}', '{"x":1}'),
        (["-c"], '@json "v=\\(.)", @base64 "\\(.a)", (@base64 | @base64d), @uri "q=\\(.a)", @html "\\(.a)", @sh "echo \\(.a)"', '{"a":"x <y> & \'z\'"}'),
        # Regular expressions.
        (["-c"], 'test("B"; "i"), [match("o+"; "g") | .offset], sub("o"; "0"), gsub("(?<v>[aeiou])"; "<\\(.v)>")', '"foo bar boo"'),
        (["-c"], '[scan("[0-9]+")], capture("(?<w>[a-z]+) (?<n>[0-9]+)"), split(", *"; null), [splits("[0-9]")]', '"abc 123, de 45"'),
        # Generators and control flow.
        (["-c"], "[limit(3; repeat(1))], [range(0; 10; 3)], [1 | until(. > 100; . * 2)], [1 | while(. < 10; . * 2)]", "null"),
        (["-c"], "[label $out | foreach .[] as $i (0; . + $i; if . > 3 then ., break $out else . end)]", "[1,2,3,4]"),
        (["-c"], "first(.[]), last(.[]), nth(1; .[]), isempty(empty), [.[] | select(. > 1)] | add", "[1,2,3]"),
        (["-c"], "try error(\"x\") catch ., (try error({\"a\":1}) catch .a), [.[] | try tonumber catch \"bad\"]", '["1","x"]'),
        (["-c"], "INDEX(.id), (map(.id) | IN(.[]; 2)), ([.[] | .id] | combinations(1))", '[{"id":1},{"id":2}]'),
        # Dates in UTC, numbers, strings.
        (["-c"], "todate, gmtime, (gmtime | mktime), strftime(\"%Y-%m-%d %H:%M:%S\")", "1425599507"),
        (["-c"], 'fromdate, strptime("%Y-%m-%dT%H:%M:%SZ")', '"2015-03-05T23:51:47Z"'),
        (["-c"], "floor, sqrt, pow(.; 2), log10, (. % 3), ([., 2] | max)", "100"),
        (["-c"], "ascii_downcase, ltrimstr(\"A\"), (\"  x \" | trim), explode, (explode | implode), utf8bytelength", '"AbCé"'),
        (["-c"], "tojson, (tojson | fromjson), tostring, @text", '[1,"a",null]'),
        # Options.
        (["-c", "--arg", "x", "hi", "--argjson", "y", "[1,2]"], "{$x, $y, n: $ARGS.named}", "null"),
        (["-n", "-c"], "[inputs]", "1 2 3"),
        (["-c"], "[., input]", "1 2"),
        (["-s", "-c"], "., length", "1 2 3"),
        (["-R"], ".", "line one"),
        (["-n", "-c"], "$__loc__", "null"),
    ],
    "errors": [
        (["-c"], ".a", '[1]'),
        (["-c"], ".a", '{"a":1} [1] {"a":2}'),
        (["-c"], ".[0]", '{"a":1}'),
        (["-c"], ".[]", "1"),
        (["-c"], "1 / 0", "null"),
        (["-c"], 'error("custom")', "null"),
        (["-c"], ".", '{"a": [1 2]}'),
        (["-c"], '(1, error("x"), 2) // 3', "null"),
        (["-c"], 'error("x") // 1', "null"),
        (["-c"], "reverse", '"ab"'),
        (["-c"], '"\\u41"', "null"),
        (["-c"], '"\\ud800"', "null"),
        (["-c"], ".a?//1", "null"),
    ],
    "files": [
        (["-c"], ".statuses[0].user.screen_name", "@twitter.json"),
        (["-c"], ".statuses | length", "@twitter.json"),
        (["-r"], ".statuses[].user.lang", "@twitter.json"),
        (["-c"], ".statuses[] | select(.retweet_count > 10) | {id, n: .retweet_count}", "@twitter.json"),
        (["-c"], ".search_metadata", "@twitter.json"),
        (["-c"], "[.performances[] | .seatCategories | length] | add", "@citm_catalog.json"),
        (["-c"], ".areaNames | keys | length", "@citm_catalog.json"),
    ],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jq", required=True)
    ap.add_argument("--data", default=os.path.join(HERE, "data"))
    args = ap.parse_args()
    os.makedirs(os.path.join(HERE, "cases"), exist_ok=True)
    for name, specs in SPECS.items():
        out = [f"# Recorded from jq 1.8.2 by make_golden.py; do not edit by hand.\n"]
        for flags, program, inp in specs:
            if inp.startswith("@"):
                argv = [args.jq] + flags + [program, os.path.join(args.data, inp[1:])]
                stdin = b""
            else:
                argv = [args.jq] + flags + [program]
                stdin = (inp + "\n").encode("utf-8")
            p = subprocess.run(argv, input=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            block = []
            if flags:
                block.append("%%FLAGS " + " ".join(flags))
            if inp.startswith("@"):
                block.append("%%FILE " + inp[1:])
            if p.returncode:
                block.append(f"%%EXIT {p.returncode}")
            block.append(program)
            if not inp.startswith("@"):
                block.append(inp)
            text = p.stdout.decode("utf-8")
            if text and not text.endswith("\n"):
                raise SystemExit(f"{name}: output of {program!r} does not end with a newline")
            if "\n\n" in text or (text.startswith("\n")):
                raise SystemExit(f"{name}: output of {program!r} has an empty line")
            block.extend(text.split("\n")[:-1] if text else [])
            out.append("\n".join(block) + "\n\n")
        with open(os.path.join(HERE, "cases", name + ".test"), "w", encoding="utf-8", newline="\n") as f:
            f.write("".join(out))
        print(f"wrote cases/{name}.test ({len(specs)} cases)")


if __name__ == "__main__":
    main()
