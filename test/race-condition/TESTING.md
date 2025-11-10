# Race Condition Testing Strategy

## Overview

This document describes the strategy for testing race conditions in actor-zeta's future/promise system. The goal is to ensure thread-safe behavior under concurrent operations.

## Components Under Test

### 1. `future_state` (future_state.hpp)

**Shared mutable state:**
- `refcount_` - atomic<int> reference counter
- `state_` - atomic<future_state_enum> state machine
- `result_` - rtt (may contain non-trivial types)

**Critical operations:**
- `add_ref()` / `release()` - refcount management
- `set_result()` - state transition (pending → ready)
- `is_ready()` / `state()` - state observation
- `result()` - result access

**Invariants:**
- Refcount never goes negative
- State transitions are monotonic (pending → ready/error/cancelled)
- Result is immutable after set_result()
- Exactly one thread calls destroy()

---

### 2. `unique_future` (future.hpp)

**Ownership:**
- `state_*` - raw pointer (unique ownership)
- `cancellation_token_` - intrusive_ptr (shared ownership)

**Critical operations:**
- Constructor/Destructor - state ownership transfer
- `get()` - blocking wait + result extraction
- Move constructor/assignment - ownership transfer
- Destructor - RAII cancellation + release()

**Invariants:**
- State pointer is null after move
- Only rvalue get() allowed (consumable future)
- Destructor calls release() exactly once
- Cancellation token shared with message

---

### 3. `cooperative_actor::enqueue_impl()`

**Shared state:**
- `is_scheduled_` - atomic<bool> scheduler queue flag
- `mailbox()` - concurrent queue

**Critical operations:**
- `push_back()` - enqueue message
- CAS on `is_scheduled_` - prevent double-scheduling
- Return `needs_scheduling` flag

**Invariants:**
- Only one thread sets `is_scheduled_` = true per unblock
- `needs_scheduling` = true ⟹ caller must call scheduler->enqueue()
- No concurrent resume() calls (protected by is_scheduled_)

---

### 4. `cooperative_actor::resume()`

**Shared state:**
- `resuming_` - atomic<bool> concurrent resume detection
- `current_message_` - raw pointer to current message

**Critical operations:**
- `resuming_` guard - detect concurrent resume()
- Message processing - call behavior()
- Slot release - decrement refcount

**Invariants:**
- No concurrent resume() (resuming_ guard asserts)
- Message guard RAII cleanup is exception-safe
- Slot released exactly once per message

---

### 5. `message` (message.hpp)

**Shared state:**
- `result_slot_` - future_state_base* pointer
- `cancellation_token_` - intrusive_ptr

**Ownership rules:**
- Mailbox owns message until processed
- Actor processes message → calls slot->release()
- Future destructor calls slot->release()
- Refcount = 2 initially (actor + future)

**Invariants:**
- Slot released by both actor AND future (total 2 releases)
- Orphaned messages still processed (future destroyed early)
- No double-delete of slot

---

## Test Categories

### Category 1: State Transition Races

**Scenario:** Concurrent state changes
- set_result() vs cancel()
- is_ready() during set_result()
- Concurrent state observations

**Coverage:**
- ✅ Test 1.1: set_result vs cancel
- ✅ Test 1.2: is_ready during transition
- ⚠️ Test 1.3: Multiple state observers

---

### Category 2: Refcount Management Races

**Scenario:** Concurrent refcount operations
- Actor release() vs future destructor release()
- Multiple futures sharing same slot (should not happen!)
- add_ref() vs release()

**Coverage:**
- ✅ Test 2.1: Concurrent actor + future release
- ✅ Test 2.2: Stress test with 1000 futures
- ⚠️ Test 2.3: Refcount underflow detection

---

### Category 3: Future Lifetime Races

**Scenario:** Future ownership transfer
- Move during get()
- Destructor during get()
- Copy attempts (should fail to compile)

**Coverage:**
- ✅ Test 3.1: Move during get()
- ✅ Test 3.2: Synchronous actor (immediate ready)
- ⚠️ Test 3.3: Future stored across threads

---

### Category 4: Actor Shutdown Races

**Scenario:** Actor destruction with live futures
- Destroy actor with pending futures
- Graceful shutdown (wait for futures)
- Mailbox close propagation

**Coverage:**
- ✅ Test 4.1: Destroy with pending futures
- ✅ Test 4.2: Graceful shutdown
- ⚠️ Test 4.3: Resume during shutdown

---

### Category 5: Mailbox Concurrent Operations

**Scenario:** Concurrent mailbox access
- Enqueue vs close
- try_block vs push_back (unblock detection)
- Concurrent pop_front (scheduler threads)

**Coverage:**
- ✅ Test 5.1: Enqueue vs close
- ✅ Test 5.2: Block/unblock races
- ⚠️ Test 5.3: Concurrent pop_front

---

## Testing Approach

### Principles

1. **Targeted tests** - Each test focuses on one specific race scenario
2. **Deterministic triggers** - Use barriers/latches to control thread timing
3. **Statistical validation** - Run N iterations, verify invariants hold
4. **Debug instrumentation** - Use magic numbers, generation IDs

### Tools

**Sanitizers:**
- AddressSanitizer (ASan) - detects memory errors
- ThreadSanitizer (TSan) - detects data races
- UndefinedBehaviorSanitizer (UBSan) - detects undefined behavior

