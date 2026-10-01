#include "HttpServer.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include "Logging.h"

HttpServer::~HttpServer() {
    Stop();
}

bool HttpServer::Start(uint16_t p_Port, Handler p_Handler) {
    m_Handler = std::move(p_Handler);

    WSADATA s_WsaData;
    if (WSAStartup(MAKEWORD(2, 2), &s_WsaData) != 0) {
        Logger::Error("MCPBridge: WSAStartup failed");
        return false;
    }

    SOCKET s_Socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_Socket == INVALID_SOCKET) {
        Logger::Error("MCPBridge: socket() failed");
        return false;
    }

    int s_Opt = 1;
    setsockopt(s_Socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&s_Opt), sizeof(s_Opt));

    sockaddr_in s_Addr{};
    s_Addr.sin_family = AF_INET;
    s_Addr.sin_port = htons(p_Port);
    inet_pton(AF_INET, "127.0.0.1", &s_Addr.sin_addr);

    if (bind(s_Socket, reinterpret_cast<sockaddr*>(&s_Addr), sizeof(s_Addr)) == SOCKET_ERROR ||
        listen(s_Socket, 8) == SOCKET_ERROR) {
        Logger::Error("MCPBridge: bind/listen failed on port {}", p_Port);
        closesocket(s_Socket);
        return false;
    }

    m_ListenSocket = s_Socket;
    m_Port = p_Port;
    m_Running = true;
    m_Thread = std::thread(&HttpServer::Loop, this);

    Logger::Info("MCPBridge: listening on 127.0.0.1:{}", p_Port);
    return true;
}

void HttpServer::Stop() {
    m_Running = false;

    // Wake the accept() thread: connect to ourselves so accept returns, then
    // close the listener. closesocket() alone does not reliably unblock a
    // pending accept() on Winsock and can hang the join.
    if (m_ListenSocket != ~uintptr_t(0)) {
        SOCKET s_Wake = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_Wake != INVALID_SOCKET) {
            sockaddr_in s_Addr{};
            s_Addr.sin_family = AF_INET;
            s_Addr.sin_port = htons(m_Port);
            inet_pton(AF_INET, "127.0.0.1", &s_Addr.sin_addr);
            connect(s_Wake, reinterpret_cast<sockaddr*>(&s_Addr), sizeof(s_Addr));
            closesocket(s_Wake);
        }
        closesocket(static_cast<SOCKET>(m_ListenSocket));
        m_ListenSocket = ~uintptr_t(0);
    }

    if (m_Thread.joinable())
        m_Thread.join();
    WSACleanup();
}

void HttpServer::Loop() {
    while (m_Running) {
        SOCKET s_Client = accept(static_cast<SOCKET>(m_ListenSocket), nullptr, nullptr);
        if (s_Client == INVALID_SOCKET || !m_Running) {
            if (s_Client != INVALID_SOCKET)
                closesocket(s_Client);
            break;
        }

        DWORD s_Timeout = 15000;
        setsockopt(s_Client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&s_Timeout), sizeof(s_Timeout));

        // Read headers.
        std::string s_Request;
        s_Request.reserve(4096);
        char s_Buf[4096];
        size_t s_HeaderEnd = std::string::npos;

        while (s_HeaderEnd == std::string::npos && s_Request.size() < 65536) {
            int s_Recv = recv(s_Client, s_Buf, sizeof(s_Buf), 0);
            if (s_Recv <= 0)
                break;
            s_Request.append(s_Buf, s_Recv);
            s_HeaderEnd = s_Request.find("\r\n\r\n");
        }

        if (s_HeaderEnd == std::string::npos) {
            closesocket(s_Client);
            continue;
        }

        // Parse request line.
        std::string s_Method, s_Target;
        {
            size_t s_Sp1 = s_Request.find(' ');
            size_t s_Sp2 = s_Request.find(' ', s_Sp1 + 1);
            if (s_Sp1 == std::string::npos || s_Sp2 == std::string::npos) {
                closesocket(s_Client);
                continue;
            }
            s_Method = s_Request.substr(0, s_Sp1);
            s_Target = s_Request.substr(s_Sp1 + 1, s_Sp2 - s_Sp1 - 1);
        }

        // Content-Length.
        size_t s_ContentLength = 0;
        {
            auto s_Pos = s_Request.find("Content-Length:");
            if (s_Pos == std::string::npos)
                s_Pos = s_Request.find("content-length:");
            if (s_Pos != std::string::npos)
                s_ContentLength = strtoul(s_Request.c_str() + s_Pos + 15, nullptr, 10);
        }

        // Read body.
        std::string s_Body = s_Request.substr(s_HeaderEnd + 4);
        while (s_Body.size() < s_ContentLength) {
            int s_Recv = recv(s_Client, s_Buf, sizeof(s_Buf), 0);
            if (s_Recv <= 0)
                break;
            s_Body.append(s_Buf, s_Recv);
        }
        s_Body = s_Body.substr(0, s_ContentLength);

        std::string s_Path = s_Target;
        std::string s_Query;
        if (auto s_Q = s_Target.find('?'); s_Q != std::string::npos) {
            s_Path = s_Target.substr(0, s_Q);
            s_Query = s_Target.substr(s_Q + 1);
        }

        auto [s_Status, s_ResponseBody] = m_Handler(s_Method, s_Path, s_Query, s_Body);

        std::string s_Response = std::format(
            "HTTP/1.1 {} OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
            s_Status, s_ResponseBody.size()
        );
        s_Response += s_ResponseBody;

        size_t s_Sent = 0;
        while (s_Sent < s_Response.size()) {
            const int s_N = send(
                s_Client, s_Response.data() + s_Sent,
                static_cast<int>(s_Response.size() - s_Sent), 0
            );
            if (s_N <= 0)
                break;
            s_Sent += static_cast<size_t>(s_N);
        }
        closesocket(s_Client);
    }
}
