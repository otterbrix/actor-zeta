#pragma once

#include <cstddef>

namespace actor_zeta {

    using max_throughput_t = size_t;

    namespace scheduler {

        enum class resume_result {
            resume,
            awaiting,
            done,
            shutdown
        };

        struct resume_info {
            resume_result result;
            size_t messages_processed;

            resume_info() noexcept
                : result(resume_result::done)
                , messages_processed(0) {
            }

            resume_info(resume_result r, size_t processed = 0) noexcept
                : result(r)
                , messages_processed(processed) {
            }

            operator resume_result() const noexcept {
                return result;
            }
        };

    } // namespace scheduler
} // namespace actor_zeta