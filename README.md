# sqlitecxx  

`sqlitecxx` is a small C++20 wrapper around SQLite with a simple, explicit API for opening databases, executing SQL, querying rows, binding parameters, reusing prepared statements, materializing results, and managing transactions.

# Requirements  

* C++20
* SQLite 3.37.0 or later

```c++
#include "sqlitecxx.hpp"
```

Typical GCC/Clang build:  

```bash
g++ -std=c++20 main.cpp -lsqlite3
clang++ -std=c++20 main.cpp -lsqlite3
```

`sqlitecxx` is intentionally distributed as a single-header library. Add this
repository as a submodule, or copy `sqlitecxx.hpp` into your project, add its
directory to the compiler's include path, and link SQLite. The repository's
CMake configuration only builds the tests; it intentionally does not provide a
consumer target or FetchContent integration.  

# Thread safety  

`sqlitecxx` intentionally targets single-threaded use and does not provide
internal synchronization. A connection and the transactions, prepared
statements, cursors, iterators, rows, and fields created from it must not be
used from multiple threads. Cross-thread use is outside the library contract.  

# Opening a connection  

```c++
sqlitecxx::connection db("example.db");
```

For an in-memory database:  

```c++
sqlitecxx::connection db(":memory:");
```

`connection` is move-only.  

# Executing SQL  

Use `execute()` for statements that do not return result columns:  

```c++
db.execute(R"(
    CREATE TABLE users (
        id   INTEGER PRIMARY KEY,
        name TEXT NOT NULL,
        age  INTEGER
    )
)");
```

For inserts, updates, and deletes:  

```c++
db.execute(
    "INSERT INTO users(name, age) VALUES(?1, ?2)",
    {"Alice", 30}
);
```

`execute()` returns `void`.  

After a write:  

```c++
auto changed = db.changes();
auto id = db.last_insert_rowid();
```

# Querying rows  

Use `query()` for statements that return columns:  

```c++
auto result = db.query(
    "SELECT id, name, age FROM users ORDER BY id"
);

for (const auto row : result) {
    auto id   = row[0].as<std::int64_t>();
    auto name = row["name"].as<std::string>();
    auto age  = row["age"].as<std::optional<int>>();
}
```

Rows can be accessed by zero-based index or by column name:  

```c++
auto id = row[0];
auto name = row["name"];
```

If duplicate column names are returned, lookup by name uses the first matching column.  

# Reading fields  

`field_view::as<T>()` performs strict SQLite storage-class checking.  

## Integers  

SQLite `INTEGER` values can be read into supported C++ integer types:  

```c++
auto a = row["value"].as<int>();
auto b = row["value"].as<std::int64_t>();
```

Integer conversions are range-checked. If the SQLite integer does not fit the requested C++ type, `std::range_error` is thrown.  

`bool` and character types are not supported as integer result types.  

## Floating point  

SQLite `REAL` values are read as `double`:  

```c++
auto value = row["value"].as<double>();
```

`float` is intentionally not supported as a result type, which is to prevent potential floating-point precision loss.  

## Text  

```c++
std::string name = row["name"].as<std::string>();
```

## BLOBs  

Live fields support:  

```c++
auto bytes = row["data"].as<std::vector<std::byte>>();
```

and:  

```c++
auto bytes = row["data"].as<std::vector<unsigned char>>();
```

## NULL  

Use `std::optional<T>` when SQL `NULL` is allowed:  

```c++
auto age = row["age"].as<std::optional<int>>();
```

Or test explicitly:  

```c++
if (row["age"].is_null()) {
    // SQL NULL
}
```

Or provide a default value:  

```c++
auto age = row["age"].as<int>(0);
```

# Strict result typing  

`as<T>()` does not perform SQLite-style cross-type coercion.  

For example, an SQLite `TEXT` value cannot be read as an integer:  

```c++
row["value"].as<int>(); // invalid if storage class is TEXT
```

Likewise, an SQLite `INTEGER` is not silently converted to `double`, and an SQLite `REAL` is not silently converted to an integer.  

# Parameters  

Parameters are passed with `sqlitecxx::params`.  

A temporary parameter list can be passed directly:  

