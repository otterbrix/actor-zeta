#pragma once

#include <cassert>
#include <cstddef>

#include <actor-zeta/detail/memory_resource.hpp>

namespace actor_zeta { namespace detail {

    template<class Signature>
    class unique_function;

    template<class R, class... Args>
    class unique_function<R(Args...)> final {
    private:
        using invoke_fn_t = R (*)(void*, Args&&...);
        using destroy_fn_t = void (*)(void*, pmr::memory_resource*);
        static constexpr std::size_t buffer_size = 3 * sizeof(void*);

        union {
            alignas(alignof(std::max_align_t)) char small_buffer_[buffer_size];
            struct {
                void* ptr_;
                pmr::memory_resource* res_;
            } large_obj_;
        };

        invoke_fn_t invoke_ = nullptr;
        destroy_fn_t destroy_ = nullptr;
        bool uses_small_buffer_ = false;

        void* small_buffer() noexcept {
            return static_cast<void*>(small_buffer_);
        }

        const void* small_buffer() const noexcept {
            return static_cast<const void*>(small_buffer_);
        }

        template<typename F>
        static R do_invoke(void* obj_ptr, Args&&... args) noexcept(noexcept(
            std::declval<F&>()(std::declval<Args>()...))) {
            F& f = *static_cast<F*>(obj_ptr);
            return f(std::forward<Args>(args)...);
        }

        template<typename F>
        static void do_destroy_small(void* obj_ptr, pmr::memory_resource*) noexcept {
            F& f = *static_cast<F*>(obj_ptr);
            f.~F();
        }

        template<typename F>
        static void do_destroy_large(void* obj_ptr, pmr::memory_resource* resource) noexcept {
            F& f = *static_cast<F*>(obj_ptr);
            f.~F();

            if (obj_ptr != nullptr && resource != nullptr) {
                resource->deallocate(obj_ptr, sizeof(F), alignof(F));
            }
        }

        void destroy() noexcept {
            if (destroy_) {
                if (uses_small_buffer_) {
                    destroy_(small_buffer(), nullptr);
                } else {
                    destroy_(large_obj_.ptr_, large_obj_.res_);
                }
                destroy_ = nullptr;
                invoke_ = nullptr;
            }
        }

        template<typename F>
        static constexpr bool is_small_object() {
            return sizeof(F) <= buffer_size &&
                   std::is_nothrow_move_constructible<F>::value &&
                   alignof(F) <= alignof(std::max_align_t) &&
                   std::is_nothrow_destructible<F>::value;
        }

        template<typename F>
        void init_from_functor(F&& f, pmr::memory_resource* resource) {
            using DecayedF = typename std::decay<F>::type;

            if (is_small_object<DecayedF>()) {
                uses_small_buffer_ = true;
                new (small_buffer()) DecayedF(std::forward<F>(f));
                invoke_ = &do_invoke<DecayedF>;
                destroy_ = &do_destroy_small<DecayedF>;
            } else {
                if (!resource) {
                    assert(0 && "Memory resource required for large objects");
                }

                uses_small_buffer_ = false;
                void* new_memory = resource->allocate(sizeof(DecayedF), alignof(DecayedF));
                new (new_memory) DecayedF(std::forward<F>(f));
                large_obj_.ptr_ = new_memory;
                large_obj_.res_ = resource;
                invoke_ = &do_invoke<DecayedF>;
                destroy_ = &do_destroy_large<DecayedF>;
            }
        }

    public:
        unique_function() = delete;
        explicit unique_function(pmr::memory_resource* resource) = delete;
        unique_function(pmr::memory_resource* resource, std::nullptr_t) = delete;

        unique_function(unique_function&& other) noexcept
            : invoke_(other.invoke_)
            , destroy_(other.destroy_)
            , uses_small_buffer_(other.uses_small_buffer_) {
            if (other.destroy_) {
                if (uses_small_buffer_) {
                    std::memcpy(small_buffer_, other.small_buffer_, buffer_size);
                } else {
                    large_obj_.ptr_ = other.large_obj_.ptr_;
                    large_obj_.res_ = other.large_obj_.res_;
                    other.large_obj_.ptr_ = nullptr;
                }
                other.invoke_ = nullptr;
                other.destroy_ = nullptr;
            }
        }

        unique_function(pmr::memory_resource* resource, unique_function&& other)
            : invoke_(nullptr)
            , destroy_(nullptr)
            , uses_small_buffer_(true) {
            if (!resource) {
                assert(0 && "Memory resource cannot be null");
                return;
            }

            if (other.destroy_) {
                if (other.uses_small_buffer_) {
                    // Small objects always move successfully
                    uses_small_buffer_ = true;
                    invoke_ = other.invoke_;
                    destroy_ = other.destroy_;
                    std::memcpy(small_buffer_, other.small_buffer_, buffer_size);

                    // Source becomes empty
                    other.invoke_ = nullptr;
                    other.destroy_ = nullptr;
                } else {
                    // Large objects require compatible resources
                    if (other.large_obj_.res_ != resource) {
                        // With different resources - explicitly free source memory
                        // and make both objects empty

                        // Save pointers for deallocation
                        void* ptr = other.large_obj_.ptr_;
                        pmr::memory_resource* res = other.large_obj_.res_;
                        destroy_fn_t destroy_fn = other.destroy_;

                        // Make source empty
                        other.invoke_ = nullptr;
                        other.destroy_ = nullptr;
                        other.large_obj_.ptr_ = nullptr;

                        // Free source memory via its destroy function
                        destroy_fn(ptr, res);

                        // Target object remains empty
                    } else {
                        // With compatible resources - standard move
                        uses_small_buffer_ = false;
                        invoke_ = other.invoke_;
                        destroy_ = other.destroy_;
                        large_obj_.ptr_ = other.large_obj_.ptr_;
                        large_obj_.res_ = resource;
                        other.large_obj_.ptr_ = nullptr;
                        other.invoke_ = nullptr;
                        other.destroy_ = nullptr;
                    }
                }
            }
        }

