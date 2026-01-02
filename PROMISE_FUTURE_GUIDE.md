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
};
```

### Caller

```cpp
// Method 1: co_await in coroutine (recommended)
unique_future<int> caller() {
    auto future = send(worker, address(), &Worker::compute, 42);
    int result = co_await std::move(future);
    co_return result;
}

// Method 2: Polling
auto future = send(worker, address(), &Worker::compute, 42);
while (!future.available()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
int result = std::move(future).get();

// Method 3: Fire-and-forget
send(logger, address(), &Logger::log, "message");  // Ignore future
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
    futures.push_back(send(worker.get(), address(), &Worker::compute, data));
}

for (auto& future : futures) {
    int result = std::move(future).get();
}
```

### Timeout

```cpp
auto future = send(worker, address(), &Worker::slow_task, data);
auto start = std::chrono::steady_clock::now();

while (!future.available()) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
        future.cancel();
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
```

### Chaining

```cpp
unique_future<int> chain(int x) {
    auto f = send(other, address(), &Other::process, x);
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