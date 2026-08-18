#pragma once

#include <concepts>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_map>
#include <memory>
#include <variant>
#include <functional>
#include <iterator>
#include <span>
#include <type_traits>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <cstdint>
#include <cmath>

#include <sqlite3.h>

namespace sqlitecxx {

class params;

class sqlite_error : public std::runtime_error {
public:
    sqlite_error(int code, int extended_code, std::string message) : std::runtime_error(std::move(message)),
                                                                     code_(code),
                                                                     extended_code_(extended_code) {
    }

    [[nodiscard]] int code() const noexcept {
        return code_;
    }

    [[nodiscard]] int extended_code() const noexcept {
        return extended_code_;
    }

    [[nodiscard]] bool has_extended_code() const noexcept {
        return extended_code_ != code_;
    }

private:
    int code_;
    int extended_code_;
};

namespace detail {

void bind_parameters(sqlite3_stmt *stmt, const params &parameters);

struct transparent_string_hash {
    using is_transparent = void;
    using hash_type = std::hash<std::string_view>;

    [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept {
        return hash_type{}(text);
    }
};

struct column_schema {
    std::vector<std::string> names;
    std::unordered_map<std::string, int, transparent_string_hash, std::equal_to<>> by_name;
};

using default_materialized_value = std::variant<
    std::nullptr_t,
    sqlite3_int64,
    double,
    std::string,
    std::vector<std::byte>>;

template <typename T>
concept character_type =
    std::same_as<std::remove_cvref_t<T>, char> ||
    std::same_as<std::remove_cvref_t<T>, signed char> ||
    std::same_as<std::remove_cvref_t<T>, unsigned char> ||
    std::same_as<std::remove_cvref_t<T>, wchar_t> ||
    std::same_as<std::remove_cvref_t<T>, char8_t> ||
    std::same_as<std::remove_cvref_t<T>, char16_t> ||
    std::same_as<std::remove_cvref_t<T>, char32_t>;

template <typename T>
concept integer_parameter =
    std::integral<std::remove_cvref_t<T>> &&
    !std::same_as<std::remove_cvref_t<T>, bool> &&
    !character_type<T>;

template <typename T>
concept real_parameter =
    std::same_as<std::remove_cvref_t<T>, float> ||
    std::same_as<std::remove_cvref_t<T>, double>;

template <typename T>
concept text_parameter =
    std::convertible_to<T, std::string_view>;

template <typename T>
concept null_parameter =
    std::same_as<std::remove_cvref_t<T>, std::nullptr_t>;

template <typename T>
concept blob_element =
    !std::is_volatile_v<T> &&
    (std::same_as<std::remove_const_t<T>, std::byte> ||
     std::same_as<std::remove_const_t<T>, unsigned char>);

template <typename T>
struct is_blob_span : std::false_type {
};

template <typename Element, std::size_t Extent>
struct is_blob_span<std::span<Element, Extent>>
    : std::bool_constant<blob_element<Element>> {
};

template <typename T>
concept blob_parameter =
    is_blob_span<std::remove_cvref_t<T>>::value;

template <typename T>
concept direct_bindable_parameter =
    null_parameter<T> ||
    integer_parameter<T> ||
    real_parameter<T> ||
    text_parameter<T> ||
    blob_parameter<T>;

template <typename T>
struct is_optional : std::false_type {
};

template <typename T>
struct is_optional<std::optional<T>> : std::true_type {
};

template <typename T>
constexpr bool is_optional_v = is_optional<std::remove_cvref_t<T>>::value;

template <typename T>
struct remove_optional {
    using type = T;
};

template <typename T>
struct remove_optional<std::optional<T>> {
    using type = T;
};

template <typename T>
using remove_optional_t =
    typename remove_optional<std::remove_cvref_t<T>>::type;

template <typename T>
concept bindable_parameter =
    !std::is_volatile_v<std::remove_reference_t<T>> &&
    !std::is_volatile_v<remove_optional_t<T>> &&
    direct_bindable_parameter<remove_optional_t<T>>;

template <typename T>
concept plain_value =
    std::same_as<T, std::remove_cvref_t<T>>;

template <typename T>
concept integer_field_value =
    plain_value<T> &&
    integer_parameter<T>;

template <typename T>
concept real_field_value =
    std::same_as<T, double>;

template <typename T>
concept text_field_value =
    std::same_as<T, std::string>;

template <typename T>
concept byte_blob_field_value =
    std::same_as<T, std::vector<std::byte>>;

template <typename T>
concept unsigned_char_blob_field_value =
    std::same_as<T, std::vector<unsigned char>>;

template <typename T>
concept live_field_value =
    integer_field_value<T> ||
    real_field_value<T> ||
    text_field_value<T> ||
    byte_blob_field_value<T> ||
    unsigned_char_blob_field_value<T>;

template <typename T>
concept optional_live_field_value =
    plain_value<T> &&
    is_optional_v<T> &&
    live_field_value<remove_optional_t<T>>;

template <typename T>
concept live_field_result =
    live_field_value<T> ||
    optional_live_field_value<T>;

template <typename T>
concept default_materialized_field_value =
    integer_field_value<T> ||
    real_field_value<T> ||
    text_field_value<T> ||
    byte_blob_field_value<T>;

template <typename T>
concept optional_default_materialized_field_value =
    plain_value<T> &&
    is_optional_v<T> &&
    default_materialized_field_value<remove_optional_t<T>>;

template <typename T>
concept default_materialized_field_result =
    default_materialized_field_value<T> ||
    optional_default_materialized_field_value<T>;

inline void validate_sql_text(std::string_view sql, std::string_view kind) {
    if (sql.empty()) {
        throw std::invalid_argument(std::string(kind) + " cannot be empty.");
    }

    if (sql.find('\0') != std::string_view::npos) {
        throw std::invalid_argument(
            std::string(kind) + " contains an embedded NUL byte.");
    }

    if (sql.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error(
            std::string(kind) + " exceeds SQLite's maximum input size.");
    }
}

struct error_snapshot {
    int primary = SQLITE_OK;
    int extended = SQLITE_OK;
    std::string message;
};

[[nodiscard]] inline error_snapshot snapshot_sqlite_error(
    sqlite3 *db,
    int returned_code,
    std::string_view operation,
    const char *preferred_message = nullptr) {
    int extended = returned_code;

    if (db) {
        const auto candidate = sqlite3_extended_errcode(db);
        if (candidate != SQLITE_OK) {
            extended = candidate;
        }
    }

    const int primary = extended & 0xFF;
    const char *raw_message = preferred_message;
    if (!raw_message || *raw_message == '\0') {
        raw_message = db ? sqlite3_errmsg(db) : sqlite3_errstr(returned_code);
    }

    std::string message(operation);
    message += " failed (SQLite code ";
    message += std::to_string(primary);
    if (extended != primary) {
        message += ", extended code ";
        message += std::to_string(extended);
    }
    message += ")";
    if (raw_message && *raw_message != '\0') {
        message += ": ";
        message += raw_message;
    }

    return error_snapshot{primary, extended, std::move(message)};
}

class sqlite_error_message {
public:
    sqlite_error_message() noexcept = default;

    sqlite_error_message(const sqlite_error_message &) = delete;
    sqlite_error_message &operator=(const sqlite_error_message &) = delete;

