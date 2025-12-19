# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Table of Contents

- [Critical: Read Full Files](#critical-read-full-files-before-making-changes)
- [Core Principles](#core-principles)
- [Project Overview](#project-overview)
- [Quick Start](#quick-start-commands)
- [File Structure](#file-structure-reference)
- [Architecture](#architecture-deep-dive)
- [Code Conventions](#code-conventions)
- [Build Configurations](#common-build-configurations)
- [CMake Options](#cmake-options-reference)
- [Testing](#testing)
- [Examples](#examples)
- [Dependencies](#dependencies)
- [CLion Workflows](#clion-specific-workflows)
- [When Making Changes](#when-making-changes)
- [Common Mistakes](#common-mistakes-to-avoid)
- [Recent Changes](#recent-changes)
- [Promise/Future System](#promisefuture-system)

---

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

actor-zeta is a C++20 **header-only** virtual actor model implementation with cooperative scheduling, custom memory management, and no dependencies on RTTI or exceptions.

**Minimum C++ standard: C++20**

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
./build/bin/balancer
./build/bin/broadcast
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
│   │   │   ├── future.hpp          # Promise/Future with coroutine support
│   │   │   ├── future_state.hpp    # Future state management
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
│   ├── coroutines/
│   └── ...
├── examples/                       # Usage examples
│   ├── balancer/                   # Load balancing pattern
│   ├── broadcast/                  # Message broadcasting
│   ├── supervisor/                 # Supervisor pattern
│   └── coroutine/                  # Coroutine examples
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

### Message System

- **message** - Intrusive-pointer-based message with type erasure
- **message_id** - Unique message identification
- **Priority levels:**
  - `high_priority_message` - Processed first
  - `normal_priority_message` - Standard priority

Messages are **immutable** after creation. Use `make_message()` to create.

**Key concept:** Messages now created in **receiver's** memory resource (not sender's) to avoid cross-arena migration issues.

### Scheduler

Cooperative scheduling, not preemptive:
- **scheduler_t** - Abstract interface: `start()`, `stop()`, `schedule(resumable_t*)`
- **sharing_scheduler** - Shared thread pool implementation
- **resumable_t** - Interface for resumable execution contexts (actors)

Actors run until they yield control (no timeslicing).

### Memory Management

Uses `std::pmr::memory_resource` for memory allocation.

- **spawn()** - **ALWAYS use this for actor allocation:**
  ```cpp
  auto actor = spawn<MyActor>(std::pmr::get_default_resource(), constructor_args...);
  // Returns: unique_ptr<MyActor, actor_zeta::pmr::deleter_t>
  ```

**Never use `new MyActor` directly** - breaks memory resource tracking.

### Type System

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

    void behavior(actor_zeta::mailbox::message* msg) {
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

**If adding linker options:** Check `CMakeLists.txt` for Clang-specific handling.

## Common Build Configurations

### Development (Debug, all features)
```bash
cmake -B build -GNinja \
  -DCMAKE_BUILD_TYPE=Debug \
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
  -DRTTI_DISABLE=ON \
  -DEXCEPTIONS_DISABLE=ON
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
| `CMAKE_CXX_STANDARD` | 20 | C++ standard (C++20 only) |

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
- **balancer** - Load balancing across multiple worker actors
- **broadcast** - Broadcasting messages to multiple actors
- **supervisor** - Supervisor pattern for actor management
- **coroutine** - Mixed sync/async coroutine patterns

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

### Using CLion with Different Build Types

Create multiple CMake profiles in CLion (`Settings → Build → CMake → +`):
- **Debug**: `-DALLOW_TESTS=ON -DALLOW_EXAMPLES=ON`
- **Release**: `-DCMAKE_BUILD_TYPE=Release`

Switch between profiles using the dropdown in CLion's toolbar.

## When Making Changes

1. ✅ **Read the full file first** (not just the function you're changing)
2. ✅ **Follow existing patterns** (PMR allocation, no RTTI/exceptions, intrusive_ptr)
3. ✅ **Build after every change:** CLion auto-builds, or `cmake --build build`
4. ✅ **Run tests:** In CLion or `cd build && ctest --output-on-failure`
5. ✅ **Check examples still work** if you changed core headers

## Common Mistakes to Avoid

### Memory Management
- ❌ Using `new`/`delete` instead of `spawn()`
- ❌ Using `std::shared_ptr` for actors (use `intrusive_ptr` instead)
- ❌ Using `std::memcpy` for non-trivial types (breaks `std::string`, etc.)
- ❌ Cross-arena migration for type-erased containers (RTT, message) - only same-arena supported
- ❌ Using alignment < `sizeof(void*)` with `posix_memalign`

### Standard Library Replacements
- ❌ `std::function` → Use `actor_zeta::detail::unique_function` instead
- ❌ `std::shared_ptr` → Use `actor_zeta::detail::intrusive_ptr` instead
- ❌ `std::optional` → Use custom optional or raw pointers with null checks (C++11 compatibility)

### RTTI and Exceptions
- ❌ Using `throw` or `try/catch` (exceptions disabled)
- ❌ Using `typeid` or `dynamic_cast` (RTTI disabled)

### Code Reading
- ❌ Reading only part of a header file (miss template specializations)
- ❌ Skipping implementation files (.ipp) when modifying headers

### API Usage
✅ **These are LEGITIMATE APIs** - do not "fix" them:
- `make_behavior(resource, this, &Class::method)` - creates behavior with automatic argument unpacking
- `behavior_t` - type-erased behavior object
- `void behavior(mailbox::message*)` - virtual method called by base class
- Direct `message*` usage in low-level code

**Current API (all code should use this):**
- Define `dispatch_traits<&Actor::method...>` with method pointers
- Implement `void behavior(mailbox::message*)` to dispatch based on `msg_id<Actor, &Actor::method>`
- Create `behavior_t` members using `make_behavior(resource, this, &Actor::method)`
- Send messages using `send(actor, sender, &Actor::method, args...)`

## Recent Changes

**For detailed change history, see [`CHANGELOG.md`](CHANGELOG.md).**

### Key Recent Improvements (2025-01)

**Actor State Management:**
- Unified state management with single `atomic<actor_state>` (replaced 3 atomics)
- Atomic state transitions prevent invalid states

**Message System:**
- Messages now created in **receiver's** memory resource (eliminates cross-arena issues)
- API change: `enqueue_impl(sender, cmd, args...)` instead of `enqueue_impl(message_ptr)`
- For library users: No changes needed
- For custom actors/balancers: Update `enqueue_impl()` signature if overridden

**Scheduler:**
- Manual scheduling pattern (actors no longer self-schedule)
- `resume()` returns `resume_info` with execution statistics
- Shutdown race condition fixed

**Promise/Future:**
- Exponential backoff in `get()` (99% CPU reduction)
- Multiple bug fixes (memory leaks, PMR resource usage, thread safety)

**Build System:**
- Conan 2.x integration fixed
- CI matrix updated for C++20 compiler support

**See [`CHANGELOG.md`](CHANGELOG.md) for detailed descriptions and migration guides.**

## Promise/Future System

**For complete guide with patterns and examples, see [`PROMISE_FUTURE_GUIDE.md`](PROMISE_FUTURE_GUIDE.md).**

### Quick Overview

Actor-zeta supports **async request-response** via `promise<T>` and `unique_future<T>`:

**Fire-and-forget:**
```cpp
send(target_actor, sender, &TargetActor::handle_command, arg1, arg2);
```

**Request-response:**
```cpp
auto future = send(target_actor, sender, &TargetActor::compute, arg1);
int result = std::move(future).get();  // Blocks until ready
```

**Handler returns future (coroutine):**
```cpp
class Worker : public basic_actor<Worker> {
    unique_future<int> compute(int x) {
        int result = x * 2;
        co_return result;  // Use co_return for coroutines
    }
};
```

### Key Patterns

**1. Multiple Futures (Parallel Requests)**
```cpp
std::vector<unique_future<int>> futures;
futures.reserve(workers.size());  // CRITICAL: Reserve to avoid reallocation!

for (auto& worker : workers) {
    futures.push_back(send(worker.get(), address(), &Worker::compute, data));
}

// Wait for all results
for (auto& future : futures) {
    int result = std::move(future).get();
    process(result);
}
```

**2. Fire-and-Forget (Orphaned Futures)**
```cpp
{
    auto future = send(logger, address(), &Logger::log, "message");
}  // Future destroyed - message still processed, result ignored
```

**3. Timeout with Polling**
```cpp
auto future = send(worker, address(), &Worker::slow_task, data);

auto start = std::chrono::steady_clock::now();
while (!future.available()) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
        future.cancel();  // Best-effort cancellation
        // Handle timeout - no exceptions
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

int result = std::move(future).get();
```

### Coroutine Patterns

**4. Typed Coroutines with co_await**
```cpp
class MyActor : public basic_actor<MyActor> {
    // ✅ Typed coroutines support co_await
    unique_future<int> compute_async(int x) {
        auto future = send(worker_, address(), &Worker::process, x);
        int result = co_await std::move(future);  // OK!
        co_return result + 10;
    }
};
```

**5. Void Coroutines**
```cpp
// ✅ unique_future<void> coroutines fully support co_await:
unique_future<void> async_void_method() {
    co_await send(other_actor, address(), &OtherActor::do_work);  // OK!
    co_return;
}

// ✅ Simple void coroutine (no co_await) - also OK:
unique_future<void> simple_void() {
    co_return;  // OK
}
```

**6. Storing Pending Coroutines (CRITICAL)**
```cpp
class MyActor : public basic_actor<MyActor> {
    void behavior(mailbox::message* msg) {
        switch (msg->command()) {
            case msg_id<MyActor, &MyActor::async_method>: {
                // CRITICAL: Store pending coroutine!
                // If destroyed immediately, coroutine is destroyed → refcount underflow!
                auto future = dispatch(this, &MyActor::async_method, msg);
                if (!future.available()) {
                    pending_.push_back(std::move(future));
                }
                break;
            }
        }
    }

    // Poll and clean up completed coroutines
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
    std::vector<unique_future<int>> pending_;
};
```

### Best Practices

✅ **DO:**
- Use `available()` for non-blocking checks
- Reserve vector capacity before adding futures
- Ensure actor outlives all futures
- Use fire-and-forget for notifications/logging
- Move futures when calling `get()`
- **Store pending coroutines** in behavior() to prevent premature destruction
- Use **typed coroutines** (`unique_future<T>`) when you need `co_await`

❌ **DON'T:**
- Call `get()` in tight loop without `available()` check
- Destroy actor while futures are pending
- Store futures indefinitely
- Use exceptions for error handling

### Performance

Current implementation uses **exponential backoff** in `get()`:
- Start: 1 microsecond sleep
- Growth: Doubles each iteration (1μs → 2μs → 4μs → ... → 1ms cap)
- CPU usage: ~0.1-1% while waiting

### Complete Example

```cpp
#include <actor-zeta.hpp>
#include <iostream>

class Worker : public actor_zeta::basic_actor<Worker> {
public:
    // Handler as coroutine
    actor_zeta::unique_future<int> compute(int x) {
        co_return x * x;  // Use co_return for coroutines
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&Worker::compute>;

    explicit Worker(std::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<Worker>(ptr) {}

    void behavior(actor_zeta::mailbox::message* msg) {
        if (msg->command() == actor_zeta::msg_id<Worker, &Worker::compute>) {
            actor_zeta::dispatch(this, &Worker::compute, msg);
        }
    }
};

int main() {
    auto* resource = std::pmr::get_default_resource();
    auto worker = actor_zeta::spawn<Worker>(resource);

    auto future = actor_zeta::send(
        worker.get(),
        actor_zeta::address_t::empty_address(),
        &Worker::compute,
        42
    );

    // Process message
    worker->resume(100);

    int result = std::move(future).get();
    std::cout << "Result: " << result << "\n";  // Output: Result: 1764

    return 0;
}
```

**See [`PROMISE_FUTURE_GUIDE.md`](PROMISE_FUTURE_GUIDE.md) for:**
- Detailed patterns and examples
- Message lifetime & ownership rules
- Debugging common issues
- Known limitations
- Performance optimization options

---

## Additional Resources

- **[CHANGELOG.md](CHANGELOG.md)** - Detailed change history and migration guides
- **[PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md)** - Complete promise/future documentation
- **[MESSAGE_CREATION_REFACTORING.md](MESSAGE_CREATION_REFACTORING.md)** - Message creation refactoring notes
- **Examples:** `examples/` directory for working code samples
- **Tests:** `test/` directory for usage patterns and edge cases