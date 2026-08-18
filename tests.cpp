#include "sqlitecxx.hpp"

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class test_failure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message, const char *file, int line) {
    throw test_failure(
        std::string(file) + ":" + std::to_string(line) + ": " + std::move(message));
}

void check(bool condition, const char *expression, const char *file, int line) {
    if (!condition) {
        fail(std::string("CHECK failed: ") + expression, file, line);
    }
}

template <typename ExpectedException, typename Callable>
void check_throws(Callable &&callable, const char *expected_name, const char *expression, const char *file, int line) {
    try {
        std::forward<Callable>(callable)();
    } catch (const ExpectedException &) {
        return;
    } catch (const std::exception &error) {
        fail(
            std::string("Expected ") + expected_name + " from " + expression +
                ", but caught a different std::exception: " + error.what(),
            file,
            line);
    } catch (...) {
        fail(
            std::string("Expected ") + expected_name + " from " + expression +
                ", but caught a non-standard exception.",
            file,
            line);
    }

    fail(
        std::string("Expected ") + expected_name + " from " + expression +
            ", but no exception was thrown.",
        file,
        line);
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define CHECK_THROWS(exception_type, expression) \
    check_throws<exception_type>([&] { static_cast<void>(expression); }, #exception_type, #expression, __FILE__, __LINE__)

template <typename T>
concept live_field_readable_as = requires(const sqlitecxx::field_view &field) {
    { field.template as<T>() } -> std::same_as<T>;
};

template <typename T>
concept materialized_field_readable_as = requires(const sqlitecxx::materialized_field &field) {
    { field.template as<T>() } -> std::same_as<T>;
};

static_assert(live_field_readable_as<int>);
static_assert(live_field_readable_as<double>);
static_assert(!live_field_readable_as<bool>);
static_assert(!live_field_readable_as<char>);
static_assert(!live_field_readable_as<float>);
static_assert(materialized_field_readable_as<std::vector<std::byte>>);
static_assert(!materialized_field_readable_as<std::vector<unsigned char>>);

// -----------------------------------------------------------------------------
// Test: Connection, execution, and typed queries
// Verifies inserts, change metadata, row lookup, typed reads, and SQL NULL.
// -----------------------------------------------------------------------------

void test_connection_execution_and_typed_queries() {
    sqlitecxx::connection db(":memory:");

    db.execute(R"(
        CREATE TABLE users (
            id   INTEGER PRIMARY KEY,
            name TEXT NOT NULL,
            age  INTEGER
        )
    )");

    db.execute(
        "INSERT INTO users(name, age) VALUES(?1, ?2)",
        {"Alice", 30});

    CHECK(db.changes() == 1);
    CHECK(db.last_insert_rowid() == 1);

    sqlitecxx::params bob;
    bob.append("Bob");
    bob.append(std::optional<int>{});
    db.execute(
        "INSERT INTO users(name, age) VALUES(?1, ?2)",
        bob);

    CHECK(db.changes() == 1);
    CHECK(db.last_insert_rowid() == 2);

    auto result = db.query(
        "SELECT id, name, age FROM users ORDER BY id");

    CHECK(!result.empty());
    CHECK(result.column_count() == 3);
    CHECK(result.column_name(0) == "id");
    CHECK(result.column_name(1) == "name");
    CHECK(result.column_name(2) == "age");

    auto iterator = result.begin();
    auto alice = *iterator;
    CHECK(alice[0].as<std::int64_t>() == 1);
    CHECK(alice["name"].as<std::string>() == "Alice");
    CHECK(alice["age"].as<std::optional<int>>() == std::optional<int>{30});

    ++iterator;
    auto bob_row = *iterator;
    CHECK(bob_row["id"].as<int>() == 2);
    CHECK(bob_row[1].as<std::string>() == "Bob");
    CHECK(bob_row["age"].is_null());
    CHECK(!bob_row["age"].as<std::optional<int>>().has_value());
    CHECK(bob_row["age"].as<int>(99) == 99);

    ++iterator;
    CHECK(iterator == std::default_sentinel);
    CHECK(result.done());
    CHECK(result.empty());
}

// -----------------------------------------------------------------------------
// Test: Parameter binding and slot numbering
// Verifies SQLite slot rules, optional values, NaN rejection, and infinity.
// -----------------------------------------------------------------------------

void test_parameter_binding_and_slot_numbering() {
    sqlitecxx::connection db(":memory:");

    auto slots = db.query(
        "SELECT ?2 AS second, ?1 AS first, ?1 AS repeated, ?5 AS fifth",
        {10, 20, nullptr, nullptr, 50});

    auto slot_row = *slots.begin();
    CHECK(slot_row["second"].as<int>() == 20);
    CHECK(slot_row["first"].as<int>() == 10);
    CHECK(slot_row["repeated"].as<int>() == 10);
    CHECK(slot_row["fifth"].as<int>() == 50);
    slots.finish();

    CHECK_THROWS(
        std::invalid_argument,
        db.query("SELECT ?5", {123}));
    CHECK_THROWS(
        std::invalid_argument,
        db.query("SELECT ?1", {1, 2}));

    std::optional<int> no_value;
    std::optional<int> seven = 7;
    auto optionals = db.query(
        "SELECT ?1 AS missing, ?2 AS present",
        {no_value, seven});

    auto optional_row = *optionals.begin();
    CHECK(optional_row["missing"].is_null());
    CHECK(optional_row["present"].as<int>() == 7);
    optionals.finish();

    CHECK_THROWS(
        std::invalid_argument,
        sqlitecxx::params{std::numeric_limits<double>::quiet_NaN()});

    auto infinity = db.query(
        "SELECT ?1 AS value",
        {std::numeric_limits<double>::infinity()});
    const auto infinity_value = (*infinity.begin())["value"].as<double>();
    CHECK(std::isinf(infinity_value));
    CHECK(infinity_value > 0.0);
    infinity.finish();
}

// -----------------------------------------------------------------------------
// Test: Strict result typing and conversion errors
// Verifies storage-class checks, NULL handling, range checks, and lookup errors.
// -----------------------------------------------------------------------------

void test_strict_result_typing_and_conversion_errors() {
    sqlitecxx::connection db(":memory:");

    auto result = db.query(
        "SELECT 42 AS integer_value, 1.5 AS real_value, "
        "'42' AS text_value, NULL AS null_value");
    auto row = *result.begin();

    CHECK(row["integer_value"].as<int>() == 42);
    CHECK(row["real_value"].as<double>() == 1.5);
    CHECK(row["text_value"].as<std::string>() == "42");
    CHECK(row["integer_value"].as<std::optional<int>>() == std::optional<int>{42});

    CHECK_THROWS(
        std::runtime_error,
        row["integer_value"].as<double>());
    CHECK_THROWS(
        std::runtime_error,
        row["real_value"].as<int>());
    CHECK_THROWS(
        std::runtime_error,
        row["text_value"].as<int>());
    CHECK_THROWS(
        std::runtime_error,
        row["null_value"].as<int>());
    CHECK(!row["null_value"].as<std::optional<int>>().has_value());
    CHECK(row["null_value"].as<int>(123) == 123);

    CHECK_THROWS(
        std::out_of_range,
        row[4]);
    CHECK_THROWS(
        std::out_of_range,
        row["missing"]);

    result.finish();

    auto negative = db.query("SELECT -1 AS value");
    CHECK_THROWS(
        std::range_error,
        (*negative.begin())["value"].as<unsigned int>());
    negative.finish();
}

// -----------------------------------------------------------------------------
// Test: Text and BLOB round trips
// Verifies embedded NUL text, both live BLOB views, and empty BLOB semantics.
// -----------------------------------------------------------------------------

void test_text_and_blob_round_trips() {
    sqlitecxx::connection db(":memory:");
    db.execute(R"(
        CREATE TABLE payloads (
            id         INTEGER PRIMARY KEY,
            text_value TEXT NOT NULL,
            blob_value BLOB NOT NULL
        )
    )");

    const std::string text_with_nul("A\0B", 3);
    const std::array<unsigned char, 4> bytes{0, 1, 255, 0};

    db.execute(
        "INSERT INTO payloads(text_value, blob_value) VALUES(?1, ?2)",
        {text_with_nul, std::span<const unsigned char>(bytes)});

    const std::span<const std::byte> empty_blob;
    db.execute(
        "INSERT INTO payloads(text_value, blob_value) VALUES(?1, ?2)",
        {"empty", empty_blob});

    auto result = db.query(
        "SELECT text_value, blob_value FROM payloads ORDER BY id");
    auto iterator = result.begin();

    auto first = *iterator;
    CHECK(first["text_value"].as<std::string>() == text_with_nul);

    const auto byte_blob = first["blob_value"].as<std::vector<std::byte>>();
    const auto unsigned_blob = first["blob_value"].as<std::vector<unsigned char>>();
    CHECK(byte_blob.size() == bytes.size());
    CHECK(unsigned_blob == std::vector<unsigned char>(bytes.begin(), bytes.end()));
    CHECK(std::to_integer<unsigned int>(byte_blob[0]) == 0);
    CHECK(std::to_integer<unsigned int>(byte_blob[1]) == 1);
    CHECK(std::to_integer<unsigned int>(byte_blob[2]) == 255);
    CHECK(std::to_integer<unsigned int>(byte_blob[3]) == 0);

    ++iterator;
    auto second = *iterator;
    CHECK(second["text_value"].as<std::string>() == "empty");
    CHECK(!second["blob_value"].is_null());
    CHECK(second["blob_value"].as<std::vector<std::byte>>().empty());

    ++iterator;
    CHECK(iterator == std::default_sentinel);
}

// -----------------------------------------------------------------------------
// Test: Cursor metadata and live-view lifetime
// Verifies duplicate-name lookup and invalidation after advancing or closing.
// -----------------------------------------------------------------------------

void test_cursor_metadata_and_live_view_lifetime() {
    sqlitecxx::connection db(":memory:");
    auto result = db.query(
        "SELECT 1 AS value, 2 AS value "
        "UNION ALL SELECT 3, 4");

    CHECK(result.column_count() == 2);
    CHECK(result.column_name(0) == "value");
    CHECK(result.column_name(1) == "value");

    auto iterator = result.begin();
    auto old_row = *iterator;
    auto old_field = old_row[0];

    CHECK(old_row["value"].as<int>() == 1);
    CHECK(old_row[1].as<int>() == 2);

    ++iterator;
    CHECK_THROWS(
        std::logic_error,
        old_row[0].as<int>());
    CHECK_THROWS(
        std::logic_error,
        old_field.as<int>());

    auto current_row = *iterator;
    auto current_field = current_row["value"];
    CHECK(current_field.as<int>() == 3);

    result.close();
    CHECK(result.done());
    CHECK_THROWS(
        std::logic_error,
        current_row[0].as<int>());
    CHECK_THROWS(
        std::logic_error,
        current_field.as<int>());

    auto empty = db.query("SELECT 1 AS value WHERE 0");
    CHECK(empty.empty());
    CHECK(empty.done());
    CHECK(empty.column_count() == 1);
    CHECK(empty.column_name(0) == "value");
}

// -----------------------------------------------------------------------------
// Test: Prepared statement lifetime and lease release
// Verifies iterator ownership, exclusive execution, cleanup, and statement reuse.
// -----------------------------------------------------------------------------

void test_prepared_statement_lifetime_and_lease_release() {
    sqlitecxx::connection db(":memory:");
    auto prepared = db.prepare(
        "SELECT ?1 AS value UNION ALL SELECT ?1 + 1 ORDER BY value");

    auto iterator = [&] {
        auto result = db.query(prepared, {41});
        return result.begin();
    }();

    CHECK((*iterator)["value"].as<int>() == 41);
    CHECK_THROWS(
        std::logic_error,
        db.query(prepared, {100}));

    ++iterator;
    CHECK((*iterator)["value"].as<int>() == 42);
    ++iterator;
    CHECK(iterator == std::default_sentinel);

    auto reused = db.query(prepared, {100});
    CHECK((*reused.begin())["value"].as<int>() == 100);
    reused.finish();

    {
        auto early_iterator = [&] {
            auto result = db.query(prepared, {7});
            return result.begin();
        }();
        CHECK((*early_iterator)["value"].as<int>() == 7);
    }

    auto reused_after_early_destruction = db.query(prepared, {9});
    CHECK((*reused_after_early_destruction.begin())["value"].as<int>() == 9);
    reused_after_early_destruction.close();

    CHECK_THROWS(
        std::invalid_argument,
        db.query(prepared));

    auto reused_after_binding_failure = db.query(prepared, {11});
    CHECK((*reused_after_binding_failure.begin())["value"].as<int>() == 11);
    reused_after_binding_failure.finish();

    db.execute("CREATE TABLE prepared_values(value INTEGER NOT NULL UNIQUE)");
    auto insert = db.prepare(
        "INSERT INTO prepared_values(value) VALUES(?1)");

    CHECK_THROWS(
        std::invalid_argument,
        db.execute(insert));
    db.execute(insert, {5});
    CHECK_THROWS(
        sqlitecxx::sqlite_error,
        db.execute(insert, {5}));
    db.execute(insert, {6});

    auto values = db.query(
        "SELECT value FROM prepared_values ORDER BY rowid");
    auto value_iterator = values.begin();
    CHECK((*value_iterator)["value"].as<int>() == 5);
    ++value_iterator;
    CHECK((*value_iterator)["value"].as<int>() == 6);
    ++value_iterator;
    CHECK(value_iterator == std::default_sentinel);
}

// -----------------------------------------------------------------------------
// Test: Owning materialized results
// Verifies canonical value types, mapper ownership, and cleanup after failures.
// -----------------------------------------------------------------------------

void test_materialized_results() {
    sqlitecxx::connection db(":memory:");
    const std::array<std::byte, 3> blob{
        std::byte{1},
        std::byte{2},
        std::byte{3}};

    auto materialized = db
                            .query(
                                "SELECT NULL AS null_value, ?1 AS integer_value, "
                                "?2 AS real_value, ?3 AS text_value, ?4 AS blob_value",
                                {7, 2.5, "hello", std::span<const std::byte>(blob)})
                            .materialize();

    CHECK(materialized.size() == 1);
    CHECK(!materialized.empty());
    CHECK(materialized.column_count() == 5);

    const auto &row = *materialized.begin();
    CHECK(row["null_value"].is_null());
    CHECK(!row["null_value"].as<std::optional<int>>().has_value());
    CHECK(row["integer_value"].as<int>() == 7);
    CHECK(row["real_value"].as<double>() == 2.5);
    CHECK(row["text_value"].as<std::string>() == "hello");
    CHECK(row["blob_value"].as<std::vector<std::byte>>() ==
          std::vector<std::byte>(blob.begin(), blob.end()));
    CHECK_THROWS(
        std::runtime_error,
        row["integer_value"].as<double>());
    CHECK_THROWS(
        std::runtime_error,
        row["null_value"].as<int>());
    CHECK_THROWS(
        std::out_of_range,
        row["missing"]);

    struct mapped_user {
        int id;
        std::string name;
    };

    auto users = db
                     .query(
                         "SELECT 1 AS id, 'Alice' AS name "
                         "UNION ALL SELECT 2, 'Bob' ORDER BY id")
                     .materialize([](const sqlitecxx::row_view &live_row) {
                         return mapped_user{
                             live_row["id"].as<int>(),
                             live_row["name"].as<std::string>()};
                     });

    CHECK(users.size() == 2);
    CHECK(users[0].id == 1);
    CHECK(users[0].name == "Alice");
    CHECK(users[1].id == 2);
    CHECK(users[1].name == "Bob");

    struct mapper_failure {
    };

    auto prepared = db.prepare(
        "SELECT 1 AS value UNION ALL SELECT 2");
    CHECK_THROWS(
        mapper_failure,
        db.query(prepared).materialize([](const sqlitecxx::row_view &) -> int {
            throw mapper_failure{};
        }));

    auto reused = db.query(prepared);
    CHECK((*reused.begin())["value"].as<int>() == 1);
    reused.finish();
}

// -----------------------------------------------------------------------------
// Test: Transaction state and cursor rules
// Verifies commit, rollback, RAII rollback, active-cursor guards, and staleness.
// -----------------------------------------------------------------------------

void test_transaction_state_and_cursor_rules() {
    sqlitecxx::connection db(":memory:");
    db.execute("CREATE TABLE entries(value INTEGER NOT NULL)");

    const auto count_entries = [&db] {
        auto result = db.query("SELECT COUNT(*) AS count FROM entries");
        return (*result.begin())["count"].as<int>();
    };

    {
        auto tx = db.begin_transaction();
        tx.execute("INSERT INTO entries(value) VALUES(?1)", {1});
        CHECK_THROWS(
            std::logic_error,
            db.execute("INSERT INTO entries(value) VALUES(99)"));
        tx.commit();
    }
    CHECK(count_entries() == 1);

    {
        auto tx = db.begin_transaction();
        tx.execute("INSERT INTO entries(value) VALUES(?1)", {2});
        tx.rollback();
    }
    CHECK(count_entries() == 1);

    {
        auto tx = db.begin_transaction();
        tx.execute("INSERT INTO entries(value) VALUES(?1)", {3});
    }
    CHECK(count_entries() == 1);

    auto connection_cursor = db.query("SELECT value FROM entries");
    CHECK_THROWS(
        std::logic_error,
        db.begin_transaction());
    connection_cursor.close();

    {
        auto tx = db.begin_transaction();
        auto transaction_cursor = tx.query("SELECT value FROM entries");

        CHECK_THROWS(
            std::logic_error,
            tx.commit());
        CHECK_THROWS(
            std::logic_error,
            tx.rollback());

        transaction_cursor.close();
        tx.commit();
    }

    {
        auto tx = db.begin_transaction();
        auto transaction_cursor = tx.query("SELECT value FROM entries");
        transaction_cursor.finish();
        tx.commit();

        CHECK_THROWS(
            std::logic_error,
            tx.commit());
        CHECK_THROWS(
            std::logic_error,
            tx.execute("INSERT INTO entries(value) VALUES(4)"));
    }

    auto escaped = [&db] {
        auto tx = db.begin_transaction();
        return tx.query("SELECT value FROM entries");
    }();

    CHECK_THROWS(
        std::logic_error,
        escaped.finish());
    CHECK(escaped.done());

    auto final_transaction = db.begin_transaction();
    final_transaction.rollback();
}

// -----------------------------------------------------------------------------
// Test: SQL validation and error reporting
// Verifies API misuse errors, SQLite codes, connection recovery, and ownership.
// -----------------------------------------------------------------------------

void test_sql_validation_and_error_reporting() {
    sqlitecxx::connection db(":memory:");
    db.execute(
        "CREATE TABLE unique_values("
        "value INTEGER NOT NULL UNIQUE)");
    db.execute("INSERT INTO unique_values(value) VALUES(1)");

    bool constraint_caught = false;
    try {
        db.execute("INSERT INTO unique_values(value) VALUES(1)");
    } catch (const sqlitecxx::sqlite_error &error) {
        constraint_caught = true;
        CHECK(error.code() == SQLITE_CONSTRAINT);
        CHECK(error.extended_code() == SQLITE_CONSTRAINT_UNIQUE);
        CHECK(error.has_extended_code());
    }
    CHECK(constraint_caught);

    bool syntax_error_caught = false;
    try {
        db.execute("THIS IS NOT VALID SQL");
    } catch (const sqlitecxx::sqlite_error &error) {
        syntax_error_caught = true;
        CHECK(error.code() == SQLITE_ERROR);
    }
    CHECK(syntax_error_caught);

    auto still_usable = db.query("SELECT COUNT(*) AS count FROM unique_values");
    CHECK((*still_usable.begin())["count"].as<int>() == 1);
    still_usable.finish();

    CHECK_THROWS(
        std::logic_error,
        db.execute("SELECT 1"));
    CHECK_THROWS(
        std::logic_error,
        db.query("CREATE TABLE unused(value INTEGER)"));
    CHECK_THROWS(
        std::invalid_argument,
        db.execute("CREATE TABLE first(value); CREATE TABLE second(value)"));
    CHECK_THROWS(
        std::invalid_argument,
        db.execute(""));
    CHECK_THROWS(
        std::invalid_argument,
        db.execute("   -- comment only"));

    constexpr char sql_with_nul[] = "SELECT 1\0SELECT 2";
    const std::string_view embedded_nul_sql(
        sql_with_nul,
        sizeof(sql_with_nul) - 1);
    CHECK_THROWS(
        std::invalid_argument,
        db.query(embedded_nul_sql));

    auto prepared = db.prepare("SELECT 1 AS value");
    sqlitecxx::connection other(":memory:");
    CHECK_THROWS(
        std::logic_error,
        other.query(prepared));

    CHECK_THROWS(
        std::logic_error,
        db.execute("BEGIN"));

    auto recovered = db.query("SELECT 1 AS value");
    CHECK((*recovered.begin())["value"].as<int>() == 1);
}

// -----------------------------------------------------------------------------
// Test runner
// Runs every independent test and reports all failures before returning.
// -----------------------------------------------------------------------------

struct test_case {
    std::string_view name;
    void (*run)();
};

constexpr std::array test_cases{
    test_case{"connection, execution, and typed queries", test_connection_execution_and_typed_queries},
    test_case{"parameter binding and slot numbering", test_parameter_binding_and_slot_numbering},
    test_case{"strict result typing and conversion errors", test_strict_result_typing_and_conversion_errors},
    test_case{"text and BLOB round trips", test_text_and_blob_round_trips},
    test_case{"cursor metadata and live-view lifetime", test_cursor_metadata_and_live_view_lifetime},
    test_case{"prepared statement lifetime and lease release", test_prepared_statement_lifetime_and_lease_release},
    test_case{"owning materialized results", test_materialized_results},
    test_case{"transaction state and cursor rules", test_transaction_state_and_cursor_rules},
    test_case{"SQL validation and error reporting", test_sql_validation_and_error_reporting},
};

} // namespace

int main() {
    std::size_t failures = 0;

    for (const auto &test : test_cases) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << test_cases.size() << " test(s) passed.\n";
    return 0;
}
