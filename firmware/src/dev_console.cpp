#ifdef HUBBO_DEV_CONSOLE
#include "dev_console.h"

#include <Arduino.h>
#include <esp_core_dump.h>
#include <esp_partition.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "board_stats.h"
#include "commands.h"
#include "console.h"
#include "hubbo/http/route.h"
#include "image_marker.h"

namespace hubbo::fw::dev_console {
namespace {

using hubbo::http::queryValue;  // form-decoded ('+' and %XX)

const Board *board = nullptr;

// --- /debug/run: one read-only console command, run on the loop task like a
// typed one (its output lands in /debug/log). Nothing that changes state,
// pairs, erases or reboots.
constexpr const char *kQueueable[] = {"status", "mem", "lat", "lat reset", "tasks", "help"};
std::atomic<const char *> queued{nullptr};

HttpServer::DebugReply runQuery(std::string_view query) {
    const std::string name = queryValue(query, "cmd");
    if (commands::isIrCommand(name)) {  // sends a code; takes the rest of the line
        con.printf("> %s\n", name.c_str());
        commands::irCommand(std::string_view(name).substr(2));  // already on httpd
        return {"ran (read /debug/log)\n"};
    }
    for (const char *command : kQueueable) {
        if (name == command) {
            const char *expected = nullptr;
            if (!queued.compare_exchange_strong(expected, command)) {
                return {"busy: a command is already queued\n", 409};
            }
            return std::string("queued: ") + command + " (read /debug/log)\n";
        }
    }
    std::string allowed = "allowed:";
    for (const char *command : kQueueable) {
        allowed += std::string(" ") + command + ",";
    }
    allowed.back() = '\n';
    return {allowed, 400};
}

// The core dump the last panic left in flash (raw partition image: header,
// ELF, checksum). Decode with esp-coredump against that build's firmware.elf.
std::string coreDump(std::string_view) {
    size_t address = 0;
    size_t size = 0;
    if (esp_core_dump_image_get(&address, &size) != ESP_OK) {
        return "no core dump stored\n";
    }
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (partition == nullptr || address < partition->address || size > partition->size) {
        return "core dump partition not found\n";
    }
    std::string image(size, '\0');
    if (esp_partition_read(partition, address - partition->address, image.data(), size) != ESP_OK) {
        return "core dump read failed\n";
    }
    return image;
}

// Online updates against a local test feed (the API's routes need a login):
// ?op=test&base=<url>&running=<X.Y.Z> sets the feed and the version checks
// compare against (signatures still need the release key, or this build's
// test key: release_key.h); op=check and op=install&version=X call the
// API's port; op=crash restarts at once (no flush), as a crash would, to try
// the trial's rollback. Every answer ends with the status.
std::string releaseDebug(std::string_view query) {
    FirmwareReleases &releases = board->releases;
    const std::string op = queryValue(query, "op");
    std::string r;
    if (op == "test") {
        r = releases.setTest(queryValue(query, "base"), queryValue(query, "running")) ? "test feed queued\n"
                                                                                      : "update task busy\n";
    } else if (op == "check") {
        r = releases.requestCheck() == hubbo::api::ReleaseCheckResult::Accepted ? "check started\n" : "busy\n";
    } else if (op == "install") {
        const hubbo::api::ReleaseInstallResult result = releases.install(queryValue(query, "version"));
        r = result == hubbo::api::ReleaseInstallResult::Accepted       ? "install started\n"
            : result == hubbo::api::ReleaseInstallResult::NotAvailable ? "not available\n"
                                                                        : "busy\n";
    } else if (op == "crash") {
        board->http.post([] {
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        });
        return "restarting with no flush\n";
    }
    const hubbo::api::FirmwareReleaseStatus status = releases.releaseStatus();
    const hubbo::api::FirmwareUpdateStatus update = board->update.status();
    const char *error = hubbo::release::checkErrorName(status.check.error);
    char line[256];
    snprintf(line, sizeof(line),
             "running %s; checking %d, checked %d (%lu ms ago), error %s, available %s, rolled_back_from %s; "
             "update %s %u%% %s, trial %d\n",
             image::version(), status.check.checking, status.check.checked,
             static_cast<unsigned long>(status.checkedAgoMs), error != nullptr ? error : "null",
             status.check.available ? status.check.available->version.c_str() : "null",
             status.check.rolledBackFrom.empty() ? "null" : status.check.rolledBackFrom.c_str(),
             hubbo::api::firmwareUpdateStateName(update.state), static_cast<unsigned>(update.progress),
             update.error.c_str(), board->update.trial());
    return r + line;
}

}  // namespace

void begin(const Board &b) {
    static const Board kept = b;
    board = &kept;
    b.http.addDebugGet("/debug/log", [](std::string_view) { return con.recent(); });
    b.http.addDebugGet("/debug/run", runQuery);
    b.http.addDebugGet("/debug/coredump", coreDump);
    b.http.addDebugGet("/debug/release", releaseDebug);  // online updates against a test feed
    b.http.addDebugGet("/debug/stats", [](std::string_view query) {
        return boardStats(board->runtime, board->ir, queryValue(query, "reset") == "1");
    });
    con.println("debug: dev console on /debug/log, /debug/run, /debug/coredump, /debug/release, /debug/stats");
}

void poll() {
    if (queued.load(std::memory_order_relaxed) != nullptr) {
        char line[32];
        snprintf(line, sizeof(line), "%s", queued.exchange(nullptr, std::memory_order_acq_rel));
        con.printf("> %s\n", line);
        commands::run(line);
    }
}

}  // namespace hubbo::fw::dev_console
#endif
