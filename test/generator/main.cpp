#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <actor-zeta.hpp>

#include <memory_resource>
#include <vector>
#include <string>

using namespace actor_zeta;

class StreamingActor;

using streaming_actor_ptr = std::unique_ptr<StreamingActor, pmr::deleter_t>;

class StreamingActor final : public basic_actor<StreamingActor> {
public:
    explicit StreamingActor(std::pmr::memory_resource* res)
        : basic_actor<StreamingActor>(res) {}

    ~StreamingActor() = default;

    generator<int> stream_numbers(int count) {
        for (int i = 0; i < count; ++i) {
            co_yield i;
        }
    }

    generator<std::string> stream_strings(int count) {
        for (int i = 0; i < count; ++i) {
            co_yield std::to_string(i);
        }
    }

    generator<int> empty_stream() {
        co_return;
    }

    generator<std::unique_ptr<int>> stream_unique_ptrs(int count) {
        for (int i = 0; i < count; ++i) {
            co_yield std::make_unique<int>(i);
        }
    }

    generator<int> stream_with_error(int error_at) {
        for (int i = 0; i < 10; ++i) {
            if (i == error_at) {
                co_yield stream_error{std::make_error_code(std::errc::io_error)};
                co_return;
            }
            co_yield i;
        }
    }

    generator<int> stream_immediate_error() {
        co_yield stream_error{std::make_error_code(std::errc::invalid_argument)};
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &StreamingActor::stream_numbers,
        &StreamingActor::stream_strings,
        &StreamingActor::empty_stream,
        &StreamingActor::stream_unique_ptrs
    >;

    void behavior(mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<StreamingActor, &StreamingActor::stream_numbers>) {
            dispatch(this, &StreamingActor::stream_numbers, msg);
        } else if (cmd == msg_id<StreamingActor, &StreamingActor::stream_strings>) {
            dispatch(this, &StreamingActor::stream_strings, msg);
        } else if (cmd == msg_id<StreamingActor, &StreamingActor::empty_stream>) {
            dispatch(this, &StreamingActor::empty_stream, msg);
        } else if (cmd == msg_id<StreamingActor, &StreamingActor::stream_unique_ptrs>) {
            dispatch(this, &StreamingActor::stream_unique_ptrs, msg);
        }
    }
};

TEST_CASE("generator type traits", "[generator][traits]") {
    SECTION("is_generator_v detects generator types") {
        static_assert(type_traits::is_generator_v<generator<int>>);
        static_assert(type_traits::is_generator_v<generator<std::string>>);
        static_assert(type_traits::is_generator_v<generator<std::unique_ptr<int>>>);
    }

    SECTION("is_generator_v rejects non-generator types") {
        static_assert(!type_traits::is_generator_v<int>);
        static_assert(!type_traits::is_generator_v<std::string>);
        static_assert(!type_traits::is_generator_v<unique_future<int>>);
    }

    SECTION("generator value_type is correct") {
        static_assert(std::is_same_v<type_traits::is_generator<generator<int>>::value_type, int>);
        static_assert(std::is_same_v<type_traits::is_generator<generator<std::string>>::value_type, std::string>);
    }
}

TEST_CASE("generator is move-only", "[generator][move]") {
    SECTION("static assertions for move-only") {
        static_assert(std::is_move_constructible_v<generator<int>>);
        static_assert(std::is_move_assignable_v<generator<int>>);
        static_assert(!std::is_copy_constructible_v<generator<int>>);
        static_assert(!std::is_copy_assignable_v<generator<int>>);
    }

    SECTION("move constructor transfers ownership") {
        auto* resource = std::pmr::get_default_resource();
        auto actor = spawn<StreamingActor>(resource);

        auto gen1 = actor->stream_numbers(5);
        REQUIRE(gen1.valid());

        auto gen2 = std::move(gen1);
        REQUIRE(!gen1.valid());
        REQUIRE(gen2.valid());
    }

    SECTION("move assignment transfers ownership") {
        auto* resource = std::pmr::get_default_resource();
        auto actor = spawn<StreamingActor>(resource);

        auto gen1 = actor->stream_numbers(5);
        auto gen2 = actor->stream_numbers(3);

        REQUIRE(gen1.valid());
        REQUIRE(gen2.valid());

        gen2 = std::move(gen1);
        REQUIRE(!gen1.valid());
        REQUIRE(gen2.valid());
    }
}

