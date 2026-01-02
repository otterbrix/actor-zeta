# GCC Coroutine operator new Bug

## Summary

GCC 11.4 has a bug where coroutine `operator new` doesn't receive arguments when the coroutine method has rvalue reference parameters to move-only types.

## Affected Versions

| GCC Version | Status |
|-------------|--------|
| 11.4.0 (Ubuntu 22.04) | **BUG** - operator new receives no arguments |
| 11.5.0 | Fixed |
| 12.x | Fixed |
| 13.x | Fixed |
| 14.x | Fixed |

## Symptom

```cpp
struct Task {
    struct promise_type {
        template<typename... Args>
        static void* operator new(std::size_t size, const Args&... args) {
            // On GCC 11.4: sizeof...(Args) == 0 for rvalue ref params!
            return ::operator new(size);
        }
    };
};

struct Actor {
    // FAILS on GCC 11.4: operator new receives NO arguments
    Task method(std::unique_ptr<int>&& ptr) { co_return; }

    // WORKS: operator new receives all arguments
    Task method(std::unique_ptr<int> ptr) { co_return; }
};
```

Error message on GCC 11.4:
```
error: 'operator new' is provided by 'promise_type' but is not usable
with the function signature 'Task Actor::method(std::unique_ptr<int>&&)'
```

## Root Cause

GCC 11.4 fails to pass coroutine arguments to `operator new` when:
1. The coroutine is a member function
2. Any parameter is an rvalue reference (`T&&`)
3. The parameter type is move-only (e.g., `std::unique_ptr<T>`)

Per C++ standard [dcl.fct.def.coroutine], coroutine parameters should be passed as lvalues to `operator new`. GCC 11.4 incorrectly handles the conversion from rvalue reference parameters.

## Related GCC Bugs

- [PR104981](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=104981) - passing *this to promise type
- [PR115550](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=115550) - reference to reference in promise constructor

These bugs were fixed in GCC 14.3 but the fixes address related but different issues.

## Solution: Use By-Value Parameters

Change rvalue reference to by-value for move-only types:

```cpp
// Before (fails on GCC 11.4)
unique_future<void> process(std::unique_ptr<Data>&& data) { ... }

// After (works on all GCC versions)
unique_future<void> process(std::unique_ptr<Data> data) { ... }
```

### Why This Works

1. **Move semantics preserved**: Callers still use `std::move()`, the value is moved into the parameter
2. **RTT compatibility**: `decay_t<T&&>` == `decay_t<T>` - both store as `T`
3. **No performance difference**: Move-only types are moved, not copied

### Compile-Time Verification

```cpp
// Verify T&& and T are equivalent after decay_t
static_assert(
    std::is_same_v<
        actor_zeta::type_traits::decay_t<std::unique_ptr<T>&&>,
        actor_zeta::type_traits::decay_t<std::unique_ptr<T>>
    >,
    "decay_t<T&&> must equal decay_t<T> for RTT compatibility"
);
```

## What Doesn't Work

| Approach | Result on GCC 11.4 |
|----------|-------------------|
| `operator new(size, Actor&, Args&&...)` | FAIL - template deduction fails |
| `operator new(size, Actor&, const Args&...)` | FAIL - still doesn't match |
| `operator new(size, const Args&...)` | FAIL - no args passed at all |
| `std::coroutine_traits` specialization | FAIL - doesn't change what GCC passes |

The only working solution is to avoid rvalue reference parameters in coroutine methods.

## Affected Components

### Does NOT Affect

- **make_message / send**: Regular functions work correctly, only coroutine `operator new` is affected
- **RTT storage**: Uses `decay_t`, so `T&&` and `T` are stored identically
- **dispatch**: Works correctly regardless of parameter type

### Affects

- **Coroutine method signatures**: Methods returning `unique_future<T>` or `generator<T>`
- **Promise type operator new**: Cannot extract resource from arguments

## Test Case

See `test/non_copy_obj/main.cpp`:

```cpp
// By-value for move-only types (GCC 11.4 bug workaround)
actor_zeta::unique_future<void> check(std::unique_ptr<dummy_data> data, ...) {
    co_return;
}
```

## Compile-Time Protection

actor-zeta enforces this rule at compile time via `dispatch_traits`:

```cpp
// This will fail to compile with a clear error message:
class BadActor : public basic_actor<BadActor> {
    unique_future<void> method(std::unique_ptr<Data>&& data) { co_return; }

    using dispatch_traits = actor_zeta::dispatch_traits<&BadActor::method>;
    // ERROR: Coroutine methods must not have T&& parameters for move-only types
    //        (e.g., std::unique_ptr<T>&&). GCC 11.4 has a bug where operator new
    //        doesn't receive arguments for such signatures. Use by-value instead: T (not T&&).
};
```

The check is implemented in `dispatch_traits.hpp`:

```cpp
template<typename T>
concept rvalue_ref_to_move_only =
    std::is_rvalue_reference_v<T> &&
    std::is_move_constructible_v<std::remove_reference_t<T>> &&
    !std::is_copy_constructible_v<std::remove_reference_t<T>>;
```

This catches:
- `std::unique_ptr<T>&&`
- `std::unique_lock<T>&&`
- Any custom move-only type passed by `T&&`

## Recommendations

1. **For library users**: Use by-value parameters for move-only types in coroutine methods
2. **For CI**: Test on both GCC 11.4 (Ubuntu 22.04) and GCC 11.5+ to catch regressions
3. **For new code**: Prefer `T` over `T&&` for move-only coroutine parameters

## Historical Context

### Previous Workarounds (Removed)

1. **`std::coroutine_traits` specialization with `actor_promise<Actor>`** - didn't help because GCC still doesn't pass arguments
2. **Fallback to `get_default_resource()`** - worked but used wrong memory resource
3. **TLS for resource passing** - rejected as too complex

### Current Solution

Simple API guideline: use by-value for move-only types in coroutines.

## References

- [C++ Standard: dcl.fct.def.coroutine](https://eel.is/c++draft/dcl.fct.def.coroutine)
- [Lewis Baker: Understanding the promise type](https://lewissbaker.github.io/2018/09/05/understanding-the-promise-type)
- [cppreference: Coroutines](https://en.cppreference.com/w/cpp/language/coroutines)