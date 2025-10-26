# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 🚨 CRITICAL: READ FULL FILES BEFORE MAKING CHANGES

**ALWAYS read entire header files (at least the full file, not just snippets) before making changes.** This codebase has intricate template metaprogramming, PMR allocators, and cooperative actor patterns. Reading 50-100 lines will cause you to:
- Add duplicate functions that exist deeper in the file
- Break the carefully crafted RTTI-free/exception-free design
- Misunderstand the actor lifecycle and memory management patterns

**Once you've read the full file, you understand it completely.** Trust your full read and make changes directly.

## Core Principles

1. **READ FIRST**: Read entire files to understand context fully
2. **NO RTTI, NO EXCEPTIONS**: Code MUST compile with `-fno-rtti -fno-exceptions`
3. **FOLLOW EXISTING PATTERNS**: This library has unique patterns (PMR, cooperative actors, custom RTTI)
4. **BUILD AND TEST**: Always run build + tests after changes
5. **USE PMR ALLOCATORS**: Never use `new`/`delete` directly for actors

## Project Overview

actor-zeta is a C++17/20 **header-only** virtual actor model implementation with cooperative scheduling, custom memory management, and no dependencies on RTTI or exceptions.

**Minimum C++ standard: C++17**

## Quick Start Commands

### Using CLion (Recommended for Development)

CLion automatically handles CMake configuration. Just:

1. **Open project in CLion** - it will detect `CMakeLists.txt`
2. **Configure Conan dependencies:**
   ```bash
   # Run once in terminal (inside CLion or external):
   conan profile detect --force
   conan install . -of cmake-build-debug/conan -s build_type=Debug --build=missing
   ```
3. **Reload CMake** in CLion (`Tools → CMake → Reload CMake Project`)
4. **Set CMake options** in `Settings → Build → CMake`:
   - Build type: `Debug` or `Release`
   - CMake options:
     ```
     -DALLOW_EXAMPLES=ON
     -DALLOW_TESTS=ON
     -DRTTI_DISABLE=ON
     -DEXCEPTIONS_DISABLE=ON
     -DCMAKE_TOOLCHAIN_FILE=cmake-build-debug/conan/Debug/generators/conan_toolchain.cmake
     ```
5. **Build** using CLion's build button or `Ctrl+F9` / `Cmd+F9`
6. **Run tests** from `Run → Edit Configurations → Add CTest`
7. **Run examples** - CLion auto-creates run configurations from executables

**CLion Build Directory:** CLion uses `cmake-build-debug/` or `cmake-build-release/` by default.

### Command Line (Alternative)

```bash
# Full build cycle (development)
conan profile detect --force
conan install . -of build -s build_type=Debug --build=missing
cmake -B build -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_STANDARD=17 \
  -DALLOW_EXAMPLES=ON \
  -DALLOW_TESTS=ON \
  -DRTTI_DISABLE=ON \
  -DEXCEPTIONS_DISABLE=ON \
  -DCMAKE_TOOLCHAIN_FILE=./build/Debug/generators/conan_toolchain.cmake
cmake --build build

# Run all tests
cd build && ctest --output-on-failure

# Run specific test
./build/bin/tests_message
./build/bin/tests_behavior

# Run examples
./build/bin/dataflow
./build/bin/balancer
```

## File Structure Reference

```
actor-zeta/
├── header/
│   ├── actor-zeta.hpp              # Main public API entry point
│   ├── actor-zeta/
│   │   ├── core.hpp                # Core types (actor_abstract_t, address_t, message, scheduler_t)
│   │   ├── src.hpp                 # Header-only mode: include in ONE .cpp file
│   │   ├── spawn.hpp               # Actor allocation: spawn<T>(memory_resource, args...)
│   │   ├── make_message.hpp        # Message creation
│   │   ├── send.hpp                # Message sending
│   │   ├── base/                   # Actor base classes
│   │   │   ├── actor_abstract.hpp  # Abstract actor interface
│   │   │   ├── cooperative_actor.hpp # Cooperative actor implementation
│   │   │   ├── address.hpp         # Actor addressing
│   │   │   ├── behavior.hpp        # Message handlers
│   │   │   └── handler.hpp         # Handler registration
│   │   ├── mailbox/                # Message system
│   │   │   ├── message.hpp         # Core message type
│   │   │   ├── message_id.hpp      # Message identification
│   │   │   ├── priority.hpp        # Priority levels
│   │   │   └── *_priority_message.hpp
│   │   ├── scheduler/              # Cooperative scheduler
│   │   │   ├── scheduler.hpp       # Abstract scheduler interface
│   │   │   ├── sharing_scheduler.hpp
│   │   │   └── resumable.hpp       # Resumable execution
│   │   ├── detail/                 # Internal implementation
│   │   │   ├── pmr/                # Polymorphic memory resources
│   │   │   │   ├── memory_resource.hpp
│   │   │   │   └── polymorphic_allocator.hpp
│   │   │   ├── rtt.hpp             # Custom RTTI (no typeid)
│   │   │   ├── intrusive_ptr.hpp   # Reference counting
│   │   │   ├── unique_function.hpp # Move-only function wrapper
│   │   │   ├── type_list.hpp       # Metaprogramming
│   │   │   └── queue/              # Lock-free queues
│   │   └── impl/                   # Implementation files (.ipp)
├── test/                           # Test suite (Catch2)
│   ├── message/
│   ├── behavior/
│   ├── actor-id/
│   └── ...
├── examples/                       # Usage examples
│   ├── dataflow/
│   ├── balancer/
│   └── broadcast/
└── source/src.cpp                  # Library implementation (if not header-only)
```

