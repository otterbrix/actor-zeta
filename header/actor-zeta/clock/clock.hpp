#pragma once

#include <chrono>
#include <string>

#include <actor-zeta/base/forwards.hpp>
#include <actor-zeta/mailbox/message.hpp>
#include <actor-zeta/detail/unique_function.hpp>

namespace actor_zeta { namespace clock {

    using handler = detail::unique_function<void()>;
    using timespan = std::chrono::duration<int64_t, std::nano>;

    class clock_t {
    public:
        using clock_type = std::chrono::steady_clock;
        using time_point = typename clock_type::time_point;
        using duration_type = typename clock_type::duration;

        virtual ~clock_t();
        virtual time_point now() const noexcept;
        void schedule_message(time_point , base::address_t target, mailbox::message_ptr content);

    protected:
        virtual void schedule_periodically(time_point first_run, handler, duration_type period) = 0;
        void schedule(time_point , handler );
        void schedule(handler);
    };

}} // namespace actor_zeta::clock
