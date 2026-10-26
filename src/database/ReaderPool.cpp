#include "ReaderPool.hpp"
#include <iostream>

namespace database {

    bool ReaderPool::Initialize(const std::string& path, size_t poolSize) {
        Shutdown();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = false;
        for (size_t i = 0; i < poolSize; ++i) {
            try {
                auto conn = std::make_unique<sqlite::Connection>(
                    sqlite::Connection::Open(path, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX));
                conn->BusyTimeout(3000);
                m_available.push_back(conn.get());
                m_connections.push_back(std::move(conn));
            } catch (const std::exception& e) {
                std::cerr << "[Database] Reader connection " << i << " failed: " << e.what() << "\n";
            }
        }
        return !m_connections.empty();
    }

    void ReaderPool::Shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        std::lock_guard<std::mutex> lock(m_mutex);
        m_available.clear();
        m_connections.clear();
    }

    bool ReaderPool::HasPool() const {
        return !m_connections.empty();
    }

    ReaderPool::Lease ReaderPool::Acquire() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_stopping || !m_available.empty(); });
        if (m_stopping || m_available.empty()) {
            return Lease();
        }
        sqlite::Connection* conn = m_available.back();
        m_available.pop_back();

        try {
            conn->Execute("BEGIN;");
        } catch (const std::exception& e) {
            std::cerr << "[Database] Cannot begin read snapshot on reader connection: " << e.what() << "\n";
            m_available.push_back(conn);
            return Lease();
        }

        return Lease(conn, [this](sqlite::Connection* c) {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_available.push_back(c);
            m_cv.notify_one();
        });
    }

    ReaderPool::Lease::Lease(sqlite::Connection* conn, std::function<void(sqlite::Connection*)> returnFn)
        : m_conn(conn), m_returnFn(std::move(returnFn)) {}

    ReaderPool::Lease::~Lease() {
        if (m_conn) {
            try {
                m_conn->Execute("ROLLBACK;");
            } catch (...) {
            }
            if (m_returnFn) {
                m_returnFn(m_conn);
            }
            m_conn = nullptr;
        }
    }

    ReaderPool::Lease::Lease(Lease&& other) noexcept
        : m_conn(other.m_conn), m_returnFn(std::move(other.m_returnFn)) {
        other.m_conn = nullptr;
    }

    ReaderPool::Lease& ReaderPool::Lease::operator=(Lease&& other) noexcept {
        if (this != &other) {
            if (m_conn) {
                try {
                    m_conn->Execute("ROLLBACK;");
                } catch (...) {
                }
                if (m_returnFn) {
                    m_returnFn(m_conn);
                }
            }
            m_conn = other.m_conn;
            m_returnFn = std::move(other.m_returnFn);
            other.m_conn = nullptr;
        }
        return *this;
    }

} // namespace database
