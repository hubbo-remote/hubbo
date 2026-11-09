#include "hubbo/ha/services.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cmath>

#include "hubbo/domain/limits.h"
#include "hubbo/ha/capabilities.h"

namespace hubbo::ha {

namespace {

constexpr uint8_t kNesting = 16;

std::string bounded(JsonVariantConst v, size_t max) {
    if (!v.is<const char *>()) return {};
    std::string text = v.as<const char *>();
    if (text.size() > max) text.resize(max);
    return text;
}

// Feature masks as validated by HA: a list of ints (one may be a list of
// alternatives). False if something else is there.
bool readFeatures(JsonVariantConst v, std::vector<uint32_t> *out) {
    if (v.isNull()) return true;
    JsonArrayConst list = v.as<JsonArrayConst>();
    if (list.isNull()) {
        if (!v.is<uint32_t>()) return false;
        out->push_back(v.as<uint32_t>());
        return true;
    }
    for (JsonVariantConst item : list) {
        if (!item.is<uint32_t>()) return false;
        out->push_back(item.as<uint32_t>());
    }
    return true;
}

bool featuresMatch(const std::vector<uint32_t> &masks, uint32_t features) {
    if (masks.empty()) return true;
    for (uint32_t mask : masks) {
        if ((features & mask) == mask) return true;
    }
    return false;
}

std::vector<std::string> stringList(JsonVariantConst v) {
    std::vector<std::string> out;
    if (v.is<const char *>()) {
        out.emplace_back(v.as<const char *>());
    } else {
        for (JsonVariantConst item : v.as<JsonArrayConst>()) {
            if (item.is<const char *>()) out.emplace_back(item.as<const char *>());
        }
    }
    return out;
}

ServiceDirectory::EntityFilter readEntityFilter(JsonObjectConst f) {
    ServiceDirectory::EntityFilter filter;
    filter.integration = bounded(f["integration"], 64);
    filter.domains = stringList(f["domain"]);
    filter.unknownFeature = !readFeatures(f["supported_features"], &filter.features);
    return filter;
}

// Integral and within int32.
bool isInt(double v) {
    return std::isfinite(v) && std::floor(v) == v && v >= -2147483648.0 && v <= 2147483647.0;
}

// One field's selector -> typed param. False: unsupported (skips the service).
bool readSelector(JsonObjectConst selector, SnapshotParam *param, size_t *dropped) {
    if (selector.size() != 1) return false;
    const JsonPairConst only = *selector.begin();
    const std::string_view kind = only.key().c_str();
    JsonVariantConst config = only.value();
    if (kind == "boolean") {
        param->type = SnapshotParamType::Bool;
        return true;
    }
    if (kind == "number") {
        JsonVariantConst lo = config["min"];
        JsonVariantConst hi = config["max"];
        if (!lo.is<double>() || !hi.is<double>()) return false;
        param->minimum = lo.as<double>();
        param->maximum = hi.as<double>();
        if (!std::isfinite(param->minimum) || !std::isfinite(param->maximum) || param->minimum > param->maximum) {
            return false;
        }
        JsonVariantConst step = config["step"];
        double s = 1;
        bool any = false;
        if (step.is<double>()) {
            s = step.as<double>();
            if (!(s > 0) || !std::isfinite(s)) return false;
        } else if (step.is<const char *>() && std::string_view(step.as<const char *>()) == "any") {
            any = true;
        } else if (!step.isNull()) {
            return false;
        }
        const bool integral = !any && isInt(s) && isInt(param->minimum) && isInt(param->maximum);
        param->type = integral ? SnapshotParamType::Int : SnapshotParamType::Number;
        param->step = any ? 0 : s;
        param->unit = bounded(config["unit_of_measurement"], kMaxSnapshotUnitLength);
        return true;
    }
    if (kind == "select") {
        if (config["multiple"] | false) return false;
        std::vector<std::string> options;
        for (JsonVariantConst option : config["options"].as<JsonArrayConst>()) {
            if (option.is<const char *>()) {
                options.emplace_back(option.as<const char *>());
            } else if (option["value"].is<const char *>()) {
                options.emplace_back(option["value"].as<const char *>());
            } else {
                return false;
            }
        }
        if (!filterEnumOptions(options, dropped)) return false;
        param->type = SnapshotParamType::Enum;
        param->options = std::move(options);
        return true;
    }
    return false;  // text (no string params yet), entity, object, time, ...
}

void readFields(JsonObjectConst fields, ServiceDirectory::Service *service) {
    for (JsonPairConst pair : fields) {
        JsonObjectConst field = pair.value().as<JsonObjectConst>();
        // A section ({collapsed, fields}) groups fields for the UI: flatten.
        if (field["selector"].isNull() && field["fields"].is<JsonObjectConst>()) {
            readFields(field["fields"].as<JsonObjectConst>(), service);
            continue;
        }
        ServiceDirectory::Field f;
        f.param.name = pair.key().c_str();
        f.param.label = bounded(field["name"], kMaxSnapshotLabelLength);
        f.param.required = field["required"] | false;
        JsonObjectConst filter = field["filter"].as<JsonObjectConst>();
        for (JsonPairConst key : filter) {
            if (std::string_view(key.key().c_str()) != "supported_features") f.unknownFilter = true;
        }
        if (!readFeatures(filter["supported_features"], &f.features)) f.unknownFilter = true;
        f.supported = isHaName(f.param.name) && !isForbiddenParamName(f.param.name) &&
                      readSelector(field["selector"].as<JsonObjectConst>(), &f.param, &service->droppedOptions);
        service->fields.push_back(std::move(f));
    }
}

}  // namespace

ServiceDirectory ServiceDirectory::parse(std::string_view json, const std::vector<std::string> &platforms) {
    ServiceDirectory directory;
    JsonDocument filter;
    for (const auto &platform : platforms) {
        if (isHaName(platform)) filter[platform] = true;
    }
    if (filter.isNull()) return directory;
    JsonDocument doc;
    if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::Filter(filter),
                        DeserializationOption::NestingLimit(kNesting))) {
        return directory;
    }
    for (JsonPairConst domain : doc.as<JsonObjectConst>()) {
        for (JsonPairConst entry : domain.value().as<JsonObjectConst>()) {
            JsonObjectConst description = entry.value().as<JsonObjectConst>();
            Service service;
            service.domain = domain.key().c_str();
            service.service = entry.key().c_str();
            service.nameValid = isHaName(service.domain) && isHaName(service.service);
            service.label = bounded(description["name"], kMaxSnapshotLabelLength);
            JsonVariantConst entity = description["target"]["entity"];
            if (entity.is<JsonObjectConst>()) {
                service.targets.push_back(readEntityFilter(entity.as<JsonObjectConst>()));
            } else {
                for (JsonVariantConst f : entity.as<JsonArrayConst>()) {
                    if (f.is<JsonObjectConst>()) service.targets.push_back(readEntityFilter(f.as<JsonObjectConst>()));
                }
            }
            readFields(description["fields"].as<JsonObjectConst>(), &service);
            directory.services_.push_back(std::move(service));
        }
    }
    std::sort(directory.services_.begin(), directory.services_.end(), [](const Service &a, const Service &b) {
        return a.domain != b.domain ? a.domain < b.domain : a.service < b.service;
    });
    return directory;
}

