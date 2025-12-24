#pragma once

/// @file common_types.hpp
/// @brief Domain types for dispatcher-pattern tests
///
/// This file contains domain types used across the test actors:
/// - session_id_t: Session identification
/// - collection_full_name_t: Database.collection naming
/// - size_result_t, cursor_t: Operation results
/// - logical_plan_t: Query plan representation
/// - transaction_result_t, aggregate_result_t: Complex operation results

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace dispatcher_test {

// ============================================================================
// Session identification
// ============================================================================

/// @brief Session identifier for tracking requests
struct session_id_t {
    std::string id_;

    session_id_t() = default;
    explicit session_id_t(std::string id) : id_(std::move(id)) {}

    const std::string& data() const { return id_; }

    bool operator==(const session_id_t& other) const { return id_ == other.id_; }
};

/// @brief Hash functor for session_id_t (for use in unordered containers)
struct session_id_hash {
    std::size_t operator()(const session_id_t& s) const {
        return std::hash<std::string>{}(s.data());
    }
};

// ============================================================================
// Collection naming
// ============================================================================

/// @brief Full name of a database collection
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

// ============================================================================
// Operation results
// ============================================================================

/// @brief Result of size operation with error handling
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

// ============================================================================
// Cursor - query execution result
// ============================================================================

/// @brief Cursor holding query results
struct cursor_t {
    std::vector<std::string> data;  // Result rows
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

// ============================================================================
// Logical plan - query representation
// ============================================================================

/// @brief Logical query plan (simplified)
struct logical_plan_t {
    std::string operation;  // "select", "insert", "update", "delete"
    collection_full_name_t collection;
    std::string filter;     // Filter condition

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

// ============================================================================
// Transaction result
// ============================================================================

/// @brief Result of transaction (sequential queries)
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

// ============================================================================
// Aggregate result
// ============================================================================

/// @brief Result of aggregation (parallel queries + nested coroutines)
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