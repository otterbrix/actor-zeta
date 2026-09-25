/// @file
/// Relacy model of how an actor's turn -- the right to call resume() -- moves, and of close().
/// The actor's own words come from detail/actor_protocol.hpp itself, instantiated with Relacy's
/// atomics. The inbox is a copy of lifo_inbox.hpp's head algorithm: lifo_inbox is std::atomic
/// only, so the model cannot instantiate it -- keep the two in step. Threads stand in for
/// senders, a worker and the owner. Checked on every explored interleaving: at most one turn;
/// every accepted message processed or cancelled, once; close() settles; no data race; no use
/// after free. Each mutant breaks one rule the actor depends on, and the model must catch it.
/// Linux only, see CMakeLists.txt.
///
///   protocol_model            every scenario passes
///   protocol_model <mutant>   the scenario the mutant targets fails, with the expected result

#include <relacy/relacy.hpp>

#include <actor-zeta/detail/actor_protocol.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

    struct rl_atomics final {
        template<class T>
        using atomic = rl::atomic<T>;

        static constexpr rl::memory_order relaxed = rl::memory_order_relaxed;
        static constexpr rl::memory_order acquire = rl::memory_order_acquire;
        static constexpr rl::memory_order release = rl::memory_order_release;
        static constexpr rl::memory_order acq_rel = rl::memory_order_acq_rel;

        // Reported with the execution history; the failing thread does not come back.
        static void violation(const char* what) {
            RL_ASSERT_IMPL(false, rl::test_result_user_assert_failed, what, RL_INFO);
        }
    };

    using protocol = actor_zeta::detail::actor_protocol<rl_atomics>;

    enum class mutant {
        none,
        park_ignores_cache,        // parks with messages still cached: they are stranded
        leave_before_park,         // leaves the guard, then parks: the owner frees the actor under the CAS
        sender_ignores_destroying, // hands out the turn of a dying actor: it is run after the free
        close_forgets_parked       // close()'s push takes a parked actor's turn and drops it: never settles
    };

    mutant g_mutant = mutant::none;

    // Every wait in the model: after a failed check the waiting thread sits out the next 16 steps
    // of the others. What is waited for only ever becomes true, and a thread's first check may
    // come at any step, so acting right after it turned true stays reachable -- while the search
    // stops re-exploring each lap of the wait (111M iterations with yield(1); 3K with this).
    void spin() {
        rl::yield(16, RL_INFO);
    }

    enum message_state : int { queued = 0, processed = 1, cancelled = 2, closed_ok = 3 };

    struct message {
        rl::var<std::uintptr_t> next;
        rl::var<int> state;
        rl::var<bool> marker; // close()'s marker: however it dies, its future settles with a value

        void reset(bool is_marker) {
            next($) = 0;
            state($) = queued;
            marker($) = is_marker;
        }
    };

    std::uintptr_t address(message* m) {
        return reinterpret_cast<std::uintptr_t>(m);
    }

    message* from_address(std::uintptr_t p) {
        return reinterpret_cast<message*>(p);
    }

    // The scheduler side: a queue of turns, a ghost count of live turns, close()'s future.
    struct world {
        rl::atomic<int> turns;       // handed out by send(), not yet run
        rl::atomic<int> live_turns;  // ghost: turns queued or held -- never more than one
        rl::atomic<int> close_ready; // markers settled
        rl::atomic<int> in_behavior; // scenario hook: a message is being processed

        void reset() {
            turns.store(0, rl::memory_order_relaxed);
            live_turns.store(0, rl::memory_order_relaxed);
            close_ready.store(0, rl::memory_order_relaxed);
            in_behavior.store(0, rl::memory_order_relaxed);
        }

        void take_up_turn() {
            RL_ASSERT(live_turns.fetch_add(1, rl::memory_order_relaxed) == 0);
        }

        void hand_out_turn() {
            take_up_turn();
            turns.fetch_add(1, rl::memory_order_release);
        }

        bool take_turn() {
            if (turns.load(rl::memory_order_acquire) == 0) {
                return false;
            }
            turns.fetch_sub(1, rl::memory_order_relaxed);
            return true;
        }
    };

    // A message's end: processed, or dropped -- a marker's future settles with a value either way.
    void settle(world& w, message* m, int state) {
        RL_ASSERT(m->state($) == queued); // once
        if (m->marker($)) {
            m->state($) = closed_ok;
            w.close_ready.fetch_add(1, rl::memory_order_release);
            return;
        }
        m->state($) = state;
    }

    // lifo_inbox.hpp's head: E = 0, B = &head (blocked: parked), C = &head + 1 (closed for good),
    // L = the newest message. Every operation is seq_cst there, and so here.
    struct inbox {
        rl::atomic<std::uintptr_t> head;

        std::uintptr_t blocked_tag() const {
            return reinterpret_cast<std::uintptr_t>(&head);
        }

        std::uintptr_t closed_tag() const {
            return blocked_tag() + 1;
        }

        enum class push_result { success, unblocked_reader, queue_closed };

        // lifo_inbox::push_front
        push_result push_front(message* m) {
            std::uintptr_t last = head.load(rl::memory_order_seq_cst);
            while (last != closed_tag()) {
                m->next($) = last != blocked_tag() ? last : 0;
                if (head.compare_exchange_strong(last, address(m), rl::memory_order_seq_cst)) {
                    return last == blocked_tag() ? push_result::unblocked_reader : push_result::success;
                }
            }
            return push_result::queue_closed;
        }

        // lifo_inbox::take_head(new_head)
        std::uintptr_t take_head(std::uintptr_t new_head) {
            std::uintptr_t last = head.load(rl::memory_order_seq_cst);
            RL_ASSERT(last != closed_tag());
            RL_ASSERT(last != blocked_tag() || new_head == closed_tag());
            while (last != new_head) {
                if (head.compare_exchange_weak(last, new_head, rl::memory_order_seq_cst)) {
                    RL_ASSERT(last != closed_tag());
                    if (last == 0 || last == blocked_tag()) {
                        RL_ASSERT(new_head == closed_tag());
                        return 0;
                    }
                    return last;
                }
            }
            return 0;
        }

        // lifo_inbox::try_block
        bool try_block() {
            std::uintptr_t expected = 0;
            return head.compare_exchange_strong(expected, blocked_tag(), rl::memory_order_seq_cst);
        }

        bool blocked() const {
            return head.load(rl::memory_order_seq_cst) == blocked_tag();
        }

        bool closed() const {
            return head.load(rl::memory_order_seq_cst) == closed_tag();
        }
    };

    // The actor: its words from actor_protocol.hpp, the inbox, and the cache only the turn holder
    // touches (default_mailbox's local queue).
    struct model_actor {
        inbox box;
        protocol::guard_word g;
        protocol::flag_word inside;
        rl::var<message*> cache; // taken messages, oldest first

        model_actor() {
            box.head.store(0, rl::memory_order_relaxed);
            g.store(0, rl::memory_order_relaxed);
            inside.store(false, rl::memory_order_relaxed);
            cache($) = nullptr;
            box.try_block(); // born parked
        }
    };

    // default_mailbox::fetch_more: the taken list is newest first; reverse it onto the cache.
    bool fetch_more(model_actor* a) {
        if (a->box.closed()) {
            return false;
        }
        std::uintptr_t list = a->box.take_head(0);
        if (list == 0) {
            return false;
        }
        message* reversed = nullptr;
        while (list != 0) {
            message* m = from_address(list);
            list = m->next($);
            m->next($) = address(reversed);
            reversed = m;
        }
        message* tail = a->cache($);
        if (tail == nullptr) {
            a->cache($) = reversed;
            return true;
        }
        while (tail->next($) != 0) {
            tail = from_address(tail->next($));
        }
        tail->next($) = address(reversed);
        return true;
    }

    message* pop_front(model_actor* a) {
        if (a->cache($) == nullptr && !fetch_more(a)) {
            return nullptr;
        }
        message* m = a->cache($);
        a->cache($) = from_address(m->next($));
        return m;
    }

    void cancel_list(world& w, std::uintptr_t list) {
        while (list != 0) {
            message* m = from_address(list);
            list = m->next($);
            settle(w, m, cancelled);
        }
    }

    // default_mailbox::close_impl: bounce the cache, close the inbox for good, bounce what it held.
    void close_mailbox(world& w, model_actor* a) {
        while (a->cache($) != nullptr) {
            message* m = a->cache($);
            a->cache($) = from_address(m->next($));
            settle(w, m, cancelled);
        }
        cancel_list(w, a->box.take_head(a->box.closed_tag()));
    }

    // delete: no new participant, wait out those inside; the mailbox destructor bounces the rest.
    void destroy(world& w, model_actor* a) {
        protocol::begin_destroy(a->g);
        while (!protocol::drained(a->g)) {
            spin();
        }
        if (!a->box.closed()) {
            close_mailbox(w, a);
        }
        delete a;
    }

    enum class verdict { resume, awaiting, done };

    // cooperative_actor::resume() over the model: one step of the loop -- at most `budget` messages,
    // the marker closes -- then park; a park that fails keeps the turn (`resume`) and ends the step.
    // default_mailbox's try_block_impl refuses while messages are cached.
    verdict pull(world& w, model_actor* a, int budget) {
        if (!protocol::enter(a->g)) {
            return verdict::done;
        }
        protocol::begin_turn(a->inside);
        if (a->box.blocked()) {
            protocol::violation("resume() on a parked actor: no send() handed out a turn");
        }

        verdict v = verdict::resume;
        int handled = 0;
        for (;;) {
            if (handled < budget) {
                if (message* m = pop_front(a)) {
                    if (m->marker($)) {
                        close_mailbox(w, a);
                        w.live_turns.fetch_sub(1, rl::memory_order_relaxed); // ghost: no turn after close
                        settle(w, m, processed);
                        v = verdict::done;
                        break;
                    }
                    w.in_behavior.store(1, rl::memory_order_release);
                    settle(w, m, processed);
                    ++handled;
                    continue;
                }
            }

            w.live_turns.fetch_sub(1, rl::memory_order_relaxed); // ghost: the turn may go back now
            protocol::end_turn(a->inside);
            if (g_mutant == mutant::leave_before_park) {
                protocol::leave(a->g);
                a->box.try_block();
                return verdict::awaiting;
            }
            const bool cache_empty = a->cache($) == nullptr || g_mutant == mutant::park_ignores_cache;
            if (cache_empty && a->box.try_block()) {
                protocol::leave(a->g);
                return verdict::awaiting;
            }
            if (a->box.closed()) {
                protocol::violation("resume() on a closed actor: after close() nobody holds a turn");
            }
            // One step per resume(): a message that came before the park waits for the next one.
            w.live_turns.fetch_add(1, rl::memory_order_relaxed);
            protocol::begin_turn(a->inside);
            break;
        }

        protocol::end_turn(a->inside);
        const bool dying = protocol::leave(a->g); // the last touch
        return (dying && v == verdict::resume) ? verdict::done : v;
    }

    enum class sent { refused, queued, owed };

    // cooperative_actor::enqueue_impl. `pause` runs once, inside the actor, before the push.
    template<class Pause>
    sent send(model_actor* a, message* m, Pause&& pause) {
        if (!protocol::enter(a->g)) {
            return sent::refused;
        }
        pause();
        const auto pushed = a->box.push_front(m);
        const bool dying = protocol::leave(a->g); // the last touch
        if (pushed == inbox::push_result::queue_closed) {
            return sent::refused;
        }
        if (pushed == inbox::push_result::unblocked_reader &&
            (!dying || g_mutant == mutant::sender_ignores_destroying)) {
            return sent::owed;
        }
        return sent::queued;
    }

    // A refused message is cancelled by its sender; an owed turn goes to the scheduler.
    void sender(world& w, model_actor* a, message* m) {
        switch (send(a, m, [] {})) {
            case sent::refused:
                settle(w, m, cancelled);
                break;
            case sent::owed:
                w.hand_out_turn();
                break;
            case sent::queued:
                break;
        }
    }

    // cooperative_actor::close(): the marker queues behind everything sent; a push that takes a
    // parked actor's turn closes at once; a refused marker settles by itself.
    void owner_close(world& w, model_actor* a, message* marker) {
        switch (a->box.push_front(marker)) {
            case inbox::push_result::queue_closed:
                settle(w, marker, cancelled);
                return;
            case inbox::push_result::unblocked_reader:
                w.take_up_turn(); // ghost: the owner holds it now
                if (g_mutant == mutant::close_forgets_parked) {
                    return;
                }
                protocol::begin_turn(a->inside);
                close_mailbox(w, a);
                protocol::end_turn(a->inside);
                w.live_turns.fetch_sub(1, rl::memory_order_relaxed); // no turn after close
                return;
            case inbox::push_result::success:
                return; // the turn holder reaches it in order
        }
    }

    // ---- A: two senders, one worker. The turn passes back and forth through the inbox. -------
    struct turn_handover : rl::test_suite<turn_handover, 3> {
        world w;
        model_actor* a = nullptr;
        message m[2];

        void before() {
            w.reset();
            a = new model_actor;
            m[0].reset(false);
            m[1].reset(false);
        }

        void thread(unsigned index) {
            if (index < 2) {
                sender(w, a, &m[index]);
                return;
            }
            for (;;) {
                if (m[0].state($) == processed && m[1].state($) == processed) {
                    return;
                }
                if (w.take_turn()) {
                    while (pull(w, a, 1) == verdict::resume) {
                    }
                } else {
                    spin();
                }
            }
        }

        void after() {
            RL_ASSERT(m[0].state($) == processed);
            RL_ASSERT(m[1].state($) == processed);
            delete a;
        }
    };

    // ---- B: close() with senders in flight, then delete. A send after the free is undefined
    //         (address_t is a raw pointer), so the owner deletes once the senders are done. -------
    struct close_then_destroy : rl::test_suite<close_then_destroy, 4> {
        world w;
        model_actor* a = nullptr;
        message m[2];
        message marker;
        rl::atomic<int> senders_done;

        void before() {
            w.reset();
            a = new model_actor;
            m[0].reset(false);
            m[1].reset(false);
            marker.reset(true);
            senders_done.store(0, rl::memory_order_relaxed);
        }

        void thread(unsigned index) {
            if (index < 2) {
                sender(w, a, &m[index]);
                senders_done.fetch_add(1, rl::memory_order_release);
                return;
            }
            if (index == 2) { // the worker
                for (;;) {
                    if (w.take_turn()) {
                        verdict v;
                        while ((v = pull(w, a, 1)) == verdict::resume) {
                        }
                        if (v == verdict::done) {
                            return;
                        }
                    } else if (w.close_ready.load(rl::memory_order_acquire)) {
                        return; // closed: no turn can be handed out any more
                    } else {
                        spin();
                    }
                }
            }
            // the owner
            owner_close(w, a, &marker);
            while (!w.close_ready.load(rl::memory_order_acquire) ||
                   senders_done.load(rl::memory_order_acquire) != 2) {
                spin();
            }
            destroy(w, a);
        }

        void after() {
            RL_ASSERT(marker.state($) == closed_ok);
            RL_ASSERT(m[0].state($) != queued);
            RL_ASSERT(m[1].state($) != queued);
        }
    };

    // ---- C: a sender entered before D, leaving after it. It unblocks a dying actor and must not
    //         hand out the turn: the worker would run the actor after the free. -----------------
    struct destroy_during_send : rl::test_suite<destroy_during_send, 3> {
        world w;
        model_actor* a = nullptr;
        message m[1];
        rl::atomic<int> registered;
        rl::atomic<int> destroying_published;
        rl::atomic<int> owner_done;

        void before() {
            w.reset();
            a = new model_actor;
            m[0].reset(false);
            registered.store(0, rl::memory_order_relaxed);
            destroying_published.store(0, rl::memory_order_relaxed);
            owner_done.store(0, rl::memory_order_relaxed);
        }

        void thread(unsigned index) {
            if (index == 0) { // the sender
                const sent result = send(a, &m[0], [&] {
                    registered.store(1, rl::memory_order_release);
                    while (!destroying_published.load(rl::memory_order_acquire)) {
                        spin();
                    }
                });
                if (result == sent::owed) {
                    w.hand_out_turn();
                }
                return;
            }
            if (index == 1) { // the worker
                while (!owner_done.load(rl::memory_order_acquire)) {
                    if (w.take_turn()) {
                        while (pull(w, a, 1) == verdict::resume) {
                        }
                    } else {
                        spin();
                    }
                }
                return;
            }
            // the owner
            while (!registered.load(rl::memory_order_acquire)) {
                spin();
            }
            protocol::begin_destroy(a->g);
            destroying_published.store(1, rl::memory_order_release);
            while (!protocol::drained(a->g)) {
                spin();
            }
            if (!a->box.closed()) {
                close_mailbox(w, a);
            }
            delete a;
            owner_done.store(1, rl::memory_order_release);
        }

        void after() {
            RL_ASSERT(m[0].state($) == cancelled);
        }
    };

    // ---- D: the owner deletes while the worker is inside resume(): it must wait the worker out.
    struct destroy_while_held : rl::test_suite<destroy_while_held, 2> {
        world w;
        model_actor* a = nullptr;
        message m[1];

        void before() {
            w.reset();
            a = new model_actor;
            m[0].reset(false);
            RL_ASSERT(send(a, &m[0], [] {}) == sent::owed); // unblocks: the worker starts with the turn
            w.hand_out_turn();
        }

        void thread(unsigned index) {
            if (index == 0) { // the worker
                RL_ASSERT(w.take_turn());
                while (pull(w, a, 1) == verdict::resume) {
                }
                return;
            }
            // the owner: it knows the turn is held, as a test does from a behavior's side effect
            while (!w.in_behavior.load(rl::memory_order_acquire)) {
                spin();
            }
            destroy(w, a);
        }

        void after() {
            RL_ASSERT(m[0].state($) == processed);
        }
    };

    template<class Scenario>
    rl::test_result_e run(rl::scheduler_type_e search, unsigned context_bound) {
        rl::test_params p;
        p.search_type = search;
        p.context_bound = context_bound;
        rl::simulate<Scenario>(p);
        if (g_mutant == mutant::none && p.test_result != rl::test_result_success) {
            // Replay just the failing iteration, with its history.
            rl::test_params replay;
            replay.search_type = search;
            replay.context_bound = context_bound;
            replay.initial_state = p.final_state;
            replay.iteration_count = 1;
            replay.collect_history = replay.output_history = true;
            rl::simulate<Scenario>(replay);
        }
        return p.test_result;
    }

    // Full search where it stays small (C, D); A and B are bounded by preemptions.
    rl::test_result_e run_turn_handover() { return run<turn_handover>(rl::sched_bound, 4); }
    rl::test_result_e run_close_then_destroy() { return run<close_then_destroy>(rl::sched_bound, 3); }
    rl::test_result_e run_destroy_during_send() { return run<destroy_during_send>(rl::sched_full, 0); }
    rl::test_result_e run_destroy_while_held() { return run<destroy_while_held>(rl::sched_full, 0); }

    struct mutant_case {
        const char* name;
        mutant which;
        rl::test_result_e (*scenario)();
        rl::test_result_e expected;
    };

    const mutant_case mutants[] = {
        {"park_ignores_cache", mutant::park_ignores_cache, run_turn_handover, rl::test_result_livelock},
        {"leave_before_park", mutant::leave_before_park, run_destroy_while_held,
         rl::test_result_access_to_freed_memory},
        {"sender_ignores_destroying", mutant::sender_ignores_destroying, run_destroy_during_send,
         rl::test_result_access_to_freed_memory},
        {"close_forgets_parked", mutant::close_forgets_parked, run_close_then_destroy, rl::test_result_livelock},
    };

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        const bool green = run_turn_handover() == rl::test_result_success &&
                           run_close_then_destroy() == rl::test_result_success &&
                           run_destroy_during_send() == rl::test_result_success &&
                           run_destroy_while_held() == rl::test_result_success;
        std::printf(green ? "protocol model: every scenario passes\n" : "protocol model: FAILED\n");
        return green ? 0 : 1;
    }
    for (const auto& c : mutants) {
        if (std::strcmp(argv[1], c.name) != 0) {
            continue;
        }
        g_mutant = c.which;
        const rl::test_result_e got = c.scenario();
        if (got == c.expected) {
            std::printf("mutant %s: caught (%s)\n", c.name, rl::test_result_str(got));
            return 0;
        }
        std::printf("mutant %s: NOT caught as %s, got %s\n", c.name, rl::test_result_str(c.expected),
                    rl::test_result_str(got));
        return 1;
    }
    std::printf("unknown mutant: %s\n", argv[1]);
    return 2;
}
