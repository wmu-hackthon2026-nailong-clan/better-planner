#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <iostream>
#include <memory>
#include <string>

#include "api/http_server.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* name) {
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL: " << name << std::endl;
    }
}

std::string request(std::uint16_t port, const std::string& raw) {
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        return std::string();
    }
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(socket, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR) {
        closesocket(socket);
        return std::string();
    }
    std::size_t sent = 0;
    while (sent < raw.size()) {
        const int result = send(socket, raw.data() + sent,
                                static_cast<int>(raw.size() - sent), 0);
        if (result <= 0) {
            closesocket(socket);
            return std::string();
        }
        sent += static_cast<std::size_t>(result);
    }
    std::string response;
    while (true) {
        char buffer[4096];
        const int count = recv(socket, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    closesocket(socket);
    return response;
}

std::string post(std::uint16_t port, const std::string& body) {
    return request(port,
                   "POST /events HTTP/1.1\r\nHost: localhost\r\n"
                   "Content-Type: application/json\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
                       body);
}

}  // namespace

int main() {
    std::cout << std::unitbuf;
    auto store = std::make_shared<bp::EventStore>();
    std::atomic<int> changes = 0;
    bp::api::HttpServer server(0);
    const bool started = server.start(store, [&changes] { ++changes; });
    check(started, "http: starts on ephemeral port");
    if (!started) {
        std::cout << server.lastError() << std::endl;
        return 1;
    }

    const std::string health = request(
        server.port(), "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    check(health.starts_with("HTTP/1.1 200"), "http: health 200");
    check(health.find("{\"ok\":true}") != std::string::npos,
          "http: health body");

    const std::string response = post(
        server.port(),
        "[{\"type\":\"schedule\",\"start\":100,\"durationSeconds\":60,"
        "\"title\":\"http schedule\"},{\"type\":\"deadline\",\"when\":500,"
        "\"title\":\"http deadline\"}]");
    check(response.starts_with("HTTP/1.1 200"), "http: post 200");
    check(response.find("{\"added\":2}") != std::string::npos,
          "http: added count");
    check(store->size() == 2, "http: store updated");
    check(changes == 1, "http: change callback once per batch");

    const std::string events = request(
        server.port(), "GET /events HTTP/1.1\r\nHost: localhost\r\n\r\n");
    check(events.starts_with("HTTP/1.1 200"), "http: get 200");
    check(events.find("http schedule") != std::string::npos &&
              events.find("http deadline") != std::string::npos,
          "http: get exports events");

    const std::string bad = post(
        server.port(),
        "[{\"type\":\"deadline\",\"when\":700},"
        "{\"type\":\"schedule\",\"start\":100}]");
    check(bad.starts_with("HTTP/1.1 400"), "http: invalid post 400");
    check(store->size() == 2, "http: invalid batch is atomic");
    check(changes == 1, "http: invalid batch does not notify");

    const std::string missing = request(
        server.port(), "GET /missing HTTP/1.1\r\nHost: localhost\r\n\r\n");
    check(missing.starts_with("HTTP/1.1 404"), "http: unknown path 404");
    const std::string wrongMethod = request(
        server.port(), "PUT /events HTTP/1.1\r\nHost: localhost\r\n\r\n");
    check(wrongMethod.starts_with("HTTP/1.1 405"), "http: wrong method 405");
    const std::string duplicateLength = request(
        server.port(),
        "POST /events HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 0\r\nContent-Length: 0\r\n\r\n");
    check(duplicateLength.starts_with("HTTP/1.1 400"),
          "http: duplicate content-length 400");

    server.stop();
    check(!server.running(), "http: clean stop");
    if (g_failures != 0) {
        std::cout << g_failures << " HTTP check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "all HTTP checks passed" << std::endl;
    return 0;
}
