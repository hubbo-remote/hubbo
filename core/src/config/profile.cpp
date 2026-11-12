#include "hubbo/config/profile.h"

#include <cctype>
#include <cstdio>

#include "hubbo/config/json.h"
#include "hubbo/domain/limits.h"
#include "hubbo/remote/button.h"
#include "profile_fields.h"

namespace hubbo::config {

namespace detail {

namespace {

constexpr const char *kSources[] = {"hubbo", "harmony", "user"};
constexpr const char *kPowerTypes[] = {"discrete", "toggle", "none"};
constexpr int64_t kProfileSchemaVersion = 1;  // the spec's, kinds.json's and buttons.json's
constexpr size_t kMaxProfileJsonBytes = 128 * 1024;  // the validator's hard limit
constexpr uint8_t kProfileNestingLimit = 8;
constexpr size_t kMaxModelLength = 64;
constexpr size_t kSha256HexLength = 64;

bool isCommandName(const std::string &s) {
    if (s.empty() || s.size() > limits::kMaxProfileCommandNameLength) return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool sameIgnoringCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// "<source>:<slug>", slug 1-48 of a-z 0-9 . -
bool isProfileId(const std::string &id, std::string *source) {
    const size_t colon = id.find(':');
    if (colon == std::string::npos || id.size() > limits::kMaxProfileIdLength) return false;
    const std::string slug = id.substr(colon + 1);
    if (slug.empty() || slug.size() > 48) return false;
    for (char c : slug) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    *source = id.substr(0, colon);
    return oneOf(*source, kSources);
}

bool isSha256(const std::string &s) {
    if (s.size() != kSha256HexLength) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// Whole milliseconds in [lo, hi]. True if present and valid.
bool readMs(Fields &f, const char *key, uint32_t lo, uint32_t hi, std::optional<uint32_t> *out) {
    JsonVariantConst v = f.get(key, false);
    if (v.isUnbound()) return false;
    if (!v.is<int64_t>()) return f.failAt(ErrorCode::WrongType, key);
    const int64_t n = v.as<int64_t>();
    if (n < lo || n > hi) return f.failAt(ErrorCode::ParamOutOfRange, key);
    *out = static_cast<uint32_t>(n);
    return true;
}

std::string formatUsage(HidPage page, uint16_t usage) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), page == HidPage::Keyboard ? "0x%02X" : "0x%04X", usage);
    return buf;
}

// Uppercase hex, two digits (keyboard) or four (consumer), within the HID
// report map. WrongType for the format, ParamOutOfRange for the range.
ErrorCode parseUsage(const std::string &text, HidPage page, uint16_t *out) {
    const size_t digits = page == HidPage::Keyboard ? 2 : 4;
    if (text.size() != 2 + digits || text[0] != '0' || text[1] != 'x') return ErrorCode::WrongType;
    uint32_t value = 0;
    for (size_t i = 2; i < text.size(); i++) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            value = value * 16 + static_cast<uint32_t>(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            value = value * 16 + static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return ErrorCode::WrongType;
        }
    }
    const bool keyboard = page == HidPage::Keyboard;
    const uint32_t lo = keyboard ? limits::kHidKeyboardUsageMin : limits::kHidConsumerUsageMin;
    const uint32_t hi = keyboard ? limits::kHidKeyboardUsageMax : limits::kHidConsumerUsageMax;
    if (value < lo || value > hi) return ErrorCode::ParamOutOfRange;
    *out = static_cast<uint16_t>(value);
    return ErrorCode::Ok;
}

bool decodeCommand(JsonVariantConst json, ProfileCommand *out, ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    std::string page;
    if (f.string("page", &page, true)) {
        if (page == "keyboard") {
            out->page = HidPage::Keyboard;
        } else if (page == "consumer") {
            out->page = HidPage::Consumer;
        } else {
            f.failAt(ErrorCode::WrongType, "page");
            page.clear();
        }
    }
    std::string usage;
    if (f.string("usage", &usage, true) && !page.empty()) {
        const ErrorCode code = parseUsage(usage, out->page, &out->usage);
        if (code != ErrorCode::Ok) f.failAt(code, "usage");
    }
    readMs(f, "hold_ms", 1, limits::kMaxHoldMs, &out->holdMs);
    if (f.string("label", &out->label, false, limits::kMaxProfileLabelLength) && out->label.empty()) {
        f.failAt(ErrorCode::TooLong, "label");
    }
    return f.finish();
}

bool decodeCommands(JsonVariantConst json, std::vector<ProfileCommand> *out, ValidationReport *report,
                    const std::string &path) {
    const auto object = objectWithin(json, limits::kMaxProfileCommands, report, path);
    if (!object) return false;
    if (object->size() == 0) {
        addIssue(report, ErrorCode::Required, path);
        return false;
    }
    bool ok = true;
    out->clear();
    for (JsonPairConst kv : *object) {
        ProfileCommand command;
        command.name = kv.key().c_str();
        const std::string at = child(path, command.name);
        if (!isCommandName(command.name)) {
            addIssue(report, ErrorCode::InvalidCommandId, at);
            ok = false;
        }
        for (const ProfileCommand &other : *out) {
            if (sameIgnoringCase(other.name, command.name)) {
                addIssue(report, ErrorCode::DuplicateId, at);
                ok = false;
            }
        }
        ok = decodeCommand(kv.value(), &command, report, at) && ok;
        out->push_back(std::move(command));
    }
    return ok;
}

bool decodeCommandRef(Fields &f, const char *key, bool required, const DeviceProfile &profile,
                      std::string *out) {
    if (!f.string(key, out, required, limits::kMaxProfileCommandNameLength)) return false;
    if (profile.findCommand(*out) == nullptr) return f.failAt(ErrorCode::UnknownCommand, key);
    return true;
}

bool decodeStep(JsonVariantConst json, ProfileStep *out, const DeviceProfile &profile,
                ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    const bool command = !f.get("command", false).isUnbound();
    const bool delay = !f.get("delay_ms", false).isUnbound();
    if (command == delay) {
        f.fail(command ? ErrorCode::UnknownField : ErrorCode::Required, f.at(command ? "delay_ms" : "command"));
    }
    if (command) {
        decodeCommandRef(f, "command", true, profile, &out->command);
        readMs(f, "hold_ms", 1, limits::kMaxHoldMs, &out->holdMs);
    } else {
        std::optional<uint32_t> ms;
        if (readMs(f, "delay_ms", 1, limits::kMaxStepDelayMs, &ms)) out->delayMs = *ms;
    }
    return f.finish();
}

bool decodeSteps(JsonVariantConst json, std::vector<ProfileStep> *out, const DeviceProfile &profile,
                 ValidationReport *report, const std::string &path) {
    if (json.is<JsonArrayConst>() && json.as<JsonArrayConst>().size() == 0) {
        addIssue(report, ErrorCode::Required, path);
        return false;
    }
    return decodeList(json, limits::kMaxProfileSteps, out, report, path,
                      [&](JsonVariantConst j, ProfileStep *s, const std::string &p) {
                          return decodeStep(j, s, profile, report, p);
                      });
}

bool decodeButtons(JsonVariantConst json, std::vector<ProfileButton> *out, const DeviceProfile &profile,
                   ValidationReport *report, const std::string &path) {
    const auto object = objectWithin(json, remote::kButtonCount, report, path);
    if (!object) return false;
    bool ok = true;
    out->clear();
    for (JsonPairConst kv : *object) {
        ProfileButton button;
        button.button = kv.key().c_str();
        const std::string at = child(path, button.button);
        remote::Button parsed;
        if (!remote::buttonFromName(button.button.c_str(), &parsed)) {
            addIssue(report, ErrorCode::InvalidTrigger, at);
            ok = false;
        }
        if (!kv.value().isNull()) {
            if (!kv.value().is<const char *>()) {
                addIssue(report, ErrorCode::WrongType, at);
                ok = false;
            } else {
                button.command = kv.value().as<const char *>();
                if (profile.findCommand(*button.command) == nullptr) {
                    addIssue(report, ErrorCode::UnknownCommand, at);
                    ok = false;
                }
            }
        }
        out->push_back(std::move(button));
    }
    return ok;
}

bool decodePower(JsonVariantConst json, ProfilePower *out, const DeviceProfile &profile,
                 ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    if (f.string("type", &out->type, true) && !oneOf(out->type, kPowerTypes)) f.failAt(ErrorCode::WrongType, "type");
    const bool discrete = out->type == "discrete";
    const bool toggle = out->type == "toggle";
    const auto steps = [&](const char *key, bool wanted, std::vector<ProfileStep> *list) {
        // Required when wanted, else an unknown field (not part of this type).
        if (wanted) {
            f.nested(key, true, [&](JsonVariantConst v, const std::string &p) {
                return decodeSteps(v, list, profile, report, p);
            });
        }
    };
    steps("on", discrete, &out->on);
    steps("off", discrete, &out->off);
    steps("toggle", toggle, &out->toggle);
    readMs(f, "on_delay_ms", 0, limits::kMaxStepDelayMs, &out->onDelayMs);
    return f.finish();
}

bool decodeTiming(JsonVariantConst json, ProfileTiming *out, ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    readMs(f, "key_delay_ms", 0, limits::kMaxKeyDelayMs, &out->keyDelayMs);
    readMs(f, "input_delay_ms", 0, limits::kMaxStepDelayMs, &out->inputDelayMs);
    return f.finish();  // ir_repeats: IR only (unknown_field here)
}

bool decodeInputs(JsonVariantConst json, std::vector<ProfileInput> *out, const DeviceProfile &profile,
                  ValidationReport *report, const std::string &path) {
    const bool ok = decodeList(json, limits::kMaxProfileInputs, out, report, path,
                               [&](JsonVariantConst j, ProfileInput *input, const std::string &p) {
                                   Fields f(j, report, p);
                                   if (f.string("name", &input->name, true, limits::kMaxProfileLabelLength) &&
                                       input->name.empty()) {
                                       f.failAt(ErrorCode::TooLong, "name");
                                   }
                                   f.nested("steps", true, [&](JsonVariantConst v, const std::string &sp) {
                                       return decodeSteps(v, &input->steps, profile, report, sp);
                                   });
                                   return f.finish();
                               });
    if (!ok) return false;
    for (size_t i = 0; i < out->size(); i++) {
        for (size_t j = 0; j < i; j++) {
            if ((*out)[i].name == (*out)[j].name) {
                addIssue(report, ErrorCode::DuplicateId, child(child(path, i), "name"));
                return false;
            }
        }
    }
    return true;
}

bool decodeChannels(JsonVariantConst json, ProfileChannels *out, const DeviceProfile &profile,
                    ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    std::optional<uint32_t> digits;
    if (readMs(f, "digits", 1, 4, &digits)) out->digits = static_cast<uint8_t>(*digits);
    decodeCommandRef(f, "start", false, profile, &out->start);
    decodeCommandRef(f, "finish", false, profile, &out->finish);
    bool ok = f.finish();
    for (char d = '0'; d <= '9'; d++) {
        if (profile.findCommand(std::string(1, d)) == nullptr) {
            addIssue(report, ErrorCode::UnknownCommand, child(path, std::string(1, d)));
            ok = false;
        }
    }
    return ok;
}

void encodeSteps(const std::vector<ProfileStep> &steps, JsonArray out) {
    for (const ProfileStep &step : steps) {
        JsonObject s = out.add<JsonObject>();
        if (step.command.empty()) {
            s["delay_ms"] = step.delayMs;
        } else {
            s["command"] = step.command;
            if (step.holdMs) s["hold_ms"] = *step.holdMs;
        }
    }
}

}  // namespace