    ~sqlite_error_message() {
        sqlite3_free(message_);
    }

    [[nodiscard]] char **out() noexcept {
        return &message_;
    }

    [[nodiscard]] const char *get() const noexcept {
        return message_;
    }

private:
    char *message_ = nullptr;
};

inline void exec_control_sql(sqlite3 *db,
                             const char *sql,
                             std::string_view operation) {
    sqlite_error_message error_message;

    const auto rc =
        sqlite3_exec(
            db,
            sql,
            nullptr,
            nullptr,
            error_message.out());

    if (rc == SQLITE_OK) {
        return;
    }

    auto error =
        snapshot_sqlite_error(
            db,
            rc,
            operation,
            error_message.get());

    throw sqlite_error(
        error.primary,
        error.extended,
        std::move(error.message));
}

inline void exec_control_sql_noexcept(sqlite3 *db, const char *sql) noexcept {
    if (db) {
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    }
}

[[noreturn]] inline void throw_sqlite(sqlite3 *db, int returned_code, std::string operation) {
    const auto extended = db ? sqlite3_extended_errcode(db) : returned_code;

    const int primary = extended & 0xFF;

    const char *raw_message =
        db ? sqlite3_errmsg(db)
           : sqlite3_errstr(returned_code);

    std::string message = std::move(operation);
    message += ": ";
    message += raw_message ? raw_message : "Unknown SQLite error";

    throw sqlite_error(
        primary,
        extended,
        std::move(message));
}

class db_guard {
public:
    explicit db_guard(sqlite3 *db) noexcept : db_(db) {
    }
    db_guard(const db_guard &) = delete;
    db_guard &operator=(const db_guard &) = delete;

    ~db_guard() {
        if (db_) {
            sqlite3_close_v2(db_);
        }
    }

    [[nodiscard]] sqlite3 *release() noexcept {
        return std::exchange(db_, nullptr);
    }

private:
    sqlite3 *db_ = nullptr;
};

class stmt_guard {
public:
    explicit stmt_guard(sqlite3_stmt *stmt) noexcept : stmt_(stmt) {
    }

    stmt_guard(const stmt_guard &) = delete;
    stmt_guard &operator=(const stmt_guard &) = delete;

    sqlite3_stmt *release() noexcept {
        return std::exchange(stmt_, nullptr);
    }

    ~stmt_guard() {
        if (stmt_) [[likely]] {
            sqlite3_finalize(stmt_);
        }
    }

private:
    sqlite3_stmt *stmt_ = nullptr;
};

struct statement_state;

struct connection_state {
    explicit connection_state(sqlite3 *raw_db) noexcept
        : db(raw_db) {
    }

    connection_state(const connection_state &) = delete;
    connection_state &operator=(const connection_state &) = delete;

    ~connection_state() {
        close_noexcept();
    }

    void register_statement(std::shared_ptr<statement_state> &statement) {
        statements.erase(
            std::remove_if(
                statements.begin(),
                statements.end(),
                [](const auto &weak) {
                    return weak.expired();
                }),
            statements.end());

        statements.emplace_back(statement);
    }

    void close_noexcept() noexcept;

    [[nodiscard]] bool usable() const noexcept {
        return !closed && db != nullptr;
    }

    sqlite3 *db = nullptr;
    bool closed = false;
    bool transaction_active = false;
    std::uint64_t transaction_generation = 0;
    std::size_t active_cursors = 0;
    std::vector<std::weak_ptr<statement_state>> statements;
};

[[nodiscard]] inline bool connection_usable(const std::shared_ptr<connection_state> &state) noexcept {
    return state && state->usable();
}

[[nodiscard]] inline bool transaction_stale(const connection_state &state, std::uint64_t expected_generation) noexcept {
    return !state.transaction_active ||
           state.transaction_generation != expected_generation;
}

struct statement_state {
    statement_state(
        const std::shared_ptr<connection_state> &connection_state,
        sqlite3_stmt *raw_stmt) noexcept
        : connection(connection_state),
          stmt(raw_stmt) {
    }

    statement_state(const statement_state &) = delete;
    statement_state &operator=(const statement_state &) = delete;

    ~statement_state() {
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }

    void invalidate() noexcept {
        if (stmt) {
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
    }

    std::weak_ptr<connection_state> connection;
    sqlite3_stmt *stmt = nullptr;
    bool leased = false;
};

inline void connection_state::close_noexcept() noexcept {
    if (closed) {
        return;
    }

    closed = true;

    for (auto &weak : statements) {
        if (auto statement = weak.lock()) {
            statement->invalidate();
        }
    }
    statements.clear();
    active_cursors = 0;

    if (db && transaction_active && sqlite3_get_autocommit(db) == 0) {
        exec_control_sql_noexcept(db, "ROLLBACK;");
    }

    transaction_active = false;
    ++transaction_generation;

    sqlite3_close_v2(db);
    db = nullptr;
}

inline void clean_prepared_statement_noexcept(statement_state &statement) noexcept {
    if (statement.stmt) {
        (void)sqlite3_reset(statement.stmt);
        if (sqlite3_clear_bindings(statement.stmt) != SQLITE_OK) [[unlikely]] {
            statement.invalidate();
            return;
        }
    }
    statement.leased = false;
}

inline void mark_transaction_ended_noexcept(const std::shared_ptr<connection_state> &state) noexcept {
    if (state && state->transaction_active) {
        state->transaction_active = false;
        ++state->transaction_generation;
    }
}

inline void restore_autocommit_noexcept(const std::shared_ptr<connection_state> &state) noexcept {
    if (!connection_usable(state)) {
        return;
    }

    if (sqlite3_get_autocommit(state->db) != 0) {
        return;
    }

    exec_control_sql_noexcept(state->db, "ROLLBACK;");
    if (sqlite3_get_autocommit(state->db) == 0) {
        state->close_noexcept();
    }
}

inline void reconcile_autocommit_noexcept(const std::shared_ptr<connection_state> &state, bool transaction_owner) noexcept {
    if (!connection_usable(state)) {
        return;
    }

    const bool expected_autocommit = !transaction_owner;
    const bool current_autocommit = sqlite3_get_autocommit(state->db) != 0;
    if (current_autocommit == expected_autocommit) {
        return;
    }

    if (expected_autocommit) {
        restore_autocommit_noexcept(state);
    } else {
        mark_transaction_ended_noexcept(state);
    }
}

class autocommit_guard {
public:
    autocommit_guard(
        std::shared_ptr<connection_state> state,
        bool transaction_owner)
        : state_(std::move(state)),
          expected_autocommit_(!transaction_owner) {
    }

    autocommit_guard(const autocommit_guard &) = delete;
    autocommit_guard &operator=(const autocommit_guard &) = delete;

    ~autocommit_guard() noexcept {
        if (!checked_) {
            reconcile_noexcept();
        }
    }

    void check() {
        checked_ = true;
        if (!connection_usable(state_)) {
            throw std::logic_error(
                "The SQLite connection became invalid while executing SQL.");
        }
        if (current_autocommit() == expected_autocommit_) {
            return;
        }

        reconcile_noexcept();
        throw std::logic_error(
            "The SQL text changed the connection's autocommit state "
            "directly (e.g. via BEGIN/COMMIT/ROLLBACK or an outermost "
            "SAVEPOINT/RELEASE); use begin_transaction() so the library "
            "can track it.");
    }

private:
    [[nodiscard]] bool current_autocommit() const noexcept {
        return sqlite3_get_autocommit(state_->db) != 0;
    }

