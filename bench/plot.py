#!/usr/bin/env python3
"""Draws the benchmark charts (SVG, light and dark) and a Markdown table.

Usage: plot.py RESULTS_DIR OUT_DIR [--machine TEXT]

RESULTS_DIR holds what run_bench.sh wrote: index.tsv and, per workload,
NN-jq.json, NN-sjq.json, NN-sjq1.json (hyperfine exports). OUT_DIR gets
bench-<dataset>-<light|dark>.svg and results.md.
"""
import argparse
import json
import os
import shlex
from html import escape

# Series, in legend order: name, result suffix, light color, dark color.
SERIES = [
    ("sjq -P", "sjq", "#2a78d6", "#3987e5"),
    ("sjq", "sjq1", "#eb6834", "#d95926"),
    ("jq 1.8.2", "jq", "#1baf7a", "#199e70"),
]
THEMES = {
    "light": {"surface": "#fcfcfb", "ink": "#0b0b0b", "ink2": "#52514e", "muted": "#898781",
              "grid": "#e1e0d9", "axis": "#c3c2b7"},
    "dark": {"surface": "#1a1a19", "ink": "#f0efec", "ink2": "#c3c2b7", "muted": "#898781",
             "grid": "#2c2c2a", "axis": "#383835"},
}
DATASETS = {
    "tweets.ndjson": ("ndjson", "1 GB of NDJSON (230,138 tweets, one per line)"),
    "timeline.json": ("document", "One 840 MB document ({\"statuses\": [133,229 tweets]})"),
}
FONT = "system-ui, -apple-system, 'Segoe UI', sans-serif"


def load(results):
    rows = []
    with open(os.path.join(results, "index.tsv"), encoding="utf-8") as f:
        for line in f:
            wid, label, data = line.rstrip("\n").split("\t")
            times = {}
            for _, suffix, _, _ in SERIES:
                with open(os.path.join(results, f"{wid}-{suffix}.json"), encoding="utf-8") as g:
                    r = json.load(g)["results"][0]
                if "summary" in r:  # hyperfine 2
                    wall = r["summary"]["time_wall_clock"]
                    times[suffix] = (wall["mean"], wall.get("stddev") or 0.0, r["command"])
                else:
                    times[suffix] = (r["mean"], r["stddev"] or 0.0, r["command"])
            rows.append({"id": wid, "label": label, "data": data, "times": times})
    return rows


def fmt_s(t):
    return f"{t:.2f} s" if t < 10 else f"{t:.1f} s"


