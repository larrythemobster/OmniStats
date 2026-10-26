#include "ReaderPool.hpp"
#include <iostream>

namespace database {

    bool ReaderPool::Initialize(const std::string& path, size_t poolSize) {
        Shutdown();
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->stopping = false;
        m_state->connections.reserve(poolSize);
        m_state->available.reserve(poolSize);
        for (size_t i = 0; i < poolSize; ++i) {
            try {
                auto conn = std::make_unique<sqlite::Connection>(
                    sqlite::Connection::Open(path, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX));
                conn->BusyTimeout(3000);
                m_state->available.push_back(conn.get());
                m_state->connections.push_back(std::move(conn));
            } catch (const std::exception& e) {
                std::cerr << "[Database] Reader connection " << i << " failed: " << e.what() << "\n";
            }
        }
        return !m_state->connections.empty();
    }

    void ReaderPool::Shutdown() {
        std::unique_lock<std::mutex> lock(m_state->mutex);
        m_state->stopping = true;
        m_state->cv.notify_all();
        m_state->cv.wait(lock, [this] {
            return m_state->available.size() == m_state->connections.size();
        });
        m_state->available.clear();
        m_state->connections.clear();
    }

    bool ReaderPool::HasPool() const {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        return !m_state->connections.empty();
    }

    ReaderPool::Lease ReaderPool::Acquire() {
        auto state = m_state;
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait(lock, [&state] { return state->stopping || !state->available.empty(); });
        if (state->stopping || state->available.empty()) {
            return Lease();
        }
        std::function<void(sqlite::Connection*)> returnConnection = [state](sqlite::Connection* c) {
            std::lock_guard<std::mutex> lk(state->mutex);
            state->available.push_back(c);
            state->cv.notify_all();
        };
        sqlite::Connection* conn = state->available.back();
        state->available.pop_back();
        lock.unlock();

        try {
            conn->Execute("BEGIN;");
        } catch (const std::exception& e) {
            std::cerr << "[Database] Cannot begin read snapshot on reader connection: " << e.what() << "\n";
            returnConnection(conn);
            return Lease();
        }

        return Lease(conn, std::move(returnConnection));
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
