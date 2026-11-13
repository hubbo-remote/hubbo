#pragma once

// The showcase: a real Home Assistant's devices, for looking over the web
// admin on hub-sim (`hub-sim serve --showcase`). sim/fixtures/local/
// real-ha-sample.json holds states, platforms and get_services captured from
// a real HA, and real-ha-translations.json its service translations: local
// only (gitignored), never committed. The fake HA serves them as captured
// (FakeHaServer::enableShowcase), and showcaseConfig() registers each entity
// the way the hub does: entity details fetched by the real HA client, then
// normalized into the snapshot registration stores. What the normalizer
// skips stays skipped. Host-only.

#include <string>
#include <utility>
#include <vector>

#include "fake_ha.h"
#include "hubbo/domain/config.h"

namespace hubbo::sim {

// The first missing capture file, or "" when both are there.
std::string showcaseMissing();

inline constexpr const char *kShowcaseActivityId = "act_watch_tv";

// The captured entities, as the fake HA serves them. A missing or broken
// sample stops the process with a message.
const std::vector<FakeHaEntity> &showcaseHaEntities();

// The captured services in get_services' shape (domain -> service ->
// description), converted from /api/services' list.
const std::string &showcaseServicesJson();

// sim/fixtures/local/real-ha-translations.json: frontend/get_translations'
// resources (category services, English) captured from the same HA, for
// the sample's integrations: "component.<platform>.services.<...>" -> text.
const std::vector<std::pair<std::string, std::string>> &realHaTranslations();

// The fixture's Bluetooth Apple TV, a device per captured entity of an
// allowlisted domain plus the fake's made-up entity for each domain the
// capture lacks (snapshots included, kinds from ha::suggestedKind), the Fire TV Stick and SHIELD as Bluetooth
// devices, the light buttons on the first captured light, and one activity
// on the first captured player with sources.
ConfigSpec showcaseConfig();

}  // namespace hubbo::sim
