# Changelog

All notable changes to actor-zeta. Format based on [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Removed
- **Blocking `unique_future` API**: `.get()`, `.wait()`, `.available()`, `.cancel()`,
  `.is_cancelled()` are gone, along with the yield/exponential-backoff spin that
  used to live inside `get()`. Bring the future to ready via `co_await` /
  `run_until_complete` / an external producer; extract the value with the
  non-blocking `take_ready()`. Cancellation is observed via `failed()` / `error()`
  (the producer sets `std::errc::operation_canceled` through `promise<T>::error`).
- **Default `enqueue_impl` on `actor_mixin`**: each Derived must now define its
  own (enforced by the `has_enqueue_impl` concept).
- **Legacy `future_state<T>` family**: `detail/future_state.hpp`,
  `impl/detail/future_state.ipp`, `future_state_base`, `future_state_enum`,
  `future_states::`, and the intrusive_ptr overloads for the base. `generator_state`
  no longer inherits from anything — it owns its own refcount/state-byte/coroutine-
  handles directly.
- `shared_state<T>::is_future_released()` and
  `shared_state<T>::deallocate_from_finalizer()` (both specializations; zero callers).
- Backward-compat shim `mailbox/message_result.hpp` (and its `#include` in
  `mailbox.hpp`).

### Added
- **`take_ready()`** on `unique_future<T>`: non-blocking value extraction; asserts
  the future is ready.
- **`run_until_complete(f, pump)`** in `<actor-zeta/detail/run_loop.hpp>`: the
  canonical top-level driver. `pump` is invoked repeatedly until `is_ready()`,
  then the value is taken via `take_ready()`. Typical pumps:
  `[&]{ actor->resume(n); }` (same-thread), `[]{ std::this_thread::yield(); }`
  (cross-thread, scheduler worker is producer).
- **`message::set_command(message_id)`**: enables non-blocking router/delegation
  patterns — the router restamps the command and forwards the same message_ptr
  to a worker's `enqueue_impl`; the caller's future is filled by the worker via
  the message's type-erased `result_slot`.
- New example `examples/delegation/`: round-robin router on `actor_mixin`
  delegating to a pool of `cooperative_actor` workers via `set_command`.
- New header `detail/result_storage.hpp`: extracted `result_storage<T>` /
  `result_storage<void>` (still used by `shared_state`).
- New test
  `test/future-state-fixes/main.cpp::"Concurrent: is_ready acquire synchronizes
  side effect on shared_state"` — ported from the removed `test/slot-refcount/`
  to preserve release-acquire coverage on the surviving `shared_state` type.

### Changed
- **`send()` API**: Removed sender address parameter. Now: `send(actor, &Method, args...)` returns `std::pair<bool, unique_future<T>>`
- **`make_message()` API**: Removed sender address parameter
- **`enqueue_impl()` return type**: Changed to `std::pair<bool, enqueue_result>` (bool first)
- **`behavior()` signature**: Returns `behavior_t` (coroutine), use `co_await dispatch(...)` inside

### Added (earlier)
- Compile-time check for `T&&` to move-only types in coroutines (GCC 11.4 bug workaround)
- Documentation: `docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md`
- Cross-thread stress tests for `unique_future` (`test/race-condition/`)

### Fixed
- Cross-thread race condition in `unique_future` (PR #182): the lock-free CAS
  handshake between producer and consumer continuation now lives in a single
  `future_awaiter_mixin` consumed by all three promise types (unique_future,
  behavior_t, task), eliminating the previously duplicated copies.
- Clang-14 compatibility: Structured bindings cannot be captured in lambdas

## [2025-01] - Major Refactoring

### Added
- `resume_info` struct with execution statistics from `resume()`
- `[[nodiscard]]` attributes on future methods
- Generator `generator<T>` coroutine type for streaming data

### Changed
- **Actor State**: Single `atomic<actor_state>` replaces three separate atomics
- **Message Creation**: Messages created in receiver's memory resource (not sender's)
- **Scheduling**: Manual scheduling - callers must explicitly schedule actors
- **RTT**: Same-arena only migration (cross-arena removed)
- **PMR**: Migrated from custom `actor_zeta::pmr` to `std::pmr`
- **Future API**: `is_ready()` renamed to `available()`

### Fixed
- Shutdown race condition in `resume_core_()` - check `blocked()` before `empty()`
- Memory leak in `release_message_ref()` - check state before deleting
- Wrong PMR resource in `promise::set_value()` - use message's resource
- `current_msg_guard` destructor - restore previous pointer
- Exponential backoff in `get()` - 99% CPU reduction

### Removed
- Cross-arena RTT migration (unsafe with non-trivial types)
- Auto-scheduling when messages enqueued
- `dataflow` example

## [2025-12] - C++20 Coroutines

### Added
- `generator<T>` coroutine type with `co_yield` support
- `unique_future<T>` with `co_await` and `co_return`
- Coroutine examples: `coroutine/`, `generator/`
- Type traits: `is_generator<T>`, `is_unique_future<T>`

### Changed
- All actor methods returning `unique_future<T>` must be coroutines
- Examples updated to use `std::pmr` and coroutine patterns

---

## Migration Guides

### Blocking `unique_future` API removal (Unreleased)

```cpp
// Before
while (!f.available()) std::this_thread::yield();
int r = std::move(f).get();

// After — top-level driver:
int r = actor_zeta::run_until_complete(f, []{ std::this_thread::yield(); });

// After — inside a coroutine:
int r = co_await std::move(f);

// After — when readiness is already guaranteed (e.g. promise.set_value() before get_future()):
int r = std::move(f).take_ready();
```

`cancel()` / `is_cancelled()` are gone too; cancellation is now a value the producer
sets via `promise<T>::error(std::make_error_code(std::errc::operation_canceled))`
and the consumer observes via `f.failed()` / `f.error()`.

### `actor_mixin` no longer provides a default `enqueue_impl` (Unreleased)

```cpp
// Before: sync actors inherited a default that just called behavior() and discarded
// the returned behavior_t.

// After: each Derived must define its own. For a "sync" actor that wants the old
// behavior, paste:
[[nodiscard]] std::pair<bool, actor_zeta::detail::enqueue_result>
enqueue_impl(actor_zeta::mailbox::message_ptr msg) {
    behavior(msg.get());
    return {false, actor_zeta::detail::enqueue_result::success};
}
```

`cooperative_actor` / `basic_actor` are unaffected — they always had their own
`enqueue_impl`. The new contract enables non-blocking router/delegation patterns:
a router defines an `enqueue_impl` that restamps `msg->set_command(...)` and
forwards to a worker's `enqueue_impl` without ever calling `behavior()` —
see `examples/delegation/`.

### behavior() Signature Change (Unreleased)

```cpp
// Before (returns void)
void behavior(mailbox::message* msg) {
    if (msg->command() == msg_id<Actor, &Actor::method>) {
        dispatch(this, &Actor::method, msg);
    }
}

// After (returns behavior_t, coroutine with co_await)
behavior_t behavior(mailbox::message* msg) {
    if (msg->command() == msg_id<Actor, &Actor::method>) {
        co_await dispatch(this, &Actor::method, msg);
    }
}
```

### enqueue_impl() Return Type (Unreleased)

```cpp
// Before
enqueue_result enqueue_impl(mailbox::message_ptr msg);

// After (pair with bool first)
std::pair<bool, enqueue_result> enqueue_impl(mailbox::message_ptr msg);
```

### send() API Change (2025-01)

```cpp
// Before (with sender address)
auto future = send(worker, sender, &Worker::process, data);
scheduler->schedule(worker.get());

// After (no sender address, returns pair)
auto [needs_sched, future] = send(worker.get(), &Worker::process, data);
if (needs_sched) scheduler->enqueue(worker.get());
```

### Structured Bindings in Lambdas (Clang-14)

```cpp
// Clang-14 doesn't support capturing structured bindings in lambdas

// WRONG (compile error on clang-14):
auto [needs_sched, future] = send(actor.get(), &Actor::compute, 42);
std::thread t([&future]() { ... });  // Error!

// CORRECT:
auto send_result = send(actor.get(), &Actor::compute, 42);
auto& future = send_result.second;
std::thread t([&future]() { ... });  // OK
```

### PMR Migration (2025-12)

```cpp
// Before
actor_zeta::pmr::memory_resource* resource;

// After
std::pmr::memory_resource* resource;
```

---

## See Also

- [GENERATOR_GUIDE.md](GENERATOR_GUIDE.md) - Generator patterns
- [PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md) - Promise/Future patterns
- [CLAUDE.md](CLAUDE.md) - Development guide