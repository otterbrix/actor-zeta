# Changelog

All notable changes, fixes, and refactorings to actor-zeta are documented here.

## [2025-01] - Major Refactoring & Stability Improvements

### Actor State Management

#### Actor State Refactoring - Unified State Management
**Date:** 2025-01

**Change:** Replaced three atomic flags with single `atomic<actor_state>` using bit flags.

**Motivation:** Simplify synchronization, prevent invalid state combinations, enable atomic state transitions.

**Implementation:**
```cpp
// Before: three separate atomics
std::atomic<bool> resuming_{false};
std::atomic<bool> is_scheduled_{false};
std::atomic<bool> destroying_{false};

// After: single atomic enum
std::atomic<actor_state> state_{actor_state::idle};

enum class actor_state : uint8_t {
    idle                         = 0b000,
    scheduled                    = 0b001,
    running                      = 0b010,
    running_scheduled            = 0b011,
    idle_destroying              = 0b100,
    running_destroying           = 0b110,
    // Invalid states prevented by design
};
```

**Benefits:**
- Single atomic operation instead of multiple
- Invalid state combinations impossible
- Atomic state transitions with CAS validation
- Follows `future_state` pattern for consistency

**Files affected:**
- `header/actor-zeta/base/detail/cooperative_actor_classic.hpp`

**Testing:** All 59 tests pass with TSAN (100%) and ASAN (100%)

---

### Message System

#### Message Creation Refactoring - Receiver-Side Allocation
**Date:** 2025-11-19

**Change:** Messages now created in **receiver's** memory resource instead of sender's.

**Reason:** Eliminate cross-arena migration issues with RTT containing non-trivial types.

**Problem (before):**
- Message created in sender's `resource()` via `make_message()`
- Passed to receiver's mailbox (potentially different `memory_resource`)
- RTT move constructor with different resources → assertion failure

**Solution (after):**
- `send()` passes arguments directly to `enqueue_impl(sender, cmd, args...)`
- Receiver creates message in its own memory: `make_message(this->resource(), sender, cmd, args...)`
- No cross-arena migration, proper memory ownership

**API Changes:**
```cpp
// OLD API (removed)
template<typename R>
unique_future<R> enqueue_impl(mailbox::message_ptr msg);

// NEW API (current)
template<typename R, typename... Args>
unique_future<R> enqueue_impl(address_t sender, message_id cmd, Args&&... args);
```

**Impact:**
- **For library users:** No changes needed - `send()` API unchanged
- **For custom actors/balancers:** Update `enqueue_impl()` signature if overridden

**Benefits:**
- ✅ Eliminated cross-arena RTT migration
- ✅ Proper ownership semantics (receiver owns messages)
- ✅ Single allocation in correct arena
- ✅ Perfect forwarding for efficiency

**Files affected:**
- `header/actor-zeta/base/detail/cooperative_actor_classic.hpp`
- `header/actor-zeta/send.hpp`
- `header/actor-zeta/base/actor_mixin.hpp`
- `examples/balancer/main.cpp`
- `examples/supervisor/main.cpp`
- `examples/broadcast/main.cpp`

**Testing:** 63/64 tests pass (98%)

**See also:** `MESSAGE_CREATION_REFACTORING.md`

---

#### RTT - Same-Arena Only Migration
**Date:** 2025-01

**Issue:** Cross-arena migration used `std::memcpy` which broke non-trivial types like `std::string`.

**Solution:** RTT allocator-extended move constructor now **only supports same-arena migration**.

**Implementation:** Added `assert(resource == other.memory_resource_)` to prevent cross-arena usage.

**Reason:** Type-erased containers can't properly copy non-trivial types without runtime type info.

**Files affected:**
- `header/actor-zeta/detail/rtt.hpp`
- `test/message/main.cpp` (removed cross-arena migration tests)

---

### Scheduler & Execution

#### Resume Info - Extended Actor Resume Result
**Date:** 2025-01

**Feature:** `resume()` now returns `resume_info` struct with execution statistics.

**Motivation:** Enable graceful shutdown monitoring and scheduler observability.

**Implementation:**
```cpp
struct resume_info {
    resume_result result;           // Execution status
    size_t messages_processed;      // Messages handled this resume

    operator resume_result() const noexcept { return result; }
};
```

**Use cases:**
- Graceful shutdown monitoring (track message draining)
- Scheduler telemetry (throughput, fairness)
- Load balancing decisions

**Files affected:**
- `header/actor-zeta/scheduler/resumable.hpp`
- `header/actor-zeta/base/detail/cooperative_actor_classic.hpp`
- `header/actor-zeta/impl/scheduler/sharing_scheduler.ipp`

**Backward compatibility:** Existing code works unchanged via implicit conversion.

