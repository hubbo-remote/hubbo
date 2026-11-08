#pragma once

// Minimal blocking HTTP/1.1 listener for hub-sim (POSIX sockets). One
// request per connection, single-threaded, driven from the simulator's main
// loop so the engine stays on one thread. It only adapts bytes to
// hubbo::http::Request/Response; routing and handlers are transport-free.

#include <cstdint>
#include <functional>
#include <string>

#include "host_guard.h"
#include "hubbo/http/route.h"

namespace hubbo::sim {

struct ListenOptions {
    std::string bind = "127.0.0.1";
    uint16_t port = 8080;
    bool allowNonLoopback = false;  // must be explicit; refused otherwise
};

class HttpServer {
public:
    using Handler = std::function<void(http::Request &, http::Response &)>;

    ~HttpServer();
    bool listen(const ListenOptions &options, std::string *error);
    // Waits up to timeoutMs for one connection and serves it.
    void serveOnce(int timeoutMs, const Handler &handler);

private:
    int fd_ = -1;
};

bool isLoopbackAddress(const std::string &address);

}  // namespace hubbo::sim
