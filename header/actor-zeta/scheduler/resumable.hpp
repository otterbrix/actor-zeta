#pragma once

#include <cstddef>

namespace actor_zeta {

    using max_throughput_t = size_t;

    namespace scheduler {

        /// @brief Result of resume operation
        enum class resume_result {
            resume,   ///< Job needs to be resumed again (has more work)
            awaiting, ///< Job is waiting for external event (blocked)
            done,     ///< Job is completed
            shutdown  ///< Job is shutdown sentinel
        };

        /// @brief Extended resume result with execution statistics
        ///
        /// Contains both the execution status and the number of messages processed.
        /// Supports implicit conversion to resume_result for backward compatibility.
        struct resume_info {
            resume_result result;      ///< Execution status
            size_t messages_processed; ///< Number of messages processed in this resume call

            resume_info() noexcept
                : result(resume_result::done)
                , messages_processed(0) {
            }

            resume_info(resume_result r, size_t processed = 0) noexcept
                : result(r)
                , messages_processed(processed) {
            }

            /// @brief Implicit conversion to resume_result for backward compatibility
            operator resume_result() const noexcept {
                return result;
            }
        };

    } // namespace scheduler
} // namespace actor_zeta