---

#### Manual Scheduling - Removed Auto-Scheduling
**Date:** 2025-01

**Change:** Actors no longer self-schedule when messages are enqueued.

**Reason:** Transitioning to fully manual scheduling for better control (load balancing, custom schedulers).

**Behavior:**
- **Before:** Actor automatically scheduled itself when transitioning from blocked to unblocked
- **After:** Caller must explicitly call `scheduler->schedule(actor)` after `enqueue()`

**Pattern (balancer):**
```cpp
template<typename R, typename... Args>
unique_future<R> enqueue_impl(address_t sender, message_id cmd, Args&&... args) {
    auto& worker = workers_[cursor_++ % workers_.size()];
    auto future = worker->template enqueue_impl<R>(sender, cmd, std::forward<Args>(args)...);
    scheduler_->schedule(worker.get());  // Manual - caller's responsibility
    return future;
}
```

**Files affected:**
- `header/actor-zeta/base/detail/cooperative_actor_classic.hpp`
- `test/shutdown/main.cpp` (balancer example)

---

#### Shutdown Race Condition Fix
**Date:** 2025-01

**Issue:** Assertion failure `assert(!blocked())` in `lifo_inbox.hpp` during scheduler shutdown.

**Root cause:** Race condition when `resume_core_()` calls `inbox().empty()` while inbox is in `blocked` state.

**Solution:** Check `inbox().blocked()` BEFORE calling `inbox().empty()` in `resume_core_()`.

**Implementation:**
```cpp
// Check if inbox is blocked first to avoid assertion in empty()
if (inbox().blocked()) {
    return scheduler::resume_result::resume;
}

if (inbox().empty()) {
    return inbox().try_block()
           ? scheduler::resume_result::awaiting
           : scheduler::resume_result::resume;
}
```

**Why this works:**
- `blocked()` has no preconditions, safe to call anytime
- `empty()` requires `!blocked()` precondition per API contract
- If blocked, resume anyway to check for new messages

**Files affected:**
- `header/actor-zeta/base/detail/cooperative_actor_classic.hpp`
- `test/shutdown/main.cpp` (reproduces bug with balancer pattern)

---

### Promise/Future System

#### Promise/Future Code Quality Improvements
**Date:** 2025-01

Multiple fixes to promise/future implementation:

**✅ Bug Fix #1: Memory Leak in `release_message_ref()`**
- **Issue:** Future deleted message even if actor hadn't processed it yet
- **Solution:** Check `error() != pending` before deleting
- **File:** `cooperative_actor_classic.hpp`

**✅ Bug Fix #2: Wrong PMR Resource in `promise::set_value()`**
- **Issue:** Used `get_default_resource()` instead of message's resource
- **Solution:** Use `slot_->body().memory_resource()` (added `rtt::memory_resource()` getter)
- **Files:** `detail/rtt.hpp`, `cooperative_actor_classic.hpp`

**✅ Improvement #3: Exponential Backoff in `get()`**
- **Before:** Busy-wait with `yield()` (~100% CPU)
- **After:** Exponential backoff 1μs → 1ms (~0.1-1% CPU)
- **Impact:** 99% CPU reduction while waiting

**✅ Improvement #4: Thread Safety in Destructor**
- **Issue:** `pending_futures_count_` used `memory_order_relaxed`
- **Solution:** Use `memory_order_acquire` to synchronize with future destructors

**✅ Improvement #5: Orphaned Check in `promise::set_error()`**
- Added assertion to catch setting error on orphaned promises

**✅ Code Quality: Added `[[nodiscard]]` Attributes**
- Methods: `is_cancelled()`, `is_valid()`, `is_ready()`, `error()`, `valid()`

---

#### Cooperative Actor Bug Fixes
**Date:** 2025-01

**✅ `current_msg_guard` Destructor Fix**
- **Issue:** Saved `prev` pointer but didn't restore it
- **Solution:** `self->current_message_ = prev;`
- **Impact:** Nested message processing now works correctly

**✅ Edge Case: `max_throughput == 0` Validation**
- Added `assert(max_throughput > 0)` in `resume()`

**✅ Code Modernization**
- Deleted constructors: Changed to explicit `= delete`

---

### Build System & Testing

#### Conan 2.x + CMake Integration
**Date:** 2025-01

**Issue:** `cmake_layout` in `conanfile.txt` created nested directory structures.

**Solution:** Removed `[layout]` section from `conanfile.txt`.

**Toolchain path:** `build/${BUILD_TYPE}/conan_toolchain.cmake` (NOT in `generators/` subdirectory)

**Files affected:**
- `conanfile.txt`
- `.github/workflows/ubuntu_clang.yaml`
- `.github/workflows/ubuntu_gcc.yaml`
- `.github/workflows/macos.yml`

