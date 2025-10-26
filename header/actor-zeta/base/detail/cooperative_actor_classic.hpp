#pragma once

#include "traits_actor.hpp"
#include <actor-zeta/base/actor_abstract.hpp>
#include <actor-zeta/base/behavior.hpp>
#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/detail/memory.hpp>
#include <actor-zeta/detail/type_traits.hpp>
#include <actor-zeta/detail/slot_refcount.hpp>
#include <actor-zeta/scheduler/resumable.hpp>
#include <actor-zeta/detail/ignore_unused.hpp>
#include <actor-zeta/detail/queue/enqueue_result.hpp>

#include <actor-zeta/mailbox/mailbox.hpp>
#include <actor-zeta/mailbox/default_mailbox.hpp>

#include <thread>
#include <chrono>

namespace actor_zeta { namespace base {

    template<class Target>
    Target* check_ptr(Target* ptr) {
        assert(ptr);
        return ptr;
    }

    template<class Actor, class MailBox>
    class cooperative_actor<Actor, MailBox, actor_type::classic>
        : public actor_abstract_t {
    private:
        // Ленивая проверка dispatch_traits - инстанцируется только при вызове
        static constexpr bool check_dispatch_traits_exists() {
            using dispatch_traits_check = typename Actor::dispatch_traits;
            (void)sizeof(dispatch_traits_check); // Suppress unused warning
            return true;
        }

    public:
        using mailbox_t = MailBox;
        using unique_actor = std::unique_ptr<cooperative_actor<Actor, MailBox, actor_type::classic>, pmr::deleter_t>;

        // Forward declarations for promise/future
        template<typename T> class promise;
        template<typename T> class unique_future;

        /// @brief Promise - write interface for async operations
        template<typename T>
        class promise final {
        public:
            promise() = delete;
            promise(const promise&) = delete;
            promise& operator=(const promise&) = delete;

            /// @brief Construct promise from slot pointer
            explicit promise(detail::slot_refcount* slot, pmr::memory_resource* res) noexcept
                : slot_(slot)
                , resource_(res) {
                assert(slot_ && "promise constructed with null slot");
            }

            /// @brief Move constructor
            promise(promise&& other) noexcept
                : slot_(other.slot_)
                , resource_(other.resource_) {
                other.slot_ = nullptr;
            }

            /// @brief Move assignment
            promise& operator=(promise&& other) noexcept {
                if (this != &other) {
                    slot_ = other.slot_;
                    resource_ = other.resource_;
                    other.slot_ = nullptr;
                }
                return *this;
            }

            ~promise() noexcept = default;

            /// @brief Set successful result
            void set_value(T&& value) {
                assert(slot_ && "set_value() on moved-from promise");
                slot_->set_result(detail::rtt(resource_, std::forward<T>(value)));
            }

            void set_value(const T& value) {
                assert(slot_ && "set_value() on moved-from promise");
                slot_->set_result(detail::rtt(resource_, value));
            }

            /// @brief Check if promise is valid
            [[nodiscard]] bool is_valid() const noexcept {
                return slot_ != nullptr;
            }

            /// @brief Get slot pointer (does NOT transfer ownership)
            [[nodiscard]] detail::slot_refcount* slot() const noexcept {
                return slot_;
            }

        private:
            detail::slot_refcount* slot_;
            pmr::memory_resource* resource_;
        };

        /// @brief Promise specialization for void
        template<>
        class promise<void> final {
        public:
            promise() = delete;
            promise(const promise&) = delete;
            promise& operator=(const promise&) = delete;

            /// @brief Construct promise from slot pointer
            explicit promise(detail::slot_refcount* slot, pmr::memory_resource* res) noexcept
                : slot_(slot)
                , resource_(res) {
                assert(slot_ && "promise<void> constructed with null slot");
            }

            /// @brief Move constructor
            promise(promise&& other) noexcept
                : slot_(other.slot_)
                , resource_(other.resource_) {
                other.slot_ = nullptr;
            }

            /// @brief Move assignment
            promise& operator=(promise&& other) noexcept {
                if (this != &other) {
                    slot_ = other.slot_;
                    resource_ = other.resource_;
                    other.slot_ = nullptr;
                }
                return *this;
            }

            ~promise() noexcept = default;

            /// @brief Set successful result (void - no value, store dummy)
            void set_value() {
                assert(slot_ && "set_value() on moved-from promise<void>");
                slot_->set_result(detail::rtt(resource_, int{0}));  // Dummy value for void
            }

            /// @brief Check if promise is valid
            [[nodiscard]] bool is_valid() const noexcept {
                return slot_ != nullptr;
            }

            /// @brief Get slot pointer (does NOT transfer ownership)
            [[nodiscard]] detail::slot_refcount* slot() const noexcept {
                return slot_;
            }

        private:
            detail::slot_refcount* slot_;
            pmr::memory_resource* resource_;
        };

        /// @brief Future for async results
        template<typename T>
        class unique_future final {
        public:
            explicit unique_future(pmr::memory_resource* /*res*/) noexcept
                : slot_(nullptr)
                , needs_scheduling_(false) {
            }

            unique_future(const unique_future&) = delete;
            unique_future& operator=(const unique_future&) = delete;

            explicit unique_future(detail::slot_refcount* slot, bool needs_sched = false) noexcept
                : slot_(slot)
                , needs_scheduling_(needs_sched) {
            }

            /// @brief Move constructor
            unique_future(unique_future&& other) noexcept
                : slot_(other.slot_)
                , needs_scheduling_(other.needs_scheduling_) {
                other.slot_ = nullptr;
                other.needs_scheduling_ = false;
            }

            /// @brief Move assignment
            unique_future& operator=(unique_future&& other) noexcept {
                if (this != &other) {
                    // Release current slot if any
                    if (slot_) {
                        slot_->release();
                    }

                    // Transfer ownership
                    slot_ = other.slot_;
                    needs_scheduling_ = other.needs_scheduling_;
                    other.slot_ = nullptr;
                    other.needs_scheduling_ = false;
                }
                return *this;
            }

            /// @brief Destructor - releases slot reference
            ~unique_future() noexcept {
                if (slot_) {
                    slot_->release();
                }
            }

            /// @brief Blocking get - wait for result with exponential backoff
            T get() && {
                assert(slot_ && "get() on invalid future");

                // Exponential backoff waiting strategy
                auto backoff = std::chrono::microseconds(1);
                constexpr auto max_backoff = std::chrono::microseconds(1000);

                while (!slot_->is_ready()) {
                    std::this_thread::sleep_for(backoff);
                    if (backoff < max_backoff) {
                        backoff *= 2;
                    }
                }

                // Extract result from rtt
                return slot_->result().template get<T>(0);
            }

            /// @brief Lvalue get() DELETED
            T get() & = delete;

            /// @brief Check if ready
            [[nodiscard]] bool is_ready() const noexcept {
                return slot_ && slot_->is_ready();
            }

            /// @brief Check if valid
            [[nodiscard]] bool valid() const noexcept {
                return slot_ != nullptr;
            }

            /// @brief Check if needs scheduling
            [[nodiscard]] bool needs_scheduling() const noexcept {
                return needs_scheduling_;
            }

        private:
            detail::slot_refcount* slot_;  // Slot ownership (null after move or invalid future)
            bool needs_scheduling_;
        };

        /// @brief unique_future specialization for void - direct slot ownership
        template<>
        class unique_future<void> final {
        public:
            /// @brief Constructor for invalid future - requires memory_resource
            explicit unique_future(pmr::memory_resource* /*res*/) noexcept
                : slot_(nullptr)
                , needs_scheduling_(false) {
            }

            /// @brief Construct from slot pointer
            /// @param slot Slot pointer (ownership transferred)
            /// @param needs_sched true if actor was unblocked (needs scheduling)
            explicit unique_future(detail::slot_refcount* slot, bool needs_sched = false) noexcept
                : slot_(slot)
                , needs_scheduling_(needs_sched) {
            }

            unique_future(const unique_future&) = delete;
            unique_future& operator=(const unique_future&) = delete;

            /// @brief Move constructor
            unique_future(unique_future&& other) noexcept
                : slot_(other.slot_)
                , needs_scheduling_(other.needs_scheduling_) {
                other.slot_ = nullptr;
                other.needs_scheduling_ = false;
            }

            /// @brief Move assignment
            unique_future& operator=(unique_future&& other) noexcept {
                if (this != &other) {
                    // Release current slot if any
                    if (slot_) {
                        slot_->release();
                    }

                    // Transfer ownership
                    slot_ = other.slot_;
                    needs_scheduling_ = other.needs_scheduling_;
                    other.slot_ = nullptr;
                    other.needs_scheduling_ = false;
                }
                return *this;
            }

            /// @brief Destructor - releases slot reference
            ~unique_future() noexcept {
                if (slot_) {
                    slot_->release();
                }
            }

            /// @brief Blocking get - wait for completion with exponential backoff
            void get() && {
                assert(slot_ && "get() on invalid future");

                auto backoff = std::chrono::microseconds(1);
                constexpr auto max_backoff = std::chrono::microseconds(1000);

                while (!slot_->is_ready()) {
                    std::this_thread::sleep_for(backoff);
                    if (backoff < max_backoff) {
                        backoff *= 2;
                    }
                }
            }

            /// @brief Lvalue get() DELETED - future is consumable
            void get() & = delete;

            /// @brief Non-blocking check if result is ready
            [[nodiscard]] bool is_ready() const noexcept {
                return slot_ && slot_->is_ready();
            }

            /// @brief Check if future is valid (enqueue succeeded)
            [[nodiscard]] bool valid() const noexcept {
                return slot_ != nullptr;
            }

            /// @brief Check if actor needs scheduling
            /// @return true if actor was unblocked (needs scheduler->enqueue()), false otherwise
            [[nodiscard]] bool needs_scheduling() const noexcept {
                return needs_scheduling_;
            }

        private:
            detail::slot_refcount* slot_;  // Slot ownership (null after move or invalid future)
            bool needs_scheduling_;
        };

        /// @brief Enqueue message and return future
        template<typename R>
        unique_future<R> enqueue_impl(mailbox::message_ptr msg) {
            assert(msg.get() != nullptr);

            void* mem = resource()->allocate(sizeof(detail::slot_refcount), alignof(detail::slot_refcount));
            auto* slot = new (mem) detail::slot_refcount(resource());
            msg->set_result_slot(slot);

            auto result = mailbox().push_back(std::move(msg));
            switch (result) {
                case detail::enqueue_result::unblocked_reader:
                    return unique_future<R>{slot, true};

                case detail::enqueue_result::success:
                    return unique_future<R>{slot, false};

                case detail::enqueue_result::queue_closed:
                    slot->release();
                    return unique_future<R>{resource()};

                default:
                    assert(false && "enqueue_result: unreachable");
                    slot->release();
                    return unique_future<R>{resource()};
            }
        }

        /// @brief Resume execution - process messages from mailbox
        /// @param max_throughput Maximum number of messages to process
        /// @return Resume information with status and messages processed
        ///
        /// Flow scenarios:
        /// A) Scheduled after enqueue: inbox is unblocked, has messages
        /// B) Batch: many enqueue() calls, then single schedule - inbox unblocked with multiple messages
        /// C) Synchronous: direct resume() call on fresh actor - inbox may be blocked
        scheduler::resume_info resume(size_t max_throughput) noexcept {
            assert(max_throughput > 0 && "max_throughput must be greater than 0");

            // CONCURRENT RESUME DETECTION: RAII guard to detect concurrent resume()
            struct resume_guard {
                std::atomic<bool>& flag;
                explicit resume_guard(std::atomic<bool>& f) : flag(f) {
                    bool expected = false;
                    if (!flag.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
                        assert(false && "Concurrent resume() detected - scheduler BUG!");
                    }
                }
                ~resume_guard() {
                    flag.store(false, std::memory_order_release);
                }
            };
            resume_guard guard(resuming_);

            size_t handled = 0;

            // Check if inbox is closed first (shutdown scenario)
            if (mailbox().closed()) {
                return scheduler::resume_info(scheduler::resume_result::done, 0);
            }

            // Check if blocked - can happen in:
            // - Synchronous resume() on fresh actor (Flow C)
            // - Spurious scheduler wakeup (rare)
            // For scheduled actors (Flow A, B), inbox is already unblocked by enqueue()
            if (mailbox().blocked()) {
                return scheduler::resume_info(scheduler::resume_result::awaiting, 0);
            }

            if (mailbox().empty()) {
                auto result = mailbox().try_block()
                                  ? scheduler::resume_result::awaiting
                                  : scheduler::resume_result::resume;
                return scheduler::resume_info(result, 0);
            }

            while (handled < max_throughput) {
                // Check if inbox closed during processing
                if (mailbox().closed()) {
                    return scheduler::resume_info(scheduler::resume_result::done, handled);
                }

                // CRITICAL: Check if blocked before pop_front() to avoid assertion
                // Can happen in multithreaded scenario with concurrent enqueue()
                if (mailbox().blocked()) {
                    return scheduler::resume_info(scheduler::resume_result::awaiting, handled);
                }

                const size_t before = handled;

                auto msg = mailbox().pop_front();
                if (msg) {
                    // RAII guard for message processing
                    struct message_guard {
                        cooperative_actor* actor_;
                        mailbox::message_ptr message_;
                        mailbox::message* prev_message_;

                        message_guard(cooperative_actor* actor, mailbox::message_ptr msg) noexcept
                            : actor_(actor)
                            , message_(std::move(msg))
                            , prev_message_(actor->current_message_) {
                            actor_->current_message_ = message_.get();
                        }

                        ~message_guard() noexcept {
                            actor_->current_message_ = prev_message_;

                            // Release actor's refcount if message has result slot
                            if (message_ && message_->result_slot()) {
                                message_->result_slot()->release();
                            }
                        }

                        mailbox::message* get() const noexcept { return message_.get(); }
                    };

                    message_guard guard(this, std::move(msg));

                    // Check for cancelled messages - skip processing if cancelled
                    if (!guard.get()->is_cancelled()) {
                        // Normal processing (includes orphaned messages - still process them)
                        self()->behavior(guard.get());
                    }

                    ++handled;
                }

                if (handled == before) {
                    // Check again before try_block
                    if (mailbox().closed()) {
                        return scheduler::resume_info(scheduler::resume_result::done, handled);
                    }
                    auto result = mailbox().try_block()
                                      ? scheduler::resume_result::awaiting
                                      : scheduler::resume_result::resume;
                    return scheduler::resume_info(result, handled);
                }
            }

            // Check before final try_block
            if (mailbox().closed()) {
                return scheduler::resume_info(scheduler::resume_result::done, handled);
            }
            auto result = mailbox().try_block()
                              ? scheduler::resume_result::awaiting
                              : scheduler::resume_result::resume;
            return scheduler::resume_info(result, handled);
        }

        ~cooperative_actor() override {
            // Fail-fast: Assert that all futures are resolved before actor destruction
            // Use acquire ordering to ensure we see all decrements from future destructors
            assert(pending_futures_count_.load(std::memory_order_acquire) == 0
                   && "Actor destroyed with pending futures - caller must ensure all futures are resolved");
        }

    protected:
        explicit cooperative_actor(pmr::memory_resource* in_resource)
            : actor_abstract_t(check_ptr(in_resource))
            , current_message_(nullptr)
            , mailbox_() {
            // Проверка наличия dispatch_traits (Actor уже полностью определен здесь)
            static_assert(check_dispatch_traits_exists(),
                "Actor must define nested 'struct dispatch_traits { using methods = type_list<...>; }'");
            mailbox().try_block();
        }

    private:
        mailbox::message* current_message() noexcept { return current_message_; }

        inline const Actor* self() const noexcept {
            return static_cast<const Actor*>(this);
        }

        inline Actor* self() noexcept {
            return static_cast<Actor*>(this);
        }

        mailbox_t& mailbox() noexcept {
            return mailbox_;
        }

        /// @brief Increment pending futures counter
        /// Called when a new future is created for this actor
        /// Only accessible by nested unique_future class
        void increment_pending_futures() noexcept {
            pending_futures_count_.fetch_add(1, std::memory_order_relaxed);
        }

        /// @brief Decrement pending futures counter
        /// Called when a future is resolved or destroyed
        /// Only accessible by nested unique_future class
        void decrement_pending_futures() noexcept {
            pending_futures_count_.fetch_sub(1, std::memory_order_relaxed);
        }

        /// @brief Get current pending futures count
        /// @return Number of unresolved futures
        size_t pending_futures_count() const noexcept {
            return pending_futures_count_.load(std::memory_order_relaxed);
        }

        mailbox::message* current_message_;
        mailbox_t mailbox_;
        std::atomic<size_t> pending_futures_count_{0};
        std::atomic<bool> resuming_{false};  // Concurrent resume() detection
    };

}} // namespace actor_zeta::base