bool decodeProfileFields(Fields &f, DeviceProfile *out, ValidationReport *report) {
    const auto text = [&](const char *key, std::string *s) {
        if (f.string(key, s, true, kMaxModelLength) && s->empty()) f.failAt(ErrorCode::Required, key);
    };
    text("manufacturer", &out->manufacturer);
    text("model", &out->model);
    // Commands first: everything else refers to them.
    f.nested("commands", true, [&](JsonVariantConst v, const std::string &p) {
        return decodeCommands(v, &out->commands, report, p);
    });
    const DeviceProfile &profile = *out;  // its commands, decoded first
    f.nested("buttons", false, [&](JsonVariantConst v, const std::string &p) {
        return decodeButtons(v, &out->buttons, profile, report, p);
    });
    f.nested("power", false, [&](JsonVariantConst v, const std::string &p) {
        out->power.emplace();
        return decodePower(v, &*out->power, profile, report, p);
    });
    f.nested("timing", false, [&](JsonVariantConst v, const std::string &p) {
        out->timing.emplace();
        return decodeTiming(v, &*out->timing, report, p);
    });
    f.nested("inputs", false, [&](JsonVariantConst v, const std::string &p) {
        return decodeInputs(v, &out->inputs, profile, report, p);
    });
    f.nested("channels", false, [&](JsonVariantConst v, const std::string &p) {
        out->channels.emplace();
        return decodeChannels(v, &*out->channels, profile, report, p);
    });
    return true;  // failures are the caller's f.finish()
}

