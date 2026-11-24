| Input | Workload | Options and filter | jq 1.8.2 | sjq --threads 1 | sjq | Speedup |
|---|---|---|---:|---:|---:|---:|
| tweets.ndjson | minify (-c .) | `-c '.'` | 24.4 s | 1.24 s | 0.14 s | 169x |
| tweets.ndjson | pretty-print (.) | `'.'` | 24.5 s | 1.50 s | 0.18 s | 139x |
| tweets.ndjson | extract a field | `-r '.user.screen_name'` | 11.5 s | 0.30 s | 0.06 s | 181x |
| tweets.ndjson | select and build objects | `-c 'select(.retweet_count > 100) \| {id, user: .user.screen_name}'` | 11.4 s | 0.29 s | 0.06 s | 184x |
| tweets.ndjson | string interpolation | `-r '"\(.id)\t\(.user.screen_name)"'` | 11.6 s | 0.35 s | 0.07 s | 167x |
| tweets.ndjson | regular expression | `-r '.text \| ascii_downcase \| select(test("rt"))'` | 25.2 s | 0.59 s | 0.59 s | 43x |
| timeline.json | count elements | `'.statuses \| length'` | 11.8 s | 0.67 s | 0.67 s | 18x |
| timeline.json | minify (-c .) | `-c '.'` | 16.7 s | 1.33 s | 1.33 s | 13x |
| timeline.json | group_by | `-c '.statuses \| group_by(.user.lang) \| map({lang: .[0].user.lang, n: length})'` | 12.2 s | 0.96 s | 0.96 s | 13x |

Machine: dual Intel Xeon Gold 6548N (64 cores, 128 threads), Linux, GCC 16.2, jq 1.8.2 static release binary, hyperfine 2.0; the runs on one thread (jq, sjq --threads 1, and sjq on the single document) are pinned to one socket.