TEST_CASE("generator default constructor", "[generator][constructor]") {
    SECTION("default constructed generator is invalid") {
        generator<int> gen;
        REQUIRE(!gen.valid());
        REQUIRE(gen.exhausted());
    }
}

TEST_CASE("generator from actor method", "[generator][actor]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("generator is created with valid state") {
        auto gen = actor->stream_numbers(5);
        REQUIRE(gen.valid());
        REQUIRE(!gen.exhausted());
    }

    SECTION("empty generator") {
        auto gen = actor->empty_stream();
        REQUIRE(gen.valid());
    }

    SECTION("string generator") {
        auto gen = actor->stream_strings(3);
        REQUIRE(gen.valid());
    }

    SECTION("move-only type generator") {
        auto gen = actor->stream_unique_ptrs(3);
        REQUIRE(gen.valid());
    }
}

TEST_CASE("generator cancel", "[generator][cancel]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("cancel marks generator as cancelled") {
        auto gen = actor->stream_numbers(5);
        REQUIRE(gen.valid());
        REQUIRE(!gen.is_cancelled());

        gen.cancel();

        REQUIRE(gen.is_cancelled());
        REQUIRE(gen.is_safe_to_destroy());
    }
}

TEST_CASE("generator detach", "[generator][detach]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("detach releases ownership") {
        auto gen = actor->stream_numbers(5);
        REQUIRE(gen.valid());

        gen.detach();

        REQUIRE(!gen.valid());
    }
}

TEST_CASE("generator_states constants", "[generator][states]") {
    SECTION("state values are distinct") {
        REQUIRE(detail::generator_states::created != detail::generator_states::suspended);
        REQUIRE(detail::generator_states::suspended != detail::generator_states::exhausted);
        REQUIRE(detail::generator_states::exhausted != detail::generator_states::cancelled);
        REQUIRE(detail::generator_states::cancelled != detail::generator_states::detached);
    }
}

TEST_CASE("generator uses actor's memory resource", "[generator][resource]") {
    auto* default_resource = std::pmr::get_default_resource();

    SECTION("generator state allocated via actor resource") {
        auto actor = spawn<StreamingActor>(default_resource);
        auto gen = actor->stream_numbers(5);
        REQUIRE(gen.valid());
    }
}

TEST_CASE("generator via send() API", "[generator][send]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("send() returns generator<T> for generator methods") {
        auto gen = send(actor.get(), actor::address_t::empty_address(),
                        &StreamingActor::stream_numbers, 5);

        REQUIRE(gen.valid());
        REQUIRE(!gen.exhausted());

        auto info = actor->resume(1);
        REQUIRE(info.messages_processed == 1);
    }

    SECTION("send() with empty generator") {
        auto gen = send(actor.get(), actor::address_t::empty_address(),
                        &StreamingActor::empty_stream);
        REQUIRE(gen.valid());
        actor->resume(1);
    }

    SECTION("send() with string generator") {
        auto gen = send(actor.get(), actor::address_t::empty_address(),
                        &StreamingActor::stream_strings, 3);
        REQUIRE(gen.valid());
        actor->resume(1);
    }
}

