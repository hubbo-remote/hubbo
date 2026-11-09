#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/ha/snapshot.h"

// Integration-specific Home Assistant services (docs/ha-capabilities.md,
// "Integration services"; decided 2026-11-08).
//
// get_services lists every service HA has. For an entity provided by
// integration P (its registry `platform`, e.g. "sonos"), a service P.S is
// offered when:
//   - its `target` has an `entity` filter that matches the entity: the
//     filter's integration (if any) is P, its domain list (if any) has the
//     entity's domain, and its supported_features (if any) has an entry the
//     entity fully supports. Services without an entity target (domain-wide,
//     device-only) are never offered: the call's target is always exactly
//     the registered entity;
//   - every field it accepts (after dropping fields whose own feature filter
//     the entity doesn't meet) maps to a typed param:
//       boolean -> bool; number with min and max -> int (integer step and
//       bounds) or number; select (single, string options) -> enum;
//     anything else (text, entity, object, time, multiple selects, ...)
//     skips the whole service, as do more than limits::kMaxParams fields,
//     a field name that isn't a valid param name or looks like a target
//     (isForbiddenParamName), and a service or platform name that isn't
//     ^[a-z][a-z0-9_]{0,31}$. Only P's own services are considered, never the
//     entity domain's (those are the curated allowlist), and never when P is
//     the entity's domain.
//
// Runs on the integration/config task at discovery and registration, never
// on the RF path.
namespace hubbo::ha {

// get_services, reduced to the platforms of interest and parsed once per
// discovery.
class ServiceDirectory {
public:
    // `json`: get_services' result (domain -> service -> description).
    // `platforms`: the integrations whose services to keep (others are
    // skipped while parsing, to bound memory). Malformed JSON gives an empty
    // directory.
    static ServiceDirectory parse(std::string_view json, const std::vector<std::string> &platforms);

    struct Offer {
        std::vector<SnapshotService> services;  // offered, sorted by id
        std::vector<std::string> skipped;       // "<platform>.<service>" matching the entity but not offered
        size_t droppedOptions = 0;              // select options left out (too long or too many)
    };
    // The services offered to `entityId` (whose domain is its prefix) from
    // `platform` with `supportedFeatures`. At most kMaxSnapshotServices.
    Offer offerFor(std::string_view entityId, std::string_view platform, uint32_t supportedFeatures) const;

    size_t size() const { return services_.size(); }

    struct EntityFilter {
        std::string integration;           // "" = any
        std::vector<std::string> domains;  // empty = any
        std::vector<uint32_t> features;    // empty = any; else one entry fully supported
        bool unknownFeature = false;       // a feature we couldn't read: never matches
    };
    struct Field {
        SnapshotParam param;
        bool supported = false;
        std::vector<uint32_t> features;  // the field's own filter; empty = always
        bool unknownFilter = false;      // a filter we can't evaluate: unsupported
    };
    struct Service {
        std::string domain;   // == platform
        std::string service;
        std::string label;
        bool nameValid = false;
        std::vector<EntityFilter> targets;  // empty: no entity target
        std::vector<Field> fields;
        size_t droppedOptions = 0;
    };

private:
    std::vector<Service> services_;
};

struct EntityInfo;  // capabilities.h
// The snapshot to store for a newly registered (or re-linked) entity: its
// attributes plus the integration services offered to it. `offered`, if
// given, gets the offer's skipped services and dropped options (its
// `services` are moved into the snapshot).
CapabilitySnapshot makeSnapshot(const EntityInfo &entity, const std::string &platform,
                                const ServiceDirectory &services, ServiceDirectory::Offer *offered = nullptr);

}  // namespace hubbo::ha
