#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace actor_zeta::detail {

    // An actor's side of its run protocol. The turn -- the right to call resume() -- lives in the
    // mailbox: a blocked inbox holds it and the push that unblocks it takes it (lifo_inbox.hpp).
    // Here is what the actor keeps itself:
    //
    // G, the guard: bit 0 is D, destroying; above it, the participants -- senders in flight and the
    //   puller -- in steps of 2. delete waits the count out.
    // F: a puller is inside resume(). Two at once is a contract violation (checked in debug builds).
    //
    // Parameterized by the atomics so that test/relacy model-checks this code, not a copy of it.
    struct std_atomics final {
        template<class T>
        using atomic = std::atomic<T>;

        static constexpr std::memory_order relaxed = std::memory_order_relaxed;
        static constexpr std::memory_order acquire = std::memory_order_acquire;
        static constexpr std::memory_order release = std::memory_order_release;
        static constexpr std::memory_order acq_rel = std::memory_order_acq_rel;

        [[noreturn]] static void violation(const char* what) noexcept {
            std::fprintf(stderr, "actor-zeta: protocol violation: %s\n", what);
            std::abort();
        }
    };

    template<class Atomics = std_atomics>
    struct actor_protocol final {
        using guard_word = typename Atomics::template atomic<std::uint32_t>;
        using flag_word = typename Atomics::template atomic<bool>;

        static constexpr std::uint32_t destroying = 1;
        static constexpr std::uint32_t participant = 2;

        // For the checks a caller makes around these functions.
        static void violation(const char* what) {
            Atomics::violation(what);
        }

        // ---- everyone inside the actor but its owner: senders and the puller ----------------

        // Registers, so a destroyer waits for us. false: the actor is being destroyed; we are out
        // again, untouched. Relaxed: nothing is read through a registration.
        static bool enter(guard_word& g) noexcept {
            if (g.fetch_add(participant, Atomics::relaxed) & destroying) {
                g.fetch_sub(participant, Atomics::release);
                return false;
            }
            return true;
        }

        // The last touch of the actor: past it the owner may free it. Release: carries every access
        // before it to the destroyer's acquire wait. true: the actor is being destroyed.
        static bool leave(guard_word& g) noexcept {
            return g.fetch_sub(participant, Atomics::release) & destroying;
        }

        static bool is_destroying(const guard_word& g) noexcept {
            return g.load(Atomics::relaxed) & destroying;
        }

        // ---- the puller: one resume() inside at a time ---------------------------------------

        // Set on the way in, and again after a park that failed.
        static void begin_turn(flag_word& inside) noexcept {
            if (inside.exchange(true, Atomics::acq_rel)) {
                Atomics::violation("two resume() calls on one actor at once");
            }
        }

        // Before the park: once the inbox is blocked, the send that unblocks it may start the next
        // puller at once. Before leave() when the turn stays with the caller.
        static void end_turn(flag_word& inside) noexcept {
            inside.store(false, Atomics::release);
        }

        // ---- the owner, destroying: begin_destroy(), then wait until drained() --------------

        static void begin_destroy(guard_word& g) noexcept {
            if (g.fetch_or(destroying, Atomics::acq_rel) & destroying) {
                Atomics::violation("an actor destroyed twice");
            }
        }

        static bool drained(const guard_word& g) noexcept {
            return g.load(Atomics::acquire) < participant;
        }
    };

} // namespace actor_zeta::detail
