# Promise/Future Guide

Request-response over `unique_future<T>`. `header/actor-zeta/detail/future.hpp` is the
reference; this page shows the shapes that work and names the traps.

## Handler

Every actor method returns `unique_future<T>` and is a coroutine.

```cpp
class Worker : public basic_actor<Worker> {
public:
    unique_future<int> compute(int x) { co_return x * 2; }

    using dispatch_traits = actor_zeta::dispatch_traits<&Worker::compute>;
    explicit Worker(std::pmr::memory_resource* res) : basic_actor<Worker>(res) {}

    behavior_t behavior(mailbox::message* msg) {
        if (msg->command() == msg_id<Worker, &Worker::compute>) {
            co_await dispatch(this, &Worker::compute, msg);
        }
    }
};
```

## Caller

`send()` returns `std::pair<bool, unique_future<T>>`. The bool is the obligation to
schedule the target; nothing in the library discharges it for you.

```cpp
// 1. Inside an actor coroutine: an actor holds an address, not a scheduler, so it
//    records the obligation and its owner enqueues the worker (CLAUDE.md, "Who May Schedule")
unique_future<int> caller() {
    auto [needs_sched, future] = send(worker_address_, &Worker::compute, 42);
    if (needs_sched) { worker_owed_.fetch_add(1, std::memory_order_release); }
    co_return co_await std::move(future);
}

// 2. Outside any coroutine: poll. A scheduler worker produces; yield between checks.
auto [needs_sched, future] = send(worker.get(), &Worker::compute, 42);
if (needs_sched) scheduler->enqueue(worker.get());
while (!future.is_ready()) { std::this_thread::yield(); }
if (future.failed()) { handle(future.error()); }
else { int result = std::move(future).take_ready(); }

// 3. Fire-and-forget: drop the future, never the obligation
auto [needs_sched, fut] = send(logger.get(), &Logger::log, std::string("message"));
if (needs_sched) scheduler->enqueue(logger.get());
fut.detach();
```

`co_await send(...)` is a compile error. `co_await` on a `unique_future` works only
inside an actor coroutine; a plain coroutine has no `operator co_await` and must poll
(`examples/external-drive/`). An actor never awaits its own `send()`: the reply waits in
its own mailbox, behind the await. A debug build stops the process, a release build spins;
call the method directly instead, `co_await this->compute(42)`.

## API

| Method | Meaning |
|--------|---------|
| `co_await std::move(f)` | Wait inside an actor coroutine; refuses (aborts) if the future settled without a value |
| `is_ready()` | The `promise_released` bit. Not a value gate: a promise that dies without a value sets it too |
| `failed()` | Settled with an error code |
| `error()` | The code, or a default-constructed `std::error_code` |
| `take_ready() &&` | Extract the value. Aborts in every build if there is none -- not settled yet, or settled with an error; with exceptions enabled, rethrows a captured exception first |
| `detach()` | Release without consuming |
| `valid()` | Not moved-from |

The trap: `is_ready()` alone lets a poll loop fall through to `take_ready()` on a
future with no value. That state is ordinary: a `send()` to a closing mailbox
(`operation_canceled`), a promise dropped unsettled (`broken_pipe`), a promise released
with no outcome (`state_not_recoverable`), a method that threw (`interrupted`). Check
`failed()` first, and bound every spin so a producer that never runs fails visibly.

## Patterns

### Many futures

```cpp
std::vector<unique_future<int>> futures;
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

### Deadline

No built-in timeout or `cancel()`. Poll against a clock; a producer that wants to
cancel calls `promise<T>::error(std::make_error_code(std::errc::operation_canceled))`.

```cpp
auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
while (!future.is_ready() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
}
if (!future.is_ready())      { future.detach(); }   // give up; the producer settles the state on its own
else if (!future.failed())   { int result = std::move(future).take_ready(); }
```

### Chaining

```cpp
unique_future<int> chain(int x) {
    auto [needs_sched, f] = send(other_address_, &Other::process, x);
    if (needs_sched) { other_owed_.fetch_add(1, std::memory_order_release); }   // the owner enqueues
    co_return co_await std::move(f) + 10;
}
```

### Exceptions

Only with `EXCEPTIONS_DISABLE=OFF`. A throw inside an actor method reaches the
caller's future: `take_ready()` and `co_await` rethrow it, and a poller sees
`failed()` with `std::errc::interrupted`. A throw from `behavior()` itself is
reported on stderr and discarded. With `-fno-exceptions` (the default) failure is
`failed()` + `error()` and nothing else. CLAUDE.md tables every route.

## Do / Don't

| Do | Don't |
|----|-------|
| `co_await` inside actor coroutines | Call `.get()` / `.wait()` / `.available()`: they no longer exist |
| Gate `take_ready()` on `is_ready() && !failed()` | Extract after `is_ready()` alone |
| Bound every poll loop | Spin forever on a producer nobody scheduled |
| Discharge `needs_sched` before awaiting; inside an actor, record it for the owner | `co_await send(...)`; hold a scheduler in an actor |
| Call your own method directly: `co_await this->m(...)` | Await your own `send()`: the reply never comes |
| `detach()` futures you will not read | Keep futures you never consume |
| Stop the scheduler before destroying actors | Destroy an actor the scheduler still holds |

## Ownership

`send()` allocates one `shared_state<T>` from the receiver's memory resource; the
message carries a pointer to it (`result_slot_`) and the caller holds the
`unique_future`. `dispatch()` settles it with the method's value, with the error code the
method `co_return`ed, or with the exception the method threw. A message destroyed before dispatch (closed mailbox, actor torn
down with a queued message) settles it with `operation_canceled`; a promise destroyed
unsettled, or a method torn down mid-body with its actor, produces `broken_pipe`. The state is freed by whichever side releases last, so a future may
outlive its actor and reads as `failed()`. The memory resource must outlive both.

## Debugging

| Symptom | Cause |
|---------|-------|
| `take_ready()` aborts with "holds no value" | The future has not settled, or settled with an error: check `is_ready()`, then `failed()` |
| Poll loop hits its bound | Nothing drove the producer: `needs_sched` not discharged, wrong actor pumped, or the handler never returned |
| "protocol violation: an actor awaits its own send()" | A method awaited a `send()` to its own actor; call the method directly |
| "double co_await on unique_future" assert | Single consumer: a future is awaited once |
| Use-after-free in `resume()` | The actor was destroyed while a scheduler still held it; see CLAUDE.md "Actor Shutdown" |
| Other "protocol violation" messages | A turn taken out of order; `docs/LIFECYCLE.md` lists them |

## See Also

- `examples/coroutine/`, `examples/external-drive/`
- `test/coroutines/`, `test/cancelled-extraction/`, `test/exception-propagation/`
