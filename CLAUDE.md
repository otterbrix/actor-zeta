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
│   ├── spawn.hpp               # Actor allocation
│   ├── send.hpp                # Message sending
│   ├── actor/                  # Actor implementation
│   │   ├── basic_actor.hpp     # Basic actor alias
│   │   ├── dispatch.hpp        # Message dispatch
│   │   └── dispatch_traits.hpp # Dispatch traits
│   ├── mailbox/                # Message system
│   └── detail/                 # Internal (future.hpp, rtt.hpp)
test/                           # Catch2 tests
examples/                       # Usage examples
```

## Architecture

### Actor Lifecycle
1. Define actor class inheriting from `basic_actor<Actor>`
2. Define `dispatch_traits<&Actor::method1, &Actor::method2>`
3. Implement `behavior_t behavior(message*)` - coroutine with `co_await dispatch(...)`
4. Spawn: `auto actor = spawn<MyActor>(memory_resource, args...)`
5. Send: `auto [needs_sched, future] = send(actor.get(), &MyActor::method, args...)`
6. Schedule: `if (needs_sched) scheduler->enqueue(actor.get())`

### Actor Shutdown (CRITICAL)

**`scheduler->stop()` MUST be called BEFORE destroying actors.**

The scheduler holds raw pointers (`job_ptr`) to actors. If an actor is destroyed while the scheduler is running, worker threads may call `resume()` on freed memory (use-after-free).

#### Safe Pattern 1: Stop Scheduler First (Recommended)

```cpp
void safe_shutdown() {
    auto scheduler = std::make_unique<sharing_scheduler>(4, 1000);
    scheduler->start();

    auto actor = spawn<MyActor>(resource);

    // Send messages (fire-and-forget is OK)
    for (int i = 0; i < 100; ++i) {
        auto [needs_sched, future] = send(actor.get(), &MyActor::process, i);
        if (needs_sched) scheduler->enqueue(actor.get());
        future.detach();  // Fire-and-forget
    }

    scheduler->stop();  // All workers exit, no more resume() calls
}  // Actor destroyed here - SAFE
```

#### Safe Pattern 2: Wait for All Work

```cpp
void wait_for_work() {
    auto scheduler = std::make_unique<sharing_scheduler>(4, 1000);
    scheduler->start();

    std::vector<unique_future<int>> futures;

    {
        auto actor = spawn<MyActor>(resource);

        for (int i = 0; i < 10; ++i) {
            auto [needs_sched, future] = send(actor.get(), &MyActor::compute, i);
            if (needs_sched) scheduler->enqueue(actor.get());
            futures.push_back(std::move(future));  // Keep ALL futures
        }

        // Wait for ALL futures. The scheduler's workers produce; poll here.
        // is_ready() is the promise_released bit -- a promise dying without a
        // value sets it too -- so gate on failed() before taking.
        for (auto& f : futures) {
            while (!f.is_ready()) { std::this_thread::yield(); }
            if (f.failed()) { continue; }
            auto result = std::move(f).take_ready();
        }
    }  // Actor destroyed - SAFE (all work complete)

    scheduler->stop();
}
```

#### Safe Pattern 3: Actor Outlives Scheduler (RAII)

```cpp
class Application {
    std::unique_ptr<MyActor, pmr::deleter_t> actor_;    // Declared FIRST
    std::unique_ptr<sharing_scheduler> scheduler_;       // Declared SECOND

public:
    Application(std::pmr::memory_resource* res)
        : actor_(spawn<MyActor>(res))
        , scheduler_(std::make_unique<sharing_scheduler>(4, 1000)) {
        scheduler_->start();
    }

    ~Application() {
        // C++ destroys in REVERSE order:
        // 1. ~scheduler_ → stop() called, workers exit
        // 2. ~actor_ → SAFE
    }
};
```

#### Unsafe Patterns (DO NOT USE)

```cpp
// WRONG: Actor destroyed while scheduler running
{
    auto actor = spawn<MyActor>(resource);
    auto [needs_sched, fut] = send(actor.get(), &MyActor::process, data);
    if (needs_sched) scheduler->enqueue(actor.get());
}  // CRASH: workers may call resume() on freed memory
scheduler->stop();

// WRONG: Destroy actor with pending futures
unique_future<int> future;
{
    auto actor = spawn<MyActor>(resource);
    auto [_, f] = send(actor.get(), &MyActor::slow_compute, 42);
    future = std::move(f);
}  // CRASH: Actor freed while slow_compute running
auto result = std::move(future).take_ready();  // Use-after-free
```

| Scenario | Safe? |
|----------|-------|
| `stop()` then destroy actor | Yes |
| Wait all futures, then destroy | Yes |
| Actor declared before scheduler (RAII) | Yes |
| Destroy actor while scheduler running | **NO** |
| Destroy actor with pending futures | **NO** |

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

    actor_zeta::behavior_t behavior(actor_zeta::mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<MyActor, &MyActor::compute>) {
            co_await dispatch(this, &MyActor::compute, msg);
        } else if (cmd == msg_id<MyActor, &MyActor::notify>) {
            co_await dispatch(this, &MyActor::notify, msg);
        }
    }
};
```