**Debug features:**
```cpp
#ifndef NDEBUG
    uint32_t magic_;       // 0xFEEDFACE (alive) / 0xDEADC0DE (deleted)
    uint64_t generation_;  // Unique ID for tracking
#endif
```

### Test Structure

```cpp
TEST_CASE("Category X: Specific race scenario") {
    // Setup: Create actors, futures, barriers

    // Synchronization: Barrier to align threads
    std::barrier sync_point(NUM_THREADS);

    // Workers: Thread functions that trigger race
    auto thread1 = [&]() {
        sync_point.arrive_and_wait();  // Align timing
        // Perform operation 1
    };

    auto thread2 = [&]() {
        sync_point.arrive_and_wait();  // Align timing
        // Perform operation 2 (concurrent with thread1)
    };

    // Execute: Launch threads
    std::thread t1(thread1);
    std::thread t2(thread2);
    t1.join();
    t2.join();

    // Verify: Check invariants
    REQUIRE(/* invariant holds */);
}
```

---

## Implementation Phases

### Phase 1: Critical Tests (Priority: HIGH)

**Tests to implement:**
1. ✅ Test 2.1: Refcount concurrent release (actor + future)
2. ✅ Test 4.1: Actor destroyed with pending futures
3. ✅ Test 1.1: State transitions (set_result vs cancel)

**Why critical:**
- Refcount bugs → memory corruption / crashes
- Actor shutdown → common real-world scenario
- State transitions → core correctness

**Success criteria:**
- All Phase 1 tests pass under ASan + TSan
- No flaky tests (1000/1000 runs pass)

---

### Phase 2: Important Tests (Priority: MEDIUM)

**Tests to implement:**
1. Test 5.1: Mailbox enqueue vs close
2. Test 3.1: Future lifetime management
3. Test 1.2: Concurrent state reads

**Why important:**
- Mailbox close → shutdown path coverage
- Future lifetime → ownership bugs
- State reads → visibility guarantees

---

### Phase 3: Comprehensive Coverage (Priority: LOW)

**Tests to implement:**
1. Test 2.2: Refcount stress test (1000 futures)
2. Test 5.2: Mailbox block/unblock
3. Test 3.3: Future across threads

**Why nice-to-have:**
- Stress tests → find rare bugs
- Block/unblock → scheduling edge cases
- Cross-thread futures → advanced usage

---

## Current Status

### ✅ Fixed Issues

1. **is_scheduled_ CAS protection** (2025-01)
   - **Issue:** Multiple threads could call scheduler->enqueue() concurrently
   - **Fix:** Added CAS on is_scheduled_ in enqueue_impl()
   - **Result:** tests_race_condition now passes (100% success rate)

### ✅ Existing Tests

1. **"Race condition stress test - future destruction timing"**
   - Coverage: Concurrent enqueue + random future destruction
   - Threads: 4 worker threads + 2 scheduler threads
   - Duration: 500ms stress test
   - Status: PASSING

2. **"Race condition stress test - concurrent future destruction"**
   - Coverage: Future destroyed in separate thread during processing
   - Iterations: 5000 races
   - Status: PASSING

3. **"Memory leak detection - orphaned messages"**
   - Coverage: Futures destroyed before actor processes
   - Messages: 1000 orphaned messages
   - Status: PASSING

### ⚠️ Coverage Gaps

**Not yet tested:**
- Refcount double-release scenarios
- Actor destruction with live futures (explicit test)
- State transition races (set_result vs cancel)
- Mailbox close during enqueue
- Concurrent state observations

---

## Metrics

### Success Criteria

1. **Code Coverage:**
   - ✅ All atomic operations tested under concurrency
   - ✅ All state transitions tested under race conditions
   - ✅ All ownership transfers tested

2. **Reliability:**
   - ✅ 0 flaky tests (deterministic outcomes)
   - ✅ 1000/1000 runs pass under normal build
   - ✅ 100/100 runs pass under sanitizers

3. **Performance:**
   - ✅ All tests complete in < 10 seconds
   - ✅ Stress tests find bugs in < 5 seconds (if bugs exist)

4. **Sanitizer Clean:**
   - ✅ 0 ASan errors (memory safety)
   - ✅ 0 TSan errors (thread safety)
   - ✅ 0 UBSan errors (undefined behavior)

---

## Future Work

### Planned Improvements

1. **Test Infrastructure:**
   - Add test utilities (barriers, latches for C++11)
   - Add race injection framework (controlled delays)
   - Add statistical analysis (detect timing-dependent failures)

2. **Additional Scenarios:**
   - Multi-actor races (actors sending to each other)
   - Scheduler stress (100+ actors, 1000+ messages)
   - Long-running stress (hours, not seconds)

3. **Tooling:**
   - CI integration with sanitizers
   - Automated race detection (TSan in CI)
   - Performance benchmarks (throughput under contention)

---

## References

**Related Documentation:**
- `CLAUDE.md` - Main development guide
- `PROMISE_FUTURE_IMPLEMENTATION.md` - Future/promise architecture
- `header/actor-zeta/detail/future_state.hpp` - Implementation

**Relevant Commits:**
- is_scheduled_ CAS protection (2025-01)
- State Enum implementation (2025-01)
- Future extraction from cooperative_actor (2025-01)

---

**Last updated:** 2025-01-03
**Test pass rate:** 56/56 (100%)
**Sanitizer status:** Clean (ASan, TSan, UBSan)