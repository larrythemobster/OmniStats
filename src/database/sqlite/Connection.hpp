#pragma once

#include "SqliteError.hpp"
#include <cstdint>
#include <sqlite3.h>
#include <string>
#include <string_view>

namespace database::sqlite {

    class Statement;

    class Connection {
      public:
        Connection() = default;
        explicit Connection(sqlite3* db, bool owns = true) : m_db(db), m_owns(owns) {}
        ~Connection() {
            Close();
        }

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        Connection(Connection&& other) noexcept
            : m_db(other.m_db), m_owns(other.m_owns) {
            other.m_db = nullptr;
            other.m_owns = false;
        }

        Connection& operator=(Connection&& other) noexcept {
            if (this != &other) {
                Close();
                m_db = other.m_db;
                m_owns = other.m_owns;
                other.m_db = nullptr;
                other.m_owns = false;
            }
            return *this;
        }

        static Connection Open(const std::string& path, int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        static Connection OpenReadOnly(const std::string& path);
        static Connection OpenInMemory();

        void Close();
        bool IsOpen() const noexcept {
            return m_db != nullptr;
        }
        sqlite3* Handle() const noexcept {
            return m_db;
        }
        operator sqlite3*() const noexcept {
            return m_db;
        }

        void Execute(std::string_view sql);
        int64_t LastInsertRowId() const noexcept;
        int Changes() const noexcept;
        int TotalChanges() const noexcept;
        void BusyTimeout(int ms);

        bool TableExists(std::string_view tableName);
        bool TableHasColumn(std::string_view tableName, std::string_view columnName);
        int UserVersion();
        void SetUserVersion(int version);

        Statement Prepare(std::string_view sql);

      private:
        sqlite3* m_db = nullptr;
        bool m_owns = true;
    };

} // namespace database::sqlite
