// hub-sim: the Hubbo core on a developer machine, with simulated adapters.
// Scenario script syntax: sim/src/scenario.h.
//
//   hub-sim run <script|->...  [-v]   run scenario scripts; exit 1 on failure
//   hub-sim serve [--port 8780] [--data DIR] [--reset] [--seed FILE | --showcase] [--auth]
//                 [--storage-unavailable] [--ha-token] [--bind 127.0.0.1] [--allow-remote] [-v]
//       --showcase: the fake HA also serves a real HA's captured entities
//       (sim/fixtures/local/, gitignored: see sim/src/showcase.h), and the
//       seed registers them all, for the web admin.
//       Home Assistant is the in-process fake HA (never a real one). It's
//       not_configured until a token is set (PUT /admin/integrations/ha_main
//       with "fake-ha-token"); --ha-token stores that token at boot if none is.
//       --storage-unavailable boots with the storage medium unavailable
//       (config state storage_unavailable); POST /sim/storage?health=...
//       (admin) or, locally, SIGUSR1 (unavailable) / SIGUSR2 (available)
//       switch it at runtime.
//   hub-sim write-record --data DIR --name NAME --file FILE
//       write FILE's bytes as the next generation of record NAME, unvalidated
//       (e.g. put a newer-schema or non-compiling config in place to test the
//       Blocked state: --name config --file sim/fixtures/config.newer-schema.json)

#include <chrono>
#include <csignal>
#include <filesystem>
#include <sstream>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "host_guard.h"
#include "http_server.h"
#include "hubbo/config/json.h"
#include "hubbo/auth/credentials.h"
#include "hubbo/auth/crypto.h"
#include "hubbo/auth/secret_store.h"
#include "hubbo/storage/record_store.h"
#include "hubbo/storage/storage.h"
#include "noauth.h"
#include "scenario.h"
#include "runtime.h"
#include "showcase.h"
#include "sim_api.h"