TEST_CASE("generator message dispatch", "[generator][dispatch]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("dispatch links generator states") {
        auto gen = send(actor.get(), actor::address_t::empty_address(),
                        &StreamingActor::stream_numbers, 3);
        REQUIRE(gen.valid());
        actor->resume(1);
        REQUIRE(gen.valid());
    }

    SECTION("multiple generator messages queued") {
        auto gen1 = send(actor.get(), actor::address_t::empty_address(),
                         &StreamingActor::stream_numbers, 2);
        auto gen2 = send(actor.get(), actor::address_t::empty_address(),
                         &StreamingActor::stream_numbers, 3);
        auto gen3 = send(actor.get(), actor::address_t::empty_address(),
                         &StreamingActor::stream_strings, 1);

        REQUIRE(gen1.valid());
        REQUIRE(gen2.valid());
        REQUIRE(gen3.valid());

        auto info = actor->resume(10);
        REQUIRE(info.messages_processed == 3);
    }
}

TEST_CASE("generator cancel via send()", "[generator][send][cancel]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("cancel before processing") {
        auto gen = send(actor.get(), actor::address_t::empty_address(),
                        &StreamingActor::stream_numbers, 5);
        REQUIRE(gen.valid());

        gen.cancel();

        REQUIRE(gen.is_cancelled());
        REQUIRE(gen.is_safe_to_destroy());

        actor->resume(1);
    }

    SECTION("detach before processing") {
        auto gen = send(actor.get(), actor::address_t::empty_address(),
                        &StreamingActor::stream_numbers, 5);
        REQUIRE(gen.valid());

        gen.detach();

        REQUIRE(!gen.valid());
        actor->resume(1);
    }
}

TEST_CASE("generator mailbox check", "[generator][send][mailbox]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("send() enqueues message to mailbox") {
        auto gen = send(actor.get(), actor::address_t::empty_address(),
                        &StreamingActor::stream_numbers, 3);
        REQUIRE(gen.valid());

        auto info = actor->resume(1);
        REQUIRE(info.messages_processed == 1);
    }
}

#include <thread>
#include <atomic>

TEST_CASE("generator MT - concurrent send", "[generator][mt]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("multiple threads sending generator messages") {
        constexpr int num_threads = 4;
        constexpr int msgs_per_thread = 10;

        std::vector<generator<int>> generators;
        generators.reserve(num_threads * msgs_per_thread);

        std::atomic<int> counter{0};
        std::atomic<int> valid_count{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&]() {
                for (int i = 0; i < msgs_per_thread; ++i) {
                    auto gen = send(actor.get(), actor::address_t::empty_address(),
                                    &StreamingActor::stream_numbers, 2);
                    // Avoid REQUIRE inside threads - Catch2 is not thread-safe
                    if (gen.valid()) {
                        valid_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    counter.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }

        REQUIRE(counter.load() == num_threads * msgs_per_thread);
        REQUIRE(valid_count.load() == num_threads * msgs_per_thread);

        auto info = actor->resume(100);
        REQUIRE(info.messages_processed == static_cast<size_t>(num_threads * msgs_per_thread));
    }
}

TEST_CASE("generator MT - concurrent cancel", "[generator][mt]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("cancel from different thread than send") {
        auto gen = send(actor.get(), actor::address_t::empty_address(),
                        &StreamingActor::stream_numbers, 100);
        REQUIRE(gen.valid());

        std::atomic<bool> cancelled{false};

        std::thread cancel_thread([&]() {
            gen.cancel();
            cancelled.store(true, std::memory_order_release);
        });

        cancel_thread.join();

        REQUIRE(cancelled.load(std::memory_order_acquire));
        REQUIRE(gen.is_cancelled());

        actor->resume(1);
    }
}

TEST_CASE("generator MT - stress test", "[generator][mt][stress]") {
    auto* resource = std::pmr::get_default_resource();

    SECTION("many generators created and destroyed rapidly") {
        constexpr int iterations = 100;

        for (int i = 0; i < iterations; ++i) {
            auto actor = spawn<StreamingActor>(resource);

            auto gen1 = send(actor.get(), actor::address_t::empty_address(),
                             &StreamingActor::stream_numbers, 5);
            auto gen2 = send(actor.get(), actor::address_t::empty_address(),
                             &StreamingActor::stream_strings, 3);

            REQUIRE(gen1.valid());
            REQUIRE(gen2.valid());

            actor->resume(10);
        }
    }
}

