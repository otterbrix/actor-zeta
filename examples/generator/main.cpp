#include <actor-zeta.hpp>

#include <iostream>
#include <memory_resource>
#include <string>
#include <vector>

using namespace actor_zeta;

class DataProducer;
using data_producer_ptr = std::unique_ptr<DataProducer, pmr::deleter_t>;

class DataProducer final : public basic_actor<DataProducer> {
public:
    explicit DataProducer(std::pmr::memory_resource* res)
        : basic_actor<DataProducer>(res) {
        std::cout << "[DataProducer] Created\n";
    }

    ~DataProducer() = default;

    generator<int> stream_range(int start, int end) {
        std::cout << "[DataProducer] Starting stream_range(" << start << ", " << end << ")\n";
        for (int i = start; i < end; ++i) {
            std::cout << "[DataProducer] Yielding: " << i << "\n";
            co_yield i;
        }
        std::cout << "[DataProducer] Stream complete\n";
    }

    generator<std::string> stream_messages(int count) {
        std::cout << "[DataProducer] Starting stream_messages(" << count << ")\n";
        for (int i = 0; i < count; ++i) {
            std::string msg = "Message #" + std::to_string(i);
            std::cout << "[DataProducer] Yielding: " << msg << "\n";
            co_yield std::move(msg);
        }
        std::cout << "[DataProducer] Messages complete\n";
    }

    generator<int> stream_fibonacci(int count) {
        std::cout << "[DataProducer] Starting fibonacci stream\n";
        int a = 0, b = 1;
        for (int i = 0; i < count; ++i) {
            std::cout << "[DataProducer] Yielding fib: " << a << "\n";
            co_yield a;
            int next = a + b;
            a = b;
            b = next;
        }
        std::cout << "[DataProducer] Fibonacci complete\n";
    }

    using dispatch_traits = actor_zeta::dispatch_traits<
        &DataProducer::stream_range,
        &DataProducer::stream_messages,
        &DataProducer::stream_fibonacci
    >;

    void behavior(mailbox::message* msg) {
        auto cmd = msg->command();
        if (cmd == msg_id<DataProducer, &DataProducer::stream_range>) {
            dispatch(this, &DataProducer::stream_range, msg);
        } else if (cmd == msg_id<DataProducer, &DataProducer::stream_messages>) {
            dispatch(this, &DataProducer::stream_messages, msg);
        } else if (cmd == msg_id<DataProducer, &DataProducer::stream_fibonacci>) {
            dispatch(this, &DataProducer::stream_fibonacci, msg);
        }
    }
};


int main() {
    std::cout << "=== Generator Example ===\n\n";

    auto* resource = std::pmr::get_default_resource();
    auto producer = spawn<DataProducer>(resource);

    std::cout << "--- Example 1: Direct method call ---\n";
    {
        auto gen = producer->stream_range(0, 5);
        std::cout << "Generator created, valid: " << gen.valid() << "\n";
        std::cout << "Generator exhausted: " << gen.exhausted() << "\n\n";
    }

    std::cout << "\n--- Example 2: Via send() API ---\n";
    {
        auto gen = send(producer.get(), actor::address_t::empty_address(),
                        &DataProducer::stream_fibonacci, 8);
        std::cout << "Generator from send() valid: " << gen.valid() << "\n";

        auto info = producer->resume(1);
        std::cout << "Messages processed: " << info.messages_processed << "\n";
        std::cout << "Generator still valid: " << gen.valid() << "\n\n";
    }

    std::cout << "\n--- Example 3: Multiple generators ---\n";
    {
        auto gen1 = send(producer.get(), actor::address_t::empty_address(),
                         &DataProducer::stream_range, 0, 3);
        auto gen2 = send(producer.get(), actor::address_t::empty_address(),
                         &DataProducer::stream_messages, 2);

        std::cout << "Created 2 generators\n";
        std::cout << "gen1 valid: " << gen1.valid() << "\n";
        std::cout << "gen2 valid: " << gen2.valid() << "\n";

        auto info = producer->resume(10);
        std::cout << "Messages processed: " << info.messages_processed << "\n\n";
    }

    std::cout << "\n--- Example 4: Cancel generator ---\n";
    {
        auto gen = send(producer.get(), actor::address_t::empty_address(),
                        &DataProducer::stream_range, 0, 100);

        std::cout << "Generator valid: " << gen.valid() << "\n";
        std::cout << "Generator cancelled: " << gen.is_cancelled() << "\n";

        gen.cancel();

        std::cout << "After cancel:\n";
        std::cout << "  cancelled: " << gen.is_cancelled() << "\n";
        std::cout << "  safe to destroy: " << gen.is_safe_to_destroy() << "\n";

        producer->resume(1);
    }

    std::cout << "\n--- Example 5: Detach generator ---\n";
    {
        auto gen = send(producer.get(), actor::address_t::empty_address(),
                        &DataProducer::stream_messages, 3);

        std::cout << "Before detach - valid: " << gen.valid() << "\n";

        gen.detach();

        std::cout << "After detach - valid: " << gen.valid() << "\n";

        producer->resume(1);
    }

    std::cout << "\n=== Generator Example Complete ===\n";

    return 0;
}