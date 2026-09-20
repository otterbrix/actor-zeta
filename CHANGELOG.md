# Changelog

All notable changes to actor-zeta. Format based on [Keep a Changelog](https://keepachangelog.com/).

## [Unreleased]

### Removed
- **`generator<T>` and the streaming subsystem**: `detail/generator.hpp`,
  `actor_zeta::generator`, `actor_zeta::stream_error`, `detail::generator_state`,
  `type_traits::is_generator{,_v}` / `generator_type` / `unwrap_generator{,_t}`,
  `message::init_generator_slot` / `get_generator_state`,
  `detail::make_generator_message`, the generator branch in `dispatch()`, and
  `await_transform(generator<U>&)`. Actor methods return `unique_future<T>`;
  `send()` always returns `std::pair<bool, unique_future<T>>`.
  Why: no correct way to consume a stream existed. `generator<T>` had no synchronous
  `next()`, and `while (co_await gen)` went through the one `await_transform` that
  never published an awaited chain, so the scheduler saw the consumer as idle: a lost
  wakeup, or a use-after-free of the live frame on the next message. The producer
  also resumed the consumer's frame on its own thread. Four further defects shipped
  with it, and no test asserted a yielded value. Any future design must pull through
  the mailbox, publish readiness in the `state_flags` plus continuation form
  `behavior_t::is_awaited_ready()` reads, and assert streamed values.
- **Blocking `unique_future` API**: `.get()`, `.wait()`, `.available()`, `.cancel()`,
  `.is_cancelled()`, and the backoff spin inside `get()`. Bring the future to ready
  and extract with `take_ready()`. Cancellation is a value: the producer calls
  `promise<T>::error(operation_canceled)`, the consumer reads `failed()` / `error()`.
- **Default `enqueue_impl` on `actor_mixin`**: each Derived defines its own
  (`has_enqueue_impl` concept, `actor/address.hpp`).
- **Legacy `future_state<T>` family**: `detail/future_state.hpp`,
  `impl/detail/future_state.ipp`, `future_state_base`, `future_state_enum`,
  `future_states::`, and the intrusive_ptr overloads for the base.
- `shared_state<T>::is_future_released()` and `deallocate_from_finalizer()` (no callers).
- Compat shim `mailbox/message_result.hpp` and its include in `mailbox.hpp`.
- **`co_await` on a `unique_future<T>` from a non-actor coroutine**: the nested
  `awaiter`, member `operator co_await()` and free `operator co_await(unique_future<T>&&)`.
  Inside an actor coroutine `co_await std::move(fut)` is unchanged. A foreign
  coroutine resumed an actor's frame off its thread with no `running` serialization;
  poll from outside instead (`is_ready()` + `failed()` + `take_ready()`,
  `examples/external-drive/`).
- **`behavior_t::resume()`**: dead code; `cooperative_actor::resume_impl` resumes behaviors.
- **`shared_state<T>::take_continuation()`**: dead code; the three claim sites do
  `continuation_.exchange(nullptr, acq_rel)` directly.

### Added
- **`unique_future<T>::take_ready()`**: non-blocking extraction; the future must be ready.
- **`message::set_command(message_id)`**: a router restamps the command and forwards
  the same `message_ptr` to a worker's `enqueue_impl`; the worker fills the caller's
  future through the message's type-erased `result_slot_`.
- `examples/delegation/`: round-robin router on `actor_mixin` over `cooperative_actor` workers.
- `detail/result_storage.hpp`: `result_storage<T>` / `result_storage<void>`, used by `shared_state`.
- `test/future-state-fixes/main.cpp::"Concurrent: is_ready acquire synchronizes side
  effect on shared_state"`, ported from the removed `test/slot-refcount/`.
- **`promise<T>::exception(std::exception_ptr)`**, symmetric to `error()`, guarded on
  `__cpp_exceptions`, so a router filling `msg->get_result_promise<T>()` by hand can
  pass a caught exception on instead of flattening it to a code.

### Changed
- **An actor suspended on `co_await` is no longer parked**, and `resume()` /
  `job_ptr::resume()` are `[[nodiscard]]`. The park blocked the inbox, so the next
  `send()` re-scheduled the actor and masked drivers that dropped the verdict. Now
  `resume_result::resume` is the only signal and the caller must requeue; hand
  drivers that discard it no longer compile. See the migration guide.
- **`send()`**: no sender address; returns `std::pair<bool, unique_future<T>>`.
- **`make_message()`**: no sender address.
- **`enqueue_impl()`** returns `std::pair<bool, enqueue_result>` (bool first).
- **`behavior()`** returns `behavior_t`, a coroutine; use `co_await dispatch(...)`.
- **`actor_state` widened from `uint8_t` to `uint32_t`**: three flags in the low bits,
  the in-flight sender count above them. One word gives a sender and the destructor
  one modification order without fences; five spare bits would cap the count at 31.
- **`shared_state<void>` is no longer a separate specialization**: 95 of 104 lines
  duplicated the primary template. Layout unchanged.
- **`try_schedule_after_enqueue` is now `leave_and_maybe_schedule`**: drops the
  sender's registration and claims `scheduled` in one read-modify-write.
- **The CAS bound prints before aborting, in release too** (was a bare `std::terminate()`).
- Comments and CI notes no longer name specific consuming projects.

### Added (earlier)
- Compile-time rejection of `T&&` to a move-only type in coroutine parameters (GCC 11.4 workaround).
- `docs/GCC_COROUTINE_OPERATOR_NEW_BUG.md`.
- Cross-thread stress tests for `unique_future` (`test/race-condition/`).

### Fixed
- **Every public header compiles on its own.** The concept `detail/behavior_t.hpp`
  borrowed from `detail/future.hpp` now lives in `detail/type_traits.hpp`;
  `actor/dispatch_traits.hpp` gained `<cassert>` and a complete `actor::address_t`.
  `test/header-selfsufficiency` builds one translation unit per header.
- **Extracting from a future that holds no value is refused** instead of reading an
  inactive union member: `take_ready()`, `result_storage::get()`, `co_await`. See the
  migration guide.
- **`co_await send(...)` no longer compiles.** See the migration guide.
- **Use-after-free during teardown.** `enqueue_impl` checked `is_destroying` and then
  pushed, while the destructor waited only on `running`, so the mailbox could be
  freed under a `push_back`. Senders now register in the word that carries
  `destroying`, and the destructor waits for the count to drain.
- **`exponential_backoff` shifted by 32.** `1 << (attempt - 10)` reached `INT_MIN` at
  attempt 41 and undefined behaviour from 42, on any teardown wait past about 22ms.
  The exponent is capped; a ubsan job was added to CI.
- **`try_unblock()` had no caller.** `park()` blocks the inbox on the way out;
  `resume_impl` now unblocks it on the way in, so a concurrent `send()` to a running
  actor no longer gets `needs_sched` and a second job node.
- **A contended `resume()` reported `done`.** See the migration guide.
- **A user exception no longer kills the process** and reaches the caller. See the migration guide.
- **`take_ready()` for `void` never marked the result consumed**, so `holds_value()` stayed `true`.

## [2025-01] - Major Refactoring

### Added
- `resume_info` struct with execution statistics from `resume()`
- `[[nodiscard]]` on future methods
- `generator<T>` coroutine type for streaming data

### Changed
- **Actor State**: one `atomic<actor_state>` replaces three atomics
- **Message Creation**: messages allocate in the receiver's memory resource
- **Scheduling**: manual; callers schedule actors explicitly
- **RTT**: same-arena migration only
- **PMR**: `actor_zeta::pmr` replaced by `std::pmr`
- **Future API**: `is_ready()` renamed to `available()`

### Fixed
- Shutdown race in `resume_core_()`: check `blocked()` before `empty()`
- Memory leak in `release_message_ref()`: check state before deleting
- Wrong PMR resource in `promise::set_value()`: use the message's resource
- `current_msg_guard` destructor restores the previous pointer
- Exponential backoff in `get()`: 99% CPU reduction

### Removed
- Cross-arena RTT migration (unsafe with non-trivial types)
- Auto-scheduling on enqueue
- `dataflow` example

## [2025-12] - C++20 Coroutines

### Added
- `generator<T>` coroutine type with `co_yield`
- `unique_future<T>` with `co_await` and `co_return`
- Examples `coroutine/`, `generator/`
- Type traits `is_generator<T>`, `is_unique_future<T>`

### Changed
- Actor methods returning `unique_future<T>` must be coroutines
- Examples use `std::pmr` and coroutine patterns

---

## Migration Guides

### `co_await send(...)` no longer compiles (Unreleased)

`needs_sched` is the obligation to put the target in a run queue; an awaiter could
only hand it back after a wait that cannot end until the target has run. It is a
`static_assert` now. A target that drives itself (its `enqueue_impl` always returns
`false`) needs only `co_await std::move(sent.second)`.

```cpp
// Before -- compiles, suspends, never resumes
auto [needs_sched, result] = co_await send(target, &Target::compute, x);

// After -- take the obligation, discharge it, then await
auto [needs_sched, f] = send(target, &Target::compute, x);
if (needs_sched) { scheduler->enqueue(target); }
auto result = co_await std::move(f);
```

### Extraction refuses on a future that holds no value (Unreleased)

`take_ready()` and `co_await` asserted and then read the value anyway, so under
`NDEBUG` an error-only state (ordinary: a `send()` to a closing mailbox cancels the
promise) move-constructed a `T` from unwritten bytes. Both now print why and abort in
every build. `is_ready()` reports `promise_released`, which a promise dying without a
value also sets; `failed()` is the gate. `co_await` has no error channel, so awaiting
a cancelled future refuses too: poll, or build with exceptions.

```cpp
while (!f.is_ready()) { std::this_thread::yield(); }
if (f.failed()) { return; }   // f.error(): operation_canceled, broken_pipe, interrupted, state_not_recoverable
auto value = std::move(f).take_ready();
```

### A contended `resume()` reports `awaiting`, not `done` (Unreleased)

When `resume()` cannot take `running`, the thread that holds it discharges the
obligation; the verdict is `awaiting`, "drop this node". `done` means finished and
appears only during teardown. A driver that retired an actor on `done` was retiring
one that was merely contended.

```cpp
switch (info.result) {
    case resume_result::resume:   requeue(actor); break;
    case resume_result::awaiting: /* drop the node */ break;
    case resume_result::done:     /* teardown only */ break;
}
```

### Exceptions reach the caller and no longer kill the process (Unreleased)

Only with `EXCEPTIONS_DISABLE=OFF`; `-fno-exceptions` emits no catch wrapper for a
coroutine body. Before, a throw from an actor method hit `behavior_t`'s
`unhandled_exception()` (`std::terminate()`) and the caller got `broken_pipe`.
`dispatch()` now catches and settles the caller's promise with the exception. A throw
from `behavior()` itself is reported on stderr and discarded, since `behavior_t` is
the root of the chain and nobody reads its result: throw from dispatched methods.

```cpp
unique_future<int> compute(int x) {
    if (x < 0) { throw std::runtime_error("negative"); }
    co_return x * 2;
}

auto [needs_sched, f] = send(actor, &Actor::compute, -1);
if (needs_sched) { scheduler->enqueue(actor); }
while (!f.is_ready()) { std::this_thread::yield(); }
if (f.failed()) { /* f.error() == std::errc::interrupted */ }
try {
    auto value = std::move(f).take_ready();     // rethrows the original
} catch (const std::runtime_error& e) { /* e.what() == "negative" */ }
```

### `resume()` is `[[nodiscard]]` (Unreleased)

Hand drivers that ignore the verdict stop compiling under `-Werror`. In tests, prefer
`scheduler_test_t`: `run_once()` consumes the verdict and requeues; `stop()` drains
until a full sweep makes no progress and may be called again (`run()` exits only on an
empty queue, which a behavior suspended on a cross-actor await never yields). A
`stop()`-driven step reaches quiescence where `resume(1)` advanced one message, so
"not ready yet" assertions between steps may need rethinking. `sharing_scheduler` is
not interchangeable: `start()` once, `stop()` exactly once.

```cpp
// Before
actor->resume(1);

// After, in a test
actor_zeta::test::scheduler_test_t sched(1, 100);
if (needs_sched) { sched.enqueue(actor.get()); }
sched.stop();

// After, hand-driven: `resume` means "requeue me". Bound the loop -- an actor
// awaiting a producer nobody drives reports `resume` forever.
while (actor->resume(1).result == actor_zeta::scheduler::resume_result::resume) {}
```

### `generator<T>` removal (Unreleased)

Batch instead; if a batch does not fit in memory, page it with a cursor or offset in
the message. There is no lazy pull in the framework, by design.

```cpp
// Before
generator<std::string> stream_rows(session_id_t s, collection_full_name_t name) {
    for (auto& row : rows_of(name)) { co_yield row; }
}
auto [needs_sched, gen] = send(storage, &storage_t::stream_rows, s, name);
while (co_await gen) { consume(gen.current()); }

// After
unique_future<std::vector<std::string>> fetch_rows(session_id_t s, collection_full_name_t name) {
    co_return rows_of(name);
}
auto [needs_sched, future] = send(storage, &storage_t::fetch_rows, s, name);
if (needs_sched) { scheduler->enqueue(storage); }
for (auto& row : co_await std::move(future)) { consume(row); }
```

### Blocking `unique_future` API removal (Unreleased)

`cancel()` / `is_cancelled()` are gone too: the producer sets
`promise<T>::error(std::make_error_code(std::errc::operation_canceled))`, the consumer
reads `failed()` / `error()`.

```cpp
// Before
while (!f.available()) std::this_thread::yield();
int r = std::move(f).get();

// After -- outside a coroutine
while (!f.is_ready()) { std::this_thread::yield(); }
int r = f.failed() ? fallback(f.error()) : std::move(f).take_ready();

// After -- inside an actor coroutine
int r = co_await std::move(f);
```

### `actor_mixin` no longer provides a default `enqueue_impl` (Unreleased)

`cooperative_actor` / `basic_actor` are unaffected. A router's `enqueue_impl` can
`msg->set_command(...)` and forward to a worker without calling `behavior()`
(`examples/delegation/`). The old default, for a sync actor that wants it back:

```cpp
[[nodiscard]] std::pair<bool, actor_zeta::detail::enqueue_result>
enqueue_impl(actor_zeta::mailbox::message_ptr msg) {
    behavior(msg.get());
    return {false, actor_zeta::detail::enqueue_result::success};
}
```

### `behavior()` returns `behavior_t` (Unreleased)

```cpp
void behavior(mailbox::message* msg) {        // before
    if (msg->command() == msg_id<Actor, &Actor::m>) { dispatch(this, &Actor::m, msg); }
}
behavior_t behavior(mailbox::message* msg) {  // after
    if (msg->command() == msg_id<Actor, &Actor::m>) { co_await dispatch(this, &Actor::m, msg); }
}
```

### `enqueue_impl()` return type (Unreleased)

```cpp
enqueue_result enqueue_impl(mailbox::message_ptr msg);                   // before
std::pair<bool, enqueue_result> enqueue_impl(mailbox::message_ptr msg);  // after
```

### `send()` API change (2025-01)

```cpp
auto future = send(worker, sender, &Worker::process, data);           // before
scheduler->schedule(worker.get());

auto [needs_sched, future] = send(worker.get(), &Worker::process, data);  // after
if (needs_sched) scheduler->enqueue(worker.get());
```

### Structured bindings in lambdas (Clang 14)

```cpp
auto [needs_sched, future] = send(actor.get(), &Actor::compute, 42);
std::thread t([&future]() { ... });   // clang-14: cannot capture a structured binding

auto send_result = send(actor.get(), &Actor::compute, 42);
auto& future = send_result.second;
std::thread t([&future]() { ... });   // OK
```

### PMR migration (2025-12)

```cpp
actor_zeta::pmr::memory_resource* resource;  // before
std::pmr::memory_resource* resource;         // after
```

---

## See Also

- [PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md)
- [CLAUDE.md](CLAUDE.md)