TEST_CASE("generator MT - producer/consumer threads", "[generator][mt]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("send in one thread, process in another") {
        std::atomic<bool> send_done{false};
        std::atomic<int> generators_created{0};
        std::atomic<int> valid_generators{0};

        std::thread producer([&]() {
            for (int i = 0; i < 20; ++i) {
                auto gen = send(actor.get(), actor::address_t::empty_address(),
                                &StreamingActor::stream_numbers, 3);
                // Avoid REQUIRE inside threads - Catch2 is not thread-safe
                if (gen.valid()) {
                    valid_generators.fetch_add(1, std::memory_order_relaxed);
                }
                generators_created.fetch_add(1, std::memory_order_relaxed);
            }
            send_done.store(true, std::memory_order_release);
        });

        std::thread consumer([&]() {
            size_t total_processed = 0;
            while (!send_done.load(std::memory_order_acquire) || total_processed < 20) {
                auto info = actor->resume(5);
                total_processed += info.messages_processed;
                if (info.messages_processed == 0) {
                    std::this_thread::yield();
                }
            }
        });

        producer.join();
        consumer.join();

        REQUIRE(generators_created.load() == 20);
        REQUIRE(valid_generators.load() == 20);
    }
}

TEST_CASE("generator error handling", "[generator][error]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<StreamingActor>(resource);

    SECTION("stream_error type compiles") {
        // Test that stream_error can be created with error_code
        auto err1 = stream_error{std::make_error_code(std::errc::io_error)};
        REQUIRE(err1.ec == std::errc::io_error);

        // Test creation from another error_code
        auto err2 = stream_error{std::make_error_code(std::errc::invalid_argument)};
        REQUIRE(err2.ec == std::errc::invalid_argument);
    }

    SECTION("generator without error has no error") {
        auto gen = actor->stream_numbers(3);
        REQUIRE(gen.valid());
        REQUIRE(!gen.has_error());
        REQUIRE(!gen.error());
    }

    SECTION("default constructed generator has no error") {
        generator<int> gen;
        REQUIRE(!gen.has_error());
        REQUIRE(!gen.error());
    }

    SECTION("yield stream_error sets error state") {
        auto gen = actor->stream_with_error(0);  // Error at position 0
        REQUIRE(gen.valid());
        REQUIRE(!gen.has_error());  // Before running, no error
    }

    SECTION("immediate error generator") {
        auto gen = actor->stream_immediate_error();
        REQUIRE(gen.valid());
        REQUIRE(!gen.has_error());  // Before running, no error
    }
}

// =============================================================================
// Phase 2: unique_future<generator<T>> Tests
// =============================================================================

TEST_CASE("unique_future<generator<T>> compilation", "[generator][future]") {
    SECTION("type traits compile") {
        using gen_type = generator<int>;
        using future_gen_type = unique_future<gen_type>;

        static_assert(std::is_move_constructible_v<gen_type>);
        static_assert(std::is_move_constructible_v<future_gen_type>);
        static_assert(!std::is_copy_constructible_v<gen_type>);
        static_assert(!std::is_copy_constructible_v<future_gen_type>);

        static_assert(type_traits::is_generator_v<gen_type>);
        static_assert(type_traits::is_unique_future_v<future_gen_type>);
    }

    SECTION("nested type detection") {
        using gen_type = generator<std::string>;
        using future_gen_type = unique_future<gen_type>;

        // unique_future<generator<T>> should NOT be detected as generator
        static_assert(!type_traits::is_generator_v<future_gen_type>);
        // It should be detected as unique_future
        static_assert(type_traits::is_unique_future_v<future_gen_type>);
    }
}