## Architecture Deep Dive

### Actor System

The library implements **cooperative actors** with custom memory management:

- **basic_actor** - Alias for `cooperative_actor<Actor, traits, actor_type::classic>`
- **actor_abstract_t** - Abstract base interface for all actors
- **address_t** - Actor addressing and identification
- **behavior_t** - Message handler collection

**Actor Lifecycle:**
1. Define actor class inheriting from `basic_actor`
2. Define `dispatch_traits` with method pointers: `dispatch_traits<&MyActor::method1, &MyActor::method2>`
3. Implement `behavior(message*)` method to dispatch messages
4. Spawn with PMR: `auto actor = spawn<MyActor>(memory_resource, args...)`
5. Send messages: `send(actor, sender, &MyActor::method, arg1, arg2)`
6. Scheduler runs actor's message handlers cooperatively

### Message System (`header/actor-zeta/mailbox/`)

- **message** - Intrusive-pointer-based message with type erasure
- **message_id** - Unique message identification
- **Priority levels:**
  - `high_priority_message` - Processed first
  - `normal_priority_message` - Standard priority

Messages are **immutable** after creation. Use `make_message()` to create.

### Scheduler (`header/actor-zeta/scheduler/`)

Cooperative scheduling, not preemptive:
- **scheduler_t** - Abstract interface: `start()`, `stop()`, `schedule(resumable_t*)`
- **sharing_scheduler** - Shared thread pool implementation
- **resumable_t** - Interface for resumable execution contexts (actors)

Actors run until they yield control (no timeslicing).

### Memory Management (`header/actor-zeta/detail/pmr/`)

**CRITICAL:** This is a custom PMR implementation, not std::pmr.

- **memory_resource** - Abstract allocator base class
- **polymorphic_allocator** - Type-erased allocator
- **spawn()** - **ALWAYS use this for actor allocation:**
  ```cpp
  auto actor = spawn<MyActor>(memory_resource, constructor_args...);
  // Returns: unique_ptr<MyActor, pmr::deleter_t>
  ```

**Never use `new MyActor` directly** - breaks memory resource tracking.

### Type System (`header/actor-zeta/detail/`)

Since RTTI is disabled:
- **rtt.hpp** - Custom runtime type information (replaces `typeid`)
- **type_list** - Compile-time type lists for metaprogramming
- **type_traits** - Custom type trait utilities
- **unique_function** - Move-only function wrapper (like `std::function` but move-only)
- **intrusive_ptr** - Reference-counted smart pointer with custom ref counting

### Reference Counting

Actors use **intrusive reference counting**:
```cpp
template<class T, class Target, class Traits, class Type>
void intrusive_ptr_add_ref(cooperative_actor<Target, Traits, Type>* ptr);

template<class T, class Target, class Traits, class Type>
void intrusive_ptr_release(cooperative_actor<Target, Traits, Type>* ptr);
```

Use `intrusive_ptr<T>` for actors, not `shared_ptr`.

## Code Conventions

### 1. RTTI and Exceptions

**CRITICAL:** All code MUST work with `-fno-rtti -fno-exceptions`.

❌ **NEVER:**
```cpp
throw std::runtime_error("error");  // NO EXCEPTIONS
typeid(MyClass).name();             // NO RTTI
dynamic_cast<Derived*>(ptr);        // NO RTTI
```

