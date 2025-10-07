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
- C++ compiler: GCC 7-12, Clang 9-16, or MSVC

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

**When to use each approach:**
- **High-level actors (`basic_actor<T>`)**: Use `add_handler()` in constructor
- **Low-level/test code**: Can use `make_behavior()` + `behavior(message*)` for fine control

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