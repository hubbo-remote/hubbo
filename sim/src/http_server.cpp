#include "http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace hubbo::sim {

namespace {

constexpr size_t kMaxHeaderBytes = 16 * 1024;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

class Headers : public http::HeaderSource {
public:
    std::map<std::string, std::string> values;
    std::string_view header(std::string_view name) const override {
        auto it = values.find(lower(std::string(name)));
        return it == values.end() ? std::string_view() : std::string_view(it->second);
    }
};

const char *reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 422: return "Unprocessable Entity";
        default: return status >= 500 ? "Internal Server Error" : "OK";
    }
}

void writeAll(int fd, const std::string &data) {
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

void respond(int fd, const http::Response &r) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    out += "Content-Type: " + r.contentType + "\r\n";
    out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    for (const auto &h : r.headers) out += h.first + ": " + h.second + "\r\n";
    out += "\r\n";
    out += r.body;
    writeAll(fd, out);
}

}  // namespace

bool isLoopbackAddress(const std::string &address) {
    in_addr addr{};
    if (inet_pton(AF_INET, address.c_str(), &addr) != 1) return false;
    return (ntohl(addr.s_addr) >> 24) == 127;
}

HttpServer::~HttpServer() {
    if (fd_ >= 0) ::close(fd_);
}

bool HttpServer::listen(const ListenOptions &options, std::string *error) {
    if (!isLoopbackAddress(options.bind) && !options.allowNonLoopback) {
        *error = "refusing to bind " + options.bind + ": the simulator listens on loopback only (127.0.0.1)";
        return false;
    }
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options.port);
    if (inet_pton(AF_INET, options.bind.c_str(), &addr.sin_addr) != 1) {
        *error = "bad bind address " + options.bind;
        return false;
    }
    if (::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(fd_, 16) != 0) {
        *error = "bind " + options.bind + ":" + std::to_string(options.port) + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

void HttpServer::serveOnce(int timeoutMs, const BodyLimit &bodyLimit, const Handler &handler) {
    pollfd pfd{fd_, POLLIN, 0};
    if (::poll(&pfd, 1, timeoutMs) <= 0) return;
    const int client = ::accept(fd_, nullptr, nullptr);
    if (client < 0) return;
    timeval tv{2, 0};
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string data;
    char buf[4096];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
        headerEnd = data.find("\r\n\r\n");
        if (data.size() > kMaxHeaderBytes) break;
    }

    http::Response response;
    if (headerEnd == std::string::npos) {
        http::jsonError(response, 400, "bad_request", "malformed request");
        respond(client, response);
        ::close(client);
        return;
    }

    Headers headers;
    const std::string head = data.substr(0, headerEnd);
    const size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        if (end == std::string::npos) end = head.size();
        const std::string line = head.substr(pos, end - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(' '));
            headers.values[lower(line.substr(0, colon))] = value;
        }
        pos = end + 2;
    }

    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 = requestLine.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        http::jsonError(response, 400, "bad_request", "malformed request line");
        respond(client, response);
        ::close(client);
        return;
    }
    const std::string method = requestLine.substr(0, sp1);
    const std::string target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

    http::Request request;
    if (!http::methodFromName(method, &request.method)) {
        http::jsonError(response, 405, "method_not_allowed", "method not allowed");
        respond(client, response);
        ::close(client);
        return;
    }

    const size_t q = target.find('?');
    const std::string path = target.substr(0, q);
    const std::string query = q == std::string::npos ? "" : target.substr(q + 1);

    size_t contentLength = 0;
    const std::string_view cl = headers.header("content-length");
    if (!cl.empty()) contentLength = static_cast<size_t>(std::strtoul(std::string(cl).c_str(), nullptr, 10));
    if (contentLength > bodyLimit(request.method, path)) {
        http::jsonError(response, 413, "payload_too_large", "request body too large");
        respond(client, response);
        ::close(client);
        return;
    }
    std::string body = data.substr(headerEnd + 4);
    while (body.size() < contentLength) {
        const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
        if (n <= 0) break;
        body.append(buf, static_cast<size_t>(n));
    }
    body.resize(std::min(body.size(), contentLength));

    request.path = path;
    request.query = query;
    request.body = body;
    request.headers = &headers;
    handler(request, response);
    respond(client, response);
    ::close(client);
}

}  // namespace hubbo::sim
