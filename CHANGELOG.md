# Changelog

All notable changes to actor-zeta. Format based on [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Changed
- **`send()` API**: Removed sender address parameter. Now: `send(actor, &Method, args...)` returns `std::pair<bool, unique_future<T>>`
- **`make_message()` API**: Removed sender address parameter
- **`enqueue_impl()` return type**: Changed to `std::pair<bool, enqueue_result>` (bool first)
- **`behavior()` signature**: Returns `behavior_t` (coroutine), use `co_await dispatch(...)` inside

### Added
- Compile-time check for `T&&` to move-only types in coroutines (GCC 11.4 bug workaround)
- Documentation: `docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md`
- Cross-thread stress tests for `unique_future` (`test/race-condition/`)

### Fixed
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