---

#### CI/CD - Compiler C++20 Support
**Date:** 2025-01

**Issue:** Old GCC/Clang versions don't support C++20.

**Solution:** Added matrix exclusions for C++20 builds with older compilers.

**Excluded from C++20:**
- GCC 7, 8, 9, 10
- Clang 9, 10

**Files affected:** Ubuntu workflow files

---

#### Test Memory Resource - `posix_memalign` Requirements
**Date:** 2025-01

**Issue:** `posix_memalign` requires alignment ≥ `sizeof(void*)` and power of 2.

**Solution:** Adjust alignment to minimum `sizeof(void*)` if smaller.

**Files affected:** `test/unique_function/main.cpp`

---

## Migration Guides

### Migrating to New Message Creation API (2025-11-19)

**For library users:** No changes needed - `send()` API unchanged.

**For custom actor/balancer implementers:**

**Before:**
```cpp
template<typename R>
unique_future<R> enqueue_impl(mailbox::message_ptr msg) {
    // Process pre-created message
}
```

**After:**
```cpp
template<typename R, typename... Args>
unique_future<R> enqueue_impl(address_t sender, message_id cmd, Args&&... args) {
    // Create message in receiver's resource
    auto msg = detail::make_message(this->resource(), sender, cmd, std::forward<Args>(args)...);
    // ...
}
```

**Balancer pattern:**
```cpp
template<typename R, typename... Args>
unique_future<R> enqueue_impl(address_t sender, message_id cmd, Args&&... args) {
    auto& worker = workers_[cursor_++ % workers_.size()];
    // Forward args to child (child creates message)
    return worker->template enqueue_impl<R>(sender, cmd, std::forward<Args>(args)...);
}
```

**Supervisor pattern:**
```cpp
template<typename R, typename... Args>
unique_future<R> enqueue_impl(address_t sender, message_id cmd, Args&&... args) {
    // Use helper from actor_mixin
    return enqueue_sync_impl<R>(
        sender, cmd,
        [this](auto* msg) { behavior(msg); },
        std::forward<Args>(args)...
    );
}
```

---

### Migrating to Manual Scheduling (2025-01)

**Before (auto-scheduling):**
```cpp
send(worker, sender, &Worker::process, data);
// Worker auto-scheduled when unblocked
```

**After (manual scheduling):**
```cpp
auto future = send(worker, sender, &Worker::process, data);
if (future.needs_scheduling()) {
    scheduler->schedule(worker.get());
}
```

**Balancer pattern:**
```cpp
auto future = child->enqueue_impl<R>(sender, cmd, std::forward<Args>(args)...);
scheduler_->schedule(child.get());  // Always schedule manually
return future;
```

---

---

## [2025-12] - C++20 Coroutines & std::pmr Migration

### PMR Migration
**Date:** 2025-12

**Change:** Migrated from custom `actor_zeta::pmr` to `std::pmr`.

**API Changes:**
```cpp
// OLD API (removed)
actor_zeta::pmr::memory_resource* resource;
actor_zeta::pmr::get_default_resource();

// NEW API (current)
std::pmr::memory_resource* resource;
std::pmr::get_default_resource();
```

**Files affected:**
- All header files using memory resources
- All examples and tests

---

### unique_future API Updates
**Date:** 2025-12

**Change:** Renamed `is_ready()` to `available()` for clearer semantics.

**API Changes:**
```cpp
// OLD API
if (future.is_ready()) { ... }

// NEW API (current)
if (future.available()) { ... }
```

**Additional API:**
- `failed()` - Check if future has error
- `error()` - Get error code
- `valid()` - Check if future has valid state

---

### Coroutine-First Design
**Date:** 2025-12

**Change:** All actor methods returning `unique_future<T>` must now be coroutines.

**Pattern:**
```cpp
// ALL handlers must use co_return
unique_future<int> compute(int x) {
    co_return x * 2;  // Required!
}

unique_future<void> process() {
    co_return;  // Required for void!
}
```

**Behavior dispatch with coroutines:**
```cpp
void behavior(mailbox::message* msg) {
    switch (msg->command()) {
        case msg_id<Actor, &Actor::compute>:
            dispatch(this, &Actor::compute, msg);
            break;
        case msg_id<Actor, &Actor::async_method>: {
            // CRITICAL: Store pending coroutine!
            auto future = dispatch(this, &Actor::async_method, msg);
            if (!future.available()) {
                pending_.push_back(std::move(future));
            }
            break;
        }
    }
}
```

---

### Generator<T> - Streaming Data Between Actors
**Date:** 2025-12

**Feature:** Added `generator<T>` coroutine type for streaming data between actors.