### Message Sending
```cpp
// send() returns std::pair<bool, unique_future<T>>
// - first (needs_sched): true if actor needs to be scheduled
// - second: the future to get the result

// Request-response (scheduler-driven cross-thread: poll from this thread)
auto [needs_sched, future] = send(target.get(), &Target::compute, arg);
if (needs_sched) scheduler->enqueue(target.get());
while (!future.is_ready()) { std::this_thread::yield(); }
int result = future.failed() ? -1 : std::move(future).take_ready();

// Same-thread (no scheduler): pump the actor manually
auto [_, f] = send(actor.get(), &Actor::compute, 42);
while (!f.is_ready()) { auto info = actor->resume(1); if (!info.messages_processed) break; }
int result = f.failed() ? -1 : std::move(f).take_ready();

// Fire-and-forget
auto [_, fut] = send(target.get(), &Target::method, arg1, arg2);
fut.detach();
```

**Never call** `.get()` or `.available()` — they were removed. Bring the future to ready
via `co_await` (inside a coroutine) or by pumping the producing actor with `resume()`,
then extract with `take_ready()` (non-blocking, asserts readiness). `is_ready()` polls,
but it is only the `promise_released` bit — a promise that dies without a value sets it
too — so gate on `failed()` before taking.

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

// Caller - send() returns pair<bool, future>
auto [needs_sched, future] = send(actor.get(), &Actor::compute, 42);
if (needs_sched) scheduler->enqueue(actor.get());
while (!future.is_ready()) { std::this_thread::yield(); }
int result = future.failed() ? -1 : std::move(future).take_ready();

// Coroutine chaining
unique_future<int> chain(int x) {
    auto [needs_sched, f] = send(other.get(), &Other::process, x);
    if (needs_sched) scheduler->enqueue(other.get());
    int r = co_await std::move(f);
    co_return r + 10;
}
```

**See [`PROMISE_FUTURE_GUIDE.md`](PROMISE_FUTURE_GUIDE.md) for detailed patterns.**

## Recent Changes (2026)

- **`generator<T>` removed entirely**: no `detail/generator.hpp`, no
  `actor_zeta::generator` / `stream_error`, no `is_generator_v`, no generator
  branch in `dispatch()` / `send()`. Actor methods must return
  `unique_future<T>`, and `send()` always yields
  `std::pair<bool, unique_future<T>>`. The feature had no correct consumption
  route: `generator<T>` had no synchronous `next()`, and the advertised
  `while (co_await gen)` inside an actor method went through the one
  `await_transform` overload that never published an awaited chain — leaving the
  behavior alive-but-not-busy, which the scheduler could not tell from idle.
  Stream in batches instead (`unique_future<std::vector<T>>`), paging explicitly
  when the result does not fit in memory. See CHANGELOG for the full defect list
  and the constraints any future streaming design must satisfy.
- **Blocking future API removed**: no more `.get()`/`.wait()`/`.available()`, and no
  more exponential-backoff spinning inside `get()`. Use `co_await` (in coroutines),
  pump the producing actor with `resume()`, or poll `is_ready()` + `failed()` +
  `take_ready()` (cross-thread).
- **Legacy `future_state<T>` family removed**: deleted `detail/future_state.hpp`,
  `impl/detail/future_state.ipp`, `future_state_base`, `future_state_enum`,
  `future_states::`, and the intrusive_ptr overloads for the base.
  `result_storage<T>` moved to its own `detail/result_storage.hpp` (used by
  `shared_state`).
- **`actor_mixin` has no default `enqueue_impl`**: each Derived must define its own
  (enforced by the `has_enqueue_impl` concept). `cooperative_actor` (and thus
  `basic_actor`) is unaffected — it always provided its own. Sync actors on
  `actor_mixin` must add:
  ```cpp
  [[nodiscard]] std::pair<bool, detail::enqueue_result>
  enqueue_impl(mailbox::message_ptr msg) {
      behavior(msg.get());
      return {false, detail::enqueue_result::success};
  }
  ```
- **`message::set_command(message_id)`** added: enables non-blocking
  router/delegation patterns. See `examples/delegation/main.cpp`. The router restamps
  the command on the same message and forwards it to the worker's `enqueue_impl`;
  the caller's future is filled by the worker through the message's type-erased
  `result_slot_` (so the router never co_awaits / never blocks).

## Earlier Changes (2025-01)

- **`send()` API simplified**: No sender address, returns `std::pair<bool, future>`
- **`make_message()` simplified**: No sender address parameter
- **`behavior()` is now a coroutine**: Returns `behavior_t`, use `co_await dispatch(...)`
- **`enqueue_impl()` return type**: Now `std::pair<bool, enqueue_result>`
- Messages created in receiver's memory resource
- Unified actor state management (single atomic)
- Compile-time check for `T&&` to move-only types in coroutines

**See [`CHANGELOG.md`](CHANGELOG.md) for full history.**

## Debugging

### AddressSanitizer

Detect use-after-free and other memory issues:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build
cd build && ctest --output-on-failure
```

### ThreadSanitizer

Detect data races:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build
cd build && ctest --output-on-failure
```

### Typical ASan Error (Actor Lifetime Bug)

```
==PID==ERROR: AddressSanitizer: heap-use-after-free on address 0xXXXX
READ of size 1 at 0xXXXX thread TNN
    #0 ... in std::atomic<actor_state>::load()
    #1 ... in cooperative_actor::resume()
    #2 ... in resume_impl<MyActor>()

freed by thread T0 here:
    #0 ... in ~cooperative_actor()
```

**Solution:** Call `scheduler->stop()` BEFORE destroying actors.

## Additional Resources

- **[CHANGELOG.md](CHANGELOG.md)** - Change history
- **[PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md)** - Promise/Future guide
- **[docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md](docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md)** - GCC 11.4 bug workaround
- **Examples:** `examples/` directory
- **Tests:** `test/` directory