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

Actor-zeta supports **async request-response** via nested `promise<T>` and `unique_future<T>` classes in `cooperative_actor`. This enables both fire-and-forget and request-response patterns.

**Key Features:**
- Non-blocking message sends with optional result
- Fire-and-forget pattern (ignore future)
- Request-response pattern (wait for result)
- Orphaned futures (future destroyed before actor processes)
- Cancellation support

---

## Basic Usage

### Fire-and-Forget (No Return Value)

```cpp
// Send message, don't care about result
send(target_actor, sender, &TargetActor::handle_command, arg1, arg2);
```

### Request-Response (Wait for Result)

```cpp
// Send message, get future
auto future = send(target_actor, sender, &TargetActor::compute, arg1);

// Wait for result (blocking)
int result = std::move(future).get();

// Or check if ready (non-blocking)
if (future.is_ready()) {
    int result = std::move(future).get();
}
```

### Handler Implementation

Handlers automatically return results via `unique_future<T>`:

```cpp
class Worker : public basic_actor<Worker> {
public:
    // Handler returns future
    unique_future<int> compute(int x) {
        int result = x * 2;
        // Return ready future with result
        return make_ready_future<int>(resource(), result);
    }

    // Or for void results
    unique_future<void> process_task(const std::string& task) {
        // Do work...
        return make_ready_future_void(resource());
    }
};
```

---

## Common Patterns

### Pattern 1: Simple Request-Response

**Sender side:**
```cpp
auto future = send(worker, address(), &Worker::compute, 42);
int result = std::move(future).get();  // Blocks until ready
std::cout << "Result: " << result << "\n";
```

**Worker side:**
```cpp
class Worker : public basic_actor<Worker> {
    unique_future<int> compute(int x) {
        // Do expensive computation
        int result = x * 2;

        // Return result (promise fulfilled automatically)
        return make_ready_future<int>(resource(), result);
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
    results.push_back(std::move(future).get());
}

// Or process as they complete
for (auto& future : futures) {
    while (!future.is_ready()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int result = std::move(future).get();
    process_result(result);
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

Implement timeout using `is_ready()` polling:

```cpp
auto future = send(worker, address(), &Worker::slow_task, data);

// Poll with timeout
auto start = std::chrono::steady_clock::now();
while (!future.is_ready()) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > std::chrono::seconds(5)) {
        std::cerr << "Timeout waiting for result\n";
        future.cancel();  // Request cancellation (best-effort)
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

if (future.is_ready()) {
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
if (future.is_ready()) {
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

1. **Use `is_ready()` for non-blocking checks**
   ```cpp
   if (future.is_ready()) {
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

---

### ❌ DON'T:

1. **Call `get()` in tight loop without `is_ready()` check**
   ```cpp
   // BAD: Busy-wait
   while (some_condition) {
       auto result = std::move(future).get();  // Blocks every iteration
   }

   // GOOD: Check first
   while (some_condition && future.is_ready()) {
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

   // GOOD: Check error code
   auto result = std::move(future).get();
   if (future.error() != slot_error_code::ok) {
       // Handle error
   }
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
// In unique_future::release_message_ref()
if (slot_->error() != slot_error_code::pending) {
    // Actor processed → future owns → DELETE
    mailbox::message_ptr auto_delete(slot_);
} else {
    // Actor hasn't processed → mailbox owns → DON'T delete
}
```

**Key insight:** Only delete if `error != pending`:
- `pending` → actor hasn't called `set_error()` → mailbox still owns
- `ok/error` → actor called `set_error()` AND `msg.release()` → future owns

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
slot_->error_.wait(slot_error_code::pending);
slot_->error_.notify_one();
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

`get()` blocks indefinitely.

**Workaround:**
```cpp
auto start = std::chrono::steady_clock::now();
while (!future.is_ready()) {
    if (std::chrono::steady_clock::now() - start > timeout) {
        future.cancel();
        throw timeout_error();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
```

**Future enhancement:** Add `get(timeout)` overload.

---

### 2. No Exception Support

Errors via `slot_error_code` enum only.

**Design constraint:** `-fno-exceptions` build requirement.

**Alternative:** Use `std::optional<T>` or `std::variant<T, error_code>`:
```cpp
unique_future<std::optional<int>> compute(int x) {
    if (x < 0) {
        return make_ready_future<std::optional<int>>(resource(), std::nullopt);
    }
    return make_ready_future<std::optional<int>>(resource(), x * 2);
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
#include <vector>

// Worker actor that performs computations
class ComputeWorker : public actor_zeta::basic_actor<ComputeWorker> {
public:
    // Handler returns future with result
    actor_zeta::unique_future<int> compute(int x) {
        int result = x * x;
        return actor_zeta::make_ready_future<int>(resource(), result);
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &ComputeWorker::compute
    >;

    explicit ComputeWorker(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<ComputeWorker>(ptr)
        , compute_(actor_zeta::make_behavior(resource(), this, &ComputeWorker::compute)) {
    }

    void behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<ComputeWorker, &ComputeWorker::compute>) {
            compute_(msg);
        }
    }

private:
    actor_zeta::behavior_t compute_;
};

int main() {
    auto* resource = actor_zeta::pmr::get_default_resource();

    // Create workers
    std::vector<std::unique_ptr<ComputeWorker, actor_zeta::pmr::deleter_t>> workers;
    for (int i = 0; i < 3; ++i) {
        workers.push_back(actor_zeta::spawn<ComputeWorker>(resource));
    }

    // Send parallel requests
    std::vector<ComputeWorker::unique_future<int>> futures;
    futures.reserve(workers.size());  // CRITICAL: Reserve capacity!

    for (size_t i = 0; i < workers.size(); ++i) {
        futures.push_back(actor_zeta::send(
            workers[i].get(),
            actor_zeta::address_t::empty_address(),
            &ComputeWorker::compute,
            static_cast<int>(i + 1)
        ));
    }

    // Wait for all results
    std::cout << "Results:\n";
    for (size_t i = 0; i < futures.size(); ++i) {
        int result = std::move(futures[i]).get();
        std::cout << "  Worker " << i << ": " << result << "\n";
    }

    return 0;
}
```

**Output:**
```
Results:
  Worker 0: 1
  Worker 1: 4
  Worker 2: 9
```

---

## See Also

- `CLAUDE.md` - Development guide for this codebase
- `CHANGELOG.md` - Recent promise/future improvements
- `header/actor-zeta/base/detail/cooperative_actor_classic.hpp` - Implementation