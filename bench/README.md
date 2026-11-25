# Benchmarks

    python3 bench/make_data.py DATA --mb 1024      # tweets.ndjson, timeline.json
    PIN_DOCUMENT="taskset -c 1-127:2" bench/run_bench.sh DATA RESULTS /path/to/jq build/sjq /path/to/hyperfine
    python3 bench/plot.py RESULTS docs --machine "..."

`probe.sh` runs each workload once and checks that jq and sjq print the same
output. `plot.py` writes the charts (`docs/bench-*.svg`, light and dark) and
the table (`docs/_includes/results.md`) used by the README and the
documentation.

The published numbers were measured on a dual Intel Xeon Gold 6548N (64
cores, Linux, GCC 16.2), with jq 1.8.2's official static binary and
hyperfine 2.0.

On a machine with several sockets, a run on one core is up to 25% faster on
the socket whose memory holds the file: `PIN_DOCUMENT` pins every run on one
thread (jq, sjq without `-P`, and `sjq -P` on the single document) to one socket
(here, the odd CPUs). `ONLY="07 08 09"` reruns some workloads and keeps the
other results. `SKIP_JQ=1` times sjq only and keeps jq's results: they stay
valid only on the same machine, with the same data, jq and pinning (the
results from before every one-thread run was pinned must not be mixed with
newer ones).