void encodeProfileFields(const DeviceProfile &profile, JsonObject out, bool withCommands) {
    out["manufacturer"] = profile.manufacturer;
    out["model"] = profile.model;
    if (!withCommands) return;
    JsonObject commands = out["commands"].to<JsonObject>();
    for (const ProfileCommand &c : profile.commands) {
        JsonObject o = commands[c.name].to<JsonObject>();
        o["page"] = c.page == HidPage::Keyboard ? "keyboard" : "consumer";
        o["usage"] = formatUsage(c.page, c.usage);
        if (c.holdMs) o["hold_ms"] = *c.holdMs;
        if (!c.label.empty()) o["label"] = c.label;
    }
    if (!profile.buttons.empty()) {
        JsonObject buttons = out["buttons"].to<JsonObject>();
        for (const ProfileButton &b : profile.buttons) {
            if (b.command) {
                buttons[b.button] = *b.command;
            } else {
                buttons[b.button] = nullptr;
            }
        }
    }
    if (profile.power) {
        const ProfilePower &p = *profile.power;
        JsonObject power = out["power"].to<JsonObject>();
        power["type"] = p.type;
        if (!p.on.empty()) encodeSteps(p.on, power["on"].to<JsonArray>());
        if (!p.off.empty()) encodeSteps(p.off, power["off"].to<JsonArray>());
        if (!p.toggle.empty()) encodeSteps(p.toggle, power["toggle"].to<JsonArray>());
        if (p.onDelayMs) power["on_delay_ms"] = *p.onDelayMs;
    }
    if (profile.timing) {
        JsonObject timing = out["timing"].to<JsonObject>();
        if (profile.timing->keyDelayMs) timing["key_delay_ms"] = *profile.timing->keyDelayMs;
        if (profile.timing->inputDelayMs) timing["input_delay_ms"] = *profile.timing->inputDelayMs;
    }
    if (!profile.inputs.empty()) {
        JsonArray inputs = out["inputs"].to<JsonArray>();
        for (const ProfileInput &i : profile.inputs) {
            JsonObject input = inputs.add<JsonObject>();
            input["name"] = i.name;
            encodeSteps(i.steps, input["steps"].to<JsonArray>());
        }
    }
    if (profile.channels) {
        JsonObject channels = out["channels"].to<JsonObject>();
        if (profile.channels->digits) channels["digits"] = *profile.channels->digits;
        if (!profile.channels->start.empty()) channels["start"] = profile.channels->start;
        if (!profile.channels->finish.empty()) channels["finish"] = profile.channels->finish;
    }
}

