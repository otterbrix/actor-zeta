# Changelog

All notable changes to actor-zeta. Format based on [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Removed
- **`generator<T>` and the whole streaming subsystem**: `detail/generator.hpp`,
  `actor_zeta::generator`, `actor_zeta::stream_error`, `detail::generator_state`,
  `type_traits::is_generator{,_v}` / `generator_type` / `unwrap_generator{,_t}`,
  `message::init_generator_slot` / `get_generator_state`,
  `detail::make_generator_message`, `dispatch()`'s generator branch, and the
  `await_transform(generator<U>&)` overload on `future_awaiter_mixin`.
  Actor methods must now return `unique_future<T>`; `send()` always yields
  `std::pair<bool, unique_future<T>>`.

  **Why.** The feature had no correct end-to-end consumption route. `generator<T>`
  offers no synchronous `next()`, so the only way to advance a stream was
  `co_await` — and the advertised way to do that (`while (co_await gen)` inside an
  actor method) went through the one `await_transform` overload that never
  published an awaited chain. That left the consuming behavior alive-but-not-busy,
  which the scheduler could not distinguish from idle: it either parked the actor
  on a pending await (lost wakeup) or destroyed the live coroutine frame on the
  next inbound message (use-after-free, plus leaked `dispatch`/method frames and
  their `shared_state`s). The producer also resumed the consumer's coroutine on
  its own thread, which this codebase forbids by design. Four further defects
  shipped alongside: `yield_awaiter` claimed ownership of one coroutine frame from
  two `generator_state`s (double `destroy()`); `take_producer_handle()` returned a
  handle without clearing it; `co_yield stream_error{...}` ended the consumer loop
  *before* its body ran, making in-loop `has_error()` checks dead code. No test in
  the tree ever asserted a yielded value.

  **Constraints for a future streaming design.** Pull must go through the mailbox,
  not a direct cross-thread `coroutine_handle::resume()`. Readiness must be
  published in a form `behavior_t::is_awaited_ready()` understands — a
  `state_flags`-shaped bitmask plus an atomic continuation — so the consumer is
  driven by its own scheduler. And the tests must assert streamed values, not just
  that a handle was constructed.
- **Blocking `unique_future` API**: `.get()`, `.wait()`, `.available()`, `.cancel()`,
  `.is_cancelled()` are gone, along with the yield/exponential-backoff spin that
  used to live inside `get()`. Bring the future to ready via `co_await` /
  an external producer; extract the value with the
  non-blocking `take_ready()`. Cancellation is observed via `failed()` / `error()`
  (the producer sets `std::errc::operation_canceled` through `promise<T>::error`).
- **Default `enqueue_impl` on `actor_mixin`**: each Derived must now define its
  own (enforced by the `has_enqueue_impl` concept).
- **Legacy `future_state<T>` family**: `detail/future_state.hpp`,
  `impl/detail/future_state.ipp`, `future_state_base`, `future_state_enum`,
  `future_states::`, and the intrusive_ptr overloads for the base.
- `shared_state<T>::is_future_released()` and
  `shared_state<T>::deallocate_from_finalizer()` (both specializations; zero callers).
- Backward-compat shim `mailbox/message_result.hpp` (and its `#include` in
  `mailbox.hpp`).
- **`co_await` on a `unique_future<T>` from a NON-actor coroutine**: the nested
  `unique_future<T>::awaiter`, the member `operator co_await()` and the free
  `operator co_await(unique_future<T>&&)` are gone. Inside an actor coroutine
  nothing changes — `co_await std::move(fut)` still works, served by
  `future_awaiter_mixin::await_transform`. What is gone is awaiting a future from
  a coroutine that is not an actor method, because that resumed an actor's frame
  on a foreign thread with no `running`-bit serialization. Drive such a future
  from outside instead: a bounded poll on `is_ready()` + `failed()` +
  `take_ready()`. See `examples/external-drive/`.
- **`behavior_t::resume()`** — dead code, zero callers. An actor's behavior is
  resumed by `cooperative_actor::resume_impl`, never directly.
- **`shared_state<T>::take_continuation()`** (both specializations) — dead code.
  The continuation is claimed with an explicit
  `continuation_.exchange(nullptr, acq_rel)` at each of the three sites that
  need it, which is what made the removed helper redundant.

### Added
- **`take_ready()`** on `unique_future<T>`: non-blocking value extraction; asserts
  the future is ready.
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
- **An actor suspended on `co_await` is no longer parked**, and `resume()` /
  `job_ptr::resume()` are now `[[nodiscard]]`. Before, such an actor blocked its
  own inbox, so the next `send()` reported `needs_sched` and the sender
  re-scheduled it. It no longer does, which means the ONLY signal is the verdict
  `resume_result::resume` returned by `resume()` — the caller must put the actor
  back in a run queue. That contract was always true (`work_sharing` and every
  real driver honour it); the park merely masked violations. Manual drivers that
  discard the verdict now fail to compile. Do NOT reach for a `(void)` cast to
  silence it — `(void)` is banned project-wide. Consume the verdict for real:
  hand it to a scheduler, loop on it, or assert it. See the migration guide
  below.
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
  `future_awaiter_mixin`, consumed by both promise types (`unique_future` and
  `behavior_t`) instead of being duplicated in each.
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

### `resume()` is `[[nodiscard]]` (Unreleased)

Hand-written drivers that call `resume()` and ignore the result stop compiling.
On a project built with `-Wall -Werror` this is a hard error, not a warning.

```cpp
// Before
actor->resume(1);

// After -- drive through the TEST scheduler, which consumes the verdict for you.
// NOTE: `sched` here is actor_zeta::test::scheduler_test_t. Its stop() drains
// until a full sweep makes no progress and may be called repeatedly. The
// production scheduler::sharing_scheduler is NOT interchangeable here: its
// stop() tears the worker pool down -- every later enqueue() is a no-op, and a
// second stop() joins non-joinable threads. With a real scheduler you call
// start() once, enqueue as work arrives, and stop() exactly once at shutdown.
actor_zeta::test::scheduler_test_t sched(1, 100);
if (needs_sched) {
    sched.enqueue(actor.get());
}
sched.stop();

// After -- staying with a hand driver: the verdict is an obligation, so act on it
while (actor->resume(1).result == actor_zeta::scheduler::resume_result::resume) {
    // `resume` means "put me back in a run queue"; a hand driver pays that by
    // going round again. Bound the loop: an actor suspended on a co_await whose
    // producer is never driven reports `resume` forever.
}
```

Prefer `scheduler_test_t` for deterministic tests: `run_once()` already consumes
the verdict and re-queues the job. Use `stop()` rather than `run()` to drain —
`run()`'s only exit is an empty queue, and an actor parked on a pending
cross-actor await keeps the queue non-empty indefinitely.

Note `run()` and `stop()` differ in what they wait for, so a step driven by
`stop()` drains to quiescence where a single `resume(1)` advanced one message.
Assertions of the form "not ready yet" between steps may need rethinking; value
assertions are unaffected.

### `generator<T>` removal (Unreleased)

```cpp
// Before — streaming actor method
generator<std::string> stream_rows(session_id_t s, collection_full_name_t name) {
    for (auto& row : rows_of(name)) {
        co_yield row;
    }
}

auto [needs_sched, gen] = send(storage, &storage_t::stream_rows, s, name);
while (co_await gen) {
    consume(gen.current());
}

// After — batch request/response
unique_future<std::vector<std::string>> fetch_rows(session_id_t s,
                                                   collection_full_name_t name) {
    co_return rows_of(name);
}

auto [needs_sched, future] = send(storage, &storage_t::fetch_rows, s, name);
for (auto& row : co_await std::move(future)) {
    consume(row);
}
```

If a batch does not fit in memory, page it explicitly: send one request per page
and carry a cursor/offset in the message. There is no lazy pull in the framework
any more, by design — see the constraints listed under **Removed** above.

### Blocking `unique_future` API removal (Unreleased)

```cpp
// Before
while (!f.available()) std::this_thread::yield();
int r = std::move(f).get();

// After — poll from outside a coroutine. is_ready() is the promise_released bit,
// which a promise dying without a value sets too, so gate on failed() as well:
while (!f.is_ready()) { std::this_thread::yield(); }
int r = f.failed() ? fallback(f.error()) : std::move(f).take_ready();

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

- [PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md) - Promise/Future patterns
- [CLAUDE.md](CLAUDE.md) - Development guide