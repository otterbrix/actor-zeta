#pragma once
#include "actor-zeta/clock/clock_thread_safe.hpp"
#include "actor-zeta/detail/launch_thread.hpp"

#include <algorithm>

namespace actor_zeta { namespace clock {

    thread_safe_clock_t::thread_safe_clock_t() {
        tbl_.reserve(buffer_size * 2);
    }

    void thread_safe_clock_t::schedule_periodically(time_point first_run, handler f,duration_type period) {
        auto ptr = schedule_entry_ptr{new schedule_entry{false,first_run, std::move(f), period}};
        queue_.emplace_back(std::move(ptr));
    }

    void thread_safe_clock_t::run() {
        auto is_done = [](const schedule_entry_ptr& entry) -> bool {
            if (entry) {
                return entry->done;
            }

            return true;
        };
        auto by_timeout = [](const schedule_entry_ptr& x, const schedule_entry_ptr& y) -> bool { return x->t < y->t; };
        while (running_) {
            if (tbl_.empty()) {
                queue_.wait_non_empty();
                queue_.get_all(std::back_inserter(tbl_));
                std::sort(tbl_.begin(), tbl_.end(), by_timeout);
            } else {
                auto next_timeout = (*tbl_.begin())->t;
                if (queue_.wait_non_empty(next_timeout)) {
                    queue_.get_all(std::back_inserter(tbl_));
                    std::sort(tbl_.begin(), tbl_.end(), by_timeout);
                }
            }
            auto n = now();
            for (auto i = tbl_.begin(); i != tbl_.end() && (*i)->t <= n; ++i) {
                auto& entry = **i;
                entry.f();
                if (entry.period.count() > 0) {
                    auto next = entry.t + entry.period;
                    while (next <= n) {
                        next += entry.period;
                    }
                    entry.t = next;
                } else {
                    entry.done = true; // Remove from tbl_ after the for-loop body.
                    ///i->reset();
                }
            }
            tbl_.erase(std::remove_if(tbl_.begin(), tbl_.end(), is_done), tbl_.end());
        }
    }

    void thread_safe_clock_t::start_dispatch_loop() {
        dispatcher_ = detail::launch_thread("clock", [this] { run(); });
    }

    void thread_safe_clock_t::stop_dispatch_loop() {
        schedule(handler([this]() { running_ = false; }));
        dispatcher_.join();
    }

}} // namespace actor_zeta::clock
