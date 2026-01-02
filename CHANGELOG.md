# Changelog

All notable changes to actor-zeta. Format based on [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Added
- Compile-time check for `T&&` to move-only types in coroutines (GCC 11.4 bug workaround)
- Documentation: `docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md`

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

### Message Creation API (2025-01)

**For library users:** No changes - `send()` API unchanged.

**For custom actors/balancers:**

```cpp
// Before
template<typename R>
unique_future<R> enqueue_impl(mailbox::message_ptr msg);

// After
template<typename R, typename... Args>
unique_future<R> enqueue_impl(address_t sender, message_id cmd, Args&&... args);
```

### Manual Scheduling (2025-01)

```cpp
// Before (auto-scheduling)
send(worker, sender, &Worker::process, data);

// After (manual scheduling)
auto future = send(worker, sender, &Worker::process, data);
scheduler->schedule(worker.get());
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