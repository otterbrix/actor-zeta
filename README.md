# actor-zeta

[![GCC](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_gcc.yaml/badge.svg?branch=develop)](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_gcc.yaml)
[![Clang](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_clang.yaml/badge.svg?branch=develop)](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_clang.yaml)

C++20 actor model with coroutines, PMR allocators, and no RTTI/exceptions.

## Features

- **Coroutines**: `co_await`, `co_return`, `co_yield`
- **unique_future<T>**: Async request-response
- **generator<T>**: Streaming data between actors
- **std::pmr**: Memory resource allocation
- **No RTTI/exceptions**: `-fno-rtti -fno-exceptions`

## Requirements

- C++20 (GCC 11+, Clang 12+)
- CMake >= 3.15
- Conan 2.x

## Quick Start

### Request-Response

```cpp
class Worker : public actor_zeta::basic_actor<Worker> {
public:
    actor_zeta::unique_future<int> compute(int x) {
        co_return x * 2;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&Worker::compute>;

    void behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<Worker, &Worker::compute>) {
            actor_zeta::dispatch(this, &Worker::compute, msg);
        }
    }
};

// Usage
auto future = actor_zeta::send(worker.get(), sender, &Worker::compute, 42);
int result = co_await std::move(future);
```

### Streaming

```cpp
class Producer : public actor_zeta::basic_actor<Producer> {
public:
    actor_zeta::generator<int> stream(int count) {
        for (int i = 0; i < count; ++i) {
            co_yield i;
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&Producer::stream>;
};

// Usage
auto gen = actor_zeta::send(producer.get(), sender, &Producer::stream, 10);
while (co_await gen) {
    process(gen.current());
}
```

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

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ALLOW_EXAMPLES` | OFF | Build examples |
| `ALLOW_TESTS` | OFF | Build tests |
| `RTTI_DISABLE` | ON | `-fno-rtti` |
| `EXCEPTIONS_DISABLE` | ON | `-fno-exceptions` |

## Documentation

- [CLAUDE.md](CLAUDE.md) - Development guide
- [PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md) - Request-response patterns
- [GENERATOR_GUIDE.md](GENERATOR_GUIDE.md) - Streaming patterns
- [CHANGELOG.md](CHANGELOG.md) - Change history

## License

BSD-3-Clause license