✅ **INSTEAD:**
```cpp
// Use error codes or assertions
assert(condition && "error message");

// Use custom RTT system
#include <actor-zeta/detail/rtt.hpp>
// (see rtt.hpp for type identification)

// Use static polymorphism or intrusive_ptr with manual type tracking
```

### 2. Memory Management

✅ **CORRECT:**
```cpp
auto actor = spawn<MyActor>(memory_resource, arg1, arg2);
// Returns unique_ptr with custom deleter
```

❌ **WRONG:**
```cpp
auto actor = new MyActor(arg1, arg2);  // Breaks PMR tracking!
delete actor;
```

### 3. Message Sending

✅ **CORRECT:**
```cpp
// Method pointer dispatch (recommended)
send(target_actor, sender_address, &MyActor::handle_command, arg1, arg2);

// Or with message_id
send(target_actor, sender_address, msg_id<MyActor, &MyActor::handle_command>, arg1, arg2);
```

Messages are immutable. Handler signature:
```cpp
void MyActor::handle_command(const ArgType1& arg1, const ArgType2& arg2) {
    // ...
}
```

### 4. Actor Definition Pattern

```cpp
class MyActor final : public basic_actor<MyActor> {
public:
    void handle_command1(const std::string& arg);
    void handle_command2(int arg1, double arg2);

    using dispatch_traits = actor_zeta::dispatch_traits<
        &MyActor::handle_command1,
        &MyActor::handle_command2
    >;

    explicit MyActor(actor_zeta::pmr::memory_resource* ptr, constructor_args...)
        : basic_actor<MyActor>(ptr)
        , command1_(actor_zeta::make_behavior(resource(), this, &MyActor::handle_command1))
        , command2_(actor_zeta::make_behavior(resource(), this, &MyActor::handle_command2)) {
    }

    void behavior(actor_zeta::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<MyActor, &MyActor::handle_command1>) {
            command1_(msg);
        } else if (cmd == actor_zeta::msg_id<MyActor, &MyActor::handle_command2>) {
            command2_(msg);
        }
    }

    ~MyActor() override = default;

private:
    actor_zeta::behavior_t command1_;
    actor_zeta::behavior_t command2_;

    void handle_command1(const std::string& arg) {
        // Handler implementation
    }

    void handle_command2(int arg1, double arg2) {
        // Handler implementation
    }
};
```

### 5. Header-Only Mode

To use as header-only, in **exactly one** `.cpp` file:
```cpp
#include <actor-zeta/src.hpp>
```

This includes all implementations.

### 6. Clang-Specific Considerations

On Linux with Clang, CMake auto-detects and configures:
- `libc++` instead of `libstdc++` (if available)
- `compiler-rt` instead of `libgcc`
- Linker flags in `LINKFLAGS` variable

**If adding linker options:** Check `CMakeLists.txt` lines 123-181 for Clang-specific handling.

## Common Build Configurations

### Development (Debug, all features)
```bash
cmake -B build -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_STANDARD=17 \
  -DALLOW_EXAMPLES=ON \
  -DALLOW_TESTS=ON \
  -DENABLE_TESTS_MEASUREMENTS=ON \
  -DRTTI_DISABLE=ON \
  -DEXCEPTIONS_DISABLE=ON
```

### Release (optimized)
```bash
cmake -B build -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD=17 \
  -DRTTI_DISABLE=ON \
  -DEXCEPTIONS_DISABLE=ON
```

### CI Testing (multiple standards)
```bash
# Test with C++17, 20
for std in 17 20; do
  cmake -B build-cpp$std -DCMAKE_CXX_STANDARD=$std -DALLOW_TESTS=ON
  cmake --build build-cpp$std
  (cd build-cpp$std && ctest)
done
```

## CMake Options Reference

| Option | Default | Description |
|--------|---------|-------------|
| `ALLOW_EXAMPLES` | OFF | Build example applications |
| `ALLOW_TESTS` | OFF | Build test suite (Catch2) |
| `ENABLE_TESTS_MEASUREMENTS` | OFF | Enable performance measurements in tests |
| `ALLOW_BENCHMARK` | OFF | Build benchmark suite |
| `RTTI_DISABLE` | ON | Disable RTTI (`-fno-rtti`) |
| `EXCEPTIONS_DISABLE` | ON | Disable exceptions (`-fno-exceptions`) |
| `CMAKE_CXX_STANDARD` | 17 | C++ standard (17, 20) |

## Testing

Test suite organized by component in `test/`:

```bash
# Run all tests
cd build && ctest --output-on-failure

# Run specific test executable
./build/bin/tests_message
./build/bin/tests_behavior
./build/bin/tests_actor_id
./build/bin/tests_spawn

# Run with verbose output
./build/bin/tests_message --success
```

