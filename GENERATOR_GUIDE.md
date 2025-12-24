# Generator Guide

This guide covers the `generator<T>` coroutine type in actor-zeta for streaming data between actors.

## Table of Contents

- [Overview](#overview)
- [Quick Start](#quick-start)
- [Creating Generators](#creating-generators)
- [Using send() API](#using-send-api)
- [Generator Lifecycle](#generator-lifecycle)
- [Cancellation and Detach](#cancellation-and-detach)
- [Memory Management](#memory-management)
- [Best Practices](#best-practices)
- [Limitations](#limitations)

---

## Overview

`generator<T>` enables actor methods to yield multiple values over time, supporting efficient streaming patterns without buffering all data in memory.

**Key features:**
- **Move-only** - generators cannot be copied
- **Lazy evaluation** - producer runs on-demand
- **PMR allocation** - uses actor's memory resource
- **Integrated with send()** - works seamlessly with actor messaging

**Use cases:**
- Streaming large datasets
- Event sequences
- Pagination results
- Real-time data feeds

---

## Quick Start

### Define a generator method

```cpp
class MyActor final : public basic_actor<MyActor> {
public:
    // Generator method - yields integers
    generator<int> stream_numbers(int count) {
        for (int i = 0; i < count; ++i) {
            co_yield i;  // Yield value and suspend
        }
        // Implicit co_return at end
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &MyActor::stream_numbers
    >;

    void behavior(mailbox::message* msg) {
        if (msg->command() == msg_id<MyActor, &MyActor::stream_numbers>) {
            dispatch(this, &MyActor::stream_numbers, msg);
        }
    }
};
```

### Use via send() API

```cpp
auto actor = spawn<MyActor>(resource);

// Get generator handle via send()
auto gen = send(actor.get(), sender, &MyActor::stream_numbers, 10);

// Process the message (starts generator)
actor->resume(1);

// Generator is now producing values
```

---

## Creating Generators

### Generator can ONLY be created as actor method

Generators require a memory resource for allocation. This resource is extracted from the actor's `this` pointer:

```cpp
// CORRECT - actor method
generator<int> MyActor::stream_data() {
    co_yield 42;
}

// WRONG - free function (no memory resource!)
// generator<int> free_stream() { co_yield 42; }  // Compile error
```

### Yielding values

Use `co_yield` to produce values:

```cpp
generator<std::string> stream_messages(int count) {
    for (int i = 0; i < count; ++i) {
        co_yield "Message #" + std::to_string(i);
    }
}
```

### Empty generator

Use `co_return` for generators that produce no values:

```cpp
generator<int> empty_stream() {
    co_return;  // No values yielded
}
```

### Generator with computed values

```cpp
generator<int> fibonacci(int count) {
    int a = 0, b = 1;
    for (int i = 0; i < count; ++i) {
        co_yield a;
        int next = a + b;
        a = b;
        b = next;
    }
}
```

---

## Using send() API

The `send()` function returns `generator<T>` for generator methods:

```cpp
// send() returns generator<int> (not unique_future)
auto gen = send(actor.get(), sender, &MyActor::stream_numbers, 5);

// Generator is valid but producer hasn't started yet
assert(gen.valid());

// Process the message - starts the producer coroutine
actor->resume(1);

// Producer runs until first co_yield, then suspends
```

### Multiple generators

You can queue multiple generator messages:

```cpp
auto gen1 = send(actor.get(), sender, &MyActor::stream_numbers, 5);
auto gen2 = send(actor.get(), sender, &MyActor::stream_strings, 3);

// Process all messages
actor->resume(10);
```

---

## Generator Lifecycle

### States

Generator goes through these states:

| State | Description |
|-------|-------------|
| `created` | Producer not yet started |
| `suspended` | Producer yielded a value |
| `exhausted` | Producer finished (co_return) |
| `cancelled` | Consumer cancelled |
| `detached` | Consumer detached |

### State queries

```cpp
gen.valid()           // Has valid state (not moved-from)
gen.exhausted()       // Producer finished
gen.is_cancelled()    // Consumer cancelled
gen.is_safe_to_destroy()  // In terminal state
```

### Value access

After producer yields, access the value with `current()`:

```cpp
// In coroutine context:
while (co_await gen) {      // Wait for next value
    auto& value = gen.current();  // Get reference to yielded value
    process(value);
}
```

---

## Cancellation and Detach

### Cancel

Cancel stops the producer:

```cpp
auto gen = send(actor.get(), sender, &MyActor::stream_numbers, 100);

// Cancel before or during production
gen.cancel();

assert(gen.is_cancelled());
assert(gen.is_safe_to_destroy());

// Producer will stop at next yield point
actor->resume(1);
```

### Detach

Detach releases the generator handle without stopping the producer:

```cpp
auto gen = send(actor.get(), sender, &MyActor::stream_numbers, 5);

gen.detach();  // Release handle

assert(!gen.valid());  // Handle is invalid

// Producer continues running, but values are discarded
actor->resume(1);
```

### Automatic cleanup

Destroying a generator that hasn't finished automatically cancels it:

```cpp
{
    auto gen = send(actor.get(), sender, &MyActor::stream_numbers, 100);
    // gen destroyed here - automatically cancelled
}
```

---

## Memory Management

### PMR allocation

All generator state is allocated using the actor's memory resource:

```cpp
// Generator state uses actor->resource()
auto gen = actor->stream_numbers(5);

// Message-based generators use receiver's resource
auto gen = send(actor.get(), sender, &MyActor::stream_numbers, 5);
```

### Coroutine frame

The coroutine frame (local variables, suspension state) is also allocated via PMR:

```cpp
generator<int> stream_data() {
    std::vector<int> buffer;  // Allocated in coroutine frame
    buffer.push_back(42);     // Uses actor's resource
    co_yield buffer[0];
}
```

---

## Best Practices

### 1. Use co_yield for streaming data

```cpp
// GOOD - stream one item at a time
generator<Record> stream_records() {
    for (const auto& record : database) {
        co_yield record;  // Memory efficient
    }
}

// BAD - buffer all data
unique_future<std::vector<Record>> get_all_records() {
    std::vector<Record> all;
    for (const auto& record : database) {
        all.push_back(record);  // High memory usage
    }
    co_return all;
}
```

### 2. Check generator validity

```cpp
auto gen = send(actor.get(), sender, &MyActor::stream, 5);

if (gen.valid()) {
    // Safe to use
}
```

### 3. Cancel when done early

```cpp
auto gen = send(actor.get(), sender, &MyActor::stream, 1000);

// Only need first 10 values
for (int i = 0; i < 10; ++i) {
    // consume value...
}

gen.cancel();  // Stop producer early
```

### 4. Use detach for fire-and-forget

```cpp
// Don't care about values
auto gen = send(logger.get(), sender, &Logger::stream_logs, 100);
gen.detach();  // Let it run, discard results
```

---

## Limitations

### 1. Requires coroutine context for consumption

The `co_await gen` pattern requires a coroutine context. Free functions cannot consume generators directly.

```cpp
// WORKS - in actor coroutine method
unique_future<void> process() {
    auto gen = other_actor->stream_data();
    while (co_await gen) {
        auto& value = gen.current();
    }
    co_return;
}

// DOESN'T WORK - in regular function
void process() {
    auto gen = actor->stream_data();
    // co_await gen;  // Error: not in coroutine
}
```

### 2. No synchronous API

There is no blocking `next()` method. Use `co_await` in coroutines.

### 3. Single consumer

Generator is move-only and can only have one consumer.

### 4. No exception support

Compiled with `-fno-exceptions`. Errors must be handled via return values or state checks.

---

## Example

See `examples/generator/main.cpp` for a complete working example.

```cpp
#include <actor-zeta.hpp>

class DataProducer : public basic_actor<DataProducer> {
public:
    generator<int> stream_fibonacci(int count) {
        int a = 0, b = 1;
        for (int i = 0; i < count; ++i) {
            co_yield a;
            int next = a + b;
            a = b;
            b = next;
        }
    }

    // ... dispatch_traits, behavior() ...
};

int main() {
    auto producer = spawn<DataProducer>(resource);

    auto gen = send(producer.get(), sender,
                    &DataProducer::stream_fibonacci, 10);

    producer->resume(1);  // Start generator

    // Generator now running, yielding fibonacci numbers
}
```

---

## See Also

- [PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md) - Request-response pattern with `unique_future<T>`
- [CLAUDE.md](CLAUDE.md) - Main codebase guide
- `examples/generator/` - Working example code
- `test/generator/` - Test cases