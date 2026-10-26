#pragma once

#include "SqliteError.hpp"
#include <sqlite3.h>

namespace database::sqlite {

    class Connection;

    enum class TransactionType {
        Deferred,
        Immediate,
        Exclusive
    };

    class Transaction {
      public:
        explicit Transaction(Connection& conn, TransactionType type = TransactionType::Deferred);
        explicit Transaction(sqlite3* db, TransactionType type = TransactionType::Deferred);
        ~Transaction();

        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;

        Transaction(Transaction&& other) noexcept;
        Transaction& operator=(Transaction&& other) noexcept;

        void Commit();
        void Rollback();
        bool IsCommitted() const noexcept {
            return m_committed;
        }
        bool IsActive() const noexcept {
            return m_active;
        }

      private:
        sqlite3* m_db = nullptr;
        bool m_committed = false;
        bool m_active = false;
    };

} // namespace database::sqlite
