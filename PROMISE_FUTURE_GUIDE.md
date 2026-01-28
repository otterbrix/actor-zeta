# Promise/Future Guide

Async request-response pattern using `unique_future<T>`.

## Overview

**Features:** C++20 coroutines (`co_await`, `co_return`), fire-and-forget, request-response, cancellation, PMR allocation.

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

// Method 2: Polling
auto [needs_sched, future] = send(worker.get(), &Worker::compute, 42);
if (needs_sched) scheduler->enqueue(worker.get());
while (!future.available()) {
    std::this_thread::yield();
}
int result = std::move(future).get();

// Method 3: Fire-and-forget
auto [_, fut] = send(logger.get(), &Logger::log, "message");
fut.detach();  // Ignore result
```

## API Reference

| Method | Description |
|--------|-------------|
| `available()` | Check if result is ready |
| `get() &&` | Extract result (requires `available()`) |
| `failed()` | Check for error |
| `cancel()` | Cancel (best-effort) |
| `co_await` | Wait in coroutine |

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
    while (!future.available()) std::this_thread::yield();
    int result = std::move(future).get();
}
```

### Timeout

```cpp
auto [needs_sched, future] = send(worker.get(), &Worker::slow_task, data);
if (needs_sched) scheduler->enqueue(worker.get());

auto start = std::chrono::steady_clock::now();
while (!future.available()) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
        future.cancel();
        break;
    }
    std::this_thread::yield();
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
| Use `co_await` in coroutines | Busy-wait with `get()` |
| Check `available()` before `get()` | Call `get()` blindly |
| `reserve()` vector before adding futures | Let vector reallocate |
| Wait for futures before destroying actor | Destroy actor with pending futures |
| Use fire-and-forget for logging | Store futures indefinitely |

## Ownership Rules

| State | Owner |
|-------|-------|
| `pending` | Mailbox owns message |
| `ready/error/cancelled` | Future owns message |
| Future destroyed early | Mailbox deletes after processing |

## Debugging

| Issue | Solution |
|-------|----------|
| Hang in `get()` | Check handler uses `co_return`, actor is scheduled |
| Assertion on actor destroy | Wait for all futures first |
| Memory leak | Don't store futures indefinitely |

## Limitations

- No timeout in `get()` - use polling with `available()`
- No exceptions - use `failed()` and error codes
- Single consumer - future is move-only
- Actor must outlive futures

## See Also

- [GENERATOR_GUIDE.md](GENERATOR_GUIDE.md) - Streaming with `generator<T>`
- `examples/coroutine/` - Working examples
- `test/coroutines/` - Test cases