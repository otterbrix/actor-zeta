# CLAUDE.md

Guidance for Claude Code when working with this repository. The code is the source of
truth; when this file and a header disagree, the header wins and this file gets fixed.

## Core Rules

1. **READ FULL FILES** before making changes. The template metaprogramming is intricate.
2. **NO RTTI.** Code must compile with `-fno-rtti`. **Exceptions are optional**: the
   default is `-fno-exceptions` and the library must build and pass there;
   `EXCEPTIONS_DISABLE=OFF` is a supported mode with its own CI job. Library code never
   `throw`s; user code may, and the library carries it to the caller.
3. **USE PMR.** Never `new`/`delete` an actor: `spawn<Actor>(memory_resource, args...)`.
4. **BUILD AND TEST** after every change.

## Project Overview

C++20 actor model with cooperative scheduling and `std::pmr` memory management. All
code is under `header/`; `source/src.cpp` compiles the `.ipp` implementations into the
`actor-zeta` target (or include `<actor-zeta/src.hpp>` in one translation unit).

## Quick Start

```bash
conan profile detect --force
conan install . -of build -s build_type=Debug --build=missing

cmake -B build -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DALLOW_EXAMPLES=ON \
  -DALLOW_TESTS=ON \
  -DRTTI_DISABLE=ON \
  -DEXCEPTIONS_DISABLE=ON \
  -DCMAKE_TOOLCHAIN_FILE=./build/Debug/generators/conan_toolchain.cmake
cmake --build build
cd build && ctest --output-on-failure
```

**CLion:** builds in `cmake-build-debug/`; pass the same `-DCMAKE_TOOLCHAIN_FILE` in
the CMake profile options. Options: `ALLOW_EXAMPLES`, `ALLOW_TESTS` (Catch2),
`ALLOW_BENCHMARK` (all OFF), `RTTI_DISABLE`, `EXCEPTIONS_DISABLE` (both ON).

## File Structure

```
header/
├── actor-zeta.hpp              # Umbrella header
├── actor-zeta/
│   ├── spawn.hpp               # spawn<Actor>(resource, args...)
│   ├── send.hpp                # send(actor, &Actor::method, args...)
│   ├── src.hpp                 # Pulls in every .ipp (one TU)
│   ├── actor/                  # basic_actor alias, cooperative_actor, dispatch, dispatch_traits, address_t
│   ├── mailbox/                # message, make_message, default_mailbox
│   ├── scheduler/              # scheduler_t, sharing_scheduler, worker, job_ptr, resume_result
│   ├── detail/                 # future.hpp, shared_state.hpp, behavior_t.hpp, rtt.hpp, ...
│   └── impl/                   # .ipp implementations
test/                           # Catch2 tests; test/tooltestsuites has scheduler_test_t
examples/
```

## Architecture

### Actor Lifecycle
1. Inherit from `basic_actor<Actor>`
2. Declare `using dispatch_traits = actor_zeta::dispatch_traits<&Actor::m1, &Actor::m2>`
3. Implement `behavior_t behavior(mailbox::message*)`: a coroutine that `co_await dispatch(...)`
4. `auto actor = spawn<MyActor>(memory_resource, args...)`
5. `auto [needs_sched, future] = send(actor.get(), &MyActor::method, args...)`
6. `if (needs_sched) scheduler->enqueue(actor.get())`

### Actor Shutdown (CRITICAL)

The scheduler holds raw `job_ptr`s to actors and has no destructor: `scheduler_t`
does not stop its workers when destroyed, and destroying a started scheduler runs
`~std::thread` on joinable threads, which is `std::terminate()`. Two rules follow:

- **`scheduler->stop()` must be called explicitly**, exactly once, before the scheduler is destroyed.
- **`stop()` must run before any actor the scheduler may still hold is destroyed**, or a worker calls `resume()` on freed memory.

#### Safe Pattern 1: Stop Scheduler First (Recommended)

```cpp
auto scheduler = std::make_unique<sharing_scheduler>(4, 1000);
scheduler->start();
auto actor = spawn<MyActor>(resource);
for (int i = 0; i < 100; ++i) {
    auto [needs_sched, future] = send(actor.get(), &MyActor::process, i);
    if (needs_sched) scheduler->enqueue(actor.get());
    future.detach();
}
scheduler->stop();   // every worker joined; nothing can resume the actor now
// actor destroyed after this: safe
```