        unique_function(const unique_function&) = delete;

        template<typename F,
                 typename = typename std::enable_if<
                     !std::is_same<typename std::decay<F>::type, unique_function>::value>::type>
        unique_function(pmr::memory_resource* resource, F&& f) {
            if (!resource) {
                assert(0 && "Memory resource cannot be null");
            }

            static_assert(std::is_same<
                              decltype((std::declval<typename std::decay<F>::type&>()(std::declval<Args>()...))),
                              R>::value,
                          "Function signature does not match");

            if (!std::is_same<typename std::decay<F>::type, std::nullptr_t>::value) {
                init_from_functor(std::forward<F>(f), resource);
            }
        }

        unique_function(pmr::memory_resource* resource, R (*func)(Args...)) {
            if (!resource) {
                assert(0 && "Memory resource cannot be null");
            }

            if (func) {
                init_from_functor(func, resource);
            }
        }

        ~unique_function() {
            destroy();
        }

        unique_function& operator=(unique_function&& other) noexcept {
            if (this != &other) {
                destroy();

                if (other.destroy_) {
                    if (other.uses_small_buffer_) {
                        // Small objects always move successfully
                        uses_small_buffer_ = true;
                        invoke_ = other.invoke_;
                        destroy_ = other.destroy_;
                        std::memcpy(small_buffer_, other.small_buffer_, buffer_size);

                        // Source becomes empty
                        other.invoke_ = nullptr;
                        other.destroy_ = nullptr;
                    } else {
                        // Large objects require compatible resources
                        if (!uses_small_buffer_ && large_obj_.res_ != other.large_obj_.res_) {
                            // With different resources - explicitly free source memory

                            // Save pointers for deallocation
                            void* ptr = other.large_obj_.ptr_;
                            pmr::memory_resource* res = other.large_obj_.res_;
                            destroy_fn_t destroy_fn = other.destroy_;

                            // Make source empty
                            other.invoke_ = nullptr;
                            other.destroy_ = nullptr;
                            other.large_obj_.ptr_ = nullptr;

                            // Free source memory via its destroy function
                            destroy_fn(ptr, res);

                            // Target object remains empty after destroy()
                        } else {
                            // With compatible resources - standard move
                            uses_small_buffer_ = false;
                            invoke_ = other.invoke_;
                            destroy_ = other.destroy_;
                            large_obj_.ptr_ = other.large_obj_.ptr_;
                            large_obj_.res_ = other.large_obj_.res_;
                            other.large_obj_.ptr_ = nullptr;
                            other.invoke_ = nullptr;
                            other.destroy_ = nullptr;
                        }
                    }
                }
            }
            return *this;
        }

        unique_function& operator=(const unique_function&) = delete;

        unique_function& operator=(std::nullptr_t) noexcept {
            destroy();
            return *this;
        }

        template<typename F>
        unique_function& operator=(F&& f) = delete;

        R operator()(Args... args) const { // todo fix:  R operator()(Args&&... args) const {
            if (!invoke_) {
                assert(0 && "bad_function_call");
            }

            if (uses_small_buffer_) {
                return invoke_(const_cast<void*>(small_buffer()), std::forward<Args>(args)...);
            } else {
                void* ptr = large_obj_.ptr_;
                if (!ptr) {
                    assert(0 && "bad_function_call");
                }
                return invoke_(ptr, std::forward<Args>(args)...);
            }
        }

        explicit operator bool() const noexcept {
            return invoke_ != nullptr;
        }

        bool operator!() const noexcept {
            return invoke_ == nullptr;
        }

        void swap(unique_function& other) noexcept {
            if (this == &other) {
                return;
            }

            if ((!uses_small_buffer_ && !other.uses_small_buffer_) &&
                large_obj_.res_ != other.large_obj_.res_) {
                return;
            }

            unique_function temp(std::move(*this));
            *this = std::move(other);
            other = std::move(temp);
        }

        void reset() noexcept {
            destroy();
        }

        bool empty() const noexcept {
            return invoke_ == nullptr;
        }

        bool uses_small_buffer() const noexcept {
            return uses_small_buffer_;
        }
    };

    template<class T>
    bool operator==(const unique_function<T>& x, std::nullptr_t) noexcept {
        return x.empty();
    }

    template<class T>
    bool operator==(std::nullptr_t, const unique_function<T>& x) noexcept {
        return x.empty();
    }

    template<class T>
    bool operator!=(const unique_function<T>& x, std::nullptr_t) noexcept {
        return !x.empty();
    }

    template<class T>
    bool operator!=(std::nullptr_t, const unique_function<T>& x) noexcept {
        return !x.empty();
    }

    template<class T>
    void swap(unique_function<T>& x, unique_function<T>& y) noexcept {
        x.swap(y);
    }

    template<class R, class... Args, class F>
    unique_function<R(Args...)> make_unique_function(pmr::memory_resource* resource, F&& f) {
        return unique_function<R(Args...)>(resource, std::forward<F>(f));
    }

}} // namespace actor_zeta::detail
