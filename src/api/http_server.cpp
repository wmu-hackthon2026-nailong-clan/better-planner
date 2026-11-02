#include "api/http_server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <thread>
#include <utility>

#include "api/json_io.hpp"

namespace bp::api {

namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kMaxBodyBytes = 1024 * 1024;

struct Request {
    std::string method;
    std::string target;
    std::string body;
};

void closeSocket(SOCKET& socket) {
    if (socket != INVALID_SOCKET) {
        closesocket(socket);
        socket = INVALID_SOCKET;
    }
}

bool sendAll(SOCKET socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int result = send(socket, data.data() + sent,
                                static_cast<int>(data.size() - sent), 0);
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

void respond(SOCKET socket, int status, const std::string& statusText,
             const std::string& body,
             const std::string& extraHeaders = std::string()) {
    const std::string response =
        "HTTP/1.1 " + std::to_string(status) + " " + statusText +
        "\r\nContent-Type: application/json; charset=utf-8"
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n" + extraHeaders + "\r\n" + body;
    sendAll(socket, response);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool receiveRequest(SOCKET socket, Request& request) {
    std::string data;
    std::size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        char buffer[4096];
        const int count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return false;
        }
        data.append(buffer, static_cast<std::size_t>(count));
        headerEnd = data.find("\r\n\r\n");
        if (headerEnd == std::string::npos && data.size() > kMaxHeaderBytes) {
            throw std::invalid_argument("request headers too large");
        }
        if (headerEnd != std::string::npos && headerEnd > kMaxHeaderBytes) {
            throw std::invalid_argument("request headers too large");
        }
    }

    const std::string headers = data.substr(0, headerEnd);
    const std::size_t lineEnd = headers.find("\r\n");
    const std::string requestLine = headers.substr(0, lineEnd);
    const std::size_t firstSpace = requestLine.find(' ');
    const std::size_t secondSpace = firstSpace == std::string::npos
                                        ? std::string::npos
                                        : requestLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        throw std::invalid_argument("bad request line");
    }
    request.method = requestLine.substr(0, firstSpace);
    request.target = requestLine.substr(firstSpace + 1,
                                        secondSpace - firstSpace - 1);

    std::size_t contentLength = 0;
    bool hasContentLength = false;
    std::size_t cursor = lineEnd == std::string::npos ? headers.size()
                                                      : lineEnd + 2;
    while (cursor < headers.size()) {
        const std::size_t end = headers.find("\r\n", cursor);
        const std::string line = headers.substr(
            cursor, end == std::string::npos ? std::string::npos : end - cursor);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string name = lower(line.substr(0, colon));
            std::string value = line.substr(colon + 1);
            const std::size_t first = value.find_first_not_of(" \t");
            const std::size_t last = value.find_last_not_of(" \t");
            value = first == std::string::npos
                        ? std::string()
                        : value.substr(first, last - first + 1);
            if (name == "transfer-encoding" && lower(value) != "identity") {
                throw std::invalid_argument("unsupported transfer encoding");
            }
            if (name == "content-length") {
                if (hasContentLength) {
                    throw std::invalid_argument("duplicate content length");
                }
                hasContentLength = true;
                const auto parsed = std::from_chars(
                    value.data(), value.data() + value.size(), contentLength);
                if (parsed.ec != std::errc{} ||
                    parsed.ptr != value.data() + value.size() ||
                    contentLength > kMaxBodyBytes) {
                    throw std::invalid_argument("bad content length");
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        cursor = end + 2;
    }

    const std::size_t bodyStart = headerEnd + 4;
    while (data.size() < bodyStart + contentLength) {
        char buffer[4096];
        const int count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return false;
        }
        data.append(buffer, static_cast<std::size_t>(count));
    }
    request.body = data.substr(bodyStart, contentLength);
    return true;
}

std::string pathOnly(const std::string& target) {
    return target.substr(0, target.find('?'));
}

}  // namespace

struct HttpServer::Impl {
    explicit Impl(std::uint16_t requestedPort) : requestedPort(requestedPort) {}