def nice_max(v):
    # The smallest round step that covers v in at most six ticks.
    for step in (0.5, 1, 2, 5, 10, 20, 50, 100):
        ticks = -(-v // step)
        if ticks <= 6:
            return step * ticks, step
    return v, v / 5


def chart(rows, title, theme, colors):
    t = THEMES[theme]
    bar, gap, group_gap = 14, 2, 22
    left, right, top = 210, 160, 104
    plot_w = 520
    n = len(SERIES)
    group_h = n * bar + (n - 1) * gap
    height = top + len(rows) * (group_h + group_gap) + 46
    width = left + plot_w + right
    vmax = max(r["times"][s][0] for r in rows for _, s, _, _ in SERIES)
    axis_max, step = nice_max(vmax)
    x = lambda v: left + plot_w * v / axis_max
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" '
           f'role="img" aria-label="{escape(title)}" font-family="{FONT}">',
           f'<rect width="{width}" height="{height}" rx="8" fill="{t["surface"]}"/>',
           f'<text x="24" y="32" font-size="16" font-weight="600" fill="{t["ink"]}">{escape(title)}</text>',
           f'<text x="24" y="52" font-size="12" fill="{t["ink2"]}">Wall time in seconds, lower is better</text>']
    # Legend, under the subtitle.
    lx = 24
    for name, _, light, dark in SERIES:
        c = light if theme == "light" else dark
        out.append(f'<rect x="{lx}" y="66" width="10" height="10" rx="2" fill="{c}"/>'
                   f'<text x="{lx + 15}" y="75" font-size="12" fill="{t["ink2"]}">{escape(name)}</text>')
        lx += 15 + int(6.6 * len(name)) + 22
    # Gridlines and axis ticks.
    plot_bottom = height - 40
    v = 0.0
    while v <= axis_max + 1e-9:
        gx = x(v)
        out.append(f'<line x1="{gx:.1f}" y1="{top - 8}" x2="{gx:.1f}" y2="{plot_bottom}" stroke="{t["grid"]}" stroke-width="1"/>')
        out.append(f'<text x="{gx:.1f}" y="{plot_bottom + 18}" font-size="11" fill="{t["muted"]}" text-anchor="middle" '
                   f'style="font-variant-numeric: tabular-nums">{v:g}</text>')
        v += step
    out.append(f'<line x1="{left}" y1="{top - 8}" x2="{left}" y2="{plot_bottom}" stroke="{t["axis"]}" stroke-width="1"/>')
    y = top
    for r in rows:
        out.append(f'<text x="{left - 12}" y="{y + group_h / 2 + 4:.1f}" font-size="13" fill="{t["ink"]}" text-anchor="end">{escape(r["label"])}</text>')
        jq_time = r["times"]["jq"][0]
        for i, (name, suffix, light, dark) in enumerate(SERIES):
            mean, sd, cmd = r["times"][suffix]
            c = light if theme == "light" else dark
            by = y + i * (bar + gap)
            w = max(2.0, x(mean) - left)
            # Square at the baseline, 4px rounded at the data end.
            rr = min(4.0, w / 2)
            path = (f'M{left},{by} h{w - rr:.2f} a{rr},{rr} 0 0 1 {rr},{rr} v{bar - 2 * rr} '
                    f'a{rr},{rr} 0 0 1 {-rr},{rr} h{-(w - rr):.2f} z')
            label = fmt_s(mean)
            if suffix != "jq":
                label += f"  ({jq_time / mean:.0f}x faster)"
            tip = f"{name}: {fmt_s(mean)} (sd {sd:.3f} s)"
            out.append(f'<path d="{path}" fill="{c}"><title>{escape(tip)}</title></path>')
            out.append(f'<text x="{left + w + 6:.1f}" y="{by + bar - 3}" font-size="11" fill="{t["ink2"]}" '
                       f'style="font-variant-numeric: tabular-nums">{escape(label)}</text>')
        y += group_h + group_gap
    out.append("</svg>")
    return "\n".join(out) + "\n"


def table(rows, machine):
    lines = ["| Input | Workload | Options and filter | jq 1.8.2 | sjq | sjq -P | Speedup |",
             "|---|---|---|---:|---:|---:|---:|"]
    for r in rows:
        # The options and the filter, without the program, the file and the
        # redirection.
        words = shlex.split(r["times"]["sjq1"][2].rsplit(" > ", 1)[0])
        # Skip a prefix such as taskset, then the program; drop the file.
        start = next(k for k, w in enumerate(words) if w.endswith("sjq")) + 1
        words = words[start:-1]
        args = " ".join(w if w.startswith("-") else "'" + w + "'" for w in words).replace("|", "\\|")
        j, s1, s = (r["times"][k][0] for k in ("jq", "sjq1", "sjq"))
        lines.append(f"| {r['data']} | {r['label']} | `{args}` | {fmt_s(j)} | {fmt_s(s1)} | {fmt_s(s)} | {j / s:.0f}x |")
    if machine:
        lines += ["", f"Machine: {machine}."]
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results")
    ap.add_argument("outdir")
    ap.add_argument("--machine", default="")
    args = ap.parse_args()
    rows = load(args.results)
    os.makedirs(args.outdir, exist_ok=True)
    for data, (slug, title) in DATASETS.items():
        sub = [r for r in rows if r["data"] == data]
        if not sub:
            continue
        for theme in THEMES:
            with open(os.path.join(args.outdir, f"bench-{slug}-{theme}.svg"), "w", encoding="utf-8") as f:
                f.write(chart(sub, title, theme, None))
    os.makedirs(os.path.join(args.outdir, "_includes"), exist_ok=True)
    with open(os.path.join(args.outdir, "_includes", "results.md"), "w", encoding="utf-8") as f:
        f.write(table(rows, args.machine))


if __name__ == "__main__":
    main()
