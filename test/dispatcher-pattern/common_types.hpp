#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace dispatcher_test {

struct session_id_t {
    std::string id_;

    session_id_t() = default;
    explicit session_id_t(std::string id) : id_(std::move(id)) {}

    const std::string& data() const { return id_; }

    bool operator==(const session_id_t& other) const { return id_ == other.id_; }
};

struct session_id_hash {
    std::size_t operator()(const session_id_t& s) const {
        return std::hash<std::string>{}(s.data());
    }
};

struct collection_full_name_t {
    std::string database;
    std::string collection;

    collection_full_name_t() = default;
    collection_full_name_t(std::string db, std::string coll)
        : database(std::move(db)), collection(std::move(coll)) {}

    std::string to_string() const {
        return database + "." + collection;
    }
};

struct size_result_t {
    std::size_t size{0};
    bool has_error{false};
    std::string error_message;

    size_result_t() = default;
    explicit size_result_t(std::size_t s) : size(s), has_error(false) {}

    static size_result_t error(std::string msg) {
        size_result_t r;
        r.has_error = true;
        r.error_message = std::move(msg);
        return r;
    }
};

struct cursor_t {
    std::vector<std::string> data;
    bool has_error{false};
    std::string error_message;
    bool is_open{true};

    cursor_t() = default;

    std::size_t row_count() const { return data.size(); }
    const std::string& get_row(std::size_t idx) const { return data.at(idx); }

    void close() { is_open = false; }

    static cursor_t error(std::string msg) {
        cursor_t c;
        c.has_error = true;
        c.error_message = std::move(msg);
        return c;
    }
};

using cursor_t_ptr = std::unique_ptr<cursor_t>;

struct logical_plan_t {
    std::string operation;
    collection_full_name_t collection;
    std::string filter;

    logical_plan_t() = default;
    logical_plan_t(std::string op, collection_full_name_t coll, std::string flt = "")
        : operation(std::move(op))
        , collection(std::move(coll))
        , filter(std::move(flt)) {}

    std::string to_string() const {
        return operation + " " + collection.database + "." + collection.collection;
    }
};

using logical_plan_ptr = std::unique_ptr<logical_plan_t>;

struct transaction_result_t {
    std::size_t total_rows{0};
    bool committed{false};
    bool has_error{false};
    std::string error_message;

    transaction_result_t() = default;
    explicit transaction_result_t(std::size_t rows, bool commit = true)
        : total_rows(rows), committed(commit), has_error(false) {}

    static transaction_result_t error(std::string msg) {
        transaction_result_t r;
        r.has_error = true;
        r.error_message = std::move(msg);
        return r;
    }
};

struct aggregate_result_t {
    std::size_t total_size{0};
    std::size_t collection_count{0};
    std::string detail_info;
    bool has_error{false};
    std::string error_message;

    aggregate_result_t() = default;
    aggregate_result_t(std::size_t total, std::size_t count, std::string detail)
        : total_size(total), collection_count(count), detail_info(std::move(detail)) {}

    static aggregate_result_t error(std::string msg) {
        aggregate_result_t r;
        r.has_error = true;
        r.error_message = std::move(msg);
        return r;
    }
};

} // namespace dispatcher_test