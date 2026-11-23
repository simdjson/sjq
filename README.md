# sjq

A jq-compatible command-line JSON processor built on
[simdjson](https://github.com/simdjson/simdjson).

```
sjq '.statuses[] | {id, name: .user.name}' twitter.json
```

sjq was first developed in the simdjson repository
(simdjson/simdjson#3143) and moved here.

## Building

sjq needs CMake 3.15 or later, a C++17 compiler and Python 3 for the tests.
CMake downloads simdjson 5.3.0; with `-D SJQ_SYSTEM_SIMDJSON=ON` it uses an
installed simdjson instead.

    cmake -B build
    cmake --build build
    ctest --test-dir build --output-on-failure
    cmake --install build

## License

Apache 2.0 or MIT, at your option, as simdjson. The files under `tests/jq`
come from jq (MIT, see `tests/jq/COPYING`).
