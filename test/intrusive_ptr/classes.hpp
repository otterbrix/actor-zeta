#pragma once

#include <cstddef>
#include <iostream>
#include <vector>

#include <actor-zeta/detail/intrusive_ptr.hpp>
#include <actor-zeta/detail/ref_counted.hpp>

using namespace actor_zeta;

int class0_instances = 0;
int class1_instances = 0;

class class0;
class class1;

using class0ptr = intrusive_ptr<class0>;
using class1ptr = intrusive_ptr<class1>;

class class0 : public ref_counted {
public:
    explicit class0(bool subtype = false)
        : subtype_(subtype) {
        if (!subtype) {
            ++class0_instances;
        }
    }

    virtual ~class0() {
        if (!subtype_) {
            --class0_instances;
        }
    }

    bool is_subtype() const {
        return subtype_;
    }

    virtual class0ptr create() const {
        return make_counted<class0>();
    }

private:
    bool subtype_;
};

class class1 : public class0 {
public:
    class1()
        : class0(true) {
        ++class1_instances;
    }

    ~class1() {
        --class1_instances;
    }

    class0ptr create() const override {
        return make_counted<class1>();
    }
};

class0ptr get_test_rc() {
    return make_counted<class0>();
}

class0ptr get_test_ptr() {
    return get_test_rc();
}
