// hub-sim: the Hubbo core on a developer machine, with simulated adapters.
// Scenario script syntax: sim/src/scenario.h.
//
//   hub-sim run <script|->...  [-v]   run scenario scripts; exit 1 on failure
//   hub-sim serve [--port 8080] [--bind 127.0.0.1] [--allow-remote] [--auth] [-v]

#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "host_guard.h"
#include "hubbo/domain/time.h"
#include "http_server.h"
#include "noauth.h"
#include "scenario.h"
#include "sim_api.h"

namespace {

volatile std::sig_atomic_t gStop = 0;

void onSignal(int) { gStop = 1; }

int usage() {
    std::cerr << "usage:\n"
                 "  hub-sim run <script|->... [-v]\n"
                 "  hub-sim serve [--port N] [--bind ADDR] [--allow-remote] [--auth] [-v]\n";
    return 2;
}

int runScripts(const std::vector<std::string> &args) {
    bool verbose = false;
    std::vector<std::string> files;
    for (const auto &a : args) {
        if (a == "-v") {
            verbose = true;
        } else {
            files.push_back(a);
        }
    }
    if (files.empty()) return usage();
    int failures = 0;
    for (const auto &file : files) {
        hubbo::sim::ScenarioRunner runner(verbose ? &std::cout : nullptr);
        bool ok;
        if (file == "-") {
            ok = runner.run(std::cin, "stdin");
        } else {
            std::ifstream in(file);
            if (!in) {
                std::cerr << "cannot open " << file << "\n";
                failures++;
                continue;
            }
            ok = runner.run(in, file);
        }
        if (ok) {
            std::cout << "PASS " << file << "\n";
        } else {
            std::cout << "FAIL " << runner.error() << "\n";
            failures++;
        }
    }
    return failures == 0 ? 0 : 1;
}

int serve(const std::vector<std::string> &args) {
    hubbo::sim::ListenOptions options;
    bool authEnabled = false;
    bool verbose = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        if (a == "--port" && i + 1 < args.size()) {
            options.port = static_cast<uint16_t>(std::stoi(args[++i]));
        } else if (a == "--bind" && i + 1 < args.size()) {
            options.bind = args[++i];
        } else if (a == "--allow-remote") {
            options.allowNonLoopback = true;
        } else if (a == "--auth") {
            authEnabled = true;
        } else if (a == "-v") {
            verbose = true;
        } else {
            return usage();
        }
    }
    if (!authEnabled && options.allowNonLoopback && !hubbo::sim::isLoopbackAddress(options.bind)) {
        std::cerr << "warning: no-auth simulator exposed on " << options.bind
                  << " (trusted development network only)\n";
    }

    hubbo::sim::SimulatorNoAuth noAuth;
    hubbo::sim::DenyAllAuth denyAll;
    hubbo::http::RequestAuthenticator &auth =
        authEnabled ? static_cast<hubbo::http::RequestAuthenticator &>(denyAll) : noAuth;
    const char *mode = authEnabled ? "enforced" : "simulator_no_auth";  // api/openapi.yaml AuthMode

    hubbo::sim::ScenarioRunner runner(verbose ? &std::cout : nullptr);
    hubbo::sim::SimApi api(runner, mode);
    hubbo::sim::HttpServer server;
    std::string error;
    if (!server.listen(options, &error)) {
        std::cerr << "hub-sim: " << error << "\n";
        return 1;
    }

    if (!authEnabled) {
        std::cout << "\n  ************************************************\n"
                  << "  *  " << hubbo::sim::kNoAuthBanner << "                   *\n"
                  << "  *  every request is a simulated admin;         *\n"
                  << "  *  only fake adapters are connected.           *\n"
                  << "  ************************************************\n\n";
    } else {
        std::cout << "hub-sim: auth enabled (no credentials can be issued yet: protected routes return 401)\n";
    }
    std::cout << "hub-sim listening on http://" << options.bind << ":" << options.port << std::endl;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    const auto start = std::chrono::steady_clock::now();
    while (!gStop) {
        server.serveOnce(5, [&](hubbo::http::Request &request, hubbo::http::Response &response) {
            request.context = auth.authenticate(request);
            api.handle(request, response);
            if (!authEnabled) response.headers.emplace_back("X-Hubbo-Auth", "disabled-simulator");
            if (verbose) {
                std::cout << hubbo::http::methodName(request.method) << " " << request.path << " -> "
                          << response.status << std::endl;
            }
        });
        // Real time drives simulated time in serve mode.
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        const auto target = static_cast<uint32_t>(elapsed);
        const uint32_t now = runner.hub().nowMs();
        if (hubbo::timeAfter(target, now)) runner.hub().advance(target - now);
    }
    std::cout << "hub-sim: stopped" << std::endl;
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const std::string command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);
    if (command == "run") return runScripts(args);
    if (command == "serve") return serve(args);
    return usage();
}
