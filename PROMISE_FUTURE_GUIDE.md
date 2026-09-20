# Promise/Future Guide

Async request-response pattern using `unique_future<T>`.

## Overview

**Features:** C++20 coroutines (`co_await`, `co_return`), fire-and-forget, request-response, PMR allocation.

## Basic Usage

### Handler (coroutine)

```cpp
class Worker : public basic_actor<Worker> {
public:
    unique_future<int> compute(int x) {
        co_return x * 2;  // Must use co_return
    }

    unique_future<void> process() {
        co_return;  // co_return for void
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &Worker::compute, &Worker::process>;

    explicit Worker(std::pmr::memory_resource* res)
        : basic_actor<Worker>(res) {}

    behavior_t behavior(mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<Worker, &Worker::compute>) {
            co_await dispatch(this, &Worker::compute, msg);
        } else if (cmd == msg_id<Worker, &Worker::process>) {
            co_await dispatch(this, &Worker::process, msg);
        }
    }
};
```

### Caller

```cpp
// send() returns std::pair<bool, unique_future<T>>
// - first: needs_scheduling (true if actor needs to be scheduled)
// - second: the future

// Method 1: co_await in coroutine (recommended)
unique_future<int> caller() {
    auto [needs_sched, future] = send(worker.get(), &Worker::compute, 42);
    if (needs_sched) scheduler->enqueue(worker.get());
    int result = co_await std::move(future);
    co_return result;
}

// Method 2: drive from outside a coroutine.
//   cross-thread (scheduler worker produces): poll, yielding between checks
//   same-thread (no scheduler): pump the actor yourself with resume()
auto [needs_sched, future] = send(worker.get(), &Worker::compute, 42);
if (needs_sched) scheduler->enqueue(worker.get());

while (!future.is_ready()) { std::this_thread::yield(); }
if (future.failed()) {          // is_ready() alone is not a value gate -- see below
    handle(future.error());
} else {
    int result = std::move(future).take_ready();
}

// Method 3: Fire-and-forget
auto [_, fut] = send(logger.get(), &Logger::log, "message");
fut.detach();  // Ignore result
```

The blocking `get()` / `wait()` / `available()` API was removed (see CHANGELOG):
there is no waiting method on `unique_future` at all. The future is brought to
ready externally — a `co_await` inside a coroutine, a pump of the producing actor,
or an external producer thread — and the value is then extracted with the
non-blocking `take_ready()`.

## API Reference

| Method | Description |
|--------|-------------|
| `co_await std::move(f)` | Wait inside a coroutine (primary API) |
| `is_ready() const` | Non-blocking poll: has `release_promise()` been called? |
| `take_ready() &&` | Extract the value — **asserts** the future is ready (no waiting). Pair with `co_await`, a poll loop, or external completion. |
| `failed() const` | Future completed with an error |
| `error() const` | Returns the error code (default-constructed if none) |
| `detach()` | Fire-and-forget release |
| `valid() const` | Future has a state (not moved-from) |

**`is_ready()` alone is not a readiness gate.** It is the `promise_released` bit,
which a promise that dies without a value sets too — a cancelled producer
(`promise<T>::error(std::make_error_code(std::errc::operation_canceled))`), a
dropped promise (`broken_pipe`), an actor torn down with queued work. In every
one of those cases a poll loop keyed on `is_ready()` alone falls straight through
to `take_ready()` on a future that carries no value, and `take_ready()` only
*asserts* the value is there — an assert that is gone under `NDEBUG`, which is how
a Release build ships. So always check `failed()` before extracting, and bound the
spin so a producer that never completes fails visibly instead of hanging.

## Patterns

### Multiple Futures

```cpp
std::vector<unique_future<int>> futures;
futures.reserve(workers.size());  // Required!

for (auto& worker : workers) {
    auto [needs_sched, future] = send(worker.get(), &Worker::compute, data);
    if (needs_sched) scheduler->enqueue(worker.get());
    futures.push_back(std::move(future));
}

for (auto& future : futures) {
    while (!future.is_ready()) { std::this_thread::yield(); }
    if (future.failed()) { continue; }
    int result = std::move(future).take_ready();
}
```

### Timeout (poll with deadline)

`unique_future` has no built-in timeout or `cancel()`. Either build your own
deadline-poll loop using `is_ready()`, or have the producer set
`std::errc::operation_canceled` (via `promise::error`) from another path:

```cpp
auto [needs_sched, future] = send(worker.get(), &Worker::slow_task, data);
if (needs_sched) scheduler->enqueue(worker.get());

auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
while (!future.is_ready()) {
    if (std::chrono::steady_clock::now() > deadline) {
        future.detach();           // give up on this future
        break;
    }
    std::this_thread::yield();
}
// Gate on failed() as well: is_ready() is only the promise_released bit.
if (future.is_ready() && !future.failed()) {
    int result = std::move(future).take_ready();
}
```

### Chaining

```cpp
unique_future<int> chain(int x) {
    auto [needs_sched, f] = send(other.get(), &Other::process, x);
    if (needs_sched) scheduler->enqueue(other.get());
    int r = co_await std::move(f);
    co_return r + 10;
}
```

## Best Practices

| Do | Don't |
|----|-------|
| `co_await` inside coroutines (primary API) | Try `.get()` / `.wait()` / `.available()` — they no longer exist |
| Gate every `take_ready()` on `is_ready() && !failed()` | Call `take_ready()` after `is_ready()` without checking `failed()` |
| Pair `take_ready()` with a guarantee of readiness | Call `take_ready()` on a future you haven't driven |
| `reserve()` the vector before pushing futures | Let the vector reallocate (move-only futures) |
| Stop the scheduler before destroying actors | Destroy actors with pending futures |
| Fire-and-forget via `detach()` for logging | Store futures you'll never consume |

## Ownership Rules

| State | Owner |
|-------|-------|
| `pending` | Mailbox owns message |
| `ready/error` | Future owns message |
| Future destroyed early | Mailbox deletes after processing |

## Debugging

| Issue | Solution |
|-------|----------|
| `take_ready()` aborts in debug | Future isn't ready: forgot `co_await`, pumped the wrong actor, or the handler never `co_return`ed |
| A poll loop hits its bound | The producer never makes progress — wrong actor, wrong scheduler, or the future was cancelled |
| Use-after-free on actor destruction | Stop the scheduler / wait all futures BEFORE the actor is destroyed |

## Limitations

- No built-in timeout — see the deadline-poll pattern above.
- No exceptions — observe failure via `failed()` + `error()`.
- Single consumer — `unique_future` is move-only.
- Actor must outlive any future it produced (see CLAUDE.md "Actor Shutdown").

## See Also

- `examples/coroutine/` - Working examples
- `test/coroutines/` - Test cases