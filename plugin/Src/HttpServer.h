#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

// Minimal blocking HTTP/1.x server on loopback. One request per connection,
// serial accepts - plenty for a local bridge.
class HttpServer {
public:
    // Returns {statusCode, body}. Body is always served as application/json.
    using Handler = std::function<
        std::pair<int, std::string>(
            const std::string& p_Method, const std::string& p_Path, const std::string& p_Query,
            const std::string& p_Body
        )>;

    ~HttpServer();

    bool Start(uint16_t p_Port, Handler p_Handler);
    void Stop();

private:
    void Loop();

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    uintptr_t m_ListenSocket = ~uintptr_t(0); // SOCKET, kept opaque to avoid winsock in header
    uint16_t m_Port = 0;
    Handler m_Handler;
};
