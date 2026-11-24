#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/config/json.h"
#include "hubbo/domain/config.h"
#include "hubbo/engine/compiler.h"

// Device profiles (~/src/hubbo-devices/docs/spec.md) on the hub: parsing a
// profile document, the built-in profiles and kinds, and adding a device
// from a profile. docs/design/device-profiles.md.
//
// The hub reads the spec's schema_version 1 and nothing else: unknown fields
// are errors (`meta` is accepted and dropped). It drives `ble_hid` and `ir`
// profiles. A built-in's sha256 comes from profiles/index.json, written by
// the sync script; an imported one's from profileSha256().
namespace hubbo::config {

// A profile document as published (identity, kind and platform, plus the
// part a device carries).
struct ProfileDocument {
    std::string source;    // "hubbo", "harmony" or "user"
    std::string kind;      // a kinds.json id
    std::string platform;  // "ble_hid" or "ir"
    DeviceProfile profile; // id, revision and the device fields; sha256 ""
};

// Ok, MalformedJson (not JSON, too large or too deep) or ValidationFailed
// (`report` has the issues, JSON Pointers into the document).
ErrorCode parseProfile(std::string_view json, ProfileDocument *out, ValidationReport *report);
// The same over a document already parsed (an API body): ValidationFailed or Ok.
ErrorCode parseProfile(JsonVariantConst json, ProfileDocument *out, ValidationReport *report);

// The spec's profile hash ("Profile hash"): SHA-256 of the document's
// canonical form (keys sorted by code point, no whitespace, strings as
// Python's json.dumps(ensure_ascii=False) writes them), in lowercase hex.
// Over the whole document as given, `meta` included. False (out untouched)
// if it isn't JSON or holds a number that isn't an integer.
bool profileSha256(std::string_view json, std::string *out);
bool profileSha256(JsonVariantConst json, std::string *out);  // a document already parsed

// A kind from kinds.json: {"id": "tv", "label": "TV"}.
struct DeviceKind {
    std::string id;
    std::string label;
};

// One embedded file (generated from profiles/ by core/tools/embed_profiles.py).
struct ProfileFile {
    const char *name;  // "apple-tv.json", "index.json", "kinds.json", "buttons.json"
    const char *text;
};
// Defined by that generated source, which every build compiles.
extern const ProfileFile kBuiltinProfileFiles[];
extern const size_t kBuiltinProfileFileCount;

struct BuiltinProfile {
    ProfileDocument document;  // document.profile.sha256 set from index.json
    const char *json = nullptr;  // the file as published, in its ProfileFile (never copied)
};

// The built-in profiles, kinds and button table.
class ProfileLibrary {
public:
    // Loads index.json (the profiles, in order), each profile it lists,
    // kinds.json and buttons.json. ValidationFailed with issues at
    // "/<file name>/<pointer>" if any file is missing or invalid, a profile
    // doesn't match its index entry, or a profile's kind isn't a kind; the
    // library is then empty. Ok otherwise. The library points into `files`
    // (the raw texts), so they must outlive it; the embedded ones do.
    ErrorCode load(const ProfileFile *files, size_t count, ValidationReport *report = nullptr);

    const std::vector<BuiltinProfile> &profiles() const { return profiles_; }
    const BuiltinProfile *find(std::string_view profileId) const;  // nullptr: unknown
    const std::vector<DeviceKind> &kinds() const { return kinds_; }
    bool hasKind(std::string_view kind) const;
    std::string_view buttons() const { return buttons_; }  // buttons.json, as published

private:
    std::vector<BuiltinProfile> profiles_;
    std::vector<DeviceKind> kinds_;
    std::string_view buttons_;
};

// The profiles embedded in this build, loaded on first use (thread-safe).
// Empty (and `builtinProfilesStatus()` not Ok) if the embedded set is
// invalid, which a core test catches before it ships.
const ProfileLibrary &builtinProfiles();
ErrorCode builtinProfilesStatus();

// A new device from a profile, ready for ConfigService::createDevice: no id
// or revision yet; `name` defaults to the model and `kind` to the profile's.
// The integration follows the platform (ble_hid: the built-in "ble"; ir:
// "ir"). An imported document's profile.sha256 is the caller's to set
// (profileSha256()). The document is moved in (an IR profile's codes are
// shared, but its commands aren't).
DeviceSpec deviceFromProfile(ProfileDocument &&document, std::string name = "", std::string kind = "");
DeviceSpec deviceFromProfile(const ProfileDocument &document, std::string name = "", std::string kind = "");
DeviceSpec deviceFromProfile(const BuiltinProfile &profile, std::string name = "", std::string kind = "");

}  // namespace hubbo::config
