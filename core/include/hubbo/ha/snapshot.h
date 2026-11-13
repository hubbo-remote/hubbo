#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

    bool operator==(const CapabilitySnapshot &other) const;
};

constexpr size_t kMaxSnapshotServices = 32;
constexpr size_t kMaxSnapshotLabelLength = 48;  // service and field labels (HA's `name`)
constexpr size_t kMaxSnapshotUnitLength = 16;   // number fields' unit_of_measurement
constexpr size_t kMaxSnapshotDescriptionLength = 160;  // field descriptions (bytes)

}  // namespace hubbo::ha