namespace {

volatile std::sig_atomic_t gStop = 0;
// Local, trusted storage-health control for tests and supervisors (no HTTP,
// so it works whatever the auth state): SIGUSR1 = unavailable, SIGUSR2 =
// available. 0 = nothing pending.
volatile std::sig_atomic_t gStorageSignal = 0;

void onSignal(int) { gStop = 1; }
void onStorageSignal(int signal) { gStorageSignal = signal; }

int usage() {
    std::cerr << "usage:\n"
                 "  hub-sim run <script|->... [-v]\n"
                 "  hub-sim serve [--port N] [--data DIR] [--reset] [--seed FILE | --showcase] [--auth]\n"
                 "                [--storage-unavailable] [--ha-token] [--bind ADDR] [--allow-remote] [-v]\n"
                 "      --showcase: serve a real HA's captured entities, all registered (sim/src/showcase.h)\n"
                 "      --ha-token: store the fake HA's token (fake-ha-token) at boot if none is\n"
                 "      --storage-unavailable: boot with storage down; POST /sim/storage?health=\n"
                 "      available|unavailable|read_only (admin) switches it at runtime, as do the\n"
                 "      local signals SIGUSR1 (unavailable) and SIGUSR2 (available)\n"
                 "  hub-sim write-record --data DIR --name NAME --file FILE\n"
                 "      write FILE as the next generation of record NAME (config, active_activity,\n"
                 "      secrets), without validating it; e.g. a newer-schema config to test\n"
                 "      the Blocked state (sim/fixtures/config.newer-schema.json)\n";
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

// The dataset hub-sim serves: api/fixtures/admin/backup.json, shared with
// the API contract tests and the web mock. False (with a message) if it
// can't be used.
bool loadSeed(const std::string &path, hubbo::ConfigSpec *seed) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const hubbo::config::ParseResult parsed = hubbo::config::parseBackup(text.str(), seed);
    if (!in || parsed.code != hubbo::ErrorCode::Ok) {
        std::cerr << "hub-sim: can't use seed " << path << " (" << hubbo::errorCodeName(parsed.code) << ")\n";
        return false;
    }
    return true;
}

int serve(const std::vector<std::string> &args) {
    hubbo::sim::ListenOptions options;
    bool authEnabled = false;
    bool verbose = false;
    bool reset = false;
    bool storageUnavailable = false;
    bool haToken = false;
    bool showcase = false;
    std::string dataDir;
    std::string seedPath = HUBBO_API_FIXTURES_DIR "/admin/backup.json";
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        if (a == "--port" && i + 1 < args.size()) {
            options.port = static_cast<uint16_t>(std::stoi(args[++i]));
        } else if (a == "--bind" && i + 1 < args.size()) {
            options.bind = args[++i];
        } else if (a == "--data" && i + 1 < args.size()) {
            dataDir = args[++i];
        } else if (a == "--seed" && i + 1 < args.size()) {
            seedPath = args[++i];
        } else if (a == "--showcase") {
            showcase = true;
        } else if (a == "--reset") {
            reset = true;
        } else if (a == "--storage-unavailable") {
            storageUnavailable = true;
        } else if (a == "--ha-token") {
            haToken = true;
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

    // Config, the active activity and credentials persist as records in
    // --data DIR (config.a/.b, active_activity.a/.b, secrets.a/.b), so
    // a restart is a simulated reboot. Without --data they're in memory.
    std::unique_ptr<hubbo::storage::StdioStorage> disk;
    std::unique_ptr<hubbo::storage::SlotRecordStore> secretRecords;
    std::unique_ptr<hubbo::auth::SecretStore> secrets;
    if (!dataDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dataDir, ec);
        if (ec) {
            std::cerr << "hub-sim: can't create " << dataDir << ": " << ec.message() << "\n";
            return 1;
        }
        disk = std::make_unique<hubbo::storage::StdioStorage>(dataDir);
        if (disk->health() != hubbo::storage::Health::Available) {
            std::cerr << "hub-sim: " << dataDir << " is " << hubbo::storage::healthName(disk->health()) << "\n";
        }
    }

    // Every record goes through a medium whose health can be switched.
    hubbo::storage::MemoryStorage memory;
    hubbo::sim::SwitchableStorage medium(disk ? static_cast<hubbo::storage::Storage &>(*disk) : memory);
    if (storageUnavailable) medium.setHealth(hubbo::storage::Health::Unavailable);

    hubbo::ConfigSpec seed;
    if (showcase) {
        const std::string missing = hubbo::sim::showcaseMissing();
        if (!missing.empty()) {
            std::cerr << "hub-sim: --showcase needs a local Home Assistant capture; missing " << missing << "\n";
            return 1;
        }
        seed = hubbo::sim::showcaseConfig();  // a real HA's entities, for looking over the web admin
    } else if (!loadSeed(seedPath, &seed)) {
        return 1;
    }
    // The secrets' record store shares the medium (and so its trust, see
    // Storage::distrust) with the config's.
    if (disk) {
        secretRecords = std::make_unique<hubbo::storage::SlotRecordStore>(medium, 64 * 1024);
        secrets = std::make_unique<hubbo::auth::StorageSecretStore>(*secretRecords);
    } else {
        secrets = std::make_unique<hubbo::auth::MemorySecretStore>();
    }
    hubbo::auth::SoftwareHash hash;
    hubbo::sim::HostRandom random;
    hubbo::auth::Credentials credentials(*secrets, hash, random);
    // Boots through bootHub(): config, credentials, active activity.
    // Serve runs the real HA adapter against the fake HA server, so the API
    // contract and e2e go through snapshot -> call_service -> result.
    // --ha-token: connected from the start (the fake token guards nothing).
    if (std::string token;
        haToken && secrets->read(hubbo::api::kHaTokenKey, &token) == hubbo::auth::SecretRead::NotFound) {
        secrets->write(hubbo::api::kHaTokenKey, hubbo::sim::FakeHaServer().token);
    }
    hubbo::sim::ScenarioRunner runner(&std::cout, &medium, /*pinned=*/true, &seed, /*state=*/nullptr, &credentials,
                                      /*realHomeAssistant=*/true, /*bonds=*/disk.get());
    if (!verbose) runner.hub().onLog = nullptr;
    if (showcase) runner.hub().haServer().enableShowcase();
    if (reset) runner.runLine("reset");
    hubbo::sim::SimApi api(runner, authEnabled, credentials, &medium, secrets.get());
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
        std::cout << "hub-sim: auth enabled (set up the admin with POST /api/v1/auth/setup)\n";
    }
    const hubbo::config::ConfigService &service = runner.hub().configService();
    std::cout << "hub-sim: config " << (dataDir.empty() ? "in memory" : "in " + dataDir) << ", state "
              << hubbo::config::configStateName(service.state()) << ", generation " << service.generation()
              << ", active "
              << (runner.hub().engine().hasActiveActivity() ? runner.hub().engine().activeActivityId() : "none")
              << "\n";
    std::cout << "hub-sim listening on http://" << options.bind << ":" << options.port << std::endl;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGUSR1, onStorageSignal);
    std::signal(SIGUSR2, onStorageSignal);
    hubbo::http::RequestAuthenticator &auth = api.authenticator();
    const auto start = std::chrono::steady_clock::now();
    uint64_t drivenMs = 0;  // real time already turned into hub time
    while (!gStop) {
        if (const int signal = gStorageSignal) {
            gStorageSignal = 0;
            const auto health =
                signal == SIGUSR1 ? hubbo::storage::Health::Unavailable : hubbo::storage::Health::Available;
            medium.setHealth(health);
            std::cout << "hub-sim: storage " << hubbo::storage::healthName(health) << " (signal)" << std::endl;
        }
        server.serveOnce(
            5, [&](hubbo::http::Method method, std::string_view path) { return api.bodyLimit(method, path); },
            [&](hubbo::http::Request &request, hubbo::http::Response &response) {
            request.context = auth.authenticate(request);
            api.handle(request, response);
            if (!authEnabled) response.headers.emplace_back("X-Hubbo-Auth", "disabled-simulator");
            if (verbose) {
                std::cout << hubbo::http::methodName(request.method) << " " << request.path << " -> "
                          << response.status << std::endl;
            }
        });
        // Real time drives simulated time in serve mode: each pass adds the
        // real time since the last one, so a /sim/advance jump ahead never
        // stops the clock until real time catches up.
        const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now() - start)
                                                       .count());
        if (elapsed > drivenMs) {
            runner.hub().advance(static_cast<uint32_t>(elapsed - drivenMs));
            drivenMs = elapsed;
        }
    }
    runner.hub().flush();  // graceful stop: write the debounced active activity
    std::cout << "hub-sim: stopped" << std::endl;
    return 0;
}

