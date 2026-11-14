#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hubbo {
struct DeviceSpec;  // domain/config.h
}

// What Home Assistant reported about one entity when it was registered or
// re-linked: the device's stored capabilities (docs/design/phase3-home-
// assistant.md, section 3). Plain data, no HA connection needed: the device's
// commands are derived from it (EntityCapabilities::fromSnapshot) when the
// config compiles, so they work while HA is offline.
//
// Stored with the device as `ha_capabilities` (config and backups; the
// HaCapabilitySnapshot schema in api/openapi.yaml). Not secret. Decoding
// validates it like any other input (snapshot_json.h).
namespace hubbo::ha {

enum class SnapshotParamType : uint8_t { Bool, Int, Number, Enum };

// One typed field of an integration-specific service. `name` is both the
// Hubbo param name and the HA service data key.
struct SnapshotParam {
    std::string name;
    std::string label;
    SnapshotParamType type = SnapshotParamType::Bool;
    bool required = false;
    double minimum = 0;  // Int, Number
    double maximum = 0;
    double step = 0;     // 0 = none
    std::string unit;
    std::string description;           // HA's field description (a tooltip), "" if none
    std::vector<std::string> options;  // Enum
    // HA's `default` for the field, when it fits the type and range: shown
    // to clients, and what HA uses when the param is left out.
    bool hasDefault = false;
    double defaultNumber = 0;   // Bool (0/1), Int, Number
    std::string defaultOption;  // Enum: one of `options`

    bool operator==(const SnapshotParam &other) const;
};

// An integration-specific service offered for the entity. `service` is
// "<platform>.<service>": the HA call and the Hubbo command id.
struct SnapshotService {
    std::string service;
    std::string label;
    std::vector<SnapshotParam> fields;

    bool operator==(const SnapshotService &other) const;
};

// What HA says about one field of a custom service: a hint for writing its
// data, never a typed param.
struct SnapshotHint {
    std::string name;  // the HA service data key
    bool required = false;
    std::string example;      // HA's example as compact JSON text; "" none (or over kMaxHintExampleBytes)
    std::string description;  // HA's field description (a tooltip), "" if none

    bool operator==(const SnapshotHint &other) const;
};

// An integration service matching the entity that Hubbo couldn't type (a
// text, object or other untyped field, or too many fields). It can still be
// sent as a Home Assistant action (command call_service) with data the user
// writes as JSON. Response-only services are never here.
struct SnapshotCustomService {
    std::string service;  // "<platform>.<service>"
    std::string label;
    std::vector<SnapshotHint> fields;  // target-like fields left out

    bool operator==(const SnapshotCustomService &other) const;
};

// What one custom service costs against kMaxCustomHintBytes: its strings'
// bytes (service, label, and each hint's name, example and description).
size_t customHintBytes(const SnapshotCustomService &service);

struct CapabilitySnapshot {
    std::string platform;  // HA integration behind the entity ("sonos"), "" if unknown
    uint32_t supportedFeatures = 0;
    std::vector<std::string> sourceList;
    std::vector<std::string> soundModeList;
    std::vector<std::string> supportedColorModes;
    int32_t minColorTempKelvin = 0;
    int32_t maxColorTempKelvin = 0;
    std::vector<std::string> presetModes;  // fan
    int32_t percentageStep = 0;            // fan: HA's percentage_step rounded down, 1..100; 0 = none
    int32_t minHumidity = 0;      // humidifier: whole percent (humidityRange()); both 0 = not reported
    int32_t maxHumidity = 0;
    std::vector<std::string> availableModes;  // humidifier
    std::vector<std::string> options;         // select
    std::vector<SnapshotService> services;
    std::vector<SnapshotCustomService> customServices;  // custom_services

    bool operator==(const CapabilitySnapshot &other) const;
};

// All of a snapshot's custom services' customHintBytes().
size_t customHintBytes(const CapabilitySnapshot &snapshot);
// Keeps, in order, the custom services that fit: each one is kept if it still
// fits in `budget` bytes and kMaxCustomServices, else skipped (a smaller one
// after it may fit). The one budget rule: ServiceDirectory's offer
// (kMaxCustomHintBytes) and registration and re-link (what's left of the
// config's kMaxConfigHintBytes).
void trimCustomServices(std::vector<SnapshotCustomService> *services, size_t budget);
// customHintBytes() summed over the config's HA devices, leaving out the one
// with id `exceptId` ("" = none). The total kMaxConfigHintBytes caps.
size_t configHintBytes(const std::vector<DeviceSpec> &devices, std::string_view exceptId = {});

constexpr size_t kMaxSnapshotServices = 32;
constexpr size_t kMaxSnapshotLabelLength = 48;  // service and field labels (HA's `name`)
constexpr size_t kMaxSnapshotUnitLength = 16;   // number fields' unit_of_measurement
constexpr size_t kMaxSnapshotDescriptionLength = 160;  // field descriptions (bytes)
// Custom services' bounds: services and fields kept in order until either
// count or the hint budget (the strings' bytes, per device) runs out.
constexpr size_t kMaxCustomServices = 16;
constexpr size_t kMaxCustomFields = 8;
constexpr size_t kMaxCustomHintBytes = 2048;  // a webOS TV's three take about 1 KB
constexpr size_t kMaxHintExampleBytes = 64;  // serialized
// All HA devices' hints together (customHintBytes() summed over the config):
// core's config validation refuses more (capacity_exceeded at
// /devices/N/ha_capabilities/custom_services); registration and re-link
// trim a new snapshot's custom services to what's left.
constexpr size_t kMaxConfigHintBytes = 16384;

}  // namespace hubbo::ha