    void reconcile_noexcept() noexcept {
        if (!connection_usable(state_)) {
            return;
        }
        if (current_autocommit() == expected_autocommit_) {
            return;
        }
        reconcile_autocommit_noexcept(
            state_,
            !expected_autocommit_);
    }

    std::shared_ptr<connection_state> state_;
    bool expected_autocommit_;
    bool checked_ = false;
};

template <integer_parameter T>
[[nodiscard]] T check_integer_from_sqlite(sqlite3_int64 value) {
    if (!std::in_range<T>(value)) {
        throw std::range_error(
            "SQLite integer is outside the requested integer type's range.");
    }
    return static_cast<T>(value);
}

[[nodiscard]] inline sqlite3_stmt *compile_single_statement(sqlite3 *db, std::string_view sql) {
    validate_sql_text(sql, "SQL statement");

    sqlite3_stmt *stmt = nullptr;
    const char *tail = nullptr;
    const auto rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt, &tail);
    stmt_guard guard(stmt);

    if (rc != SQLITE_OK) {
        throw_sqlite(db, rc, "Compiling SQL statement");
    }
    if (!stmt) {
        throw std::invalid_argument(
            "SQL text does not contain an executable statement.");
    }

    if (tail && tail < sql.data() + sql.size()) {
        const auto remaining_size =
            static_cast<int>(sql.data() + sql.size() - tail);

        sqlite3_stmt *extra_stmt = nullptr;
        const char *extra_tail = nullptr;

        const auto extra_rc = sqlite3_prepare_v2(
            db,
            tail,
            remaining_size,
            &extra_stmt,
            &extra_tail);

        stmt_guard extra_guard(extra_stmt);

        if (extra_rc != SQLITE_OK) {
            throw_sqlite(
                db,
                extra_rc,
                "Parsing trailing SQL text");
        }

        if (extra_stmt) {
            throw std::invalid_argument(
                "Exactly one SQL statement is allowed.");
        }
    }

    return guard.release();
}

inline void check_not_out_of_memory(sqlite3_stmt *stmt) {
    auto db = sqlite3_db_handle(stmt);
    const int rc = sqlite3_errcode(db);
    if (rc == SQLITE_NOMEM) {
        throw_sqlite(db, rc, "Reading column");
    }
}

template <typename T>
[[nodiscard]] constexpr int expected_storage_class() {
    if constexpr (integer_field_value<T>) {
        return SQLITE_INTEGER;
    } else if constexpr (real_field_value<T>) {
        return SQLITE_FLOAT;
    } else if constexpr (text_field_value<T>) {
        return SQLITE_TEXT;
    } else if constexpr (byte_blob_field_value<T> ||
                         unsigned_char_blob_field_value<T>) {
        return SQLITE_BLOB;
    } else {
        static_assert(sizeof(T) == 0, "Unsupported SQLite result type.");
    }
}

template <live_field_value T>
void require_storage_class(sqlite3_stmt *stmt, int column) {
    const int actual = sqlite3_column_type(stmt, column);
    constexpr int expected = expected_storage_class<T>();

    if (actual != expected) {
        throw std::runtime_error(
            "Column " +
            std::to_string(column) +
            " has SQLite storage class " +
            std::to_string(actual) +
            ", but the requested C++ type requires " +
            std::to_string(expected) +
            ".");
    }
}

[[nodiscard]] inline std::string read_text_column(sqlite3_stmt *stmt, int column) {
    const auto raw = sqlite3_column_text(stmt, column);
    if (!raw) {
        check_not_out_of_memory(stmt);
        throw std::logic_error(
            "SQLite returned a null pointer for a non-NULL TEXT value.");
    }
    const auto bytes = sqlite3_column_bytes(stmt, column);
    return std::string(
        reinterpret_cast<const char *>(raw),
        static_cast<std::size_t>(bytes));
}

template <typename T>
concept blob_element_type =
    std::same_as<T, std::byte> ||
    std::same_as<T, unsigned char>;

template <blob_element_type T>
[[nodiscard]] inline std::vector<T> read_blob_column(sqlite3_stmt *stmt, int column) {
    const auto raw = sqlite3_column_blob(stmt, column);
    if (!raw) {
        check_not_out_of_memory(stmt);
        return {};
    }
    const auto bytes = sqlite3_column_bytes(stmt, column);
    const auto *first = reinterpret_cast<const T *>(raw);
    return std::vector<T>(first, first + static_cast<std::size_t>(bytes));
}

inline void require_column_count(sqlite3_stmt *stmt, bool expect_columns) {
    const bool has_columns = sqlite3_column_count(stmt) != 0;
    if (has_columns == expect_columns) {
        return;
    }
    throw std::logic_error(
        expect_columns
            ? "query() requires a statement that returns columns; use execute() instead."
            : "execute() requires a statement that returns no columns; use query() instead.");
}

template <typename T>
struct value_converter;

template <std::integral T>
    requires(!std::same_as<std::remove_cvref_t<T>, bool>)
struct value_converter<T> {
    static T convert(sqlite3_stmt *stmt, int col) {
        return check_integer_from_sqlite<T>(
            sqlite3_column_int64(stmt, col));
    }
};

template <>
struct value_converter<double> {
    static double convert(sqlite3_stmt *stmt, int col) {
        return sqlite3_column_double(stmt, col);
    }
};

template <>
struct value_converter<std::string> {
    static std::string convert(sqlite3_stmt *stmt, int col) {
        return read_text_column(stmt, col);
    }
};

template <typename T>
    requires byte_blob_field_value<T> ||
             unsigned_char_blob_field_value<T>
struct value_converter<T> {
    static T convert(sqlite3_stmt *stmt, int col) {
        using element_type = typename T::value_type;
        return read_blob_column<element_type>(stmt, col);
    }
};

[[nodiscard]] inline std::shared_ptr<const column_schema> build_column_schema(sqlite3_stmt *stmt) {
    auto schema = std::make_shared<column_schema>();

    const auto count = sqlite3_column_count(stmt);
    const auto size = static_cast<std::size_t>(count);

    schema->names.reserve(size);
    schema->by_name.reserve(size);

    for (int index = 0; index < count; ++index) {
        const auto raw_name =
            sqlite3_column_name(stmt, index);

        if (!raw_name) {
            auto db = sqlite3_db_handle(stmt);
            const auto rc = sqlite3_errcode(db);

            if (rc == SQLITE_NOMEM) {
                throw_sqlite(
                    db,
                    rc,
                    "Reading result column name");
            }

            throw std::runtime_error(
                "SQLite returned a null result column name.");
        }

        schema->names.emplace_back(raw_name);
        schema->by_name.try_emplace(raw_name, index);
    }

    return schema;
}

class cursor_state {
public:
    cursor_state(const std::shared_ptr<connection_state> &connection_state, std::shared_ptr<statement_state> statement, bool transaction_scoped, std::uint64_t transaction_generation, bool reusable_statement = false) : connection_(connection_state),
                                                                                                                                                                                                                          statement_(std::move(statement)),
                                                                                                                                                                                                                          transaction_scoped_(transaction_scoped),
                                                                                                                                                                                                                          transaction_generation_(transaction_generation),
                                                                                                                                                                                                                          reusable_statement_(reusable_statement) {
    }

