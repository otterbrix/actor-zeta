# Generator Guide

Streaming data between actors using `generator<T>` coroutine type.

## Overview

`generator<T>` enables actor methods to yield multiple values via `co_yield`.

**Features:** Move-only, lazy evaluation, PMR allocation, integrated with `send()`.

**Use cases:** Streaming datasets, event sequences, pagination, real-time feeds.

## Quick Start

```cpp
class MyActor : public basic_actor<MyActor> {
public:
    generator<int> stream_numbers(int count) {
        for (int i = 0; i < count; ++i) {
            co_yield i;
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<&MyActor::stream_numbers>;

    void behavior(mailbox::message* msg) {
        if (msg->command() == msg_id<MyActor, &MyActor::stream_numbers>) {
            dispatch(this, &MyActor::stream_numbers, msg);
        }
    }
};

// Usage
auto gen = send(actor.get(), sender, &MyActor::stream_numbers, 10);
actor->resume(1);  // Start generator
```

## Generator States

| State | Description |
|-------|-------------|
| `created` | Not yet started |
| `suspended` | Yielded a value |
| `exhausted` | Finished |
| `cancelled` | Consumer cancelled |
| `detached` | Handle released |

**State queries:**
```cpp
gen.valid()              // Has valid state
gen.exhausted()          // Producer finished
gen.is_cancelled()       // Cancelled
gen.is_safe_to_destroy() // Terminal state
```

## Consuming Values

```cpp
// In coroutine context
unique_future<void> consume() {
    auto gen = other_actor->stream_data();
    while (co_await gen) {
        auto& value = gen.current();
        process(value);
    }
    co_return;
}
```

## Lifecycle Control

```cpp
// Cancel - stop producer
gen.cancel();

// Detach - release handle, let producer run
gen.detach();

// Auto-cleanup on destruction
{
    auto gen = send(actor.get(), sender, &Actor::stream, 100);
}  // Automatically cancelled
```

## Best Practices

| Do | Don't |
|----|-------|
| Use `co_yield` for streaming | Buffer all data in memory |
| Check `valid()` before use | Assume generator is valid |
| `cancel()` when done early | Let unused generators run |
| `detach()` for fire-and-forget | Store generators indefinitely |

## Limitations

- Requires coroutine context for `co_await gen` consumption
- No synchronous `next()` API
- Single consumer only (move-only)
- No `generator<void>` support
- No exception support (`-fno-exceptions`)

## See Also

- [PROMISE_FUTURE_GUIDE.md](PROMISE_FUTURE_GUIDE.md) - Request-response with `unique_future<T>`
- `examples/generator/` - Working examples
- `test/generator/` - Test cases