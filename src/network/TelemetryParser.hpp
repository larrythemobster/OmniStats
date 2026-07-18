#pragma once
#include <asio.hpp>
#include <string>
#include <memory>
#include <functional>
#include <atomic>

class TelemetryParser {
  public:
    TelemetryParser();
    ~TelemetryParser();

    using JsonLineCallback = std::function<void(const std::string& jsonLine)>;

    void ConnectAndRead(JsonLineCallback onJsonLine);
    void Stop();

  private:
    std::atomic<bool> m_stopping{false};
    asio::io_context m_ioContext;
    asio::ip::tcp::socket m_socket;
};