    cursor_state(const cursor_state &) = delete;
    cursor_state &operator=(const cursor_state &) = delete;

    ~cursor_state() noexcept {
        close_noexcept();
    }

    void close_noexcept() noexcept {
        if (done_) {
            return;
        }

        done_ = true;
        ++row_generation_;
        row_valid_ = false;

        if (statement_ && reusable_statement_) {
            clean_prepared_statement_noexcept(*statement_);
        }
        statement_.reset();

        if (active_) {
            active_ = false;
            if (auto connection_state = connection_.lock()) {
                if (connection_state->active_cursors > 0) {
                    --connection_state->active_cursors;
                }
            }
        }
    }

    [[nodiscard]] std::shared_ptr<const column_schema> schema() const noexcept {
        return schema_;
    }

    void validate_row(std::uint64_t expected_generation) const {
        const auto connection_state = connection_.lock();
        if (!connection_usable(connection_state)) {
            throw std::logic_error(
                "The row/field view is no longer valid because its connection was closed.");
        }

        if (done_ || !row_valid_ || row_generation_ != expected_generation) {
            throw std::logic_error(
                "The row/field view is no longer valid because the cursor advanced or closed.");
        }

        if (!statement_ || !statement_->stmt) {
            throw std::logic_error(
                "The row/field view is no longer valid because its statement was finalized.");
        }

        if (transaction_scoped_ &&
            transaction_stale(*connection_state, transaction_generation_)) {
            throw std::logic_error(
                "The row/field view is no longer valid because its transaction ended.");
        }
    }

    [[nodiscard]] std::size_t column_count() const noexcept {
        return schema_ ? schema_->names.size() : 0;
    }

    [[nodiscard]] const column_schema &row_schema(std::uint64_t expected_generation) const {
        validate_row(expected_generation);
        return *schema_;
    }

    void advance() {
        if (done()) {
            throw std::logic_error(
                "Cannot advance a cursor whose connection or statement is no longer valid.");
        }
        step_impl(true);
    }

    void start() {
        if (started_) {
            throw std::logic_error("Cursor execution has already started.");
        }

        auto connection_state = require_connection();
        ensure_transaction_valid(connection_state);

        started_ = true;
        active_ = true;
        ++connection_state->active_cursors;
        step_impl(false);
    }

    void close() noexcept {
        close_noexcept();
    }

    void finish() {
        try {
            while (!done()) {
                advance();
            }
        } catch (...) {
            close_noexcept();
            throw;
        }
    }

    [[nodiscard]] bool done() const noexcept {
        if (done_ || !statement_ || !statement_->stmt) {
            return true;
        }
        return !connection_usable(connection_.lock());
    }

    [[nodiscard]] sqlite3_stmt *row_handle(std::uint64_t expected_generation) const {
        validate_row(expected_generation);
        return statement_->stmt;
    }

    [[nodiscard]] std::uint64_t row_generation() {
        if (done()) {
            throw std::logic_error(
                "Cannot access a cursor whose connection or statement is no longer valid.");
        }

        const auto connection_state = require_connection();
        ensure_transaction_valid(connection_state);

        if (!row_valid_) {
            throw std::logic_error(
                "The cursor does not currently reference a row.");
        }

        return row_generation_;
    }

    [[nodiscard]] sqlite3_stmt *statement_handle() const {
        return row_handle(row_generation_);
    }

private:
    [[nodiscard]] std::shared_ptr<connection_state>
    require_connection() {
        auto connection_state = connection_.lock();
        if (!connection_usable(connection_state)) {
            close_noexcept();
            throw std::logic_error(
                "The cursor is no longer valid because its connection was closed.");
        }
        return connection_state;
    }

    void ensure_transaction_valid(const std::shared_ptr<connection_state> &connection_state) {
        if (transaction_scoped_ && transaction_stale(*connection_state, transaction_generation_)) {
            close_noexcept();
            throw std::logic_error(
                "The cursor is no longer valid because its transaction ended.");
        }
    }

    void step_impl(bool invalidate_previous) {
        auto connection_state = require_connection();
        ensure_transaction_valid(connection_state);

        if (!statement_ || !statement_->stmt) {
            close_noexcept();
            throw std::logic_error(
                "The SQLite statement is no longer valid.");
        }

        if (invalidate_previous) {
            ++row_generation_;
            row_valid_ = false;
        }

        const auto rc = sqlite3_step(statement_->stmt);
        const bool expected_autocommit = !transaction_scoped_;
        const bool current_autocommit =
            sqlite3_get_autocommit(connection_state->db) != 0;

        if (current_autocommit != expected_autocommit) {
            std::optional<error_snapshot> sqlite_error_snapshot;
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
                sqlite_error_snapshot = snapshot_sqlite_error(
                    connection_state->db,
                    rc,
                    "Executing SQL statement");
            }

            close_noexcept();
            reconcile_autocommit_noexcept(
                connection_state,
                transaction_scoped_);

            if (sqlite_error_snapshot) {
                throw sqlite_error(
                    sqlite_error_snapshot->primary,
                    sqlite_error_snapshot->extended,
                    std::move(sqlite_error_snapshot->message));
            }
            throw std::logic_error(
                "Executing the query changed the connection's autocommit "
                "state outside the transaction API.");
        }

        if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
            if (!schema_) {
                schema_ = build_column_schema(statement_->stmt);
            }
        }

        if (rc == SQLITE_ROW) {
            row_valid_ = true;
            return;
        }
        if (rc == SQLITE_DONE) {
            close_noexcept();
            return;
        }

        auto error = snapshot_sqlite_error(connection_state->db, rc, "Executing SQL statement");
        close_noexcept();
        throw sqlite_error(
            error.primary,
            error.extended,
            std::move(error.message));
    }