**Motivation:** Enable actor methods to yield multiple values over time, supporting efficient streaming patterns without buffering all data in memory.

**Key Features:**
- **Move-only** - generators cannot be copied
- **Lazy evaluation** - producer runs on-demand
- **PMR allocation** - uses actor's memory resource
- **Integrated with send()** - works seamlessly with actor messaging
- **Symmetric transfer** - efficient producer/consumer context switching
- **CAS synchronization** - thread-safe state management

**API:**
```cpp
class DataProducer : public basic_actor<DataProducer> {
    // Generator method - yields values one by one
    generator<int> stream_range(int start, int end) {
        for (int i = start; i < end; ++i) {
            co_yield i;  // Yield and suspend
        }
        // Implicit co_return at end
    }

    using dispatch_traits = dispatch_traits<&DataProducer::stream_range>;

    void behavior(mailbox::message* msg) {
        if (msg->command() == msg_id<DataProducer, &DataProducer::stream_range>) {
            dispatch(this, &DataProducer::stream_range, msg);
        }
    }
};

// Usage via send() API
auto gen = send(producer.get(), sender, &DataProducer::stream_range, 0, 10);
producer->resume(1);  // Start generator

// Generator lifecycle control
gen.cancel();   // Stop producer early
gen.detach();   // Release handle, let producer run
```

**Generator States:**
| State | Description |
|-------|-------------|
| `created` | Producer not yet started |
| `suspended` | Producer yielded a value |
| `exhausted` | Producer finished (co_return) |
| `cancelled` | Consumer cancelled |
| `detached` | Consumer detached |

**State Queries:**
```cpp
gen.valid()              // Has valid state (not moved-from)
gen.exhausted()          // Producer finished
gen.is_cancelled()       // Consumer cancelled
gen.is_safe_to_destroy() // In terminal state
```

**Consumer Pattern (in coroutine context):**
```cpp
unique_future<void> consume_stream() {
    auto gen = other_actor->stream_data();
    while (co_await gen) {          // Wait for next value
        auto& value = gen.current(); // Get reference to yielded value
        process(value);
    }
    co_return;
}
```

**Files Created:**
- `header/actor-zeta/detail/generator.hpp` - Core implementation
- `header/actor-zeta/generator.hpp` - Public API
- `test/generator/main.cpp` - 16 test cases, 309 assertions
- `test/generator/CMakeLists.txt`
- `examples/generator/main.cpp` - Usage examples
- `examples/generator/CMakeLists.txt`
- `GENERATOR_GUIDE.md` - Documentation

**Files Modified:**
- `header/actor-zeta.hpp` - Added `#include <actor-zeta/generator.hpp>`
- `header/actor-zeta/detail/type_traits.hpp` - Added `is_generator<T>`, `is_generator_v<T>`, `generator_type` concept
- `header/actor-zeta/actor/cooperative_actor.hpp` - Generator path in `enqueue_impl`
- `header/actor-zeta/actor/dispatch.hpp` - Generator dispatch and linking
- `header/actor-zeta/actor/dispatch_traits.hpp` - `dispatch_result_t` for generator return types
- `header/actor-zeta/send.hpp` - Returns `generator<T>` for generator methods
- `test/CMakeLists.txt` - Added generator subdirectory
- `examples/CMakeLists.txt` - Added generator subdirectory

**Limitations:**
- Requires coroutine context for `co_await gen` consumption
- No synchronous `next()` API (by design)
- Single consumer only (move-only)
- No `generator<void>` support
- No exception support (`-fno-exceptions`)

**Testing:** 16 test cases with 309 assertions including:
- Basic type traits and move-only semantics
- co_yield functionality and exhaustion
- Empty generator and cancellation
- Detach behavior
- send() API integration
- Multi-threaded tests (concurrent send, cancel, stress)

**See also:** `GENERATOR_GUIDE.md` for complete documentation.

---

### Examples Updated
**Date:** 2025-12

**Changes:**
- `dataflow` example removed (commented out in CMakeLists.txt)
- `coroutine` example added with `mixed_example.cpp`
- `generator` example added demonstrating streaming patterns
- All examples updated to use `std::pmr` and coroutine patterns

**Current examples:**
- `balancer` - Load balancing with coroutines
- `broadcast` - Message broadcasting
- `supervisor` - Supervisor pattern
- `coroutine` - Mixed sync/async coroutine patterns
- `generator` - Streaming data between actors

---

## See Also

- `GENERATOR_GUIDE.md` - Generator streaming patterns and usage
- `PROMISE_FUTURE_GUIDE.md` - Promise/future patterns and usage
- `MESSAGE_CREATION_REFACTORING.md` - Detailed message creation refactoring notes
- `CLAUDE.md` - Development guide for this codebase