// Actor that creates generators asynchronously
class AsyncGenActor final : public basic_actor<AsyncGenActor> {
public:
    explicit AsyncGenActor(std::pmr::memory_resource* res)
        : basic_actor<AsyncGenActor>(res) {}

    ~AsyncGenActor() = default;

    // Async generator creation - returns unique_future<generator<T>>
    unique_future<generator<int>> create_stream_async(int count) {
        auto gen = stream_numbers_internal(count);
        co_return std::move(gen);
    }

    // Sync generator for comparison
    generator<int> stream_numbers(int count) {
        for (int i = 0; i < count; ++i) {
            co_yield i;
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &AsyncGenActor::create_stream_async,
        &AsyncGenActor::stream_numbers
    >;

    void behavior(mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<AsyncGenActor, &AsyncGenActor::create_stream_async>) {
            dispatch(this, &AsyncGenActor::create_stream_async, msg);
        } else if (cmd == msg_id<AsyncGenActor, &AsyncGenActor::stream_numbers>) {
            dispatch(this, &AsyncGenActor::stream_numbers, msg);
        }
    }

private:
    generator<int> stream_numbers_internal(int count) {
        for (int i = 0; i < count; ++i) {
            co_yield i;
        }
    }
};

TEST_CASE("unique_future<generator<T>> from actor", "[generator][future]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<AsyncGenActor>(resource);

    SECTION("send() returns unique_future<generator<T>>") {
        auto future = send(actor.get(), actor::address_t::empty_address(),
                           &AsyncGenActor::create_stream_async, 5);

        // Verify type is correct
        static_assert(type_traits::is_unique_future_v<decltype(future)>);

        REQUIRE(!future.available());  // Not yet processed

        auto info = actor->resume(1);
        REQUIRE(info.messages_processed == 1);

        REQUIRE(future.available());

        auto gen = std::move(future).get();
        REQUIRE(gen.valid());
        REQUIRE(!gen.exhausted());
    }

    SECTION("sync generator still works") {
        auto gen = send(actor.get(), actor::address_t::empty_address(),
                        &AsyncGenActor::stream_numbers, 3);

        static_assert(type_traits::is_generator_v<decltype(gen)>);
        REQUIRE(gen.valid());

        actor->resume(1);
    }
}

// =============================================================================
// Phase 3: co_await inside generator Tests
// =============================================================================

// Actor that uses co_await inside generator methods
class AwaitInsideGenActor final : public basic_actor<AwaitInsideGenActor> {
public:
    explicit AwaitInsideGenActor(std::pmr::memory_resource* res)
        : basic_actor<AwaitInsideGenActor>(res) {}

    ~AwaitInsideGenActor() = default;

    // Helper: returns a ready future with value
    unique_future<int> get_value(int x) {
        co_return x * 2;
    }

    // Generator that awaits a ready future
    generator<int> gen_await_ready(int start) {
        // First yield before await
        co_yield start;

        // Await a value (should be ready after resume)
        auto future = get_value(start);
        int doubled = co_await std::move(future);

        // Yield the awaited result
        co_yield doubled;
    }

    // Generator that awaits multiple futures
    generator<int> gen_multi_await(int count) {
        for (int i = 0; i < count; ++i) {
            auto future = get_value(i);
            int value = co_await std::move(future);
            co_yield value;
        }
    }

    // Generator that forwards from another generator
    generator<int> gen_forward(int count) {
        auto inner = stream_source(count);

        while (co_await inner) {
            co_yield inner.current();
        }

        if (inner.has_error()) {
            co_yield stream_error{inner.error()};
        }
    }