    std::weak_ptr<connection_state> connection_;
    std::shared_ptr<statement_state> statement_;
    std::shared_ptr<const column_schema> schema_;
    bool row_valid_ = false;
    std::uint64_t row_generation_ = 0;
    bool transaction_scoped_ = false;
    std::uint64_t transaction_generation_ = 0;
    bool started_ = false;
    bool active_ = false;
    bool done_ = false;
    bool reusable_statement_ = false;
};

using parameter_value = std::variant<
    std::nullptr_t,
    sqlite3_int64,
    double,
    std::string,
    std::vector<std::byte>>;

template <bindable_parameter T>
[[nodiscard]] parameter_value make_parameter_value(T &&value) {
    using decayed = std::decay_t<T>;

    if constexpr (is_optional_v<decayed>) {
        if (!value) {
            return nullptr;
        }

        return make_parameter_value(*value);
    } else if constexpr (null_parameter<decayed>) {
        return nullptr;
    } else if constexpr (integer_parameter<decayed>) {
        if (!std::in_range<sqlite3_int64>(value)) {
            throw std::range_error(
                "Integer parameter is outside SQLite INTEGER range.");
        }
        return static_cast<sqlite3_int64>(value);
    } else if constexpr (real_parameter<decayed>) {
        const auto real = static_cast<double>(value);

        if (std::isnan(real)) {
            throw std::invalid_argument(
                "NaN cannot be bound as an SQLite REAL parameter.");
        }

        return real;
    } else if constexpr (text_parameter<decayed>) {
        if constexpr (std::is_pointer_v<std::remove_reference_t<T>>) {
            if (value == nullptr) {
                throw std::invalid_argument(
                    "A null character pointer cannot be bound as TEXT; "
                    "use nullptr to bind SQL NULL.");
            }
        }

        return std::string(std::string_view(value));
    } else if constexpr (blob_parameter<decayed>) {
        const auto bytes = std::as_bytes(value);

        return std::vector<std::byte>(
            bytes.begin(),
            bytes.end());
    } else {
        static_assert(
            sizeof(T) == 0,
            "Unsupported parameter type for SQLite binding.");
    }
}

inline void bind_parameter(
    sqlite3_stmt *stmt,
    int index,
    const parameter_value &parameter) {

    const int rc = std::visit(
        [&](const auto &value) -> int {
            using value_type =
                std::remove_cvref_t<decltype(value)>;

            if constexpr (
                std::same_as<value_type, std::nullptr_t>) {

                return sqlite3_bind_null(
                    stmt,
                    index);

            } else if constexpr (
                std::same_as<value_type, sqlite3_int64>) {

                return sqlite3_bind_int64(
                    stmt,
                    index,
                    value);

            } else if constexpr (
                std::same_as<value_type, double>) {

                return sqlite3_bind_double(
                    stmt,
                    index,
                    value);

            } else if constexpr (
                std::same_as<value_type, std::string>) {

                const char *text =
                    value.empty()
                        ? ""
                        : value.data();

                return sqlite3_bind_text64(
                    stmt,
                    index,
                    text,
                    static_cast<sqlite3_uint64>(
                        value.size()),
                    SQLITE_TRANSIENT,
                    SQLITE_UTF8);

            } else if constexpr (
                std::same_as<
                    value_type,
                    std::vector<std::byte>>) {

                if (value.empty()) {
                    return sqlite3_bind_zeroblob64(
                        stmt,
                        index,
                        0);
                }

                return sqlite3_bind_blob64(
                    stmt,
                    index,
                    value.data(),
                    static_cast<sqlite3_uint64>(
                        value.size()),
                    SQLITE_TRANSIENT);
            }
        },
        parameter);

    if (rc != SQLITE_OK) [[unlikely]] {
        auto db = sqlite3_db_handle(stmt);

        auto error = snapshot_sqlite_error(
            db,
            rc,
            "Binding parameter slot " +
                std::to_string(index));

        throw sqlite_error(
            error.primary,
            error.extended,
            std::move(error.message));
    }
}
} // namespace detail

class materialized_field {
public:
    materialized_field() noexcept = default;

    [[nodiscard]] bool is_null() const noexcept {
        return value_ && std::holds_alternative<std::nullptr_t>(*value_);
    }

    materialized_field(std::shared_ptr<const detail::default_materialized_value> value) noexcept : value_(std::move(value)) {
    }

    template <detail::default_materialized_field_result T>
    T as() const {
        if (!value_) {
            throw std::logic_error("The materialized field is empty.");
        }
        if (std::holds_alternative<std::nullptr_t>(*value_)) {
            if constexpr (detail::is_optional_v<T>) {
                return std::nullopt;
            }
            throw std::runtime_error(
                "Materialized field is NULL, but a non-optional type was requested.");
        }

        if constexpr (detail::is_optional_v<T>) {
            using value_type = typename T::value_type;
            return T{convert_non_null<value_type>()};
        } else {
            return convert_non_null<T>();
        }
    }

private:
    template <detail::default_materialized_field_value T>
    T convert_non_null() const {
        const detail::default_materialized_value *ptr = value_.get();
        if constexpr (detail::integer_field_value<T>) {
            if (const auto integer = std::get_if<sqlite3_int64>(ptr)) {
                return detail::check_integer_from_sqlite<T>(*integer);
            }
        } else if constexpr (detail::real_field_value<T>) {
            if (const auto real = std::get_if<double>(ptr)) {
                return *real;
            }
        } else if constexpr (std::same_as<T, std::string>) {
            if (const auto text = std::get_if<std::string>(ptr)) {
                return *text;
            }
        } else if constexpr (detail::byte_blob_field_value<T>) {
            if (const auto blob = std::get_if<std::vector<std::byte>>(ptr)) {
                return *blob;
            }
        }

        throw std::runtime_error(
            "Materialized value has an incompatible SQLite storage class.");
    }

    std::shared_ptr<const detail::default_materialized_value> value_;
};

class materialized_row {
public:
    materialized_row(
        std::shared_ptr<const detail::column_schema> schema,
        std::shared_ptr<const std::vector<detail::default_materialized_value>> values)
        : schema_(std::move(schema)),
          values_(std::move(values)) {
    }

    [[nodiscard]] materialized_field operator[](std::size_t index) const {
        if (!schema_) {
            throw std::logic_error("The materialized row has no schema.");
        }

        if (index >= values_->size()) {
            throw std::out_of_range(
                "Column index " + std::to_string(index) +
                " is outside [0, " +
                std::to_string(values_->size()) +
                ").");
        }

        return materialized_field(
            std::shared_ptr<const detail::default_materialized_value>(
                values_,
                &(*values_)[index]));
    }

    [[nodiscard]] materialized_field operator[](std::string_view name) const {
        if (!schema_) {
            throw std::logic_error("The materialized row has no schema.");
        }

        const auto it = schema_->by_name.find(name);
        if (it == schema_->by_name.end()) {
            throw std::out_of_range(
                "Column '" + std::string(name) + "' was not found.");
        }
        const auto index = static_cast<std::size_t>(it->second);
        if (index >= values_->size()) {
            throw std::out_of_range(
                "Column '" + std::string(name) +
                "' has an invalid index " + std::to_string(index) +
                " but the materialized row only contains " +
                std::to_string(values_->size()) + " values.");
        }
        return materialized_field(std::shared_ptr<const detail::default_materialized_value>(values_, &(*values_)[index]));
    }

private:
    std::shared_ptr<const detail::column_schema> schema_;
    std::shared_ptr<const std::vector<detail::default_materialized_value>> values_;
};

class materialized_result {
    friend class cursor;

public:
    using container_type = std::vector<materialized_row>;
    using const_iterator = container_type::const_iterator;

    explicit materialized_result(
        std::shared_ptr<const detail::column_schema> schema) noexcept
        : schema_(std::move(schema)) {
    }

    [[nodiscard]] bool empty() const noexcept {
        return rows_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return rows_.size();
    }

    [[nodiscard]] std::size_t column_count() const noexcept {
        return schema_ ? schema_->names.size() : 0;
    }

    [[nodiscard]] const_iterator begin() const noexcept {
        return rows_.begin();
    }

    [[nodiscard]] const_iterator end() const noexcept {
        return rows_.end();
    }

private:
    std::shared_ptr<const detail::column_schema> schema_;
    container_type rows_;
};

class field_view {
public:
    field_view(const std::shared_ptr<detail::cursor_state> &state, std::uint64_t generation, int column) noexcept : state_(state), generation_(generation), column_(column) {
    }