Tests use **Catch2 2.13.8** framework with auto-discovery via `catch_discover_tests()`.

## Examples

Located in `examples/`:
- **dataflow** - Dataflow processing pipeline with actor graph
- **balancer** - Load balancing across multiple worker actors
- **broadcast** - Broadcasting messages to multiple actors

Build with `-DALLOW_EXAMPLES=ON`, run from `build/bin/`.

## Dependencies

**Build:**
- CMake >= 3.15
- Conan 2.x (installs Catch2, Benchmark)
- C++ compiler: **GCC 9+, Clang 10+**, or MSVC (requires std::pmr support)

**Runtime:**
- None (header-only library)
- Requires Threads library (pthread on Unix)

## CLion-Specific Workflows

### Debugging in CLion

1. **Set breakpoints** in header files or test files
2. **Run test in debug mode** - Right-click on test executable → `Debug 'tests_message'`
3. **Debug specific example** - CLion auto-creates run configurations for executables in `bin/`

### Running Single Test

In CLion:
- Open test file (e.g., `test/message/main.cpp`)
- Click green arrow next to test case
- Or use CTest run configuration

From command line:
```bash
./cmake-build-debug/bin/tests_message --success  # Verbose output
./cmake-build-debug/bin/tests_message "[tag_name]"  # Run specific test tag
```

### Troubleshooting CLion Build

**Problem:** CLion can't find Conan dependencies

**Solution:**
```bash
# Re-run Conan install for CLion's build directory:
conan install . -of cmake-build-debug/conan -s build_type=Debug --build=missing

# Then in CLion: Tools → CMake → Reset Cache and Reload Project
```

**Problem:** "Cannot find conan_toolchain.cmake"

**Solution:** Update CMake options in CLion settings to point to correct toolchain file:
```
-DCMAKE_TOOLCHAIN_FILE=cmake-build-debug/conan/Debug/generators/conan_toolchain.cmake
```

### Using CLion with Different C++ Standards

Create multiple CMake profiles in CLion (`Settings → Build → CMake → +`):
- **Debug-C++11**: `-DCMAKE_CXX_STANDARD=11 -DALLOW_TESTS=ON`
- **Debug-C++17**: `-DCMAKE_CXX_STANDARD=17 -DALLOW_TESTS=ON -DALLOW_EXAMPLES=ON`
- **Release-C++20**: `-DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=20`

Switch between profiles using the dropdown in CLion's toolbar.

## When Making Changes

