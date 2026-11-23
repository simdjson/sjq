#!/bin/bash
# One timed run of each workload with jq and sjq; compares their outputs.
cd ~/sjqbench
JQ=bin/jq; SJQ=sjq/build/sjq
probe() {
  local file=$1; shift
  local t0=$(date +%s.%N); $JQ "$@" data/$file > /tmp/o_jq; local t1=$(date +%s.%N)
  $SJQ "$@" data/$file > /tmp/o_sjq; local t2=$(date +%s.%N)
  $SJQ --threads 1 "$@" data/$file > /tmp/o_sjq1; local t3=$(date +%s.%N)
  local same=no; cmp -s /tmp/o_jq /tmp/o_sjq && cmp -s /tmp/o_jq /tmp/o_sjq1 && same=yes
  printf "%-16s %-55s jq %6.2fs  sjq %6.2fs  sjq-1t %6.2fs  same=%s\n" $file "$*" $(echo "$t1-$t0" | bc) $(echo "$t2-$t1" | bc) $(echo "$t3-$t2" | bc) $same
}
probe tweets.ndjson -c .
probe tweets.ndjson .
probe tweets.ndjson -r .user.screen_name
probe tweets.ndjson -c 'select(.retweet_count > 100) | {id, user: .user.screen_name}'
probe tweets.ndjson -c '{id, lang, n: (.entities.hashtags | length)}'
probe tweets.ndjson -c 'select(.user.lang == "ja") | .id'
probe tweets.ndjson -r '.text | ascii_downcase | select(test("rt"))'
probe tweets.ndjson -r '"\(.id)\t\(.user.screen_name)"'
probe timeline.json '.statuses | length'
probe timeline.json -c '.statuses[100000].user'
probe timeline.json -c .
probe timeline.json -c '[.statuses[] | .retweet_count] | add'
probe timeline.json -c '.statuses | map(.user.followers_count) | max'
probe timeline.json -c '.statuses | group_by(.user.lang) | map({lang: .[0].user.lang, n: length})'