    [[nodiscard]] bool is_null() const {
        const auto state = require_state();
        auto stmt = handle(state);
        return sqlite3_column_type(stmt, column_) == SQLITE_NULL;
    }

    template <detail::live_field_result T>
    T as() const {
        const auto state = require_state();

        try {
            auto stmt = handle(state);

            if (sqlite3_column_type(stmt, column_) == SQLITE_NULL) {
                if constexpr (detail::is_optional_v<T>) {
                    return std::nullopt;
                }

                throw std::runtime_error(
                    "Column " + std::to_string(column_) +
                    " is NULL, but a non-optional type was requested.");
            }

            if constexpr (detail::is_optional_v<T>) {
                using value_type = typename T::value_type;

                detail::require_storage_class<value_type>(
                    stmt,
                    column_);

                return T{detail::value_converter<value_type>::convert(stmt, column_)};
            } else {
                detail::require_storage_class<T>(
                    stmt,
                    column_);

                return detail::value_converter<T>::convert(stmt, column_);
            }
        } catch (const sqlite_error &) {
            state->close();
            throw;
        }
    }

    template <detail::live_field_value T>
    T as(T default_value) const {
        const auto state = require_state();

        try {
            auto stmt = handle(state);

            if (sqlite3_column_type(stmt, column_) == SQLITE_NULL) {
                return default_value;
            }

            detail::require_storage_class<T>(
                stmt,
                column_);

            return detail::value_converter<T>::convert(stmt, column_);
        } catch (const sqlite_error &) {
            state->close();
            throw;
        }
    }

private:
    [[nodiscard]] std::shared_ptr<detail::cursor_state> require_state() const {
        auto state = state_.lock();
        if (!state) {
            throw std::logic_error(
                "The field view is no longer valid because its cursor was destroyed.");
        }
        return state;
    }

    [[nodiscard]] sqlite3_stmt *handle(const std::shared_ptr<detail::cursor_state> &state) const {
        auto stmt = state->row_handle(generation_);
        const auto count = static_cast<int>(state->column_count());
        if (column_ < 0 || column_ >= count) {
            throw std::out_of_range(
                "Column index " + std::to_string(column_) +
                " is outside [0, " + std::to_string(count) + ").");
        }
        return stmt;
    }

    std::weak_ptr<detail::cursor_state> state_;
    std::uint64_t generation_;
    int column_;
};

class row_view {
public:
    row_view(const std::shared_ptr<detail::cursor_state> &state, std::uint64_t generation) noexcept : state_(state), generation_(generation) {
    }

    [[nodiscard]] field_view operator[](std::string_view name) const {
        const auto state = require_state();
        const auto &schema = state->row_schema(generation_);

        const auto it = schema.by_name.find(name);
        if (it == schema.by_name.end()) {
            throw std::out_of_range(
                "Column '" + std::string(name) + "' was not found.");
        }

        return field_view(state, generation_, it->second);
    }

    [[nodiscard]] field_view operator[](std::size_t index) const {
        const auto state = require_state();
        const auto &schema = state->row_schema(generation_);

        if (index >= schema.names.size()) {
            throw std::out_of_range(
                "Column index " + std::to_string(index) +
                " is outside [0, " +
                std::to_string(schema.names.size()) +
                ").");
        }

        return field_view(
            state,
            generation_,
            static_cast<int>(index));
    }

private:
    [[nodiscard]] std::shared_ptr<detail::cursor_state> require_state() const {
        auto state = state_.lock();
        if (!state) {
            throw std::logic_error(
                "The row view is no longer valid because its cursor was destroyed.");
        }
        return state;
    }

    std::weak_ptr<detail::cursor_state> state_;
    std::uint64_t generation_ = 0;
};

class cursor {
public:
    class iterator {
    public:
        using iterator_concept = std::input_iterator_tag;
        using value_type = row_view;
        using difference_type = std::ptrdiff_t;

        iterator(std::shared_ptr<detail::cursor_state> state) noexcept
            : state_(std::move(state)) {
        }

        [[nodiscard]] row_view operator*() const {
            if (!state_ || state_->done()) {
                throw std::logic_error("Cannot dereference an end cursor iterator.");
            }
            return row_view(state_, state_->row_generation());
        }

        iterator &operator++() {
            if (!state_) {
                throw std::logic_error("Cannot increment an end cursor iterator.");
            }
            state_->advance();
            return *this;
        }

        void operator++(int) {
            ++(*this);
        }

        friend bool operator==(
            const iterator &value,
            std::default_sentinel_t) noexcept {
            return !value.state_ || value.state_->done();
        }

        friend bool operator==(
            std::default_sentinel_t sentinel,
            const iterator &value) noexcept {
            return value == sentinel;
        }

        friend bool operator==(const iterator &left, const iterator &right) noexcept {
            if (left.state_ == right.state_) {
                return true;
            }

            const bool left_done = !left.state_ || left.state_->done();
            const bool right_done = !right.state_ || right.state_->done();
            return left_done && right_done;
        }

    private:
        std::shared_ptr<detail::cursor_state> state_;
    };

    explicit cursor(std::shared_ptr<detail::cursor_state> state) noexcept : state_(std::move(state)) {
    }

    cursor(const cursor &) = delete;
    cursor &operator=(const cursor &) = delete;
    cursor(cursor &&) noexcept = default;
    cursor &operator=(cursor &&) noexcept = default;

    [[nodiscard]] iterator begin() noexcept {
        return iterator(state_);
    }

    [[nodiscard]] std::default_sentinel_t end() const noexcept {
        return {};
    }

    [[nodiscard]] bool done() const noexcept {
        return !state_ || state_->done();
    }

    [[nodiscard]] bool empty() const noexcept {
        return done();
    }

    [[nodiscard]] std::size_t column_count() const noexcept {
        return state_ ? state_->schema()->names.size() : 0;
    }

    [[nodiscard]] std::string_view column_name(std::size_t index) const {
        if (!state_) {
            throw std::logic_error("The cursor is empty.");
        }
        return state_->schema()->names.at(index);
    }

    void close() noexcept {
        if (state_) {
            state_->close();
        }
    }

    void finish() {
        if (state_) {
            state_->finish();
        }
    }

    [[nodiscard]] materialized_result materialize() {
        if (!state_) {
            return materialized_result(nullptr);
        }
        materialized_result result(state_->schema());

        try {
            while (!state_->done()) {
                auto stmt = state_->statement_handle();
                const auto schema = state_->schema();
                std::vector<detail::default_materialized_value> values;
                values.reserve(schema->names.size());

                for (std::size_t i = 0; i < schema->names.size(); ++i) {
                    const int column = static_cast<int>(i);
                    switch (sqlite3_column_type(stmt, column)) {
                    case SQLITE_NULL:
                        values.emplace_back(nullptr);
                        break;
                    case SQLITE_INTEGER:
                        values.emplace_back(sqlite3_column_int64(stmt, column));
                        break;
                    case SQLITE_FLOAT:
                        values.emplace_back(sqlite3_column_double(stmt, column));
                        break;
                    case SQLITE_TEXT:
                        values.emplace_back(detail::read_text_column(stmt, column));
                        break;
                    case SQLITE_BLOB:
                        values.emplace_back(detail::read_blob_column<std::byte>(stmt, column));
                        break;
                    default:
                        throw std::runtime_error(
                            "SQLite returned an unknown storage class.");
                    }
                }

                result.rows_.push_back(materialized_row(schema, std::make_shared<const std::vector<detail::default_materialized_value>>(std::move(values))));

                state_->advance();
            }
            return result;
        } catch (...) {
            state_->close();
            throw;
        }
    }

