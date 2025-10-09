#include <chrono>
#include <csignal>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <pthread.h>
#include <string.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "memusage.hpp"

#include <actor-zeta.hpp>

enum class command_t : uint64_t {
    add_link,
    add_address,
    prepare,
    send,
    process_data,
};

namespace names {

    static const std::string actor("storage");
    static const std::string supervisor("supervisor");

} // namespace names

std::atomic<int64_t> packets_a(0);

struct data_t {
    std::string data;
    std::chrono::system_clock::time_point time_point;
    int64_t producer_index;
    int64_t data_index;

    bool empty() const {
        return data.empty();
    }

    std::chrono::system_clock::time_point time() const {
        return time_point;
    }
};


class actor_test_t;

class supervisor_test_t final : public actor_zeta::actor_abstract_t {
public:
    size_t count_actors_;
    size_t count_producers_;
    size_t datasize_;

    size_t producer_latency_ms_;
    size_t consumer_latency_ms_;

    size_t memory_limit_mb_;

    std::atomic_bool running_;

    void memchecker() const {
        sdk::memory::checker_t checker(static_cast<double>(memory_limit_mb_), true);
        while (running_) {
            checker.check();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    void producer(size_t idx) {
        data_t data;
        for (size_t i = 0; i < datasize_; ++i) {
            data.data.push_back(static_cast<char>(i % 255));
        }
        data.producer_index = static_cast<int64_t>(idx);
        std::cout << std::this_thread::get_id() << " :: producer #" << idx << " started +++" << std::endl;
        int64_t counter = 0;

        while (running_) {
            data.data_index = counter;
            for (const auto& addr : address_book_) {
                packets_a++;
                data.time_point = std::chrono::system_clock::now();
                actor_zeta::send(addr.second, address(), command_t::process_data, data);

                std::this_thread::sleep_for(std::chrono::milliseconds(producer_latency_ms_));
            }
            counter++;
        }
        std::cout << std::this_thread::get_id() << " :: producer #" << idx << " stopped ---" << std::endl;
    }

public:
    supervisor_test_t(
        actor_zeta::pmr::memory_resource* ptr,
        size_t count_actors,
        size_t count_producers,
        size_t datasize,
        size_t num_worker_threads,
        size_t max_throughput_param,
        size_t producer_latency_ms,
        size_t consumer_latency_ms,
        size_t memory_limit_mb)
        : actor_zeta::cooperative_supervisor<supervisor_test_t>(ptr)
        , count_actors_(count_actors)
        , count_producers_(count_producers)
        , datasize_(datasize)
        , producer_latency_ms_(producer_latency_ms)
        , consumer_latency_ms_(consumer_latency_ms)
        , memory_limit_mb_(memory_limit_mb)
        , running_(true)
        , name_(names::supervisor)
        , e_(new actor_zeta::scheduler_t<actor_zeta::work_sharing>(
                 num_worker_threads,
                 max_throughput_param),
             thread_pool_deleter) {
        e_->start();
    }

    void prepare() {
        for (size_t i = 1; i <= count_actors_; ++i) {
            auto addr = spawn_actor([this, i](actor_test_t* ptr) {
                actors_.emplace(i, ptr);
            },
                                    consumer_latency_ms_);
            address_book_.emplace(i, std::move(addr));
            actor_zeta::send(address_book_.at(i), address(), command_t::add_address, address(), 0);
        }
    }

    void start() {
        assert(actors_.size() == count_actors_);
        memchecker_thread_ = std::thread(&supervisor_test_t::memchecker, this);
        for (size_t i = 0; i < count_producers_; ++i) {
            producers_.emplace_back(&supervisor_test_t::producer, this, i);
        }
    }

    void stop() {
        running_ = false;
    }

    void wait() {
        for (auto& producer : producers_) {
            producer.join();
        }
        memchecker_thread_.join();
    }


    auto make_scheduler() noexcept -> actor_zeta::scheduler_t* {
        return e_.get();
    }

protected:
    actor_zeta::behavior_t behavior() {
        return actor_zeta::make_behavior(
            resource(),
            [](actor_zeta::message* msg) -> void {
                std::cerr << msg->command() << std::endl;
            });
    }

    auto enqueue_impl(actor_zeta::message_ptr msg) -> void  {
        {
            auto ptr = std::move(msg);
            behavior()(ptr.get());
            delete ptr;
        }
    }

private:
    const std::string name_;
    std::unique_ptr<actor_zeta::scheduler_t, decltype(thread_pool_deleter)> e_;
    std::map<int, actor_zeta::address_t> address_book_;
    std::map<int, actor_test_t*> actors_;
    std::thread memchecker_thread_;
    std::vector<std::thread> producers_;
};

class actor_test_t final : public actor_zeta::basic_actor<actor_test_t> {
    supervisor_test_t* sup_ptr_;
    size_t consumer_latency_ms_;
    std::map<int64_t, int64_t> prev_index_;
    std::map<int, actor_zeta::address_t> address_book_;

    const std::string name_;
    actor_zeta::behavior_t add_address_;
    actor_zeta::behavior_t process_data_;

    auto perform(command_t cmd) -> void {
        actor_zeta::send(address_book_.begin()->second, address(), cmd);
    }

public:
    actor_test_t(supervisor_test_t* ptr, size_t consumer_latency_ms)
        : actor_zeta::basic_actor<actor_test_t>(ptr)
        , sup_ptr_(ptr)
        , consumer_latency_ms_(consumer_latency_ms)
        , name_(names::actor)
        , add_address_(actor_zeta::make_behavior(resource(), command_t::add_address, this, &actor_test_t::add_address))
        , process_data_(actor_zeta::make_behavior(resource(), command_t::process_data, this, &actor_test_t::process_data)) {
    }

    actor_zeta::behavior_t behavior() {
        return actor_zeta::make_behavior(
            resource(),
            [this](actor_zeta::message* msg) -> void {
                switch (msg->command()) {
                    case actor_zeta::make_message_id(command_t::add_address): {
                        add_address_(msg);
                        break;
                    }
                    case actor_zeta::make_message_id(command_t::process_data): {
                        process_data_(msg);
                        break;
                    }
                }
            });
    }

    ~actor_test_t() override = default;

    void add_address(actor_zeta::address_t address, int name) {
        if (address && this != address.get()) {
            address_book_.emplace(name, std::move(address));
            std::cout << "Add address " << name << std::endl;
        }
    }

    void process_data(data_t&& data) {
        ///auto ms_dur = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - data.time()).count();
        packets_a--;
        if (packets_a.load() > 0)
            std::cout << __func__ << " :: packets_a " << packets_a.load() << " OUT >>>" << std::endl;
        auto it = prev_index_.find(data.producer_index);
        if (it == prev_index_.end()) {
            prev_index_[data.producer_index] = data.data_index;
        } else {
            //std::cout << std::this_thread::get_id() << " :: " << data.data_index << " :: " << it->second << std::endl;
            assert(data.data_index - it->second == 1);
            it->second = data.data_index;
        }
        //if (ms_dur > 0)
        //std::cout << std::this_thread::get_id() << " :: " << __func__ << " :: ms_dur " << ms_dur << " OUT >>>" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(consumer_latency_ms_));
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////

#define CALLSTACK_SIZE 1024

std::function<void(int, int, char**)> shutdown_handler;
void signal_handler(int signal) { shutdown_handler(signal, 0, nullptr); }
void signal_ignore(int signal) { std::cout << "Ignoring SIGNAL: " << signal << std::endl; }
void signal_backtrace(int signal) {
    void* buf[CALLSTACK_SIZE + 1];
    auto nptrs = backtrace(buf, CALLSTACK_SIZE);
    char** strings = backtrace_symbols(buf, nptrs);
    shutdown_handler(signal, nptrs, strings);
}

namespace args {

    const std::string count_actors("--count_actors");
    const std::string count_producers("--count_producers");
    const std::string datasize("--datasize");
    const std::string num_worker_threads("--num_worker_threads");
    const std::string max_throughput_param("--max_throughput_param");

    const std::string producer_latency_ms("--producer_latency_ms");
    const std::string consumer_latency_ms("--consumer_latency_ms");

    const std::string memory_limit_mb("--memory_limit_mb");

    const size_t count_actors_default = 5;
    const size_t count_producers_default = 5;
    const size_t datasize_default = 1024;
    const size_t num_worker_threads_default = 10;
    const size_t max_throughput_param_default = 5;

    const size_t producer_latency_ms_default = 0;
    const size_t consumer_latency_ms_default = 0;

    const size_t memory_limit_mb_default = 4 * 1024;

} // namespace args

int main(int argc, char** argv) {
    std::map<std::string, size_t> args_set = {{{args::count_actors, args::count_actors_default},
                                               {args::count_producers, args::count_producers_default},
                                               {args::datasize, args::datasize_default},
                                               {args::num_worker_threads, args::num_worker_threads_default},
                                               {args::max_throughput_param, args::max_throughput_param_default},
                                               {args::producer_latency_ms, args::producer_latency_ms_default},
                                               {args::consumer_latency_ms, args::consumer_latency_ms_default},
                                               {args::memory_limit_mb, args::memory_limit_mb_default}}};

    auto print_help = [&args_set]() {
        for (auto& arg : args_set) {
            printf("help : %s (default %lu)\n", arg.first.c_str(), arg.second);
        }
    };

    if (argc > 1 && std::string(argv[1]) == "--help") {
        print_help();
        return 0;
    }

    int i = 1;
    while (i < argc) {
        auto strarg = std::string(argv[i++]);
        auto arg_it = args_set.find(strarg);
        if (arg_it == args_set.end()) {
            std::cerr << "!!! Unknown argument " << strarg << std::endl;
        } else {
            if (i < argc) {
                arg_it->second = static_cast<size_t>(atoll(argv[i++]));
                printf("%s :: arg '%s' = %lu\n", __func__, arg_it->first.c_str(), arg_it->second);
            } else {
                std::cerr << "!!! Missing value for " << arg_it->first.c_str() << std::endl;
            }
        }
    }

    auto* resource = actor_zeta::pmr::get_default_resource();
    auto sup_ = actor_zeta::spawn_supervisor<supervisor_test_t>(
        resource,
        args_set.at(args::count_actors),
        args_set.at(args::count_producers),
        args_set.at(args::datasize),
        args_set.at(args::num_worker_threads),
        args_set.at(args::max_throughput_param),
        args_set.at(args::producer_latency_ms),
        args_set.at(args::consumer_latency_ms),
        args_set.at(args::memory_limit_mb));
    sup_->prepare();

    packets_a.store(0);

    shutdown_handler = [&](int signal_num, int nptrs, char** strings) {
        std::cout << "shutdown_handler :: Interrupt signal is " << signal_num << " (" << strsignal(signal_num) << ")" << std::endl;

        if (nptrs > 0 && strings) {
            for (auto i = 0; i < nptrs; i++) {
                std::cout << "backtrace #" << i << " :: " << strings[i] << std::endl;
            }

            free(strings);
        }

        sup_->stop();

        //exit(signal_num);
    };

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);

    signal(SIGPIPE, signal_ignore);

    signal(SIGABRT, signal_backtrace);
    signal(SIGSEGV, signal_backtrace);

    sup_->start();
    sup_->wait();

    return 0;
}