bool decodeProfileRef(JsonVariantConst json, DeviceProfile *out, ValidationReport *report, const std::string &path) {
    Fields f(json, report, path);
    std::string source;
    if (f.string("id", &out->id, true, limits::kMaxProfileIdLength) && !isProfileId(out->id, &source)) {
        f.failAt(ErrorCode::InvalidId, "id");
    }
    f.revision("revision", &out->revision, true);
    if (f.string("sha256", &out->sha256, true, kSha256HexLength) && !isSha256(out->sha256)) {
        f.failAt(ErrorCode::WrongType, "sha256");
    }
    return f.finish();
}

void encodeProfileRef(const DeviceProfile &profile, JsonObject out) {
    out["id"] = profile.id;
    out["revision"] = profile.revision;
    out["sha256"] = profile.sha256;
}

}  // namespace detail

using namespace detail;

ErrorCode parseProfile(std::string_view json, ProfileDocument *out, ValidationReport *report) {
    JsonDocument doc;
    if (parseJson(json, doc, kMaxProfileJsonBytes, kProfileNestingLimit) != ErrorCode::Ok) {
        return ErrorCode::MalformedJson;
    }
    ValidationReport local;
    ValidationReport *r = report != nullptr ? report : &local;
    const size_t before = r->issues().size() + r->dropped();
    *out = {};
    Fields f(doc.as<JsonVariantConst>(), r, "");
    f.schemaVersion(true, kProfileSchemaVersion);
    std::string idSource;
    if (f.string("id", &out->profile.id, true, limits::kMaxProfileIdLength) &&
        !isProfileId(out->profile.id, &idSource)) {
        f.failAt(ErrorCode::InvalidId, "id");
    }
    f.revision("revision", &out->profile.revision, true);
    if (f.string("source", &out->source, true) && out->source != idSource) f.failAt(ErrorCode::InvalidId, "source");
    if (f.string("kind", &out->kind, true, limits::kMaxIdLength) && !isValidId(out->kind)) {
        f.failAt(ErrorCode::InvalidId, "kind");
    }
    if (f.string("platform", &out->platform, true) && out->platform != kBleHidType) {
        f.failAt(out->platform == "ir" ? ErrorCode::UnsupportedSchema : ErrorCode::WrongType, "platform");
    }
    decodeProfileFields(f, &out->profile, r);
    f.get("meta", false);  // notes for people: dropped
    f.finish();
    return r->issues().size() + r->dropped() == before ? ErrorCode::Ok : ErrorCode::ValidationFailed;
}

