#pragma once
#include <asio.hpp>
#include <string>
#include <memory>
#include <functional>
#include <atomic>

class TelemetryManager;

class TelemetryParser {
  public:
    TelemetryParser();
    ~TelemetryParser();

    // Receives each complete JSON message from the Stats API stream.
    using MessageCallback = std::function<void(const std::string& message)>;

    void ConnectAndRead(MessageCallback onMessage);
    void Stop();
    void SetTelemetryManager(std::shared_ptr<TelemetryManager> tm) {
        m_telemetry = std::move(tm);
    }

  private:
    std::atomic<bool> m_stopping{false};
    asio::io_context m_ioContext;
    asio::ip::tcp::socket m_socket;
    std::shared_ptr<TelemetryManager> m_telemetry;
};