#### Safe Pattern 2: Wait for All Work

```cpp
std::vector<unique_future<int>> futures;
{
    auto actor = spawn<MyActor>(resource);
    for (int i = 0; i < 10; ++i) {
        auto [needs_sched, future] = send(actor.get(), &MyActor::compute, i);
        if (needs_sched) scheduler->enqueue(actor.get());
        futures.push_back(std::move(future));   // keep every future
    }
    for (auto& f : futures) {
        while (!f.is_ready()) { std::this_thread::yield(); }
        if (f.failed()) { continue; }           // is_ready() is not a value gate
        auto result = std::move(f).take_ready();
    }
}  // actor destroyed: safe, all work complete
scheduler->stop();
```

#### Safe Pattern 3: Actor Outlives Scheduler (RAII)

```cpp
class Application {
    std::unique_ptr<MyActor, pmr::deleter_t> actor_;   // declared first
    std::unique_ptr<sharing_scheduler> scheduler_;      // declared second
public:
    explicit Application(std::pmr::memory_resource* res)
        : actor_(spawn<MyActor>(res))
        , scheduler_(std::make_unique<sharing_scheduler>(4, 1000)) { scheduler_->start(); }
    ~Application() {
        scheduler_->stop();   // not automatic; members then destroy in reverse: ~scheduler_, ~actor_
    }
};
```

The declaration order puts `~scheduler_` before `~actor_`; the `stop()` call is what
makes the pattern safe.

#### Unsafe Pattern (DO NOT USE)

```cpp
// WRONG: the actor dies while a worker may still hold its job_ptr. ~cooperative_actor
// waits for the current resume() to return, then frees; the worker's next resume() is
// a use-after-free. Keeping the future does not help: it settles as failed()
// (broken_pipe) if the frame was torn down, but the scheduler still holds the actor.
{
    auto actor = spawn<MyActor>(resource);
    auto [needs_sched, f] = send(actor.get(), &MyActor::process, data);
    if (needs_sched) scheduler->enqueue(actor.get());
    future = std::move(f);
}
scheduler->stop();   // too late
```

| Scenario | Safe? |
|----------|-------|
| `stop()` then destroy actor | Yes |
| Wait all futures, then destroy | Yes |
| Actor declared before scheduler (RAII), `stop()` called in the destructor | Yes |
| Destroy a started scheduler without `stop()` | **NO**: `std::terminate()` |
| Destroy actor while scheduler running | **NO** |
| Destroy actor with pending futures | **NO** |

### Memory Management
- `spawn()` returns `std::unique_ptr<Actor, pmr::deleter_t>`
- Messages and their `shared_state` are allocated from the **receiver's** memory resource
- Coroutine frames come from the actor's resource: a `unique_future<T>` coroutine must
  be an inline actor member function or take a `std::pmr::memory_resource*`

### Type System (no RTTI)
- `detail/rtt.hpp`: runtime-typed message bodies
- Actors are owned by the `unique_ptr` from `spawn()` and referred to by `address_t`
  (`actor->address()`); `detail/intrusive_ptr.hpp` exists but is not used for actors

## Code Conventions

### RTTI and Exceptions

```cpp
// NEVER, in library code:
typeid(MyClass).name();             // no RTTI
dynamic_cast<Derived*>(ptr);        // no RTTI
throw std::runtime_error("error");  // the library never throws
// INSTEAD: assert() for impossible states, error codes for contracts, rtt.hpp for runtime types
```

User code may throw, only with `EXCEPTIONS_DISABLE=OFF`. With `-fno-exceptions` the
compiler emits no catch wrapper for a coroutine body and none of this exists.

| Thrown from | Reaches |
|-------------|---------|
| an actor method, called directly | its own future; `take_ready()` rethrows it |
| an actor method, `co_await`ed by another method | outward along the chain |
| an actor method, reached via `send()`/`dispatch()` | the caller's future, with the original exception; a poller sees `failed()` and `std::errc::interrupted` |
| `behavior()` itself, past `dispatch()` | nowhere: `behavior_t` is the root of the chain. Reported on stderr and discarded; the actor takes the next message |

Put work that can throw in a dispatched method, not in `behavior()`.