// ---- the library ----------------------------------------------------------

namespace {

const ProfileFile *findFile(const ProfileFile *files, size_t count, std::string_view name) {
    for (size_t i = 0; i < count; i++) {
        if (files[i].name != nullptr && name == files[i].name) return &files[i];
    }
    return nullptr;
}

bool containsKind(const std::vector<DeviceKind> &kinds, std::string_view kind) {
    for (const DeviceKind &k : kinds) {
        if (k.id == kind) return true;
    }
    return false;
}

// Issues from one file, re-rooted at "/<file>".
void addFrom(ValidationReport *to, const std::string &file, const ValidationReport &from) {
    for (const ValidationIssue &issue : from.issues()) to->add(issue.code, "/" + jsonPointerToken(file) + issue.path);
}

}  // namespace

ErrorCode ProfileLibrary::load(const ProfileFile *files, size_t count, ValidationReport *report) {
    ValidationReport local;
    ValidationReport &r = report != nullptr ? *report : local;
    const size_t before = r.issues().size() + r.dropped();
    std::vector<BuiltinProfile> profiles;
    std::vector<DeviceKind> kinds;
    std::string_view buttons;

    // kinds.json: {"schema_version": 1, "kinds": [{id, label, harmony}]}
    const ProfileFile *kindsFile = findFile(files, count, "kinds.json");
    JsonDocument doc;
    if (kindsFile == nullptr) {
        r.add(ErrorCode::Required, "/kinds.json");
    } else if (parseJson(kindsFile->text, doc, kMaxProfileJsonBytes, kProfileNestingLimit) != ErrorCode::Ok) {
        r.add(ErrorCode::MalformedJson, "/kinds.json");
    } else {
        ValidationReport sub;
        Fields f(doc.as<JsonVariantConst>(), &sub, "");
        f.schemaVersion(true, kProfileSchemaVersion);
        f.nested("kinds", true, [&](JsonVariantConst v, const std::string &p) {
            return decodeList(v, 128, &kinds, &sub, p, [&](JsonVariantConst j, DeviceKind *k, const std::string &kp) {
                Fields kf(j, &sub, kp);
                if (kf.string("id", &k->id, true, limits::kMaxIdLength) && !isValidId(k->id)) {
                    kf.failAt(ErrorCode::InvalidId, "id");
                }
                kf.string("label", &k->label, true, limits::kMaxNameLength);
                kf.get("harmony", false);  // the converter's mapping
                return kf.finish();
            });
        });
        f.finish();
        addFrom(&r, "kinds.json", sub);
    }

    // buttons.json: for the web admin, served as published.
    if (const ProfileFile *b = findFile(files, count, "buttons.json")) {
        buttons = b->text;
    } else {
        r.add(ErrorCode::Required, "/buttons.json");
    }

    // index.json: [{file, id, revision, sha256}], in list order.
    const ProfileFile *index = findFile(files, count, "index.json");
    doc.clear();
    if (index == nullptr) {
        r.add(ErrorCode::Required, "/index.json");
    } else if (parseJson(index->text, doc, kMaxProfileJsonBytes, kProfileNestingLimit) != ErrorCode::Ok ||
               !doc.is<JsonArrayConst>()) {
        r.add(ErrorCode::MalformedJson, "/index.json");
    } else {
        size_t i = 0;
        for (JsonVariantConst entry : doc.as<JsonArrayConst>()) {
            const std::string at = "/index.json/" + std::to_string(i++);
            Fields f(entry, &r, at);
            std::string file;
            DeviceProfile ref;
            f.string("file", &file, true);
            f.string("id", &ref.id, true, limits::kMaxProfileIdLength);
            f.revision("revision", &ref.revision, true);
            if (f.string("sha256", &ref.sha256, true) && !isSha256(ref.sha256)) f.failAt(ErrorCode::WrongType, "sha256");
            if (!f.finish()) continue;
            const ProfileFile *pf = findFile(files, count, file);
            if (pf == nullptr) {
                r.add(ErrorCode::NotFound, at + "/file");
                continue;
            }
            BuiltinProfile builtin;
            builtin.json = pf->text;
            ValidationReport parsed;
            const ErrorCode code = parseProfile(pf->text, &builtin.document, &parsed);
            if (code == ErrorCode::MalformedJson) r.add(code, "/" + jsonPointerToken(file));
            addFrom(&r, file, parsed);
            if (code != ErrorCode::Ok) continue;
            DeviceProfile &p = builtin.document.profile;
            if (p.id != ref.id) r.add(ErrorCode::InvalidId, at + "/id");
            if (p.revision != ref.revision) r.add(ErrorCode::RevisionConflict, at + "/revision");
            for (const BuiltinProfile &other : profiles) {
                if (other.document.profile.id == p.id) r.add(ErrorCode::DuplicateId, at + "/id");
            }
            p.sha256 = ref.sha256;
            profiles.push_back(std::move(builtin));
        }
    }

    // Every profile's kind is a kind.
    for (const BuiltinProfile &p : profiles) {
        if (!containsKind(kinds, p.document.kind)) {
            r.add(ErrorCode::UnknownKind, "/" + jsonPointerToken(p.document.profile.id) + "/kind");
        }
    }

    if (r.issues().size() + r.dropped() != before) {
        *this = ProfileLibrary();
        return ErrorCode::ValidationFailed;
    }
    profiles_ = std::move(profiles);
    kinds_ = std::move(kinds);
    buttons_ = buttons;
    return ErrorCode::Ok;
}

