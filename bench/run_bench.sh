#!/bin/bash
# Times jq and sjq (default threads and --threads 1) with hyperfine.
# Usage: run_bench.sh DATA_DIR RESULTS_DIR JQ SJQ HYPERFINE
set -e
DATA=$1; OUT=$2; JQ=$3; SJQ=$4; HF=$5
mkdir -p "$OUT"
n=0
bench() {
  local label=$1 file=$2; shift 2
  n=$((n + 1))
  local args=""
  for a in "$@"; do args="$args '$a'"; done
  local id=$(printf %02d $n)
  "$HF" --shell=default --style basic --warmup 1 --runs 3 --export-json "$OUT/$id-jq.json" -n "jq" "$JQ $args $DATA/$file > /dev/null"
  "$HF" --shell=default --style basic --warmup 1 --runs 10 --export-json "$OUT/$id-sjq.json" -n "sjq" "$SJQ $args $DATA/$file > /dev/null"
  "$HF" --shell=default --style basic --warmup 1 --runs 10 --export-json "$OUT/$id-sjq1.json" -n "sjq --threads 1" "$SJQ --threads 1 $args $DATA/$file > /dev/null"
  printf '%s\t%s\t%s\n' "$(printf %02d $n)" "$label" "$file" >> "$OUT/index.tsv"
}
rm -f "$OUT/index.tsv"
bench "minify (-c .)" tweets.ndjson -c .
bench "pretty-print (.)" tweets.ndjson .
bench "extract a field" tweets.ndjson -r .user.screen_name
bench "select and build objects" tweets.ndjson -c 'select(.retweet_count > 100) | {id, user: .user.screen_name}'
bench "string interpolation" tweets.ndjson -r '"\(.id)\t\(.user.screen_name)"'
bench "regular expression" tweets.ndjson -r '.text | ascii_downcase | select(test("rt"))'
bench "count elements" timeline.json '.statuses | length'
bench "minify (-c .)" timeline.json -c .
bench "group_by" timeline.json -c '.statuses | group_by(.user.lang) | map({lang: .[0].user.lang, n: length})'
