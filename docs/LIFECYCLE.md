# Actor Lifecycle

How an actor is born, run, stopped and destroyed, who is responsible for each step, and what
stops the process when a step is taken out of turn. `cooperative_actor.hpp` is the reference.

## Who Does What

| Party | Does | Never does |
|-------|------|------------|
| The actor | Runs its loop when pulled; counts who is inside it (senders, the puller) so `delete` can wait them out | Schedules itself, holds a scheduler, knows about threads |
| `send()` | Puts a message in the mailbox; returns `{needs_sched, future}` | Runs the target |
| The owner (the supervisor that spawned the actor, or the code driving it) | Enqueues on `needs_sched`; closes and destroys the actor | Destroys an actor a scheduler may still hold |
| The scheduler | Pulls: `resume(max_throughput)` runs one step, like `next()`; follows the verdict | Anything else with the actor |

Results never wake anyone. Settling a future sets flags; the awaiting actor's loop reads them on
its next step (pull).

## The Turn

Each actor has exactly one turn: the right to call `resume()`. It lives in the mailbox:

| State | Where the turn is | Leaves the state by |
|-------|-------------------|---------------------|
| Parked | In the blocked mailbox | The next `send()` takes it: `needs_sched == true` |
| Owed | With whoever got `needs_sched` or a `resume` verdict | `scheduler->enqueue(actor)` |
| Queued | In a scheduler job | A worker calls `resume()` |
| Running | With the worker inside `resume()` | The verdict (below) |
| Closed | Nowhere: `close()` finished, no turn can appear | `delete` |
| Abandoned | Dropped by `scheduler->stop()` | `delete` |

**Whoever receives `needs_sched` or a `resume` verdict enqueues the actor, exactly once.** A
dropped obligation strands the actor for good: its turn has left the mailbox and no job holds it,
so every later `send()` reports `needs_sched == false`. An actor that sends to a peer holds an
address, not a scheduler: it records the obligation and its owner claims it (CLAUDE.md, "Who May
Schedule").

## One Step

The actor's loop is a coroutine, a generator: `resume()` resumes it once and reads what it yields.

1. A behavior suspended on a `co_await` is checked first: if its result is ready (a flag), the
   loop continues the chain.
2. Still suspended: the loop yields `busy` and the actor keeps its turn.
3. Otherwise the loop takes up to `max_throughput` messages, running `behavior()` for each.
4. The loop yields; `resume()` parks the actor only now, with the frame suspended.

| The step ends with | Verdict | The turn |
|--------------------|---------|----------|
| The mailbox empty, the park succeeded | `awaiting` | In the mailbox; the next `send()` takes it |
| Messages still waiting (budget spent, or the park refused) | `resume` | The caller: enqueue again |
| A behavior suspended on a `co_await` | `resume` | The caller: enqueue again; the next step polls the result |
| The `close()` marker | `done` | Nobody: the actor is closed |
| The actor being destroyed | `done` | Nobody |

**The turn is published only after the loop's frame is suspended.** Once the mailbox is blocked,
a `send()` on another thread may take the turn and start the next `resume()` at once; nothing may
touch the frame after the park.

A behavior suspended on a `co_await` keeps its actor in the run queue: every step polls one flag
and reports `resume`. A worker yields its thread after a step that handled nothing.

## From Spawn to Delete

1. **`spawn<A>(resource, args...)`** takes two blocks from `resource`: the actor and its loop's
   frame. The mailbox is born blocked (parked). The loop runs to its first `co_yield` and waits.
2. **The first `send()`** takes the turn; the owner enqueues; a worker calls `resume()`.
3. **Steps** as above. A behavior and its message are locals of the loop's frame: the message
   lives until its behavior finishes, so `behavior()` may read it after a `co_await`.
4. **`close()`** returns `unique_future<void>` and queues a marker behind everything sent so far.
   - A parked actor: the marker's push takes the turn, the owner closes the mailbox itself, and
     the future is ready at once.
   - Otherwise the turn holder reaches the marker in order, closes the mailbox, and the loop
     finishes: verdict `done`. A behavior suspended before the marker finishes first.
   - From then on `send()` is refused: `operation_canceled`. `close()` is idempotent: every
     call's future comes out ready.
   - The future may never become ready if the actor never gets to the marker: a dropped
     obligation, or a behavior awaiting a producer nobody runs. Bound the wait.
5. **`delete`** (a destroying `operator delete`) publishes `destroying`, waits out everyone inside
   (senders in `enqueue_impl`, the puller in `resume()`), destroys the loop's frame -- a
   suspended behavior unwinds and its callers get `broken_pipe` -- then runs `~A` and frees
   `sizeof(A)`. Messages still queued are cancelled: `operation_canceled`. An actor class must be
   `final`, since `delete` frees `sizeof(A)` (a `static_assert`).

### When `delete` is safe

| Situation | Safe? |
|-----------|-------|
| The `close()` future is ready, the scheduler still running | Yes: no job exists and none can appear |
| After `scheduler->stop()` | Yes |
| Every future waited for, nobody else sends | Only if no behavior awaits anything after its `dispatch()`; prefer `close()` |
| A job may still be queued or owed | **No**: a worker resumes freed memory; the actor cannot detect it |

## Contract Checks

A step taken out of turn stops the process with `actor-zeta: protocol violation: ...` on stderr.

| Violation | Message | Checked in |
|-----------|---------|------------|
| `resume()` without a turn (a parked actor) | `resume() on a parked actor: no send() handed out a turn` | Every build |
| `resume()` after `close()` | `resume() on a closed actor: after close() nobody holds a turn` | Every build |
| `resume()` from inside a behavior | `resume() inside resume(): the loop is running this step already` (a debug build reports two calls at once) | Every build |
| Two `resume()` calls at once (a job enqueued twice) | `two resume() calls on one actor at once` | Debug |
| `delete` twice | `an actor destroyed twice` | Every build, while the memory is not reused |
| `delete` from the actor's own behavior, or a participant that never leaves | `delete waited too long ...` (after 5 s in debug, 30 s in release) | Every build |
| Awaiting your own `send()` | `an actor awaits its own send(): ...` | Debug; release spins on `resume` verdicts |
| `get_future()` twice on one `promise` | assert | Debug |

Awaiting your own `send()` never ends: the reply waits in your own mailbox, behind the await.
Call the method directly instead: `co_await this->method(...)`.

## What a Caller's Future Can Carry

Read these by polling (`is_ready()`, then `failed()` / `error()`): `co_await` on a future that
holds an error code aborts, with exceptions enabled or not; only a thrown exception is rethrown.

| Outcome | When |
|---------|------|
| A value | The method returned one |
| The method's own `std::error_code` | The method did `co_return ec;` |
| `operation_canceled` | The message was refused (a closed or dying actor) or destroyed unprocessed (the actor was deleted with it queued) |
| `broken_pipe` | The method's frame was destroyed mid-body (the actor deleted while the behavior was suspended), or a `promise<T>` was dropped unsettled |
| `interrupted` | The method threw (`EXCEPTIONS_DISABLE=OFF`); `take_ready()` rethrows the original |
| `state_not_recoverable` | A safety net: the state was released with no outcome at all |

A method called directly (`auto f = this->method(...)`) runs in the caller's chain, not through
the mailbox, and its future owns its frame: dropping the future before the method finishes
cancels it, its locals unwinding mid-body. `co_await` it, or keep it until it is ready.
