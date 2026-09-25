#pragma once

#include <memory>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <exception>
#include <new>
#include <thread>
#include <type_traits>

#include <actor-zeta/actor/actor_mixin.hpp>
#include <actor-zeta/detail/actor_protocol.hpp>
#include <actor-zeta/detail/behavior_t.hpp>
#include <actor-zeta/detail/coro_frame_header.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/detail/forwards.hpp>
#include <actor-zeta/detail/future.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/scheduler/resumable.hpp>

namespace actor_zeta { namespace actor {

    template<class Target>
    Target* check_ptr(Target* ptr) {
        assert(ptr);
        return ptr;
    }

    inline void exponential_backoff(int attempt) noexcept {
        constexpr int kSpinPhaseEnd = 4;
        constexpr int kYieldPhaseEnd = 10;
        constexpr int kMaxSleepMicroseconds = 1000;

        if (attempt < kSpinPhaseEnd) {
        } else if (attempt < kYieldPhaseEnd) {
            std::this_thread::yield();
        } else {
            // Cap the EXPONENT: `1 << 31` is INT_MIN and past it the shift is UB. Reachable via
            // publish_destroying_and_drain().
            constexpr int kMaxShift = 10;
            const int exponent = attempt - kYieldPhaseEnd;
            const int computed = exponent >= kMaxShift ? kMaxSleepMicroseconds : (1 << exponent);
            const auto sleep_us = computed < kMaxSleepMicroseconds ? computed : kMaxSleepMicroseconds;
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
    }

    // A generator on a coroutine: the actor's loop is a coroutine, and resume() pulls one step of
    // it, like next(). The turn -- the right to call resume() -- lives in the mailbox: a blocked
    // mailbox holds it, and the send() that unblocks it takes it (needs_sched). The actor itself
    // only counts who is inside -- senders in enqueue_impl and the puller in resume() -- so that
    // delete can wait them out.
    template<class Actor, class MailBox>
    class cooperative_actor
        : public actor_mixin<Actor> {
    private:
        using protocol = detail::actor_protocol<>;

        // close()'s marker: a command no dispatch_traits index reaches, never shown to behavior().
        // Normal priority, so it queues in order with every send().
        static constexpr mailbox::message_id close_command = mailbox::make_message_id(0x0FFF'FFFF'FFFF'FFFFull);

        // One step of the loop, for resume(): the messages it took, and whether its behavior is
        // suspended -- then the actor keeps the turn and must not park.
        struct step final {
            size_t handled = 0;
            bool busy = false;
        };

        // The loop's coroutine type. Private and nested: nothing outside the actor can name the
        // loop, let alone resume it.
        struct loop final {
            struct promise_type;
            using handle_type = detail::coroutine_handle<promise_type>;

            struct promise_type {
                size_t budget_ = 0;    // resume() -> the loop: messages this step may take
                step step_{};          // the loop -> resume()
                bool running_ = false; // a step is running: resume() inside resume() is caught
                                       // in release too (the atomic check in debug also sees threads)

                loop get_return_object() noexcept {
                    return loop{handle_type::from_promise(*this)};
                }

                // Runs at once, in the constructor, to its first co_yield: armed for resume().
                detail::suspend_never initial_suspend() noexcept { return {}; }
                // Stays for delete to free.
                detail::suspend_always final_suspend() noexcept { return {}; }

                // co_yield hands the step out; on the next resume() it evaluates to the new budget.
                auto yield_value(step s) noexcept {
                    step_ = s;
                    struct budget_awaiter {
                        promise_type* promise_;
                        bool await_ready() const noexcept { return false; }
                        void await_suspend(handle_type) const noexcept {}
                        size_t await_resume() const noexcept { return promise_->budget_; }
                    };
                    return budget_awaiter{this};
                }

                void return_value(step s) noexcept { step_ = s; }

                // behavior() and dispatch() keep what user code throws; nothing reaches the loop.
                void unhandled_exception() noexcept { std::terminate(); }

                // No co_await in the loop: awaiting registers a continuation, and a result would
                // wake the actor -- push. co_yield does not go through await_transform.
                template<class U>
                void await_transform(U&&) = delete;

                // The frame comes from the actor's resource, as behavior_t's does.
                static void* operator new(std::size_t size, const cooperative_actor& self) {
                    return detail::allocate_coro_frame(self.resource_, size);
                }

                static void operator delete(void* ptr, std::size_t size) noexcept {
                    detail::deallocate_coro_frame(ptr, size);
                }

                static void operator delete(void* ptr) noexcept {
                    detail::deallocate_coro_frame_unsized(ptr);
                }
            };

            handle_type handle_;
        };

        static constexpr bool check_dispatch_traits_exists() {
            using dispatch_traits_check = typename Actor::dispatch_traits;
            detail::ignore_unused(sizeof(dispatch_traits_check));
            return true;
        }

    public:
        using is_cooperative_actor_type = void;

        using typename actor_mixin<Actor>::id_t;
        using typename actor_mixin<Actor>::placement_tag;
        using actor_mixin<Actor>::placement;

        using unique_actor = std::unique_ptr<cooperative_actor<Actor, MailBox>, pmr::deleter_t>;

        template<typename T>
        using promise = actor_zeta::promise<T>;

        template<typename T>
        using unique_future = actor_zeta::unique_future<T>;

        // Inside the actor for the whole push, so delete waits for a sender already in push_back()
        // (test/shutdown-sender-race). needs_sched: this push unblocked the mailbox and took the
        // turn -- unless the actor is being destroyed, and then nobody may run it.
        [[nodiscard]]
        std::pair<bool, detail::enqueue_result> enqueue_impl(mailbox::message_ptr msg) {
            if (!protocol::enter(guard_)) {
                return {false, detail::enqueue_result::queue_closed};
            }
            const auto result = mailbox().push_back(std::move(msg));
            const bool dying = protocol::leave(guard_); // the last touch
            return {result == detail::enqueue_result::unblocked_reader && !dying, result};
        }

        // next(): runs the actor on a turn -- one send() handed out, or one a `resume` verdict kept.
        // The verdict is an obligation: `resume` -- the caller re-enqueues the actor, nobody else
        // will; `awaiting` -- the turn is back in the mailbox, the next send() hands it out; `done`
        // -- the actor is closed or being destroyed. Without a turn resume() stops the process; so
        // does a second resume() at once, in a debug build.
        [[nodiscard]] scheduler::resume_info resume(size_t max_throughput) noexcept {
            assert(max_throughput > 0 && "max_throughput must be greater than 0");
            // `destroying` is read here only: a delete that starts later waits for this call,
            // and leave() turns its verdict into `done`.
            if (!protocol::enter(guard_)) {
                return scheduler::resume_info(scheduler::resume_result::done, 0);
            }
#ifndef NDEBUG
            protocol::begin_turn(inside_);
#endif
            if (mailbox().blocked()) {
                protocol::violation("resume() on a parked actor: no send() handed out a turn");
            }
            if (loop_.done()) {
                protocol::violation("resume() on a closed actor: after close() nobody holds a turn");
            }

            // One step of the loop per call: next().
            auto& loop_promise = loop_.promise();
            if (loop_promise.running_) {
                protocol::violation("resume() inside resume(): the loop is running this step already");
            }
            loop_promise.budget_ = max_throughput;
            loop_promise.running_ = true;
            loop_.resume();
            loop_promise.running_ = false;
            const step taken = loop_promise.step_; // read before the park: past it the loop is not ours
            auto verdict = scheduler::resume_result::resume;
            if (loop_.done()) { // close(): the mailbox is closed and the marker settled
                verdict = scheduler::resume_result::done;
            } else if (!taken.busy) { // a suspended behavior keeps the turn; otherwise park
                // Park only now: the loop is suspended. inside_ first -- once the mailbox is
                // blocked, the send that unblocks it may start the next resume() at once, and from
                // then on nothing here may touch the loop. try_block() refuses while messages
                // wait; then the turn stays with the caller and they wait for the next step.
#ifndef NDEBUG
                protocol::end_turn(inside_);
#endif
                if (mailbox().try_block()) {
                    verdict = scheduler::resume_result::awaiting;
                } else {
                    if (mailbox().closed()) {
                        protocol::violation("resume() on a closed actor: after close() nobody holds a turn");
                    }
#ifndef NDEBUG
                    protocol::begin_turn(inside_); // still ours
#endif
                }
            }
            const size_t handled = taken.handled;

#ifndef NDEBUG
            if (verdict != scheduler::resume_result::awaiting) {
                protocol::end_turn(inside_); // `awaiting` cleared it before the park
            }
#endif
            // The last touch: past it the owner may free the actor.
            if (protocol::leave(guard_) && verdict == scheduler::resume_result::resume) {
                verdict = scheduler::resume_result::done;
            }
            return scheduler::resume_info(verdict, handled);
        }

        // The owner's graceful stop. From here on send() is refused (operation_canceled); what was
        // sent before runs, a suspended behavior finishes, then the actor takes no more messages
        // and the future is ready -- at once for a parked actor, which the owner closes itself.
        // Idempotent: every call's future comes out ready, not failed. Once one is ready, no job
        // for this actor exists or can appear: delete is safe while the scheduler runs.
        unique_future<void> close() {
            auto* state = detail::allocate_shared_state<void>(resource());
            auto marker = mailbox::pmr_make_message(resource(), resource(), close_command);
            marker->init_close_slot(state);
            unique_future<void> closed(state);

            // In order behind everything sent so far. Refused, the marker settles itself: closed.
            if (mailbox().push_back(std::move(marker)) == detail::enqueue_result::unblocked_reader) {
                // It was parked, and this push took the turn: close now, the marker with the rest.
#ifndef NDEBUG
                protocol::begin_turn(inside_);
#endif
                mailbox().close();
#ifndef NDEBUG
                protocol::end_turn(inside_);
#endif
            }
            return closed;
        }

    private:
        // The actor's loop, the generator resume() pulls. It waits at its first co_yield from the
        // constructor on. A behavior and its message are locals here: the message goes only after
        // its behavior, a suspended behavior is never parked -- the loop yields `busy` inside its
        // scope -- and delete tears both down by destroying the frame.
        // `const size_t granted = co_yield ...; budget = granted;` on purpose: GCC 13 crashes
        // (internal compiler error in instantiate_type) on assigning a co_yield directly.
        loop run() {
            size_t budget = co_yield step{};
            for (;;) {
                size_t handled = 0;
                while (handled < budget) {
                    mailbox::message_ptr msg = mailbox().pop_front();
                    if (!msg) {
                        break;
                    }
                    if (msg->command() == close_command) {
                        // Everything sent before close() has run. Close for good -- what came later
                        // is cancelled -- and the marker's future settles as msg goes, still inside
                        // resume().
                        mailbox().close();
                        co_return step{handled, false};
                    }
                    ++handled;
                    behavior_t behavior = self()->behavior(msg.get());
                    // Pull: readiness is a flag -- release_promise() touches neither mailbox nor
                    // scheduler -- so the loop reads it and continues the chain itself.
                    while (behavior.is_busy()) {
                        if (auto cont = behavior.take_awaited_continuation()) {
                            cont.resume();
                        }
                        if (!behavior.is_busy()) {
                            break;
                        }
#ifndef NDEBUG
                        // Awaiting its own send(): the reply waits in this mailbox, which the loop
                        // reads only after the await -- the actor would keep its turn and spin.
                        if (behavior.handle_.promise().awaited_target_ == static_cast<const void*>(self())) {
                            protocol::violation("an actor awaits its own send(): the reply waits in its "
                                                "own mailbox behind the await -- call the method "
                                                "directly (co_await this->method(...)) instead");
                        }
#endif
                        const size_t granted = co_yield step{handled, true}; // keep the turn
                        budget = granted;
                        handled = 0;
                    }
                }
                const size_t granted = co_yield step{handled, false}; // resume() parks, or keeps the turn
                budget = granted;
            }
        }

    public:
        std::pmr::memory_resource* resource() const noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Use-after-free!");
#endif
            return resource_;
        }

        cooperative_actor() = delete;

        // `delete` on an Actor* or a cooperative_actor* lands here instead of in ~Actor: an actor
        // is destroyed as a coroutine is. With no resume() and no sender left, the suspended chain
        // unwinds while every member it may touch is alive -- method locals, awaited futures, the
        // callers' promises (broken_pipe) -- then ~Actor runs and sizeof(Actor) goes back.
        void operator delete(cooperative_actor* self, std::destroying_delete_t) noexcept {
            static_assert(std::is_final_v<Actor>,
                          "an actor class must be final: its destroying delete runs ~Actor "
                          "and frees sizeof(Actor), which a further-derived class would outgrow");
            if (!self) {
                return;
            }
            self->publish_destroying_and_drain();
            // The loop's frame holds the behavior and its message: destroying it unwinds the chain
            // (callers get broken_pipe), then drops the message -- while every member is alive.
            self->loop_.destroy();
            self->loop_ = {};

            auto* resource = self->resource_;
            auto* actor = static_cast<Actor*>(self);
            actor->~Actor();
            resource->deallocate(actor, sizeof(Actor), alignof(Actor));
        }

        // The destroying delete above hides the base's; allocate_ptr's placement new needs the
        // placement one back to free the memory if a constructor throws.
        using actor_mixin<Actor>::operator delete;

        // Also reached without the destroying delete (an actor that is not heap-allocated): then
        // the chain unwinds here, after ~Actor. Through delete, both are done already.
        ~cooperative_actor() {
            if (!protocol::is_destroying(guard_)) {
                publish_destroying_and_drain();
            }
            if (loop_) {
                loop_.destroy();
            }
        }

    protected:
        explicit cooperative_actor(std::pmr::memory_resource* in_resource)
            : actor_mixin<Actor>()
            , resource_(check_ptr(in_resource))
            , mailbox_()
#ifndef NDEBUG
            , magic_(kMagicAlive)
#endif
        {
            static_assert(check_dispatch_traits_exists(),
                          "Actor must define nested 'dispatch_traits'");
            mailbox().try_block(); // born parked: the first send() hands out the turn
            loop_ = run().handle_; // from the actor's resource; waits at its first co_yield
        }

    private:
        // Publishes `destroying` -- a second time is a second delete -- and waits out everyone
        // inside. Bounded: a participant that never leaves, such as an actor deleting itself from
        // its own behavior(), stops the process instead of hanging it.
        void publish_destroying_and_drain() noexcept {
#ifndef NDEBUG
            assert(magic_ == kMagicAlive && "Double-delete!");
#endif
            protocol::begin_destroy(guard_);

            const auto start_time = std::chrono::steady_clock::now();
#ifndef NDEBUG
            constexpr auto timeout = std::chrono::seconds(5);
#else
            constexpr auto timeout = std::chrono::seconds(30);
#endif
            for (int attempt = 0; !protocol::drained(guard_); ++attempt) {
                exponential_backoff(attempt);
                if (std::chrono::steady_clock::now() - start_time > timeout) {
                    protocol::violation("delete waited too long for a send() or resume() inside the "
                                        "actor -- does an actor delete itself from its own behavior()?");
                }
            }
        }

        inline const Actor* self() const noexcept {
            return static_cast<const Actor*>(this);
        }

        inline Actor* self() noexcept {
            return static_cast<Actor*>(this);
        }

        MailBox& mailbox() noexcept {
            return mailbox_;
        }

        std::pmr::memory_resource* resource_;
        MailBox mailbox_;
        // The loop's frame: the current behavior and its message live in it. Deliberately
        // unreachable from outside: an accessor would be a public way to advance the actor off
        // the thread holding the turn.
        typename loop::handle_type loop_{};
        // Who is inside (senders in enqueue_impl, the puller in resume()) and `destroying`.
        protocol::guard_word guard_{0};

#ifndef NDEBUG
        // A puller is inside resume(): a second one at once is a contract violation. Debug only:
        // with one turn per actor it can only diagnose, and it costs up to four atomics a call.
        protocol::flag_word inside_{false};
        static constexpr uint32_t kMagicAlive = 0xFEEDFACE;
        uint32_t magic_;
#endif
    };

}} // namespace actor_zeta::actor