```c++
db.execute(
    "INSERT INTO users(name, age) VALUES(?1, ?2)",
    {"Alice", 30}
);
```

Or built incrementally:  

```c++
sqlitecxx::params p;
p.append("Alice");
p.append(30);

db.execute(
    "INSERT INTO users(name, age) VALUES(?1, ?2)",
    p
);
```

Supported parameter values include:  

* `nullptr`
* integer types except `bool` and character types
* `float` and `double`
* values convertible to `std::string_view`
* `std::span` of `std::byte` or `unsigned char`
* `std::optional<T>` for supported parameter types

An empty `std::optional` binds SQL `NULL`.  

# Parameter slot rule  

The parameter list is positional:  

```
params[0] -> SQLite slot 1
params[1] -> SQLite slot 2
params[2] -> SQLite slot 3
...
```

The number of supplied values must equal SQLite's highest parameter slot.  

## Valid examples:  

```c++
db.query("SELECT ?1", {123});

db.query(
    "SELECT ?2, ?1",
    {10, 20}
);

db.query(
    "SELECT ?1 + ?1",
    {10}
);
```

A sparse placeholder still requires preceding slots:  

```c++
// Invalid: highest slot is 5, but only one value is supplied.
db.query("SELECT ?5", {123});
```

Use:  

```c++
db.query(
    "SELECT ?5",
    {nullptr, nullptr, nullptr, nullptr, 123}
);
```

Named SQLite parameters still follow SQLite's slot-numbering rules.  

# Floating-point parameters  

`float` and `double` parameters are bound as SQLite `REAL`.  

NaN is rejected. Infinity is allowed.  

# Prepared statements  

Use `prepare()` when the same SQL will be executed repeatedly:  

```c++
auto insert = db.prepare(
    "INSERT INTO users(name, age) VALUES(?1, ?2)"
);

db.execute(insert, {"Alice", 30});
db.execute(insert, {"Bob", 42});
db.execute(insert, {"Carol", 25});
```

Prepared queries work the same way:  

```c++
auto find_user = db.prepare(
    "SELECT id, name, age FROM users WHERE id = ?1"
);

auto result = db.query(find_user, {1});
```

A prepared statement can have only one active execution at a time. Finish, close, or destroy the active cursor before reusing it.  

# Cursor lifetime  

A `cursor` represents an active SQLite query execution.  

```c++
auto result = db.query("SELECT id, name FROM users");

for (const auto row : result) {
    // use row here
}
```

You can explicitly end a cursor:  

```c++
result.close();
```

Or consume it until `SQLITE_DONE`:  

```c++
result.finish();
```

# Row and field views are live views  

`row_view` and `field_view` refer to the cursor's current row. They do not own copied SQLite values.  

A row becomes invalid after the cursor advances:  

```c++
auto result = db.query("SELECT id FROM users ORDER BY id");
auto it = result.begin();

auto row = *it;
auto first = row[0].as<int>();

++it;

// Invalid: row belongs to the previous cursor position.
// row[0].as<int>();
```

A row or field also becomes invalid if its cursor execution is closed or destroyed.  

If values must outlive cursor advancement, copy them with `as<T>()` or use `materialize()`.  

The iterator itself keeps the query execution alive, so an iterator can continue to operate even if the public `cursor` object that created it has been destroyed.  

# Cursor metadata  

```c++
auto result = db.query("SELECT id, name FROM users");

std::size_t count = result.column_count();
std::string_view first_name = result.column_name(0);
```

You can also inspect completion state:  

```c++
result.done();
result.empty();
```

# Materializing results  

Use `materialize()` when you want an owning result independent of the live cursor:  

```c++
auto rows = db
    .query("SELECT id, name, age FROM users ORDER BY id")
    .materialize();

for (const auto& row : rows) {
    auto id = row[0].as<std::int64_t>();
    auto name = row["name"].as<std::string>();
}
```

A materialized result owns its rows and values.  

Default materialized value types are:  

* SQL `NULL`
* SQLite integer
* `double`
* `std::string`
* `std::vector<std::byte>`

# BLOB representation in materialized results  

Live fields can be read as either:  

```c++
row["data"].as<std::vector<std::byte>>();
```

or:  

