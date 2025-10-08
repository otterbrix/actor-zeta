#pragma once

#include <memory>
#include <thread>
#include <vector>

#include <actor-zeta/detail/ring_buffer.hpp>
#include <actor-zeta/clock/clock.hpp>

namespace actor_zeta { namespace clock {

    class thread_safe_clock_t final : public clock_t {
    public:
        static constexpr size_t buffer_size = 64;

        struct schedule_entry final {
            bool done;
            time_point t;
            handler f;
            duration_type period;
        };

        using schedule_entry_ptr = std::unique_ptr<schedule_entry>;

        thread_safe_clock_t();
        void start_dispatch_loop();
        void stop_dispatch_loop();

    private:
        void schedule_periodically(time_point, handler, duration_type) override;
        void run();
        detail::ring_buffer<schedule_entry_ptr, buffer_size> queue_;
        std::thread dispatcher_;
        std::atomic_bool running_{true};
        std::vector<schedule_entry_ptr> tbl_;
    };

}} // namespace actor_zeta::clock
