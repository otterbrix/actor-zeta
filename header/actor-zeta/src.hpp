#pragma once

// clang-format off
#include <actor-zeta.hpp>

#include <cstdint>
#include <actor-zeta/detail/impl/pmr/default_resource.ipp>
#include <actor-zeta/detail/pmr/polymorphic_allocator.hpp>

#include <actor-zeta/impl/actor/actor_abstract.ipp>
#include <actor-zeta/impl/actor/address.ipp>
#include <actor-zeta/impl/actor/behavior.ipp>
#include <actor-zeta/impl/actor/cooperative_actor.ipp>

#include <actor-zeta/impl/detail/pmr/memory_resource.ipp>
#include <actor-zeta/impl/detail/aligned_allocate.ipp>
#include <actor-zeta/impl/detail/launch_thread.ipp>
#include <actor-zeta/impl/detail/ref_counted.ipp>

#include <actor-zeta/impl/scheduler/resumable.ipp>
#include <actor-zeta/impl/scheduler/sharing_scheduler.ipp>

#include <actor-zeta/impl/mailbox/id.ipp>
#include <actor-zeta/impl/mailbox/message.ipp>

#include <actor-zeta/impl/make_message.ipp>
#include <actor-zeta/impl/send.ipp>
// clang-format on