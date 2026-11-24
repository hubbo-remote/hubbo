#ifdef HUBBO_DEV_CONSOLE
#include "board_stats.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>

#include <new>
#include <vector>

#include "diagnostics.h"
#include "image_marker.h"

extern "C" {
extern int _data_start, _bss_end;  // the linker's: static internal RAM
}

namespace hubbo::fw {
namespace {

// 0 if the metadata read fails; read once it succeeds (a flash read).
uint32_t runningImageBytes() {
    static uint32_t bytes = 0;
    if (bytes == 0) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_image_metadata_t metadata = {};
        const esp_partition_pos_t pos = {running->address, running->size};
        if (esp_image_get_metadata(&pos, &metadata) == ESP_OK) {
            bytes = metadata.image_len;
        }
    }
    return bytes;
}

}  // namespace

HttpServer::DebugReply boardStats(EngineRuntime &runtime, const RmtIrTransmitter &ir, bool reset) {
    diag::LoopStats loop;
    if (reset && !diag::closeWindow(&loop)) {
        return {"the loop didn't answer the window close within 200 ms\n", 503};
    }
    const hubbo::api::RuntimeSnapshot snapshot = runtime.snapshot();
    JsonDocument doc;
    doc["version"] = image::version();
    doc["uptime_ms"] = snapshot.uptimeMs;
    if (const uint32_t bytes = runningImageBytes()) {
        doc["image_bytes"] = bytes;
    } else {
        doc["image_bytes"] = nullptr;
    }
    doc["static_ram_bytes"] = reinterpret_cast<uintptr_t>(&_bss_end) - reinterpret_cast<uintptr_t>(&_data_start);
    if (reset) {
        doc["loop"]["window"]["max_gap_us"] = loop.window.maxUs;
        doc["loop"]["window"]["over_5ms"] = loop.window.over5ms;
        JsonObject hid = doc["latency"]["read_to_hid"].to<JsonObject>();
        hid["count"] = loop.hid.count;
        hid["p95_us"] = loop.hid.p95;
        hid["p99_us"] = loop.hid.p99;
        doc["integrations"]["ble"]["send_failures"] = loop.bleSendFailures;
    }

    JsonObject internal = doc["heap"]["internal"].to<JsonObject>();
    internal["free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    internal["min_free"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    internal["largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    JsonObject psram = doc["heap"]["psram"].to<JsonObject>();
    psram["free"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    psram["largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    // CPU share since the window started (the first window: since boot).
    static diag::TaskShares *shares = new (std::nothrow) diag::TaskShares;
    JsonArray tasks = doc["tasks"].to<JsonArray>();
    for (const diag::TaskShare &row : shares != nullptr ? shares->sample(reset) : std::vector<diag::TaskShare>()) {
        JsonObject t = tasks.add<JsonObject>();
        t["name"] = static_cast<const char *>(row.name);  // copied; a char array is kept by pointer
        t["stack_free_min"] = row.stackFreeMin;
        if (row.shareKnown) {
            t["cpu_pct_x100"] = row.per10k;
        } else {
            t["cpu_pct_x100"] = nullptr;
        }
    }

    doc["radio"]["queue_overflows"] = snapshot.diagnostics.queueOverflows;
    const RmtIrTransmitter::Counters counters = ir.counters();
    JsonObject irOut = doc["integrations"]["ir"].to<JsonObject>();
    irOut["frames"] = counters.frames;
    irOut["late_frames"] = counters.lateFrames;
    irOut["failures"] = counters.failures;

    std::string out;
    serializeJson(doc, out);
    out += '\n';
    return out;
}

}  // namespace hubbo::fw
#endif
