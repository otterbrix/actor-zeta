# Promise/Future System - Complete Guide

Complete guide to using the async request-response pattern in actor-zeta.

## Table of Contents

- [Overview](#overview)
- [Basic Usage](#basic-usage)
- [Common Patterns](#common-patterns)
- [Best Practices](#best-practices)
- [Message Lifetime & Ownership](#message-lifetime--ownership)
- [Performance](#performance)
- [Debugging](#debugging)
- [Known Limitations](#known-limitations)

---

## Overview

Actor-zeta supports **async request-response** via `promise<T>` and `unique_future<T>` classes. This enables both fire-and-forget and request-response patterns with full C++20 coroutine support.

**Key Features:**
- C++20 coroutines with `co_await` and `co_return`
- Non-blocking message sends with optional result
- Fire-and-forget pattern (ignore future)
- Request-response pattern (wait for result)
- Orphaned futures (future destroyed before actor processes)
- Cancellation support
- Uses `std::pmr::memory_resource` for allocation

---

## Basic Usage

### Fire-and-Forget (No Return Value)

```cpp
// Send message, don't care about result - future auto-destroyed
send(target_actor, sender, &TargetActor::handle_command, arg1, arg2);
```

### Request-Response (Wait for Result)

```cpp
// Send message, get future
auto future = send(target_actor, sender, &TargetActor::compute, arg1);

// Check if ready (non-blocking)
if (future.available()) {
    int result = std::move(future).get();
}

// Or in a coroutine - use co_await (recommended)
int result = co_await std::move(future);
```

### Handler Implementation

All handlers are coroutines using `co_return`:

```cpp
class Worker : public basic_actor<Worker> {
public:
    // Handler returns future via co_return
    unique_future<int> compute(int x) {
        int result = x * 2;
        co_return result;  // Use co_return, not return!
    }

    // For void results
    unique_future<void> process_task(std::string task) {
        // Do work...
        co_return;  // Use co_return for void
    }
};
```

**Note:** All actor methods that return `unique_future<T>` must be coroutines using `co_return`.

---

## Common Patterns

### Pattern 1: Simple Request-Response

**Sender side:**
```cpp
auto future = send(worker, address(), &Worker::compute, 42);

// In non-coroutine code: process messages first, then get result
worker->resume(100);
int result = std::move(future).get();  // Only call when available()!
std::cout << "Result: " << result << "\n";
```

**Worker side:**
```cpp
class Worker : public basic_actor<Worker> {
    unique_future<int> compute(int x) {
        // Do expensive computation
        int result = x * 2;
        co_return result;  // Use co_return for coroutines
    }
};
```

---

### Pattern 2: Multiple Futures (Parallel Requests)

Send multiple requests and wait for all results:

```cpp
// Send requests to multiple workers
std::vector<unique_future<int>> futures;
futures.reserve(workers.size());  // CRITICAL: Reserve to avoid reallocation!

for (auto& worker : workers) {
    futures.push_back(send(worker.get(), address(), &Worker::compute, data));
}

// Wait for all results
std::vector<int> results;
for (auto& future : futures) {
    // Ensure message is processed first
    while (!future.available()) {
        // Process messages or wait
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    results.push_back(std::move(future).get());
}
```

**⚠️ CRITICAL:** Always `reserve()` vector capacity before adding futures to avoid reallocation (futures are move-only).

---

### Pattern 3: Fire-and-Forget (Orphaned Futures)

Don't care about result - drop future immediately:

```cpp
{
    auto future = send(logger, address(), &Logger::log, "message");
    // future destroyed here - becomes "orphaned"
}
// Message still processed normally
// Result discarded when ready
```

**When to use:**
- Logging
- Notifications
- Events where result doesn't matter

---

### Pattern 4: Timeout with Polling

Implement timeout using `available()` polling:

```cpp
auto future = send(worker, address(), &Worker::slow_task, data);

// Poll with timeout
auto start = std::chrono::steady_clock::now();
while (!future.available()) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > std::chrono::seconds(5)) {
        std::cerr << "Timeout waiting for result\n";
        future.cancel();  // Request cancellation (best-effort)
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

if (future.available()) {
    int result = std::move(future).get();
    process_result(result);
}
```

**Note:** `cancel()` is best-effort - actor may have already processed the message.

---

### Pattern 5: Conditional Processing

Process result only if available:

```cpp
auto future = send(worker, address(), &Worker::compute, data);

// Do other work...

// Check if result is ready without blocking
if (future.available()) {
    int result = std::move(future).get();
    process_result(result);
} else {
    // Not ready yet, schedule callback or retry later
    schedule_retry(std::move(future));
}
```

---

## Best Practices

### ✅ DO:

1. **Use `available()` for non-blocking checks**
   ```cpp
   if (future.available()) {
       auto result = std::move(future).get();
   }
   ```

2. **Reserve vector capacity before adding futures**
   ```cpp
   std::vector<unique_future<int>> futures;
   futures.reserve(count);  // Prevent reallocation
   ```

3. **Ensure actor outlives all futures**
   ```cpp
   // Wait for all futures before destroying actor
   for (auto& future : futures) {
       std::move(future).get();
   }
   // Now safe to destroy actor
   ```

4. **Use fire-and-forget for notifications**
   ```cpp
   // Don't store future for fire-and-forget
   send(logger, address(), &Logger::log, "Event occurred");
   ```

5. **Move futures when calling `get()`**
   ```cpp
   int result = std::move(future).get();  // Move ownership
   ```

6. **Use `co_await` in coroutines (recommended)**
   ```cpp
   // Best: suspends without blocking
   int result = co_await std::move(future);
   ```

---

### ❌ DON'T:

1. **Call `get()` in tight loop without `available()` check**
   ```cpp
   // BAD: Busy-wait
   while (some_condition) {
       auto result = std::move(future).get();  // Blocks every iteration
   }

   // GOOD: Check first
   while (some_condition && future.available()) {
       auto result = std::move(future).get();
   }
   ```

2. **Destroy actor while futures are pending**
   ```cpp
   // BAD: Assertion failure in debug
   {
       auto actor = spawn<Worker>(resource);
       auto future = send(actor.get(), ...);
   }  // Actor destroyed, future still alive - CRASH in debug!

   // GOOD: Wait first
   {
       auto actor = spawn<Worker>(resource);
       auto future = send(actor.get(), ...);
       std::move(future).get();  // Wait before actor destruction
   }
   ```

3. **Store futures indefinitely**
   ```cpp
   // BAD: Memory leak
   std::vector<unique_future<int>> pending_futures;
   while (true) {
       pending_futures.push_back(send(...));  // Grows forever!
   }

   // GOOD: Process or discard
   auto future = send(...);
   if (care_about_result) {
       process(std::move(future).get());
   }  // Otherwise future destroyed
   ```

4. **Use exceptions for error handling**
   ```cpp
   // BAD: Exceptions disabled
   try {
       auto result = std::move(future).get();
   } catch (...) {  // Never caught - exceptions disabled!
   }

   // GOOD: Check state before get()
   if (future.is_cancelled()) {
       // Handle cancellation
       return;
   }
   auto result = std::move(future).get();  // Safe - not cancelled
   ```

---

## Message Lifetime & Ownership

Understanding message ownership is critical for correct promise/future usage.

### Ownership States

**1. Mailbox Owns (Normal)**
```cpp
send(actor, ...);  // Message in mailbox
// Actor processes → message deleted after handler returns
```

**2. Future Owns (After Actor Processes)**
```cpp
auto future = send(actor, ...);
// Actor processes → calls msg.release() → future owns message
// Future destructor deletes message when result consumed
```

**3. Orphaned (Future Destroyed Early)**
```cpp
{
    auto future = send(actor, ...);
}  // Future destroyed → message marked "orphaned"
// Actor still processes normally
// Mailbox deletes message after processing
```

### Conditional Delete Logic

Futures only delete messages if actor has processed them:

```cpp
// Future state determines ownership:
// - pending → actor hasn't processed yet → mailbox owns
// - ready/error/cancelled → actor processed → future owns
```

**Key insight:** State determines ownership:
- `pending` → actor hasn't processed → mailbox still owns
- `ready/error/cancelled` → actor processed AND released → future owns

---

## Performance

### Current Implementation: Exponential Backoff

When calling `future.get()`, the implementation uses exponential backoff:

```cpp
// Start: 1 microsecond sleep
// Growth: Doubles each iteration (1μs → 2μs → 4μs → ... → 1ms cap)
auto sleep_duration = std::chrono::microseconds(1);
while (!is_ready()) {
    std::this_thread::sleep_for(sleep_duration);
    if (sleep_duration < std::chrono::milliseconds(1)) {
        sleep_duration *= 2;  // Exponential growth
    }
}
```

**Performance characteristics:**
- CPU usage: ~0.1-1% while waiting
- Latency: Variable (1μs to 1ms wake-up time)
- Good for: Most use cases with <10ms wait times

### Optimization Options (Not Yet Implemented)

**1. C++20 Atomic Wait/Notify**
```cpp
// Zero overhead, zero CPU
state_.wait(future_state_enum::pending);
state_.notify_one();
```
- Requires: C++20 standard
- Benefit: Zero memory overhead, zero CPU usage
- Trade-off: C++20 only

**2. Condition Variable**
```cpp
// Lazy init on set_has_future()
std::unique_ptr<std::condition_variable> cv_;
std::unique_ptr<std::mutex> cv_mutex_;
```
- Benefit: Zero CPU usage while waiting
- Trade-off: +16 bytes per message with future, mutex overhead

**3. Hybrid Approach**
- Fast spin (100μs) for quick operations
- Blocking wait for slow operations
- Best of both worlds

**Recommendation:** Current exponential backoff is sufficient unless:
- Profiling shows CPU usage problem
- Futures regularly wait >10ms
- Large number of concurrent futures (>100)

---

## Debugging

### Common Issues & Solutions

**1. Assertion: "Actor destroyed with pending futures"**
```
Cause: Actor destructed while futures are still alive
Debug: Check stack trace - which future wasn't consumed?
Solution: Call future.get() or let futures destruct before actor
```

**2. Assertion: "Setting value on orphaned promise"**
```
Cause: Future was destroyed, then actor tried to set result
Debug: Check actor lifetime - is future outliving expected scope?
Solution: Check promise.is_valid() before set_value()
```

**3. Hang in `future.get()`**
```
Cause: Actor never calls set_value() or set_error()
Debug:
  - Check actor's behavior() dispatches message correctly
  - Check handler returns unique_future<T>
  - Check actor is scheduled (not stuck waiting)
Solution: Add logging in handler to verify execution
```

**4. Memory leak**
```
Cause: Storing futures indefinitely without calling get()
Debug: Track pending futures count in actor destructor
Solution: Always call get() or let future destruct
```

### Debug Build Features

- ✅ Assertions catch ownership violations
- ✅ Pending futures count checked in actor destructor
- ✅ Orphaned/cancelled checks before set_value/set_error
- ✅ Line numbers in assertion failures

### Release Build Warnings

⚠️ **Assertions disabled in release builds:**
- Ownership violations → undefined behavior
- Always test with assertions enabled first!
- Use TSAN/ASAN for production-quality validation

---

## Known Limitations

### 1. No Timeout Support

`get()` requires `available() == true` (non-blocking design).

**Pattern with timeout:**
```cpp
auto start = std::chrono::steady_clock::now();
while (!future.available()) {
    if (std::chrono::steady_clock::now() - start > timeout) {
        future.cancel();
        // Handle timeout
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
auto result = std::move(future).get();
```

---

### 2. No Exception Support

Errors via `failed()` and `error()` methods only.

**Design constraint:** `-fno-exceptions` build requirement.

**Alternative:** Use `std::error_code` return via `co_return`:
```cpp
unique_future<int> compute(int x) {
    if (x < 0) {
        co_return std::make_error_code(std::errc::invalid_argument);
    }
    co_return x * 2;
}
```

---

### 3. Single-Threaded `get()`

No concurrent `get()` calls on same future.

**Why:** Future is move-only (prevents accidental sharing).

**Safe:**
```cpp
// Same thread, multiple calls
auto result1 = future.get();  // Non-destructive
auto result2 = future.get();  // Same result
```

**Unsafe:**
```cpp
// Multiple threads, same future
std::thread t1([&] { auto r = std::move(future).get(); });
std::thread t2([&] { auto r = std::move(future).get(); });  // UB!
```

---

### 4. Actor Must Outlive Futures

No automatic lifetime management.

**Debug:** Assertion in `~cooperative_actor()` catches this.

**Release:** Undefined behavior if violated.

**Best practice:**
```cpp
// GOOD: Wait before destruction
{
    auto actor = spawn<Worker>(resource);
    auto future = send(actor.get(), ...);
    std::move(future).get();  // Wait first
}  // Actor destroyed safely

// BAD: Actor destroyed first
{
    auto actor = spawn<Worker>(resource);
    auto future = send(actor.get(), ...);
}  // Actor destroyed, future dangling - UB!
```

---

## Complete Example

```cpp
#include <actor-zeta.hpp>
#include <iostream>

// Calculator actor with coroutine methods
class calculator_actor final : public actor_zeta::basic_actor<calculator_actor> {
public:
    explicit calculator_actor(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<calculator_actor>(ptr) {}

    ~calculator_actor() = default;

    // Simple computation - coroutine
    actor_zeta::unique_future<int> compute(int x) {
        co_return x * x;
    }

    // Async method using co_await
    actor_zeta::unique_future<int> compute_chain(int x) {
        auto future = actor_zeta::send(this, address(), &calculator_actor::compute, x);
        int result = co_await std::move(future);  // Suspend until ready
        co_return result + 10;
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &calculator_actor::compute,
        &calculator_actor::compute_chain
    >;

    void behavior(actor_zeta::mailbox::message* msg) {
        switch (msg->command()) {
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::compute>:
                actor_zeta::dispatch(this, &calculator_actor::compute, msg);
                break;
            case actor_zeta::msg_id<calculator_actor, &calculator_actor::compute_chain>: {
                auto future = actor_zeta::dispatch(this, &calculator_actor::compute_chain, msg);
                if (!future.available()) {
                    pending_.push_back(std::move(future));
                }
                break;
            }
        }
    }

    // Clean up completed pending futures
    bool poll_pending() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->available()) {
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        return !pending_.empty();
    }

private:
    std::vector<actor_zeta::unique_future<int>> pending_;
};

int main() {
    auto* resource = std::pmr::get_default_resource();
    auto calculator = actor_zeta::spawn<calculator_actor>(resource);

    // Simple call
    auto future = actor_zeta::send(calculator.get(), calculator->address(),
                                   &calculator_actor::compute, 5);
    calculator->resume(100);
    int result = std::move(future).get();
    std::cout << "5^2 = " << result << "\n";  // Output: 5^2 = 25

    // Chained call with co_await
    auto future2 = actor_zeta::send(calculator.get(), calculator->address(),
                                    &calculator_actor::compute_chain, 5);
    while (!future2.available()) {
        calculator->resume(100);
        calculator->poll_pending();
    }
    int result2 = std::move(future2).get();
    std::cout << "5^2 + 10 = " << result2 << "\n";  // Output: 5^2 + 10 = 35

    return 0;
}
```

---

## See Also

- `CLAUDE.md` - Development guide for this codebase
- `CHANGELOG.md` - Recent promise/future improvements
- `header/actor-zeta/detail/future.hpp` - Promise/Future implementation
- `examples/coroutine/mixed_example.cpp` - Working coroutine example