    template <typename Mapper>
    auto materialize(Mapper &&mapper) {
        using mapper_result = std::invoke_result_t<Mapper &, const row_view &>;
        using value_type = std::remove_cvref_t<mapper_result>;

        static_assert(
            std::same_as<mapper_result, value_type>,
            "The materialization mapper must return an owning value type.");

        std::vector<value_type> result;

        if (!state_) {
            return result;
        }

        try {
            while (!state_->done()) {
                row_view row(state_, state_->row_generation());

                result.emplace_back(std::invoke(mapper, std::as_const(row)));

                state_->advance();
            }
            return result;
        } catch (...) {
            state_->close();
            throw;
        }
    }

private:
    std::shared_ptr<detail::cursor_state> state_;
};

class prepared_statement {
    friend class executor_base;

public:
    prepared_statement(const prepared_statement &) = delete;
    prepared_statement &operator=(const prepared_statement &) = delete;

    prepared_statement(prepared_statement &&) noexcept = default;
    prepared_statement &operator=(prepared_statement &&) noexcept = default;

    explicit prepared_statement(std::shared_ptr<detail::statement_state> statement) noexcept : statement_(std::move(statement)) {
    }

    ~prepared_statement() = default;

private:
    std::shared_ptr<detail::statement_state> statement_;
};

class params {
public:
    params() = default;

    template <detail::bindable_parameter... Args>
        requires(sizeof...(Args) > 0)
    params(Args &&...args) {
        values_.reserve(sizeof...(Args));

        (append(std::forward<Args>(args)), ...);
    }

    template <detail::bindable_parameter T>
    void append(T &&value) {
        values_.push_back(
            detail::make_parameter_value(
                std::forward<T>(value)));
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return values_.size();
    }

private:
    std::vector<detail::parameter_value> values_;

    friend void detail::bind_parameters(
        sqlite3_stmt *,
        const params &);
};

namespace detail {
inline void bind_parameters(
    sqlite3_stmt *stmt,
    const params &parameters) {

    const auto expected =
        sqlite3_bind_parameter_count(stmt);

    const auto actual =
        parameters.values_.size();

    if (static_cast<std::size_t>(expected) != actual) {
        throw std::invalid_argument(
            "SQL statement's highest parameter slot is " +
            std::to_string(expected) +
            ", but the parameter list has size " +
            std::to_string(actual) +
            ".");
    }

    int index = 1;

    for (const auto &parameter :
         parameters.values_) {

        bind_parameter(
            stmt,
            index++,
            parameter);
    }
}
} // namespace detail

class executor_base {
public:
    executor_base(const executor_base &) = delete;
    executor_base &operator=(const executor_base &) = delete;

    [[nodiscard]] sqlite3_int64 last_insert_rowid() const {
        const auto state = require_state();
        return sqlite3_last_insert_rowid(state->db);
    }

    [[nodiscard]] sqlite3_int64 changes() const {
        const auto state = require_state();
        return sqlite3_changes64(state->db);
    }

    [[nodiscard]] prepared_statement prepare(std::string_view sql) {
        auto state = require_state();

        auto raw_stmt = detail::compile_single_statement(state->db, sql);
        detail::stmt_guard guard(raw_stmt);

        auto stmt = std::make_shared<detail::statement_state>(state, raw_stmt);
        (void)guard.release();

        state->register_statement(stmt);

        return prepared_statement(std::move(stmt));
    }

    [[nodiscard]] cursor query(prepared_statement &prepared, const params &parameters = {}) {
        auto state = require_state();
        auto stmt = require_prepared_statement(prepared, state);
        if (stmt->leased) {
            throw std::logic_error(
                "The prepared statement already has an active execution.");
        }

        detail::require_column_count(stmt->stmt, true);
        stmt->leased = true;

        try {
            detail::bind_parameters(stmt->stmt, parameters);

            auto cursor_state = std::make_shared<detail::cursor_state>(state, stmt, transaction_owner_, transaction_generation_, true);
            cursor_state->start();
            return cursor(std::move(cursor_state));
        } catch (...) {
            if (stmt->leased) {
                detail::clean_prepared_statement_noexcept(*stmt);
            }
            throw;
        }
    }

    void execute(prepared_statement &prepared, const params &parameters = {}) {
        auto state = require_state();
        auto stmt = require_prepared_statement(prepared, state);
        if (stmt->leased) {
            throw std::logic_error(
                "The prepared statement already has an active execution.");
        }

        detail::require_column_count(stmt->stmt, false);
        detail::autocommit_guard autocommit(state, transaction_owner_);

        try {
            stmt->leased = true;
            detail::bind_parameters(stmt->stmt, parameters);

            const auto rc = sqlite3_step(stmt->stmt);
            std::optional<detail::error_snapshot> error;
            if (rc != SQLITE_DONE) {
                error = detail::snapshot_sqlite_error(
                    state->db,
                    rc,
                    "Executing prepared statement");
            }

            detail::clean_prepared_statement_noexcept(*stmt);

            if (error) {
                throw sqlite_error(
                    error->primary,
                    error->extended,
                    std::move(error->message));
            }

            autocommit.check();
        } catch (...) {
            if (stmt->leased) {
                detail::clean_prepared_statement_noexcept(*stmt);
            }
            throw;
        }
    }

    [[nodiscard]] cursor query(std::string_view sql, const params &parameters = {}) {
        auto state = require_state();

        auto raw_stmt = detail::compile_single_statement(state->db, sql);
        detail::stmt_guard guard(raw_stmt);

        detail::require_column_count(raw_stmt, true);

        detail::bind_parameters(raw_stmt, parameters);

        auto stmt = std::make_shared<detail::statement_state>(state, raw_stmt);
        (void)guard.release();

        state->register_statement(stmt);

        auto cursor_state = std::make_shared<detail::cursor_state>(state, std::move(stmt), transaction_owner_, transaction_generation_);

        try {
            cursor_state->start();
        } catch (...) {
            cursor_state->close();
            throw;
        }
        return cursor(std::move(cursor_state));
    }

    void execute(std::string_view sql, const params &parameters = {}) {
        auto state = require_state();
        detail::autocommit_guard autocommit(state, transaction_owner_);

        auto raw_stmt = detail::compile_single_statement(state->db, sql);
        detail::stmt_guard guard(raw_stmt);

        detail::require_column_count(raw_stmt, false);

        detail::bind_parameters(raw_stmt, parameters);

        const auto rc = sqlite3_step(raw_stmt);
        if (rc != SQLITE_DONE) {
            detail::throw_sqlite(state->db, rc, "Executing SQL statement");
        }

        autocommit.check();
    }

private:
    [[nodiscard]] static std::shared_ptr<detail::statement_state> require_prepared_statement(prepared_statement &prepared, const std::shared_ptr<detail::connection_state> &executor_state) {
        if (!prepared.statement_ || !prepared.statement_->stmt) {
            throw std::logic_error(
                "The prepared statement is no longer valid.");
        }

        const auto statement_connection = prepared.statement_->connection.lock();
        if (!detail::connection_usable(statement_connection)) {
            throw std::logic_error(
                "The prepared statement's connection is closed.");
        }
        if (statement_connection.get() != executor_state.get()) {
            throw std::logic_error(
                "The prepared statement belongs to a different SQLite connection.");
        }

        return prepared.statement_;
    }

protected:
    executor_base(executor_base &&) noexcept = default;
    executor_base &operator=(executor_base &&) noexcept = default;

