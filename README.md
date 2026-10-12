# actor-zeta

[![GCC](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_gcc.yaml/badge.svg?branch=master)](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_gcc.yaml)
[![Clang](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_clang.yaml/badge.svg?branch=master)](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_clang.yaml)

C++20 actor model: cooperative scheduling, request-response over coroutines and
`unique_future<T>`, `std::pmr` allocation, no RTTI, exceptions optional.

Everything lives under `header/`. Link the `actor-zeta` CMake target, or include
`<actor-zeta/src.hpp>` in exactly one translation unit to compile the `.ipp`
implementations yourself.

## Requirements

- C++20. CI builds GCC 11, 12, 13 and Clang 14, 16, 17, 18 on Ubuntu 22.04, and
  AppleClang on macOS 14 and 15.
- CMake 3.15+
- Conan 2 for the test and example dependencies (Catch2, Asio, benchmark)

## Build

```bash
conan profile detect --force
conan install . -of build -s build_type=Debug --build=missing

cmake -B build -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DALLOW_TESTS=ON \
  -DCMAKE_TOOLCHAIN_FILE=./build/Debug/generators/conan_toolchain.cmake
cmake --build build
ctest --test-dir build --output-on-failure
```

| Option | Default | Effect |
|--------|---------|--------|
| `ALLOW_EXAMPLES` | OFF | build `examples/` |
| `ALLOW_TESTS` | OFF | build `test/` (Catch2) |
| `ALLOW_BENCHMARK` | OFF | build `benchmark/` |
| `RTTI_DISABLE` | ON | `-fno-rtti` |
| `EXCEPTIONS_DISABLE` | ON | `-fno-exceptions`; OFF is a supported mode with its own CI job |

## Where next

- [CLAUDE.md](CLAUDE.md) — defining, spawning, messaging and shutting down actors; the rules the code holds to
- [docs/LIFECYCLE.md](docs/LIFECYCLE.md) — an actor from `spawn()` to `delete`: the turn, the verdicts, `close()`, the contract checks
- [PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md) — `unique_future<T>` shapes that work, and the traps
- [CHANGELOG.md](CHANGELOG.md) — history and migration guides
- `examples/` — `coroutine`, `delegation`, `balancer`, `broadcast`, `supervisor`, `external-drive`, `asio`

## License

BSD-3-Clause
