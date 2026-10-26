#pragma once

#include "SqliteError.hpp"
#include <cstdint>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <string_view>

namespace database::sqlite {

    class Connection;

    namespace detail {
        template <typename T>
        struct ColumnReader;

        template <>
        struct ColumnReader<int> {
            static int Read(sqlite3_stmt* stmt, int col) {
                return sqlite3_column_int(stmt, col);
            }
        };

        template <>
        struct ColumnReader<int64_t> {
            static int64_t Read(sqlite3_stmt* stmt, int col) {
                return sqlite3_column_int64(stmt, col);
            }
        };

        template <>
        struct ColumnReader<uint64_t> {
            static uint64_t Read(sqlite3_stmt* stmt, int col) {
                return static_cast<uint64_t>(sqlite3_column_int64(stmt, col));
            }
        };

        template <>
        struct ColumnReader<double> {
            static double Read(sqlite3_stmt* stmt, int col) {
                return sqlite3_column_double(stmt, col);
            }
        };

        template <>
        struct ColumnReader<float> {
            static float Read(sqlite3_stmt* stmt, int col) {
                return static_cast<float>(sqlite3_column_double(stmt, col));
            }
        };

        template <>
        struct ColumnReader<bool> {
            static bool Read(sqlite3_stmt* stmt, int col) {
                return sqlite3_column_int(stmt, col) != 0;
            }
        };

        template <>
        struct ColumnReader<std::string> {
            static std::string Read(sqlite3_stmt* stmt, int col) {
                const unsigned char* text = sqlite3_column_text(stmt, col);
                if (!text) return std::string{};
                int bytes = sqlite3_column_bytes(stmt, col);
                return std::string(reinterpret_cast<const char*>(text), bytes);
            }
        };

        template <typename U>
        struct ColumnReader<std::optional<U>> {
            static std::optional<U> Read(sqlite3_stmt* stmt, int col) {
                if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
                    return std::nullopt;
                }
                return ColumnReader<U>::Read(stmt, col);
            }
        };
    } // namespace detail

    class Statement {
      public:
        Statement() = default;
        Statement(sqlite3* db, std::string_view sql);
        Statement(Connection& conn, std::string_view sql);
        ~Statement();

        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;

        Statement(Statement&& other) noexcept;
        Statement& operator=(Statement&& other) noexcept;

        bool IsValid() const noexcept {
            return m_stmt != nullptr;
        }
        sqlite3_stmt* Handle() const noexcept {
            return m_stmt;
        }
        operator sqlite3_stmt*() const noexcept {
            return m_stmt;
        }

        void Reset();
        void ClearBindings();

        bool Step();
        void Execute();

        void Bind(int index, int val);
        void Bind(int index, unsigned int val);
        void Bind(int index, int64_t val);
        void Bind(int index, uint64_t val);
        void Bind(int index, double val);
        void Bind(int index, float val);
        void Bind(int index, bool val);
        void Bind(int index, const char* val);
        void Bind(int index, const std::string& val);
        void Bind(int index, std::string_view val);
        void Bind(int index, std::nullptr_t);
        void Bind(int index, std::nullopt_t);
        void BindNull(int index);

        template <typename T>
        void Bind(int index, const std::optional<T>& val) {
            if (val.has_value()) {
                Bind(index, *val);
            } else {
                BindNull(index);
            }
        }

        template <typename T>
        void Bind(std::string_view name, const T& val) {
            int idx = ParameterIndex(name);
            Bind(idx, val);
        }

        int ParameterIndex(std::string_view name) const;

        template <typename T>
        T Get(int col) const {
            if (!m_stmt) {
                throw SqliteError(SQLITE_MISUSE, "Attempted to read column from null statement", m_sql);
            }
            return detail::ColumnReader<T>::Read(m_stmt, col);
        }

        bool IsNull(int col) const;
        int ColumnCount() const;
        int ColumnType(int col) const;
        std::string_view ColumnName(int col) const;

      private:
        sqlite3* m_db = nullptr;
        sqlite3_stmt* m_stmt = nullptr;
        std::string m_sql;
    };

} // namespace database::sqlite
