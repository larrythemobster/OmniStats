#include "TelemetryParser.hpp"
#include "core/Config.hpp"
#include <iostream>

TelemetryParser::TelemetryParser() : m_socket(m_ioContext) {}

TelemetryParser::~TelemetryParser() { Stop(); }

void TelemetryParser::Stop() {
    m_stopping.store(true);
    m_ioContext.stop();
    if (m_socket.is_open()) {
        asio::error_code ec;
        m_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        m_socket.close(ec);
    }
}

void TelemetryParser::ConnectAndRead(JsonLineCallback onJsonLine) {
    m_stopping.store(false);
    if (m_socket.is_open()) {
        asio::error_code ec;
        m_socket.close(ec);
    }
    m_socket = asio::ip::tcp::socket(m_ioContext);

    ConfigData conf = Config::Read();
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address(conf.host), conf.port);

    std::cout << "[StatsClient] Connecting to " << conf.host << ":" << conf.port << "...\n";

    asio::steady_timer timer(m_ioContext, std::chrono::seconds(5));
    asio::error_code connect_ec;
    bool connect_done = false;

    m_socket.async_connect(endpoint, [&](const asio::error_code& ec) {
        connect_ec = ec;
        connect_done = true;
        timer.cancel();
    });

    timer.async_wait([&](const asio::error_code& ec) {
        if (!connect_done) {
            asio::error_code cancel_ec;
            m_socket.close(cancel_ec);
        }
    });

    m_ioContext.restart();
    m_ioContext.run();

    if (connect_ec || !m_socket.is_open()) {
        throw std::runtime_error(connect_ec ? connect_ec.message() : "Connect timeout");
    }

    std::cout << "[StatsClient] Connected to Rocket League!\n";

#ifdef _WIN32
    DWORD timeoutMs = 5000;
    setsockopt(m_socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));
#endif

    std::string dataBuffer;
    char readBuf[4096];

    while (!m_stopping.load()) {
        asio::error_code ec;
        size_t length = m_socket.read_some(asio::buffer(readBuf), ec);

        if (m_stopping.load()) {
            return;
        }

        if (ec) {
            if (ec == asio::error::would_block || ec == asio::error::timed_out || ec.value() == 10060) {
                continue;
            }
            throw asio::system_error(ec);
        }

        dataBuffer.append(readBuf, length);

        if (dataBuffer.size() > 1024 * 1024) {
            std::cerr << "[StatsClient] Error: Network packet buffer exceeded 1MB. Clearing buffer.\n";
            dataBuffer.clear();
            throw std::runtime_error("Network buffer limit exceeded");
        }

        size_t searchPos = 0;
        while (searchPos < dataBuffer.size()) {
            if (std::isspace(dataBuffer[searchPos])) { searchPos++; continue; }
            if (dataBuffer[searchPos] != '{') { searchPos++; continue; }

            int braceCount = 0;
            bool inString = false;
            bool isEscaped = false;
            size_t endIdx = std::string::npos;

            for (size_t i = searchPos; i < dataBuffer.size(); ++i) {
                char c = dataBuffer[i];
                if (isEscaped) { isEscaped = false; }
                else if (c == '\\') { isEscaped = true; }
                else if (c == '"') { inString = !inString; }
                else if (!inString) {
                    if (c == '{') braceCount++;
                    else if (c == '}') { braceCount--; if (braceCount == 0) { endIdx = i; break; } }
                }
            }

            if (endIdx != std::string::npos) {
                std::string jsonStr = dataBuffer.substr(searchPos, endIdx - searchPos + 1);
                if (onJsonLine) {
                    try { onJsonLine(jsonStr); }
                    catch (const std::exception& e) {
                        std::cout << "[StatsClient] JSON Parse Error: " << e.what() << "\n";
                    }
                }
                searchPos = endIdx + 1;
            } else {
                break;
            }
        }

        if (searchPos > 0) {
            if (searchPos >= dataBuffer.size()) dataBuffer.clear();
            else dataBuffer.erase(0, searchPos);
        }
    }
}