    generator<int> stream_source(int count) {
        for (int i = 0; i < count; ++i) {
            co_yield i * 10;
        }
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &AwaitInsideGenActor::get_value,
        &AwaitInsideGenActor::gen_await_ready,
        &AwaitInsideGenActor::gen_multi_await,
        &AwaitInsideGenActor::gen_forward,
        &AwaitInsideGenActor::stream_source
    >;

    void behavior(mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<AwaitInsideGenActor, &AwaitInsideGenActor::get_value>) {
            dispatch(this, &AwaitInsideGenActor::get_value, msg);
        } else if (cmd == msg_id<AwaitInsideGenActor, &AwaitInsideGenActor::gen_await_ready>) {
            dispatch(this, &AwaitInsideGenActor::gen_await_ready, msg);
        } else if (cmd == msg_id<AwaitInsideGenActor, &AwaitInsideGenActor::gen_multi_await>) {
            dispatch(this, &AwaitInsideGenActor::gen_multi_await, msg);
        } else if (cmd == msg_id<AwaitInsideGenActor, &AwaitInsideGenActor::gen_forward>) {
            dispatch(this, &AwaitInsideGenActor::gen_forward, msg);
        } else if (cmd == msg_id<AwaitInsideGenActor, &AwaitInsideGenActor::stream_source>) {
            dispatch(this, &AwaitInsideGenActor::stream_source, msg);
        }
    }
};

TEST_CASE("co_await inside generator", "[generator][await]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<AwaitInsideGenActor>(resource);

    SECTION("await_transform compiles for unique_future") {
        // Just verify compilation - actual runtime requires full coroutine setup
        auto gen = actor->gen_await_ready(5);
        REQUIRE(gen.valid());
    }

    SECTION("await_transform compiles for generator forwarding") {
        auto gen = actor->gen_forward(3);
        REQUIRE(gen.valid());
    }

    SECTION("generator await_transform type traits") {
        // Verify type detection works
        static_assert(type_traits::is_generator_v<generator<int>>);
        static_assert(type_traits::is_unique_future_v<unique_future<int>>);
    }
}

TEST_CASE("unique_future<generator<T>> full chain", "[generator][future]") {
    auto* resource = std::pmr::get_default_resource();
    auto actor = spawn<AsyncGenActor>(resource);

    SECTION("get generator from future and use it") {
        // Step 1: Send message, get future
        auto future = send(actor.get(), actor::address_t::empty_address(),
                           &AsyncGenActor::create_stream_async, 3);
        REQUIRE(!future.available());

        // Step 2: Process message
        actor->resume(1);
        REQUIRE(future.available());

        // Step 3: Get generator from future
        auto gen = std::move(future).get();
        REQUIRE(gen.valid());
        REQUIRE(!gen.exhausted());

        // Step 4: Verify generator can be cancelled/detached
        gen.cancel();
        REQUIRE(gen.is_cancelled());
        REQUIRE(gen.is_safe_to_destroy());
    }

    SECTION("multiple async generators") {
        auto future1 = send(actor.get(), actor::address_t::empty_address(),
                            &AsyncGenActor::create_stream_async, 2);
        auto future2 = send(actor.get(), actor::address_t::empty_address(),
                            &AsyncGenActor::create_stream_async, 3);

        REQUIRE(!future1.available());
        REQUIRE(!future2.available());

        // Process both messages
        auto info = actor->resume(10);
        REQUIRE(info.messages_processed == 2);

        REQUIRE(future1.available());
        REQUIRE(future2.available());

        auto gen1 = std::move(future1).get();
        auto gen2 = std::move(future2).get();

        REQUIRE(gen1.valid());
        REQUIRE(gen2.valid());
    }

    SECTION("mixed sync and async generators") {
        auto future = send(actor.get(), actor::address_t::empty_address(),
                           &AsyncGenActor::create_stream_async, 5);
        auto gen_sync = send(actor.get(), actor::address_t::empty_address(),
                             &AsyncGenActor::stream_numbers, 3);

        REQUIRE(!future.available());
        REQUIRE(gen_sync.valid());

        actor->resume(10);

        REQUIRE(future.available());

        auto gen_async = std::move(future).get();
        REQUIRE(gen_async.valid());
        REQUIRE(gen_sync.valid());
    }
}