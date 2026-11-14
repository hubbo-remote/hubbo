#include "hubbo/ha/snapshot.h"

#include "hubbo/domain/config.h"
#include "hubbo/ha/capabilities.h"
#include "hubbo/ha/services.h"

namespace hubbo::ha {

bool SnapshotParam::operator==(const SnapshotParam &o) const {
    return name == o.name && label == o.label && type == o.type && required == o.required && minimum == o.minimum &&
           maximum == o.maximum && step == o.step && unit == o.unit && description == o.description && options == o.options &&
           hasDefault == o.hasDefault && defaultNumber == o.defaultNumber && defaultOption == o.defaultOption;
}

bool SnapshotService::operator==(const SnapshotService &o) const {
    return service == o.service && label == o.label && fields == o.fields;
}

bool SnapshotHint::operator==(const SnapshotHint &o) const {
    return name == o.name && required == o.required && example == o.example && description == o.description;
}

bool SnapshotCustomService::operator==(const SnapshotCustomService &o) const {
    return service == o.service && label == o.label && fields == o.fields;
}

size_t customHintBytes(const SnapshotCustomService &s) {
    size_t n = s.service.size() + s.label.size();
    for (const SnapshotHint &h : s.fields) n += h.name.size() + h.example.size() + h.description.size();
    return n;
}

size_t customHintBytes(const CapabilitySnapshot &snapshot) {
    size_t n = 0;
    for (const SnapshotCustomService &s : snapshot.customServices) n += customHintBytes(s);
    return n;
}

void trimCustomServices(std::vector<SnapshotCustomService> *services, size_t budget) {
    size_t used = 0;
    std::vector<SnapshotCustomService> kept;
    for (SnapshotCustomService &s : *services) {
        const size_t bytes = customHintBytes(s);
        if (kept.size() >= kMaxCustomServices || used + bytes > budget) continue;
        used += bytes;
        kept.push_back(std::move(s));
    }
    *services = std::move(kept);
}

bool CapabilitySnapshot::operator==(const CapabilitySnapshot &o) const {
    return platform == o.platform && supportedFeatures == o.supportedFeatures && sourceList == o.sourceList &&
           soundModeList == o.soundModeList && supportedColorModes == o.supportedColorModes &&
           minColorTempKelvin == o.minColorTempKelvin && maxColorTempKelvin == o.maxColorTempKelvin &&
           presetModes == o.presetModes && percentageStep == o.percentageStep && minHumidity == o.minHumidity &&
           maxHumidity == o.maxHumidity && availableModes == o.availableModes && options == o.options &&
           services == o.services && customServices == o.customServices;
}

namespace {

constexpr int32_t kMaxKelvin = 100000;  // snapshot_json.cpp's bound

int32_t kelvin(int32_t value) { return value > 0 && value <= kMaxKelvin ? value : 0; }

}  // namespace

// Normalized the way decodeSnapshot() checks a stored one, so whatever HA
// reports, the snapshot registration stores always reloads: option lists
// through filterEnumOptions (1-64 chars, no duplicates, at most 128), the
// platform through isHaName ("" otherwise: no integration services), kelvin
// within 0..100000, a fan's percentage_step through fanSpeedStep().
CapabilitySnapshot makeSnapshot(const EntityInfo &entity, const std::string &platform,
                                const ServiceDirectory &services, ServiceDirectory::Offer *offered) {
    CapabilitySnapshot s;
    if (isHaName(platform)) s.platform = platform;
    s.supportedFeatures = entity.supportedFeatures;
    size_t dropped = 0;
    s.sourceList = entity.sourceList;
    s.soundModeList = entity.soundModeList;
    s.supportedColorModes = entity.supportedColorModes;
    filterEnumOptions(s.sourceList, &dropped);
    filterEnumOptions(s.soundModeList, &dropped);
    filterEnumOptions(s.supportedColorModes, &dropped);
    s.presetModes = entity.presetModes;
    filterEnumOptions(s.presetModes, &dropped);
    s.percentageStep = fanSpeedStep(entity.percentageStep);
    s.availableModes = entity.availableModes;
    s.options = entity.options;
    filterEnumOptions(s.availableModes, &dropped);
    filterEnumOptions(s.options, &dropped);
    humidityRange(entity.minHumidity, entity.maxHumidity, &s.minHumidity, &s.maxHumidity);
    s.minColorTempKelvin = kelvin(entity.minColorTempKelvin);
    s.maxColorTempKelvin = kelvin(entity.maxColorTempKelvin);
    ServiceDirectory::Offer offer = services.offerFor(entity.entityId, s.platform, entity.supportedFeatures);
    s.services = std::move(offer.services);
    s.customServices = std::move(offer.custom);
    if (offered != nullptr) {
        offered->skipped = std::move(offer.skipped);
        offered->droppedOptions = offer.droppedOptions + dropped;
    }
    return s;
}

size_t configHintBytes(const std::vector<DeviceSpec> &devices, std::string_view exceptId) {
    size_t total = 0;
    for (const DeviceSpec &device : devices) {
        if (device.haCapabilities && device.id != exceptId) total += customHintBytes(*device.haCapabilities);
    }
    return total;
}

}  // namespace hubbo::ha
