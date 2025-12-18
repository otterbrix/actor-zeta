# actor-zeta

|    compiler    | Master | Develop |
|:--------------:|:---:|:---:|
| gcc 9+ | |[![ubuntu](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_gcc.yaml/badge.svg?branch=develop)](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_gcc.yaml) |
| clang 10+ | |[![ubuntu](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_clang.yaml/badge.svg?branch=develop)](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_clang.yaml)|

A lightweight, high-performance C++20 virtual actor model implementation with:
- **Coroutine support** (`co_await`, `co_return`)
- **Promise/Future** for async request-response
- **Custom PMR allocators** for memory management
- **No RTTI, no exceptions** - compiles with `-fno-rtti -fno-exceptions`

## Requirements

- **C++20** compiler (GCC 9+, Clang 10+, MSVC 2019+)
- CMake >= 3.15
- Conan 2.x (for dependencies)

## Quick Example

### Coroutine Actor

```cpp
#include <actor-zeta.hpp>

class compute_actor final : public actor_zeta::basic_actor<compute_actor> {
public:
    explicit compute_actor(actor_zeta::pmr::memory_resource* res)
        : actor_zeta::basic_actor<compute_actor>(res) {}

    // Coroutine method - returns unique_future<T>
    actor_zeta::unique_future<int> compute(int x) {
        co_return x * 2;
    }

    // Coroutine with co_await
    actor_zeta::unique_future<int> compute_chain(int x) {
        auto future = actor_zeta::send(this, address(), &compute_actor::compute, x);
        int result = co_await std::move(future);  // Wait for result
        co_return result + 10;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &compute_actor::compute,
        &compute_actor::compute_chain
    >;

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<compute_actor, &compute_actor::compute>) {
            dispatch(this, &compute_actor::compute, msg);
        } else if (cmd == actor_zeta::msg_id<compute_actor, &compute_actor::compute_chain>) {
            dispatch(this, &compute_actor::compute_chain, msg);
        }
    }
};
```

### Getting Results

```cpp
// Method 1: co_await in coroutine (recommended)
actor_zeta::unique_future<int> caller_method() {
    auto future = actor_zeta::send(target, address(), &Target::compute, 21);
    int result = co_await std::move(future);  // Suspends until ready
    co_return result;
}

// Method 2: Polling (for non-coroutine code)
auto future = actor_zeta::send(target, address(), &Target::compute, 21);
while (!future.available()) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}
int result = std::move(future).get();  // Extract ready result

// Method 3: Check and extract
auto future = actor_zeta::send(target, address(), &Target::compute, 21);
if (future.available()) {
    int result = std::move(future).get();
}
```

## API Overview

### unique_future<T>

| Method | Description |
|--------|-------------|
| `available()` | Check if result is ready |
| `get() &&` | Extract result (requires `available() == true`) |
| `failed()` | Check if future has error |
| `cancel()` | Cancel the future |
| `co_await future` | Coroutine wait (via `operator co_await`) |

### Sending Messages

```cpp
// Send message and get future for result
auto future = actor_zeta::send(actor, sender_address, &Actor::method, args...);

// Wait for result in coroutine
int result = co_await std::move(future);
```

## Header-Only Mode

To use as header-only, add this line in **exactly one** source file:

```cpp
#include <actor-zeta/src.hpp>
```

## Build from Source

```bash
# Install dependencies
conan profile detect --force
conan install . -of build -s build_type=Debug --build=missing

# Configure
cmake -B build -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DALLOW_EXAMPLES=ON \
  -DALLOW_TESTS=ON \
  -DRTTI_DISABLE=ON \
  -DEXCEPTIONS_DISABLE=ON \
  -DCMAKE_TOOLCHAIN_FILE=./build/Debug/generators/conan_toolchain.cmake

# Build
cmake --build build

# Test
cd build && ctest --output-on-failure
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ALLOW_EXAMPLES` | OFF | Build examples |
| `ALLOW_TESTS` | OFF | Build tests |
| `RTTI_DISABLE` | ON | Compile with `-fno-rtti` |
| `EXCEPTIONS_DISABLE` | ON | Compile with `-fno-exceptions` |

## Documentation

- [CLAUDE.md](CLAUDE.md) - Development guide
- [PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md) - Promise/Future patterns
- [CHANGELOG.md](CHANGELOG.md) - Change history