ServiceDirectory::Offer ServiceDirectory::offerFor(std::string_view entityId, std::string_view platform,
                                                   uint32_t supportedFeatures) const {
    Offer offer;
    const size_t dot = entityId.find('.');
    if (dot == std::string_view::npos || platform.empty()) return offer;
    const std::string_view domain = entityId.substr(0, dot);
    if (platform == domain) return offer;  // the domain's own services are the curated allowlist

    // services_ is sorted by domain: only the platform's own run.
    const auto first = std::lower_bound(services_.begin(), services_.end(), platform,
                                        [](const Service &s, std::string_view p) { return s.domain < p; });
    for (auto it = first; it != services_.end() && it->domain == platform; ++it) {
        const Service &service = *it;
        bool targeted = false;
        for (const EntityFilter &f : service.targets) {
            const bool integration = f.integration.empty() || f.integration == platform;
            const bool inDomain =
                f.domains.empty() || std::find(f.domains.begin(), f.domains.end(), domain) != f.domains.end();
            targeted = targeted ||
                       (integration && inDomain && !f.unknownFeature && featuresMatch(f.features, supportedFeatures));
        }
        if (!targeted) continue;  // not for this entity (or no entity target at all)

        const std::string id = service.domain + "." + service.service;
        SnapshotService offered;
        offered.service = id;
        offered.label = service.label;
        bool usable = service.nameValid;
        for (const Field &field : service.fields) {
            if (field.unknownFilter) {
                usable = false;
                continue;
            }
            if (!featuresMatch(field.features, supportedFeatures)) continue;  // not for this entity
            usable = usable && field.supported;
            offered.fields.push_back(field.param);
        }
        usable = usable && offered.fields.size() <= limits::kMaxParams;
        if (!usable || offer.services.size() >= kMaxSnapshotServices) {
            if (offer.skipped.size() < kMaxSnapshotServices) offer.skipped.push_back(id);
            continue;
        }
        offer.droppedOptions += service.droppedOptions;
        offer.services.push_back(std::move(offered));
    }
    return offer;
}

}  // namespace hubbo::ha
