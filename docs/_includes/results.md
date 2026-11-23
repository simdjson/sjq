| Input | Workload | Options and filter | jq 1.8.2 | sjq --threads 1 | sjq | Speedup |
|---|---|---|---:|---:|---:|---:|
| tweets.ndjson | minify (-c .) | `-c '.'` | 22.3 s | 1.24 s | 0.16 s | 139x |
| tweets.ndjson | pretty-print (.) | `'.'` | 24.5 s | 1.49 s | 0.18 s | 140x |
| tweets.ndjson | extract a field | `-r '.user.screen_name'` | 11.5 s | 0.28 s | 0.09 s | 123x |
| tweets.ndjson | select and build objects | `-c 'select(.retweet_count > 100) \| {id, user: .user.screen_name}'` | 11.5 s | 0.28 s | 0.09 s | 123x |
| tweets.ndjson | string interpolation | `-r '"\(.id)\t\(.user.screen_name)"'` | 11.7 s | 0.35 s | 0.10 s | 118x |
| tweets.ndjson | regular expression | `-r '.text \| ascii_downcase \| select(test("rt"))'` | 25.2 s | 0.61 s | 0.61 s | 42x |
| timeline.json | count elements | `'.statuses \| length'` | 12.5 s | 0.69 s | 0.80 s | 16x |
| timeline.json | minify (-c .) | `-c '.'` | 16.5 s | 1.32 s | 1.39 s | 12x |
| timeline.json | group_by | `-c '.statuses \| group_by(.user.lang) \| map({lang: .[0].user.lang, n: length})'` | 12.4 s | 0.95 s | 1.00 s | 12x |

Machine: dual Intel Xeon Gold 6548N (64 cores, 128 threads), Linux, GCC 16.2, jq 1.8.2 static release binary, hyperfine 2.0.