1. ✅ **Read the full file first** (not just the function you're changing)
2. ✅ **Follow existing patterns** (PMR allocation, no RTTI/exceptions, intrusive_ptr)
3. ✅ **Build after every change:** CLion auto-builds, or `cmake --build build`
4. ✅ **Run tests:** In CLion or `cd build && ctest --output-on-failure`
5. ✅ **Check examples still work** if you changed core headers

## Common Mistakes to Avoid

### Memory Management
❌ Using `new`/`delete` instead of `spawn()`
❌ Using `std::shared_ptr` for actors (use `intrusive_ptr` instead)
❌ Assuming `std::pmr` exists (this is a C++11 library with custom PMR)
❌ Using `std::memcpy` for non-trivial types (breaks `std::string`, etc.)
❌ Cross-arena migration for type-erased containers (RTT, message) - only same-arena supported
❌ Using alignment < `sizeof(void*)` with `posix_memalign` (adjust to minimum valid alignment)

### Standard Library Replacements (MUST NOT USE)
❌ **`std::function`** → Use `actor_zeta::detail::unique_function` instead (move-only, PMR-aware)
❌ **`std::shared_ptr`** → Use `actor_zeta::detail::intrusive_ptr` instead (intrusive ref counting)
❌ **`std::optional`** → Use custom optional or raw pointers with null checks (C++11 compatibility)

### RTTI and Exceptions (DISABLED)
❌ Using `throw` or `try/catch` (exceptions disabled with `-fno-exceptions`)
❌ Using `typeid` or `dynamic_cast` (RTTI disabled with `-fno-rtti`)
❌ Any code relying on exception handling or runtime type information

### Code Reading
❌ Reading only part of a header file (miss template specializations, friend functions)
❌ Skipping implementation files (.ipp) when modifying headers

### API Usage (IMPORTANT)
✅ **`make_behavior()` and `message*` are LEGITIMATE APIs** - do not "fix" them
✅ **`behavior(message*)` method signature is CORRECT** - used by base class `cooperative_actor`
✅ **`behavior_t` with `make_behavior()`** - provides compile-time message unpacking

**These are NOT old/deprecated APIs:**
- `make_behavior(resource, this, &Class::method)` - creates behavior with automatic argument unpacking
- `behavior_t` - type-erased behavior object that unpacks message arguments
- `void behavior(message*)` - virtual method called by base class, must use pointer
- Direct `message*` usage in low-level code (tests, custom actors)

**Current API (all code should use this):**
- Define `dispatch_traits<&Actor::method...>` with method pointers
- Implement `behavior(message*)` to dispatch based on `msg_id<Actor, &Actor::method>`
- Create `behavior_t` members using `make_behavior(resource, this, &Actor::method)`
- Send messages using `send(actor, sender, &Actor::method, args...)`

## Recent Important Fixes (Session Memory)

### RTT (Runtime Type Container) - Same-Arena Only Migration
- **Issue:** Cross-arena migration used `std::memcpy` which broke non-trivial types like `std::string`
- **Solution:** RTT allocator-extended move constructor now **only supports same-arena migration**
- **Implementation:** Added `assert(resource == other.memory_resource_)` to prevent cross-arena usage
- **Reason:** Type-erased containers can't properly copy non-trivial types without runtime type info
- **Files affected:**
  - `header/actor-zeta/detail/rtt.hpp` - allocator-extended constructor and move assignment
  - `test/message/main.cpp` - removed cross-arena migration tests

### Message Cross-Arena Migration
- **Behavior:** Messages inherit RTT's same-arena-only limitation
- **Move semantics:** Safe within same arena (pointer stealing), prohibited across arenas

### Test Memory Resource - `posix_memalign` Requirements
- **Issue:** `posix_memalign` requires alignment ≥ `sizeof(void*)` and power of 2
- **Solution:** Adjust alignment to minimum `sizeof(void*)` if smaller
- **Files affected:** `test/unique_function/main.cpp`

### Conan 2.x + CMake Integration
- **Issue:** `cmake_layout` in `conanfile.txt` created nested directory structures
- **Solution:** Removed `[layout]` section from `conanfile.txt`
- **Toolchain path:** `build/${BUILD_TYPE}/conan_toolchain.cmake` (NOT in `generators/` subdirectory)
- **Files affected:**
  - `conanfile.txt` - removed cmake_layout
  - `.github/workflows/ubuntu_clang.yaml` - fixed toolchain path
  - `.github/workflows/ubuntu_gcc.yaml` - fixed toolchain path
  - `.github/workflows/macos.yml` - fixed toolchain path

### Shutdown Race Condition Fix (lifo_inbox blocked state)
- **Issue:** Assertion failure `assert(!blocked())` in `lifo_inbox.hpp:64` during scheduler shutdown
- **Root cause:** Race condition when `resume_core_()` calls `inbox().empty()` while inbox is in `blocked` state
  - Thread 1 (actor): Processes messages, inbox becomes empty, tries to call `empty()` to check if should block
  - Thread 2 (balancer): Calls `enqueue()` + `schedule()` manually, may set inbox to blocked state
  - Thread 1: Resumes but inbox is now blocked, calling `empty()` triggers assertion
- **Solution:** Check `inbox().blocked()` BEFORE calling `inbox().empty()` in `resume_core_()`
- **Implementation:**
  ```cpp
  // Check if inbox is blocked first to avoid assertion in empty()
  if (inbox().blocked()) {
      // Inbox is blocked, try to resume (another thread may have enqueued)
      return scheduler::resume_result::resume;
  }

  if (inbox().empty()) {
      return inbox().try_block()
             ? scheduler::resume_result::awaiting
             : scheduler::resume_result::resume;
  }
  ```
- **Why this works:**
  - `blocked()` has no preconditions, safe to call anytime
  - `empty()` requires `!blocked()` precondition per API contract
  - If blocked, we should resume anyway to check for new messages
- **Reproduces in:** Balancer pattern with manual `enqueue()` + `schedule()` calls
- **Files affected:**
  - `header/actor-zeta/base/detail/cooperative_actor_classic.hpp:91-95` - added blocked() check
  - `test/shutdown/main.cpp` - new test case reproduces the bug with balancer pattern

### CI/CD - Compiler C++20 Support
- **Issue:** Old GCC/Clang versions don't support C++20
- **Solution:** Added matrix exclusions for C++20 builds with older compilers
- **Excluded from C++20:**
  - GCC 7, 8, 9, 10
  - Clang 9, 10
- **Files affected:** Both Ubuntu workflow files

### Manual Scheduling - Removed Auto-Scheduling from Actors
- **Change:** Actors no longer self-schedule when messages are enqueued
- **Reason:** Transitioning to fully manual scheduling for better control (load balancing, custom schedulers)
- **Implementation:** Removed `intrusive_ptr_add_ref(this)` from `enqueue_impl()` when `unblocked_reader` occurs
- **Behavior:**
  - **Before:** Actor automatically scheduled itself when transitioning from blocked to unblocked
  - **After:** Caller must explicitly call `scheduler->schedule(actor)` after `enqueue()`
- **Usage pattern (balancer example):**
  ```cpp
  bool enqueue_impl(message_ptr msg) override {
      auto& worker = workers_[cursor_++ % workers_.size()];
      worker->enqueue(std::move(msg));        // Enqueue message
      scheduler_->schedule(worker.get());      // Manual schedule - caller responsibility
      return true;
  }
  ```
- **Files affected:**
  - `header/actor-zeta/base/detail/cooperative_actor_classic.hpp:56-59` - removed auto-scheduling
  - `test/shutdown/main.cpp` - balancer_actor example demonstrates manual scheduling pattern

### Resume Info - Extended Actor Resume Result
- **Feature:** `resume()` now returns `resume_info` struct with execution statistics
- **Motivation:** Enable graceful shutdown monitoring and scheduler observability
- **Implementation:**
  ```cpp
  struct resume_info {
      resume_result result;           // Execution status (resume/awaiting/done/shutdown)
      size_t messages_processed;      // Number of messages processed in this resume call

      // Implicit conversion to resume_result for backward compatibility
      operator resume_result() const noexcept { return result; }
  };
  ```
- **Key design decisions:**
  - **Implicit conversion:** Scheduler code doesn't need changes, can still switch on `resume_info` as `resume_result`
  - **Messages processed:** Actual count of messages handled (not remaining - can't be reliably counted in lock-free inbox)
  - **Simple API:** No conditions, modes, or extra fields - just result + count
- **Use cases:**
  - **Graceful shutdown:** Monitor `messages_processed` to know when actor has drained all messages
  - **Scheduler telemetry:** Track actor execution statistics (throughput, fairness)
  - **Load balancing:** Make scheduling decisions based on processing counts
- **Files affected:**
  - `header/actor-zeta/scheduler/resumable.hpp:21-40` - added `resume_info` struct
  - `header/actor-zeta/scheduler/resumable.hpp:45` - changed virtual `resume()` signature to return `resume_info`
  - `header/actor-zeta/base/detail/cooperative_actor_classic.hpp:27-34` - updated `resume()` methods
  - `header/actor-zeta/base/detail/cooperative_actor_classic.hpp:86-150` - `resume_core_()` now returns `resume_info` with message count
  - `header/actor-zeta/impl/scheduler/sharing_scheduler.ipp:142-148` - shutdown_helper returns `resume_info`
- **Backward compatibility:** Existing scheduler code works unchanged via implicit conversion

---

## Promise/Future System (Async Request-Response)

### Overview

Actor-zeta supports **async request-response** via nested `promise<T>` and `unique_future<T>` classes in `cooperative_actor`. This enables fire-and-forget OR request-response patterns.

### Basic Usage

```cpp
// Fire-and-forget (no return value)
send(target_actor, sender, &TargetActor::handle_command, arg1, arg2);

// Request-response (returns future)
auto future = send(target_actor, sender, &TargetActor::compute, arg1);
int result = future.get();  // Blocks until ready
```

### Promise/Future Patterns

**Pattern 1: Simple Request-Response**
```cpp
// Sender
auto future = send(worker, address(), &Worker::compute, 42);
int result = future.get();  // Wait for result
std::cout << "Result: " << result << "\n";

// Worker
class Worker : public basic_actor<Worker> {
    void compute(int x) {
        // Do expensive computation
        int result = x * 2;

        // Send response (future will receive it)
        // Response happens automatically via promise in message
    }
};
```

**Pattern 2: Multiple Futures (Parallel Requests)**
```cpp
// Send multiple requests
auto future1 = send(worker1, address(), &Worker::compute, 10);
auto future2 = send(worker2, address(), &Worker::compute, 20);
auto future3 = send(worker3, address(), &Worker::compute, 30);

// Wait for all results
int result1 = future1.get();
int result2 = future2.get();
int result3 = future3.get();
```

**Pattern 3: Fire-and-Forget (Orphaned Futures)**
```cpp
// Don't care about result - drop future immediately
{
    auto future = send(logger, address(), &Logger::log, "message");
    // future destroyed here - becomes "orphaned"
    // Message still processed, result ignored
}
```

**Pattern 4: Timeout (with is_ready())**
```cpp
auto future = send(worker, address(), &Worker::slow_task);

// Poll with timeout
auto start = std::chrono::steady_clock::now();
while (!future.is_ready()) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > std::chrono::seconds(5)) {
        future.cancel();  // Request cancellation
        throw timeout_error();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
int result = future.get();
```

### Best Practices

✅ **DO:**
- Use `future.is_ready()` for non-blocking checks
- Call `future.get()` only when ready (avoid busy-wait)
- Ensure actor outlives all futures (or wait for all futures before destruction)
- Use fire-and-forget for notifications/logging (no future storage)

❌ **DON'T:**
- Call `get()` multiple times in tight loop without `is_ready()` check
- Destroy actor while futures are pending (assertion failure in debug)
- Store futures indefinitely (memory leak of messages)
- Use exceptions for error handling (exceptions disabled)

### Performance Notes

**Current implementation uses exponential backoff:**
- Start: 1 microsecond sleep
- Growth: Doubles each iteration (1μs → 2μs → 4μs → ... → 1ms cap)
- CPU usage: ~0.1-1% while waiting
- Latency: Variable (1μs to 1ms wake-up time)

**Optimization options** (not yet implemented):
- **C++20 atomic wait/notify:** Zero memory overhead, zero CPU usage
- **Condition variable:** Zero CPU, +16 bytes per message with future
- **Hybrid approach:** Fast spin (100μs) + blocking wait

See `PROMISE_FUTURE_IMPLEMENTATION.md` for detailed architecture.

---

### Promise/Future Code Quality Improvements (2025-01)

Recent fixes to promise/future implementation:

#### ✅ Bug Fix #1: Memory Leak in `release_message_ref()`
- **Issue:** Future deleted message even if actor hadn't processed it yet
- **Solution:** Check `error() != pending` before deleting
- **Impact:** Eliminated memory leaks for all futures
- **File:** `cooperative_actor_classic.hpp:278-299`

#### ✅ Bug Fix #2: Wrong PMR Resource in `promise::set_value()`
- **Issue:** Used `get_default_resource()` instead of message's resource
- **Solution:** Added `rtt::memory_resource()` getter, use `slot_->body().memory_resource()`
- **Impact:** Correct PMR resource tracking, prevents memory corruption
- **Files:**
  - `detail/rtt.hpp` - added `memory_resource()` method
  - `cooperative_actor_classic.hpp:77-91` - fixed resource usage

#### ✅ Improvement #3: Exponential Backoff in `get()`
- **Before:** Busy-wait with `std::this_thread::yield()` (~100% CPU)
- **After:** Exponential backoff (1μs → 1ms cap) (~0.1-1% CPU)
- **Impact:** 99% reduction in CPU usage while waiting
- **File:** `cooperative_actor_classic.hpp:226-251, 360-383`

#### ✅ Improvement #4: Thread Safety in Destructor
- **Issue:** `pending_futures_count_` used `memory_order_relaxed` in destructor
- **Solution:** Use `memory_order_acquire` to synchronize with future destructors
- **Impact:** Prevents race conditions during shutdown
- **File:** `cooperative_actor_classic.hpp:571`

#### ✅ Improvement #5: Orphaned Check in `promise::set_error()`
- **Added:** Assertion to catch setting error on orphaned promises
- **Consistency:** Both `set_value()` and `set_error()` now check orphaned state
- **File:** `cooperative_actor_classic.hpp:94-98, 151-155`

#### ✅ Code Quality: Added `[[nodiscard]]` Attributes
- **Methods:** `is_cancelled()`, `is_valid()`, `is_ready()`, `error()`, `valid()`
- **Impact:** Compiler warns if result is ignored
- **File:** `cooperative_actor_classic.hpp` - all promise/future query methods

---

### Cooperative Actor Improvements (2025-01)

#### ✅ Bug Fix: `current_msg_guard` Destructor
- **Issue:** Saved `prev` pointer but didn't restore it (set to `nullptr` instead)
- **Solution:** Restore previous message: `self->current_message_ = prev;`
- **Impact:** Nested message processing now works correctly
- **File:** `cooperative_actor_classic.hpp:592`

#### ✅ Code Modernization
- **Deleted constructors:** Changed from implicit declaration to `= delete`
- **Benefit:** Better compiler error messages, explicit intent
- **File:** `cooperative_actor_classic.hpp:597-598`

#### ✅ Edge Case: `max_throughput == 0` Validation
- **Added:** `assert(max_throughput > 0)` in `resume()`
- **Impact:** Catches invalid scheduler calls
- **File:** `cooperative_actor_classic.hpp:481`

---

### Known Limitations & Future Work

#### Promise/Future Limitations

1. **No timeout support:** `get()` blocks indefinitely
   - Workaround: Use `is_ready()` polling with timer
   - Future: Add `get(timeout)` overload

2. **No exception support:** Errors via `slot_error_code` enum only
   - Design constraint: `-fno-exceptions` build requirement
   - Alternative: Use `std::optional<T>` or `std::variant<T, error_code>` in future

3. **Single-threaded get():** No concurrent `get()` calls on same future
   - Future is move-only, prevents accidental sharing
   - Multiple calls from same thread work (non-destructive get)

4. **Actor must outlive futures:** No automatic lifetime management
   - Debug: Assertion in `~cooperative_actor()` catches this
   - Release: Undefined behavior if violated
   - Best practice: Wait for all futures before actor destruction

#### Performance Optimization Options

**Not yet implemented** (see analysis in review session):

1. **Atomic wait/notify (C++20):**
   ```cpp
   // Zero overhead, zero CPU
   slot_->error_.wait(slot_error_code::pending);
   slot_->error_.notify_one();
   ```
   - Requires: C++20 standard
   - Benefit: Zero memory overhead, zero CPU usage
   - Trade-off: Requires compiler support

2. **Condition variable:**
   ```cpp
   // Lazy init on set_has_future()
   std::unique_ptr<std::condition_variable> cv_;
   std::unique_ptr<std::mutex> cv_mutex_;
   ```
   - Benefit: Zero CPU usage while waiting
   - Trade-off: +16 bytes per message with future, mutex overhead

3. **Hybrid approach:**
   - Fast spin (100μs) for quick operations
   - Blocking wait for slow operations
   - Best of both worlds

**Recommendation:** Current exponential backoff is sufficient for most use cases. Consider alternatives only if:
- Profiling shows CPU usage problem
- Futures regularly wait >10ms
- Large number of concurrent futures (>100)

---

### Message Lifetime & Ownership Rules

Understanding message ownership is critical for correct promise/future usage:

**Ownership States:**

1. **Mailbox owns (normal):**
   ```cpp
   send(actor, ...);  // Message in mailbox, unique_ptr owns it
   // Actor processes → message deleted after handler returns
   ```

2. **Future owns (after actor processes):**
   ```cpp
   auto future = send(actor, ...);
   // Actor processes → calls msg.release() → future owns message
   // Future destructor deletes message
   ```

3. **Orphaned (future destroyed early):**
   ```cpp
   {
       auto future = send(actor, ...);
   }  // Future destroyed → message marked orphaned
   // Actor still processes, mailbox deletes message
   ```

**Conditional Delete Logic:**
```cpp
// In unique_future::release_message_ref()
if (slot_->error() != slot_error_code::pending) {
    // Actor processed → future owns → DELETE
    mailbox::message_ptr auto_delete(slot_);
} else {
    // Actor hasn't processed → mailbox owns → DON'T delete
}
```

**Key Insight:** Only delete if `error != pending` because:
- `pending` → actor hasn't called `set_error()` → mailbox still owns
- `ok/error` → actor called `set_error()` AND `msg.release()` → future owns

---

### Debugging Promise/Future Issues

**Common Issues & Solutions:**

1. **Assertion: "Actor destroyed with pending futures"**
   ```
   Cause: Actor destructed while futures are still alive
   Solution: Call future.get() or let futures go out of scope before actor destruction
   ```

2. **Assertion: "Setting value on orphaned promise"**
   ```
   Cause: Future was destroyed, then actor tried to set result
   Solution: Check promise.is_valid() before set_value()
   ```

3. **Hang in `future.get()`:**
   ```
   Cause: Actor never calls set_value() or set_error()
   Debug: Check actor's behavior() dispatches message correctly
   ```

4. **Memory leak:**
   ```
   Cause: Storing futures indefinitely without calling get()
   Solution: Always call get() or let future destruct
   ```

**Debug Build Features:**
- Assertions catch most ownership violations
- Pending futures count checked in actor destructor
- Orphaned/cancelled checks before set_value/set_error

**Release Build:**
- Assertions disabled → undefined behavior if contracts violated
- Always test with assertions enabled first!