const BuiltinProfile *ProfileLibrary::find(std::string_view profileId) const {
    for (const BuiltinProfile &p : profiles_) {
        if (p.document.profile.id == profileId) return &p;
    }
    return nullptr;
}

bool ProfileLibrary::hasKind(std::string_view kind) const { return containsKind(kinds_, kind); }

namespace {

struct Builtin {
    ProfileLibrary library;
    ErrorCode status;
    Builtin() { status = library.load(kBuiltinProfileFiles, kBuiltinProfileFileCount); }
};

const Builtin &builtin() {
    static const Builtin instance;  // thread-safe initialisation
    return instance;
}

}  // namespace

const ProfileLibrary &builtinProfiles() { return builtin().library; }
ErrorCode builtinProfilesStatus() { return builtin().status; }

DeviceSpec deviceFromProfile(const BuiltinProfile &profile, std::string name, std::string kind) {
    DeviceSpec device;
    device.name = name.empty() ? profile.document.profile.model : std::move(name);
    device.kind = kind.empty() ? profile.document.kind : std::move(kind);
    device.provider.type = profile.document.platform;
    if (profile.document.platform == kBleHidType) device.integrationId = kBleIntegrationId;
    device.profile = profile.document.profile;
    return device;
}

}  // namespace hubbo::config
