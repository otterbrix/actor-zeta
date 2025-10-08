#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>

namespace actor_zeta { namespace detail {

    template<class T, size_t Size>
    class ring_buffer final {
    public:
        using guard_type = std::unique_lock<std::mutex>;

        ring_buffer()
            : write_pos_(0)
            , read_pos_(0) {
        }

        void wait_non_empty() {
            if (!empty()) {
                return;
            }
            guard_type guard{mtx_};
            while (empty()) {
                cv_empty_.wait(guard);
            }
        }

        template<class TimePoint>
        bool wait_non_empty(TimePoint timeout) {
            if (!empty()) {
                return true;
            }
            auto pred = [&] { return !empty(); };
            guard_type guard{mtx_};
            return cv_empty_.wait_until(guard, timeout, pred);
        }

        T& front() {
            return buffer_[read_pos_];
        }

        void pop_front() {
            guard_type guard{mtx_};
            auto rp = read_pos_.load();
            read_pos_ = next(rp);
            if (rp == next(write_pos_)) {
                cv_full_.notify_all();
            }
        }

        template<class OutputIterator>
        OutputIterator get_all(OutputIterator i) {
            auto first = read_pos_.load();
            auto last = write_pos_.load();
            size_t n;
            assert(first != last);

            if (first < last) {
                n = last - first;
                for (auto j = first; j != last; ++j)
                    *i++ = std::move(buffer_[j]);
            } else {
                n = (Size - first) + last;
                for (size_t j = first; j != Size; ++j) {
                    *i++ = std::move(buffer_[j]);
                }
                for (size_t j = 0; j != last; ++j) {
                    *i++ = std::move(buffer_[j]);
                }
            }
            guard_type guard{mtx_};
            read_pos_ = (first + n) % Size;

            if (first == next(write_pos_)) {
                cv_full_.notify_all();
            }

            return i;
        }

        void push_back(T&& arg) {
            guard_type guard{mtx_};
            while (full()) {
                cv_full_.wait(guard);
            }
            auto wp = write_pos_.load();
            buffer_[wp] = std::move(arg);
            write_pos_ = next(wp);
            if (read_pos_ == wp) {
                cv_empty_.notify_all();
            }
        }

        template<class... Args>
        void emplace_back(Args&&... args) {
            push_back(T{std::forward<Args>(args)...});
        }

        bool empty() const noexcept {
            return read_pos_ == write_pos_;
        }

        bool full() const noexcept {
            return read_pos_ == next(write_pos_);
        }

        size_t size() const noexcept {
            auto rp = read_pos_.load();
            auto wp = write_pos_.load();

            if (rp == wp) {
                return 0;
            }

            if (rp < wp) {
                return wp - rp;
            }

            return Size - rp + wp;
        }

    private:
        static size_t next(size_t pos) {
            return (pos + 1) % Size;
        }

        mutable std::mutex mtx_;
        std::condition_variable cv_empty_;
        std::condition_variable cv_full_;
        std::atomic<size_t> write_pos_;
        std::atomic<size_t> read_pos_;
        std::array<T, Size> buffer_;
    };

}} // namespace actor_zeta::detail