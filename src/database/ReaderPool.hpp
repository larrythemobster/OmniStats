#pragma once

#include "sqlite/Connection.hpp"
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace database {

    class ReaderPool {
      public:
        ReaderPool() = default;
        ~ReaderPool() {
            Shutdown();
        }

        ReaderPool(const ReaderPool&) = delete;
        ReaderPool& operator=(const ReaderPool&) = delete;

        bool Initialize(const std::string& path, size_t poolSize = 3);
        void Shutdown();
        bool HasPool() const;

        class Lease {
          public:
            Lease() = default;
            Lease(sqlite::Connection* conn, std::function<void(sqlite::Connection*)> returnFn);
            ~Lease();

            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;

            Lease(Lease&& other) noexcept;
            Lease& operator=(Lease&& other) noexcept;

            sqlite::Connection& Get() {
                return *m_conn;
            }
            const sqlite::Connection& Get() const {
                return *m_conn;
            }
            sqlite::Connection* operator->() {
                return m_conn;
            }
            operator sqlite::Connection&() {
                return *m_conn;
            }
            operator sqlite3*() const {
                return m_conn ? m_conn->Handle() : nullptr;
            }
            sqlite3* Handle() const {
                return m_conn ? m_conn->Handle() : nullptr;
            }
            bool IsValid() const noexcept {
                return m_conn != nullptr;
            }

          private:
            sqlite::Connection* m_conn = nullptr;
            std::function<void(sqlite::Connection*)> m_returnFn;
        };

        Lease Acquire();

      private:
        struct State {
            std::vector<std::unique_ptr<sqlite::Connection>> connections;
            std::vector<sqlite::Connection*> available;
            std::mutex mutex;
            std::condition_variable cv;
            bool stopping = false;
        };
        std::shared_ptr<State> m_state = std::make_shared<State>();
    };

} // namespace database