    ~executor_base() = default;

    executor_base(const std::shared_ptr<detail::connection_state> &state, bool transaction_owner, std::uint64_t transaction_generation) noexcept : state_(state), transaction_owner_(transaction_owner), transaction_generation_(transaction_generation) {
    }

    [[nodiscard]] std::shared_ptr<detail::connection_state> require_state() const {
        auto state = state_.lock();
        if (!connection_usable(state)) {
            throw std::logic_error(
                "The SQLite connection is moved-from or closed.");
        }

        if (transaction_owner_) {
            if (transaction_stale(*state, transaction_generation_)) {
                throw std::logic_error(
                    "The transaction has already ended.");
            }

            if (sqlite3_get_autocommit(state->db) != 0) {
                detail::mark_transaction_ended_noexcept(state);
                throw std::logic_error(
                    "The transaction was ended outside the transaction API.");
            }
        } else {
            if (state->transaction_active) {
                throw std::logic_error(
                    "The connection is controlled by an active transaction; use the transaction object.");
            }
            if (sqlite3_get_autocommit(state->db) == 0) {
                throw std::logic_error(
                    "SQLite has an untracked active transaction, possibly "
                    "created through native_handle().");
            }
        }

        return state;
    }

    std::weak_ptr<detail::connection_state> state_;
    bool transaction_owner_ = false;
    std::uint64_t transaction_generation_ = 0;
};

class transaction;

class connection : public executor_base {
public:
    using handle_type = sqlite3 *;

    explicit connection(std::string_view path)
        : connection(open(path)) {
    }

    connection(const connection &) = delete;
    connection &operator=(const connection &) = delete;
    connection(connection &&) noexcept = default;
    connection &operator=(connection &&) noexcept = default;
    ~connection() noexcept = default;

    [[nodiscard]] handle_type native_handle() noexcept {
        return owner_ ? owner_->db : nullptr;
    }

    [[nodiscard]] handle_type native_handle() const noexcept {
        return owner_ ? owner_->db : nullptr;
    }

    [[nodiscard]] transaction begin_transaction();

private:
    explicit connection(
        std::shared_ptr<detail::connection_state> state) noexcept : executor_base(state, false, 0), owner_(std::move(state)) {
    }

    [[nodiscard]] static std::shared_ptr<detail::connection_state> open(std::string_view path_view) {
        if (path_view.find('\0') != std::string_view::npos) {
            throw std::invalid_argument(
                "Database path contains an embedded NUL byte.");
        }

        std::string path(path_view);
        sqlite3 *raw_db = nullptr;
        const auto rc = sqlite3_open(path.c_str(), &raw_db);
        detail::db_guard guard(raw_db);
        if (rc != SQLITE_OK) {
            detail::throw_sqlite(raw_db, rc, "Opening SQLite connection");
        }

        auto state = std::make_shared<detail::connection_state>(raw_db);
        (void)guard.release();
        return state;
    }

    std::shared_ptr<detail::connection_state> owner_;
};

class transaction : public executor_base {
    friend class connection;

public:
    transaction(const transaction &) = delete;
    transaction &operator=(const transaction &) = delete;

    transaction(transaction &&other) noexcept
        : executor_base(std::move(other)),
          done_(std::exchange(other.done_, true)) {
    }

    transaction &operator=(transaction &&) = delete;

    ~transaction() noexcept {
        if (done_) {
            return;
        }

        auto state = state_.lock();
        if (!detail::connection_usable(state)) {
            done_ = true;
            return;
        }

        if (!detail::transaction_stale(*state, transaction_generation_)) {
            if (sqlite3_get_autocommit(state->db) == 0) {
                detail::exec_control_sql_noexcept(state->db, "ROLLBACK;");
            }

            if (sqlite3_get_autocommit(state->db) != 0) {
                detail::mark_transaction_ended_noexcept(state);
            } else {
                state->close_noexcept();
            }
        }

        done_ = true;
    }

    void commit() {
        auto state = require_state();
        ensure_no_active_cursor(state, "commit");
        try {
            detail::exec_control_sql(
                state->db,
                "COMMIT;",
                "Committing transaction");
        } catch (...) {
            if (detail::connection_usable(state) &&
                sqlite3_get_autocommit(state->db) != 0) {
                done_ = true;
                detail::mark_transaction_ended_noexcept(state);
            }
            throw;
        }

        if (sqlite3_get_autocommit(state->db) == 0) {
            throw std::logic_error(
                "SQLite reported a successful COMMIT but the transaction "
                "is still active.");
        }
        done_ = true;
        detail::mark_transaction_ended_noexcept(state);
    }

    void rollback() {
        auto state = require_state();
        ensure_no_active_cursor(state, "roll back");
        try {
            detail::exec_control_sql(
                state->db,
                "ROLLBACK;",
                "Rolling back transaction");
        } catch (...) {
            if (detail::connection_usable(state) && sqlite3_get_autocommit(state->db) != 0) {
                done_ = true;
                detail::mark_transaction_ended_noexcept(state);
            }
            throw;
        }

        if (sqlite3_get_autocommit(state->db) == 0) {
            throw std::logic_error(
                "SQLite reported a successful ROLLBACK but the transaction "
                "is still active.");
        }
        done_ = true;
        detail::mark_transaction_ended_noexcept(state);
    }

    static void ensure_no_active_cursor(const std::shared_ptr<detail::connection_state> &state, std::string_view action) {
        if (state->active_cursors != 0) {
            throw std::logic_error(
                "Cannot " + std::string(action) +
                " the transaction while a cursor is active; call finish() or close() first.");
        }
    }

private:
    explicit transaction(const std::shared_ptr<detail::connection_state> &state) : executor_base(state, true, 0) {
        if (!detail::connection_usable(state)) {
            throw std::logic_error(
                "The SQLite connection is moved-from or closed.");
        }
        if (state->transaction_active) {
            throw std::logic_error(
                "A transaction is already active on this connection.");
        }
        if (state->active_cursors != 0) {
            throw std::logic_error(
                "Cannot begin a transaction while a cursor is active on the connection.");
        }
        if (sqlite3_get_autocommit(state->db) == 0) {
            throw std::logic_error(
                "Cannot begin a transaction because SQLite already has an "
                "untracked active transaction, possibly created through "
                "native_handle().");
        }

        detail::exec_control_sql(state->db, "BEGIN;", "Beginning transaction");

        if (sqlite3_get_autocommit(state->db) != 0) {
            throw std::logic_error(
                "SQLite reported a successful BEGIN but autocommit remains enabled.");
        }

        state->transaction_active = true;
        ++state->transaction_generation;
        transaction_generation_ = state->transaction_generation;
    }

    bool done_ = false;
};

inline transaction connection::begin_transaction() {
    const auto state = require_state();
    return transaction(state);
}

} // namespace sqlitecxx