// Writes a file as the next generation of a record, correctly framed but
// unvalidated: the supported way to put an unusable config in place.
int writeRecord(const std::vector<std::string> &args) {
    std::string dataDir, name, file;
    for (size_t i = 0; i + 1 < args.size(); i += 2) {
        if (args[i] == "--data") {
            dataDir = args[i + 1];
        } else if (args[i] == "--name") {
            name = args[i + 1];
        } else if (args[i] == "--file") {
            file = args[i + 1];
        } else {
            return usage();
        }
    }
    if (dataDir.empty() || name.empty() || file.empty() || args.size() % 2 != 0) return usage();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        std::cerr << "hub-sim: can't read " << file << "\n";
        return 1;
    }
    std::stringstream text;
    text << in.rdbuf();
    std::error_code ec;
    std::filesystem::create_directories(dataDir, ec);
    hubbo::storage::StdioStorage disk(dataDir);
    hubbo::storage::SlotRecordStore records(disk, hubbo::config::kMaxConfigFileBytes);
    uint64_t generation = 0;
    const hubbo::ErrorCode next = records.nextGeneration(name, &generation);
    if (next != hubbo::ErrorCode::Ok) {
        std::cerr << "hub-sim: can't read record " << name << ": " << hubbo::errorCodeName(next) << "\n";
        return 1;
    }
    const hubbo::ErrorCode saved = records.save(name, generation, text.str());
    if (saved != hubbo::ErrorCode::Ok) {
        std::cerr << "hub-sim: can't write record " << name << ": " << hubbo::errorCodeName(saved) << "\n";
        return 1;
    }
    std::cout << "wrote " << name << " generation " << generation << " (" << text.str().size() << " bytes)\n";
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    const std::string command = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);
    if (command == "run") return runScripts(args);
    if (command == "serve") return serve(args);
    if (command == "write-record") return writeRecord(args);
    return usage();
}