```c++
row["data"].as<std::vector<unsigned char>>();
```

Default materialization intentionally uses only `std::vector<std::byte>` as
its canonical owning BLOB representation.  

`std::vector<unsigned char>` is not provided by the default materialized
field API. Keeping one canonical BLOB representation avoids maintaining
duplicate owning representations or adding an extra conversion path to the
default materializer.  

If a different owning BLOB representation is desired, use mapper-based
materialization and return that type explicitly:  

```c++
auto blobs = db
    .query("SELECT data FROM files")
    .materialize([](const sqlitecxx::row_view& row) {
        return row["data"].as<std::vector<unsigned char>>();
    });
```

# Materializing with a mapper  

You can transform each live row directly into your own owning type:  

```c++
struct user {
    std::int64_t id;
    std::string name;
};

auto users = db
    .query("SELECT id, name FROM users ORDER BY id")
    .materialize([](const sqlitecxx::row_view& row) {
        return user{
            row["id"].as<std::int64_t>(),
            row["name"].as<std::string>()
        };
    });
```

The mapper's declared return type must be an unqualified value type rather than
a reference. This syntactic check cannot prove that an arbitrary user-defined
type owns everything it contains. Ownership is therefore part of the caller's
contract: do not return `row_view`, `field_view`, or a structure that stores
either of them. Copy the required fields with `as<T>()`, as in the example
above.  


# Transactions  

Start a transaction with:  

```c++
auto tx = db.begin_transaction();
```

Run statements through the transaction object:  

```c++
auto tx = db.begin_transaction();

tx.execute(
    "INSERT INTO users(name, age) VALUES(?1, ?2)",
    {"Alice", 30}
);

tx.execute(
    "INSERT INTO users(name, age) VALUES(?1, ?2)",
    {"Bob", 42}
);

tx.commit();
```

Rollback explicitly with:  

```c++
tx.rollback();
```

If a live transaction object is destroyed without `commit()` or `rollback()`, it attempts to roll back automatically.  

While a transaction is active, perform database operations through the transaction object rather than the original connection.  

# Transaction-scoped cursor lifetime  

A cursor created through a `transaction` is scoped to that transaction.  

Do not move or store such a cursor somewhere that may outlive the transaction
object:  

```c++
auto outside = [&db] {
    auto tx = db.begin_transaction();

    // Invalid: this transaction-scoped cursor escapes with the return value.
    return tx.query("SELECT id FROM users");
}(); // tx is destroyed as the lambda returns, leaving outside stale

// `outside` is no longer a valid query execution.
```

The required lifetime relationship is:  

```
transaction-created cursor lifetime <= transaction lifetime
```

Finish, close, or destroy all cursors created by a transaction before the
transaction itself ends.  

The library detects stale transaction-scoped cursors and rejects attempts to
continue the query or read from the stale cursor, but applications should treat
a cursor escaping its transaction as invalid usage.  

# Active cursors and transaction completion  

A transaction cannot be committed or rolled back while one of its cursors is still active.  

Finish or close the cursor first:  

```c++
auto tx = db.begin_transaction();

auto result = tx.query("SELECT id FROM users");
result.finish();

tx.commit();
```

Or:  

```c++
result.close();
tx.rollback();
```

A new transaction also cannot be started while the connection has an active cursor.  

# Native SQLite handle  

The underlying `sqlite3*` is available when necessary:  

```c++
sqlite3* raw = db.native_handle();
```

This is an escape hatch. Do not manually change transaction/autocommit state through the native handle while using the wrapper's transaction API. The wrapper tracks transaction state and detects unexpected autocommit changes.  

# Errors  

SQLite failures are reported as `sqlitecxx::sqlite_error`:  

```c++
try {
    db.execute("INVALID SQL");
} catch (const sqlitecxx::sqlite_error& e) {
    int code = e.code();
    int extended = e.extended_code();

    if (e.has_extended_code()) {
        // extended contains additional SQLite detail
    }
}
```

Invalid API use and conversion failures use standard C++ exceptions such as:  

* `std::logic_error`
* `std::runtime_error`
* `std::invalid_argument`
* `std::out_of_range`
* `std::range_error`
* `std::length_error`