    std::uint16_t requestedPort = 0;
    std::uint16_t actualPort = 0;
    SOCKET listener = INVALID_SOCKET;
    std::shared_ptr<bp::EventStore> store;
    std::function<void()> onChange;
    std::thread worker;
    std::atomic<bool> isRunning{false};
    std::string error;
    bool wsaReady = false;

    void handleClient(SOCKET client) {
        DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        try {
            Request request;
            if (!receiveRequest(client, request)) {
                return;
            }
            const std::string path = pathOnly(request.target);
            if (path == "/health" && request.method == "GET") {
                respond(client, 200, "OK", "{\"ok\":true}");
                return;
            }
            if (path == "/events" && request.method == "GET") {
                respond(client, 200, "OK", dumpEventsJson(store->all()));
                return;
            }
            if (path == "/events" && request.method == "POST") {
                try {
                    const std::vector<Event> events = parseEventsJson(request.body);
                    for (const Event& event : events) {
                        store->add(event);
                    }
                    if (onChange) {
                        onChange();
                    }
                    respond(client, 200, "OK",
                            "{\"added\":" + std::to_string(events.size()) + "}");
                } catch (const std::invalid_argument&) {
                    respond(client, 400, "Bad Request",
                            "{\"error\":\"invalid JSON or event fields\"}");
                }
                return;
            }
            if (path == "/events" || path == "/health") {
                respond(client, 405, "Method Not Allowed",
                        "{\"error\":\"method not allowed\"}",
                        "Allow: GET, POST\r\n");
                return;
            }
            respond(client, 404, "Not Found", "{\"error\":\"not found\"}");
        } catch (const std::invalid_argument&) {
            respond(client, 400, "Bad Request",
                    "{\"error\":\"bad HTTP request\"}");
        } catch (...) {
            respond(client, 500, "Internal Server Error",
                    "{\"error\":\"internal error\"}");
        }
    }

    void acceptLoop() {
        while (isRunning) {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                break;
            }
            handleClient(client);
            closeSocket(client);
        }
    }
};

HttpServer::HttpServer(std::uint16_t port)
    : impl_(std::make_unique<Impl>(port)) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start(std::shared_ptr<bp::EventStore> store,
                       std::function<void()> onChange) {
    if (impl_->isRunning) {
        return true;
    }
    if (!store) {
        impl_->error = "EventStore is null";
        return false;
    }
    impl_->store = std::move(store);
    impl_->onChange = std::move(onChange);
    impl_->error.clear();

    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        impl_->error = "WSAStartup failed";
        return false;
    }
    impl_->wsaReady = true;
    impl_->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->listener == INVALID_SOCKET) {
        impl_->error = "socket failed";
        WSACleanup();
        impl_->wsaReady = false;
        return false;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(impl_->requestedPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(impl_->listener, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == SOCKET_ERROR ||
        listen(impl_->listener, SOMAXCONN) == SOCKET_ERROR) {
        impl_->error = "bind/listen failed: " + std::to_string(WSAGetLastError());
        closeSocket(impl_->listener);
        WSACleanup();
        impl_->wsaReady = false;
        return false;
    }

    int addressLength = sizeof(address);
    if (getsockname(impl_->listener, reinterpret_cast<sockaddr*>(&address),
                    &addressLength) == SOCKET_ERROR) {
        impl_->error = "getsockname failed";
        closeSocket(impl_->listener);
        WSACleanup();
        impl_->wsaReady = false;
        return false;
    }
    impl_->actualPort = ntohs(address.sin_port);
    impl_->isRunning = true;
    try {
        impl_->worker = std::thread([impl = impl_.get()] { impl->acceptLoop(); });
    } catch (...) {
        impl_->isRunning = false;
        closeSocket(impl_->listener);
        WSACleanup();
        impl_->wsaReady = false;
        impl_->error = "failed to start HTTP thread";
        return false;
    }
    return true;
}

void HttpServer::stop() {
    impl_->isRunning = false;
    closeSocket(impl_->listener);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    if (impl_->wsaReady) {
        WSACleanup();
        impl_->wsaReady = false;
    }
}

bool HttpServer::running() const {
    return impl_->isRunning;
}

std::uint16_t HttpServer::port() const {
    return impl_->actualPort;
}

std::string HttpServer::lastError() const {
    return impl_->error;
}

}  // namespace bp::api
