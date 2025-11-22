#pragma once

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/base/handler.hpp>
#include <actor-zeta/detail/callable_trait.hpp>
#include <actor-zeta/detail/memory_resource.hpp>
#include <actor-zeta/detail/coroutine.hpp>
#include <actor-zeta/config.hpp>
#include <actor-zeta/mailbox/id.hpp>
#include <actor-zeta/mailbox/message.hpp>

namespace actor_zeta {
    namespace detail {
        class future_state_base;  // Forward declaration
    }
}

namespace actor_zeta { namespace base {

    class behavior_t final {
    public:
        behavior_t() = delete;
        behavior_t(const behavior_t&) = delete;
        behavior_t& operator=(const behavior_t&) = delete;

        behavior_t(behavior_t&&) = default;
        behavior_t& operator=(behavior_t&&) = default;

        behavior_t(actor_zeta::pmr::memory_resource*, action handler)
            : handler_(std::move(handler))
            , linked_state_(nullptr)
        {}

        explicit operator bool() {
            return bool(handler_);
        }

        void operator()(mailbox::message* msg) {
            handler_(msg);
        }

        /// @brief Link this behavior with a future_state (for coroutine resumption)
        /// @param state Pointer to future_state that may contain a suspended coroutine
        /// @note Called from make_behavior when message has a result_slot
        /// @warning Nested/recursive coroutines NOT supported
        void set_linked_state(detail::future_state_base* state) noexcept {
            // CRITICAL: Detect nested/recursive coroutines (send(this, ...) from within coroutine)
            // This will DEADLOCK because actor is in "running" state and won't reschedule.
            assert(!linked_state_ && "NESTED COROUTINES NOT SUPPORTED: Recursive send(this, ...) from coroutine will deadlock!");
            linked_state_ = state;
        }

        /// @brief Resume suspended coroutine if present AND ready
        /// @note Called from behavior() before switch/dispatch
        /// @note Only resumes if future is ready to avoid blocking
        void resume_if_suspended() noexcept {
            if (linked_state_ && linked_state_->is_ready()) {
                linked_state_->resume_coroutine();
            }
        }

        /// @brief Check if there's a suspended coroutine
        /// @return true if linked_state exists and has a coroutine
        [[nodiscard]] bool has_suspended_coroutine() const noexcept {
            return linked_state_ && linked_state_->has_coroutine();
        }

        /// @brief Get linked state pointer (for debugging/testing)
        [[nodiscard]] detail::future_state_base* linked_state() const noexcept {
            return linked_state_;
        }

    private:
        action handler_;
        detail::future_state_base* linked_state_;  // Non-owning pointer to future_state
    };

    template<class Value>
    behavior_t make_behavior(actor_zeta::pmr::memory_resource* resource, Value&& f) {
        return {resource, make_handler(resource,std::forward<Value>(f))};
    }


    template<class Actor, typename F>
    behavior_t make_behavior(actor_zeta::pmr::memory_resource* resource, Actor* ptr, F&& f) {
        return {resource, make_handler(resource,ptr, std::forward<F>(f))};
    }

    /// @brief Resume all suspended coroutines in given behaviors
    /// @note Helper function to reduce boilerplate in behavior() method
    ///
    /// Example:
    /// @code
    /// void behavior(message* msg) override {
    ///     resume_all(power_, factorial_);  // Resume async behaviors
    ///     switch (msg->command()) { ... }
    /// }
    /// @endcode
    template<typename... Behaviors>
    void resume_all(Behaviors&... behaviors) noexcept {
        // Fold expression: call resume_if_suspended() for each behavior
        ([&]() {
            behaviors.resume_if_suspended();
        }(), ...);
    }

    /// @brief Resume suspended coroutines conditionally
    /// @param predicate Function that returns true if behavior should be resumed
    /// @param behaviors Behaviors to check and resume
    ///
    /// Example:
    /// @code
    /// resume_all_if([](auto& b) { return b.has_suspended_coroutine(); },
    ///               power_, factorial_);
    /// @endcode
    template<typename Predicate, typename... Behaviors>
    void resume_all_if(Predicate&& pred, Behaviors&... behaviors) noexcept {
        ([&]() {
            if (pred(behaviors)) {
                behaviors.resume_if_suspended();
            }
        }(), ...);
    }

}} // namespace actor_zeta::base

namespace actor_zeta {
    using base::resume_all;
    using base::resume_all_if;
}