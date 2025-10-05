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

actor-zeta is a C++11/14/17 **header-only** virtual actor model implementation with cooperative scheduling, custom memory management, and no dependencies on RTTI or exceptions.

## Quick Start Commands

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
2. Register handlers in constructor: `add_handler("command", &MyActor::method)`
3. Spawn with PMR: `auto actor = spawn<MyActor>(memory_resource, args...)`
4. Send messages: `send(actor, make_message("command", arg1, arg2))`
5. Scheduler runs actor's message handlers cooperatively

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

**CRITICAL:** This is a custom PMR implementation, not std::pmr (C++11 compatibility).

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
send(target_actor, make_message("command", arg1, arg2));
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
    explicit MyActor(/* supervisor reference */, constructor_args...)
        : basic_actor(/* ... */, "actor_name") {

        add_handler("command1", &MyActor::handle_command1);
        add_handler("command2", &MyActor::handle_command2);
    }

    ~MyActor() override = default;

private:
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
  -DCMAKE_CXX_STANDARD=11 \
  -DRTTI_DISABLE=ON \
  -DEXCEPTIONS_DISABLE=ON
```

### CI Testing (multiple standards)
```bash
# Test with C++11, 14, 17, 20
for std in 11 14 17 20; do
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
| `CMAKE_CXX_STANDARD` | 11 | C++ standard (11, 14, 17, 20) |

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
- C++ compiler: GCC 7-12, Clang 9-16, or MSVC

**Runtime:**
- None (header-only library)
- Requires Threads library (pthread on Unix)

## When Making Changes

1. ✅ **Read the full file first** (not just the function you're changing)
2. ✅ **Follow existing patterns** (PMR allocation, no RTTI/exceptions, intrusive_ptr)
3. ✅ **Build after every change:** `cmake --build build`
4. ✅ **Run tests:** `cd build && ctest --output-on-failure`
5. ✅ **Check examples still work** if you changed core headers

## Common Mistakes to Avoid

❌ Using `new`/`delete` instead of `spawn()`
❌ Using `std::function` instead of `unique_function` (copies may not work)
❌ Using `throw` or `try/catch` (exceptions disabled)
❌ Using `typeid` or `dynamic_cast` (RTTI disabled)
❌ Adding `std::shared_ptr` for actors (use `intrusive_ptr`)
❌ Reading only part of a header file (miss template specializations, friend functions)
❌ Assuming `std::pmr` exists (this is a C++11 library with custom PMR)