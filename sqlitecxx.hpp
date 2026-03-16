#pragma once

#include <concepts>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace evqovv
{

namespace sqlitecxx
{

namespace detail
{

template <typename T>
concept real_number = ::std::floating_point<T>;

template <typename T>
concept text_type = ::std::same_as<::std::string, ::std::remove_cvref_t<T>> ||
                    ::std::same_as<::std::string_view, ::std::remove_cvref_t<T>>;

template <typename T>
concept blob_span = ::std::same_as<::std::span<::std::byte>, T> || ::std::same_as<::std::span<const ::std::byte>, T>;

template <typename T>
concept null_type = ::std::same_as<::std::nullptr_t, T>;

template <typename T>
concept integer_type = ::std::integral<::std::remove_cvref_t<T>> && !::std::same_as<::std::remove_cvref_t<T>, bool>;

template <typename T>
concept bindable_type = blob_span<T> || text_type<T> || null_type<T> || real_number<T> || integer_type<T>;

template <typename T>
struct is_optional : ::std::false_type
{
};

template <typename T>
struct is_optional<::std::optional<T>> : ::std::true_type
{
};

template <typename T>
constexpr bool is_optional_v = is_optional<T>::value;

}; // namespace detail

class database
{
public:
    using database_handle_type = sqlite3 *;

    database(const database &) = delete;

    database &operator=(const database &) = delete;

    database(const ::std::string &path)
    {
        if (::sqlite3_open(path.c_str(), &db_) != SQLITE_OK) [[unlikely]]
        {
            ::sqlite3_close(db_);
            throw ::std::runtime_error(::std::format("failed to open database: {}.", ::sqlite3_errmsg(db_)));
        }
    }

    ~database()
    {
        if (db_) [[likely]]
        {
            ::sqlite3_close(db_);
        }
    }

    database(database &&other) noexcept : db_(::std::exchange(other.db_, nullptr))
    {
    }

    database &operator=(database &&other) noexcept
    {
        if (this != &other) [[likely]]
        {
            if (db_) [[likely]]
            {
                ::sqlite3_close(db_);
            }

            db_ = ::std::exchange(other.db_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] database_handle_type native_handle() noexcept
    {
        return db_;
    }

    void execute(const ::std::string &sql)
    {
        if (::sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) [[unlikely]]
        {
            throw ::std::runtime_error(::std::format("failed to execute sql statement: {}.", ::sqlite3_errmsg(db_)));
        }
    }

    auto changes() const noexcept
    {
        return ::sqlite3_changes64(db_);
    }

private:
    database_handle_type db_{};
};

class transaction
{
public:
    transaction(const transaction &) = delete;
    transaction(transaction &&) = delete;

    transaction &operator=(const transaction &) = delete;
    transaction &operator=(transaction &&) = delete;

    explicit transaction(database &db) : db_(db)
    {
        db_.execute("BEGIN TRANSACTION;");
    }

    ~transaction()
    {
        if (done_)
        {
            db_.execute("ROLLBACK;");
        }
    }

    void commit()
    {
        if (done_) [[unlikely]]
        {
            throw ::std::logic_error("transaction has already been done.");
        }
        db_.execute("COMMIT;");
        done_ = true;
    }

    void rollback()
    {
        if (done_) [[unlikely]]
        {
            throw ::std::logic_error("transaction has already been done.");
        }
        db_.execute("ROLLBACK;");
        done_ = true;
    }

private:
    database &db_;

    bool done_{false};
};

class statement
{
public:
    using statement_handle_type = sqlite3_stmt *;

    statement(const statement &) = delete;

    statement &operator=(const statement &) = delete;

    statement &operator=(statement &&) = delete;

    statement(database &db, const ::std::string &sql) : owner_(db)
    {
        if (::sqlite3_prepare_v2(owner_.native_handle(), sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) [[unlikely]]
        {
            throw ::std::runtime_error(
                ::std::format("failed to prepare statement: {}.", ::sqlite3_errmsg(owner_.native_handle())));
        }
    }

    ~statement()
    {
        ::sqlite3_finalize(stmt_);
    }

    statement(statement &&other) noexcept : owner_(other.owner_), stmt_(::std::exchange(other.stmt_, nullptr))
    {
    }

    [[nodiscard]] statement_handle_type native_handle() noexcept
    {
        return stmt_;
    }

    [[nodiscard]] statement_handle_type &native_handle_ref() noexcept
    {
        return stmt_;
    }

    void reset()
    {
        if (::sqlite3_reset(stmt_) != SQLITE_OK) [[unlikely]]
        {
            throw ::std::runtime_error(
                ::std::format("failed to reset statement: {}.", sqlite3_errmsg(owner_.native_handle())));
        }
    }

    bool step()
    {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW)
        {
            return true;
        }
        if (rc == SQLITE_DONE)
        {
            return false;
        }
        throw ::std::runtime_error(
            ::std::format("failed to advance statement: {}.", ::sqlite3_errmsg(owner_.native_handle())));
    }

    void execute()
    {
        const int rc = ::sqlite3_step(stmt_);
        if (rc == SQLITE_DONE) [[likely]]
        {
            return;
        }

        if (rc == SQLITE_ROW) [[unlikely]]
        {
            throw std::logic_error("execute() invalid for result-producing statement.");
        }

        throw ::std::runtime_error(
            ::std::format("failed to execute statement: {}.", ::sqlite3_errmsg(owner_.native_handle())));
    }

    template <detail::bindable_type T>
    void bind(int index, T &&value)
    {
        int rc{};
        if constexpr (detail::null_type<T>)
        {
            rc = ::sqlite3_bind_null(stmt_, index);
        }
        else if constexpr (detail::blob_span<T>)
        {
            rc = ::sqlite3_bind_blob64(stmt_, index, value.data(), value.size(), SQLITE_TRANSIENT);
        }
        else if constexpr (detail::integer_type<T>)
        {
            rc = ::sqlite3_bind_int64(stmt_, index, value);
        }
        else if constexpr (detail::real_number<T>)
        {
            rc = ::sqlite3_bind_double(stmt_, index, value);
        }
        else if constexpr (detail::text_type<T>)
        {
            rc = ::sqlite3_bind_text64(stmt_, index, value.data(), value.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
        }
        else
        {
            static_assert(false, "unsupported bind type.");
        }
        if (rc != SQLITE_OK) [[unlikely]]
        {
            throw ::std::runtime_error(
                ::std::format("failed to bind parameter to statement: {}.", ::sqlite3_errmsg(owner_.native_handle())));
        }
    }

    template <detail::bindable_type... Ts>
    void bind_all(Ts &&...values)
    {
        constexpr auto arg_count = sizeof...(Ts);
        const int expected = ::sqlite3_bind_parameter_count(stmt_);

        if (arg_count != expected) [[unlikely]]
        {
            throw ::std::runtime_error(
                ::std::format("bind argument cocunt mismatch (expected {}, got{})", expected, arg_count));
        }

        int index = 1;
        (bind(index++, ::std::forward<Ts>(values)), ...);
    }

    [[nodiscard]] bool column_is_null(int col) const noexcept
    {
        return ::sqlite3_column_type(stmt_, col) == SQLITE_NULL;
    }

    template <typename T>
    [[nodiscard]] T column(int index) const
    {
        if (index < 0 || index >= ::sqlite3_column_count(stmt_)) [[unlikely]]
        {
            throw ::std::runtime_error("column index out of range.");
        }

        if constexpr (detail::is_optional_v<T>)
        {
            if (column_is_null(index)) [[unlikely]]
            {
                return ::std::nullopt;
            }
            return static_cast<T>(column<typename T::value_type>(index));
        }
        else
        {
            if (column_is_null(index)) [[unlikely]]
            {
                throw ::std::runtime_error(::std::format("column {} is null but requested non-optional type.", index));
            }

            if constexpr (detail::integer_type<T>)
            {
                return static_cast<T>(::sqlite3_column_int64(stmt_, index));
            }
            else if constexpr (detail::real_number<T>)
            {
                return static_cast<T>(::sqlite3_column_double(stmt_, index));
            }
            else if constexpr (::std::same_as<::std::string, T>)
            {
                auto text = reinterpret_cast<const char *>(::sqlite3_column_text(stmt_, index));
                auto size = static_cast<::std::size_t>(::sqlite3_column_bytes(stmt_, index));
                return ::std::string{text, size};
            }
            else if constexpr (::std::same_as<::std::vector<::std::byte>, T>)
            {
                auto *blob = reinterpret_cast<const ::std::byte *>(::sqlite3_column_blob(stmt_, index));
                auto size = static_cast<std::size_t>(::sqlite3_column_bytes(stmt_, index));
                return ::std::vector<::std::byte>{blob, blob + size};
            }
            else
            {
                static_assert(false, "unsupported type for column retrieval.");
            }
        }
    }

    void clear_bindings()
    {
        if (::sqlite3_clear_bindings(stmt_) != SQLITE_OK) [[unlikely]]
        {
            throw ::std::runtime_error(
                ::std::format("failed to clear bindings: {}.", ::sqlite3_errmsg(owner_.native_handle())));
        }
    }

private:
    statement_handle_type stmt_{};

    database &owner_;
};

} // namespace sqlitecxx

} // namespace evqovv