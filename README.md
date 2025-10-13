
|    compiler    | Master | Develop |
|:--------------:|:---:|:---:|
| gcc 4.8.5 - 12 | |[![ubuntu](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_gcc.yaml/badge.svg?branch=develop)](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_gcc.yaml) |
| clang 3.9 - 16 | |[![ubuntu](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_clang.yaml/badge.svg?branch=develop)](https://github.com/cyberduckninja/actor-zeta/actions/workflows/ubuntu_clang.yaml)|

actor-zeta
========================

actor-zeta is an open source C++11/14/17 virtual actor model implementation featuring lightweight & fast and more.

## Example

```C++

#include <actor-zeta.hpp>

class key_value_storage_t final : public actor_zeta::basic_actor<key_value_storage_t> {
public:
    void init();
    void search(std::string& key);
    void add(const std::string& key, const std::string& value);

    using dispatch_traits = actor_zeta::dispatch_traits<
        &key_value_storage_t::init,
        &key_value_storage_t::search,
        &key_value_storage_t::add
    >;

    explicit key_value_storage_t(actor_zeta::pmr::memory_resource* ptr)
        : actor_zeta::basic_actor<key_value_storage_t>(ptr)
        , init_(actor_zeta::make_behavior(resource(), this, &key_value_storage_t::init))
        , search_(actor_zeta::make_behavior(resource(), this, &key_value_storage_t::search))
        , add_(actor_zeta::make_behavior(resource(), this, &key_value_storage_t::add)) {
    }

    void behavior(actor_zeta::message* msg) {
        auto cmd = msg->command();
        if (cmd == actor_zeta::msg_id<key_value_storage_t, &key_value_storage_t::init>) {
            init_(msg);
        } else if (cmd == actor_zeta::msg_id<key_value_storage_t, &key_value_storage_t::search>) {
            search_(msg);
        } else if (cmd == actor_zeta::msg_id<key_value_storage_t, &key_value_storage_t::add>) {
            add_(msg);
        }
    }

    ~key_value_storage_t() override = default;

private:
    actor_zeta::behavior_t init_;
    actor_zeta::behavior_t search_;
    actor_zeta::behavior_t add_;

    void init() {
       /// ...
    }

    void search(std::string& key) {
        /// ...
    }

    void add(const std::string& key, const std::string& value) {
        /// ...
    }
};

```

### Header-Only

To use as header-only; that is, to eliminate the requirement to
link a program to a static or dynamic library, simply
place the following line in exactly one new or existing source
file in your project.
```
#include <actor-zeta/src.hpp>
```

## For Users

Add the corresponding remote to your conan:

```bash
    conan remote add duckstax http://conan.duckstax.com
```

### Basic setup
```bash
    $ conan install actor-zeta/1.0.0a8@duckstax/stable
```
### Project setup

If you handle multiple dependencies in your project is better to add a *conanfile.txt*

    [requires]
    actor-zeta/1.0.0a8@duckstax/stable

    [generators]
    cmake

## Dependencies

* CMake >= 3.0
