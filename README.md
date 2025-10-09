
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

using actor_zeta::basic_async_actor;

class key_value_storage_t final : public basic_async_actor {
public:
    explicit key_value_storage_t(dummy_supervisor &ref) : basic_async_actor(ref, "storage") {

        add_handler(
                "init",
                &key_value_storage_t::init
        );

        add_handler(
                "search",
                &key_value_storage_t::search
        );

        add_handler(
                "add",
                &key_value_storage_t::add
        );

    }

    ~key_value_storage_t() override = default;

    void init() {
       /// ...
    }

private:

    void search(std::string &key) {
        /// ...
    }

    void add(const std::string &key, const std::string &value) {
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