### Actor Definition Pattern
```cpp
class MyActor final : public basic_actor<MyActor> {
public:
    unique_future<int> compute(int x) { co_return x * 2; }
    unique_future<void> notify(std::string msg) { co_return; }

    using dispatch_traits = actor_zeta::dispatch_traits<&MyActor::compute, &MyActor::notify>;

    explicit MyActor(std::pmr::memory_resource* ptr) : basic_actor<MyActor>(ptr) {}

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
// send() returns std::pair<bool, unique_future<T>>: {needs_sched, future}

// Cross-thread, scheduler-driven: poll from this thread
auto [needs_sched, future] = send(target.get(), &Target::compute, arg);
if (needs_sched) scheduler->enqueue(target.get());
while (!future.is_ready()) { std::this_thread::yield(); }
int result = future.failed() ? -1 : std::move(future).take_ready();

// Same thread, no scheduler: pump the actor. `resume` means "run me again";
// bound the loop, since an actor awaiting a producer nobody drives says it forever.
auto [_, f] = send(actor.get(), &Actor::compute, 42);
for (int i = 0; i < 100 && !f.is_ready(); ++i) {
    if (actor->resume(1).result != scheduler::resume_result::resume) break;
}
int r = f.failed() ? -1 : std::move(f).take_ready();

// Inside an actor coroutine
auto [needs_sched, f2] = send(other.get(), &Other::process, x);
if (needs_sched) scheduler->enqueue(other.get());
int v = co_await std::move(f2);

// Fire-and-forget
auto [_, fut] = send(target.get(), &Target::method, arg1, arg2);
fut.detach();
```

In tests, `actor_zeta::test::scheduler_test_t` (`test/tooltestsuites/`) pumps for
you: `enqueue()` the actor, then `stop()` drains until a full sweep makes no progress.

### Coroutine Parameters
```cpp
unique_future<void> process(std::unique_ptr<Data> data);    // OK: by value
unique_future<void> process(std::unique_ptr<Data>&& data);  // compile error, see docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md
```

## Common Mistakes

| Mistake | Correct |
|---------|---------|
| `new MyActor(...)` | `spawn<MyActor>(resource, ...)` |
| `std::shared_ptr<Actor>` | the `unique_ptr` from `spawn()`; pass `address_t` around |
| `throw` in library code | `assert()` for impossible states, error codes for contracts |
| `typeid` / `dynamic_cast` | `rtt.hpp` |
| `T&&` for a move-only type in a coroutine | `T` by value |
| `const T&` in a coroutine | `T` by value (dangles after `co_await`) |
| `co_await send(...)` | `static_assert`; split: pair, `enqueue`, `co_await std::move(f)` |
| `.get()` / `.wait()` / `.available()` | do not exist; `co_await`, pump, or poll, then `take_ready()` |
| `take_ready()` after `is_ready()` alone | check `failed()` first; a valueless extraction aborts in every build |
| ignoring `resume()`'s verdict | `[[nodiscard]]`; requeue on `resume`, drop on `awaiting`, no `(void)` cast |

## Recent Changes

`CHANGELOG.md` has the detail and the migration guides.

- `co_await send(...)` is a compile error; extracting from a valueless future aborts in every build.
- A contended `resume()` returns `awaiting`; `done` means finished. `resume()` is `[[nodiscard]]` and a suspended behavior is no longer parked, so the verdict is the only signal.
- A user exception reaches the caller's future (`EXCEPTIONS_DISABLE=OFF`) instead of killing the process.
- Gone: `generator<T>` and streaming (batch with `unique_future<std::vector<T>>`), `.get()`/`.wait()`/`.available()`/`.cancel()`, the `future_state<T>` family, `actor_mixin`'s default `enqueue_impl`.
- Added: `message::set_command()` for non-blocking routers (`examples/delegation/`), `promise<T>::exception()`.
- Earlier (2025-01): `send()`/`make_message()` lost the sender address, `behavior()` became a coroutine, `enqueue_impl()` returns `pair<bool, enqueue_result>`, messages allocate in the receiver's resource, one atomic word for actor state.

## Debugging

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"  # ASan
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=thread"                          # TSan
cmake --build build && cd build && ctest --output-on-failure
```

A typical actor-lifetime ASan report reads `heap-use-after-free` in
`std::atomic<actor_state>::load()` under `cooperative_actor::resume()` /
`resume_impl<MyActor>()`, freed by `~cooperative_actor()`. Fix: `scheduler->stop()`
before the actor is destroyed.

## Additional Resources

- [CHANGELOG.md](CHANGELOG.md), [PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md)
- [docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md](docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md)
- `examples/`, `test/`
