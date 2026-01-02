# CLAUDE.md

Guidance for Claude Code when working with this repository.

## Core Rules

1. **READ FULL FILES** before making changes - this codebase has intricate template metaprogramming
2. **NO RTTI, NO EXCEPTIONS** - code MUST compile with `-fno-rtti -fno-exceptions`
3. **USE PMR** - never use `new`/`delete` directly, always `spawn<Actor>(memory_resource, args...)`
4. **BUILD AND TEST** after every change

## Project Overview

actor-zeta is a C++20 **header-only** actor model with cooperative scheduling, PMR memory management, and no RTTI/exceptions dependencies.

## Quick Start

```bash
# Setup
conan profile detect --force
conan install . -of build -s build_type=Debug --build=missing

# Build
cmake -B build -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DALLOW_EXAMPLES=ON \
  -DALLOW_TESTS=ON \
  -DRTTI_DISABLE=ON \
  -DEXCEPTIONS_DISABLE=ON \
  -DCMAKE_TOOLCHAIN_FILE=./build/Debug/generators/conan_toolchain.cmake
cmake --build build

# Test
cd build && ctest --output-on-failure
```

**CLion:** Uses `cmake-build-debug/`. Set toolchain: `-DCMAKE_TOOLCHAIN_FILE=cmake-build-debug/conan/Debug/generators/conan_toolchain.cmake`

## File Structure

```
header/
├── actor-zeta.hpp              # Main API
├── actor-zeta/
│   ├── core.hpp                # Core types
│   ├── spawn.hpp               # Actor allocation
│   ├── send.hpp                # Message sending
│   ├── actor/                  # Actor implementation
│   │   ├── basic_actor.hpp     # Basic actor alias
│   │   ├── dispatch.hpp        # Message dispatch
│   │   └── dispatch_traits.hpp # Dispatch traits
│   ├── mailbox/                # Message system
│   └── detail/                 # Internal (future.hpp, generator.hpp, rtt.hpp)
test/                           # Catch2 tests
examples/                       # Usage examples
```

## Architecture

### Actor Lifecycle
1. Define actor class inheriting from `basic_actor<Actor>`
2. Define `dispatch_traits<&Actor::method1, &Actor::method2>`
3. Implement `behavior(message*)` to dispatch messages
4. Spawn: `auto actor = spawn<MyActor>(memory_resource, args...)`
5. Send: `send(actor, sender, &MyActor::method, args...)`

### Memory Management
- Uses `std::pmr::memory_resource`
- **spawn()** returns `unique_ptr<Actor, pmr::deleter_t>`
- Messages created in **receiver's** memory resource

### Type System (no RTTI)
- `rtt.hpp` - custom runtime type info
- `intrusive_ptr<T>` - reference counting (not `shared_ptr`)

## Code Conventions

### RTTI and Exceptions
```cpp
// NEVER:
throw std::runtime_error("error");  // NO EXCEPTIONS
typeid(MyClass).name();             // NO RTTI
dynamic_cast<Derived*>(ptr);        // NO RTTI

// INSTEAD:
assert(condition && "error message");
// Use custom RTT system from rtt.hpp
```

### Actor Definition Pattern
```cpp
class MyActor final : public basic_actor<MyActor> {
public:
    unique_future<int> compute(int x) { co_return x * 2; }
    unique_future<void> notify(std::string msg) { co_return; }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &MyActor::compute,
        &MyActor::notify
    >;

    explicit MyActor(std::pmr::memory_resource* ptr)
        : basic_actor<MyActor>(ptr) {}

    void behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<MyActor, &MyActor::compute>) {
            dispatch(this, &MyActor::compute, msg);
        } else if (cmd == msg_id<MyActor, &MyActor::notify>) {
            dispatch(this, &MyActor::notify, msg);
        }
    }
};
```

### Message Sending
```cpp
// Fire-and-forget
send(target, sender, &Target::method, arg1, arg2);

// Request-response
auto future = send(target, sender, &Target::compute, arg);
int result = std::move(future).get();
```

### Coroutine Parameters
```cpp
// For move-only types, use by-value (NOT T&&)
unique_future<void> process(std::unique_ptr<Data> data) { ... }  // OK
unique_future<void> process(std::unique_ptr<Data>&& data) { ... } // COMPILE ERROR
// See docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ALLOW_EXAMPLES` | OFF | Build examples |
| `ALLOW_TESTS` | OFF | Build tests (Catch2) |
| `RTTI_DISABLE` | ON | `-fno-rtti` |
| `EXCEPTIONS_DISABLE` | ON | `-fno-exceptions` |

## Common Mistakes

| Mistake | Correct |
|---------|---------|
| `new MyActor(...)` | `spawn<MyActor>(resource, ...)` |
| `std::shared_ptr<Actor>` | `intrusive_ptr<Actor>` |
| `throw`/`try`/`catch` | `assert()` or error codes |
| `typeid`/`dynamic_cast` | Custom RTT system |
| `T&&` for move-only in coroutines | `T` by-value |
| `const T&` in coroutines | `T` by-value (dangling after co_await) |

## Promise/Future System

Async request-response via `unique_future<T>`:

```cpp
// Handler as coroutine
unique_future<int> compute(int x) { co_return x * x; }

// Caller
auto future = send(actor, sender, &Actor::compute, 42);
int result = std::move(future).get();  // Blocks until ready

// Coroutine chaining
unique_future<int> chain(int x) {
    auto f = send(other, address(), &Other::process, x);
    int r = co_await std::move(f);
    co_return r + 10;
}
```

**See [`PROMISE_FUTURE_GUIDE.md`](PROMISE_FUTURE_GUIDE.md) for detailed patterns.**

## Generator System

Streaming data via `generator<T>` with `co_yield`:

```cpp
generator<int> stream_range(int start, int end) {
    for (int i = start; i < end; ++i) {
        co_yield i;
    }
}

// Consumer (in coroutine)
while (co_await gen) {
    auto& value = gen.current();
    process(value);
}
```

**See [`GENERATOR_GUIDE.md`](GENERATOR_GUIDE.md) for detailed patterns.**

## Recent Changes (2025-01)

- Messages created in receiver's memory resource
- Unified actor state management (single atomic)
- Exponential backoff in `future.get()` (99% CPU reduction)
- Compile-time check for `T&&` to move-only types in coroutines

**See [`CHANGELOG.md`](CHANGELOG.md) for full history.**

## Additional Resources

- **[CHANGELOG.md](CHANGELOG.md)** - Change history
- **[PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md)** - Promise/Future guide
- **[GENERATOR_GUIDE.md](GENERATOR_GUIDE.md)** - Generator guide
- **[docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md](docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md)** - GCC 11.4 bug workaround
- **Examples:** `examples/` directory
- **Tests:** `test/` directory