#include "scenario.h"

#include <istream>
#include <ostream>
#include <sstream>
#include <vector>

#include "fixtures.h"

namespace hubbo::sim {

namespace {

std::string trim(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

// Collapses runs of whitespace (outside quotes) to one space.
std::string normalize(const std::string &s) {
    std::string out;
    bool quoted = false;
    bool space = false;
    for (char c : trim(s)) {
        if (c == '"') quoted = !quoted;
        if (!quoted && (c == ' ' || c == '\t')) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

// A command line matches if equal, or equal up to " -> " when the
// expectation leaves the status out.
bool matchesCommand(const std::string &expected, const std::string &actual) {
    const std::string e = normalize(expected);
    const std::string a = normalize(actual);
    if (e == a) return true;
    if (e.find(" -> ") != std::string::npos) return false;
    const size_t arrow = a.find(" -> ");
    return arrow != std::string::npos && a.substr(0, arrow) == e;
}

bool parseUint(const std::string &s, uint32_t *out) {
    if (s.empty() || s.size() > 9) return false;
    uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint32_t>(c - '0');
    }
    *out = v;
    return true;
}

}  // namespace

ScenarioRunner::ScenarioRunner(std::ostream *trace, storage::Storage *storage, bool pinned, const ConfigSpec *seed,
                               storage::StateStore *state, auth::Credentials *credentials)
    : trace_(trace),
      storage_(storage != nullptr ? storage : &ownStorage_),
      state_(state),
      credentials_(credentials),
      pinned_(pinned),
      seed_(seed != nullptr ? *seed : fixtureConfig()) {
    newHub();
    const config::LoadReport boot = hub_->boot(seed_, credentials_);
    if (trace_ != nullptr) {
        *trace_ << "config: " << config::configStateName(boot.state)
                << (boot.reason != ErrorCode::Ok ? std::string(" (") + errorCodeName(boot.reason) + ")" : "")
                << ", generation " << hub_->config().revision << "\n";
    }
}

void ScenarioRunner::newHub() {
    hub_ = std::make_unique<SimHub>(storage_, state_);
    if (trace_ == nullptr) return;
    std::ostream *out = trace_;
    hub_->onLog = [out](const char *kind, const LogLine &line) {
        *out << "  [" << line.atMs << " ms] " << kind << ": " << line.text << "\n";
    };
}

void ScenarioRunner::reset() {
    ValidationReport report;
    if (pinned_) {
        report = hub_->factoryReset(seed_);
    } else {
        newHub();
        hub_->forgetActive();  // a reset starts with nothing active
        report = hub_->load(seed_);
    }
    if (!report.ok() && trace_ != nullptr) {
        for (const auto &issue : report.issues()) {
            *trace_ << "fixture invalid: " << errorCodeName(issue.code) << " at " << issue.path << "\n";
        }
    }
    commandCursor_ = hub_->commands().size();
    eventCursor_ = hub_->events().size();
    hidCursor_ = hub_->hid().reports().size();
    lastResult_ = ErrorCode::Ok;
}

void ScenarioRunner::reboot() {
    newHub();
    hub_->boot(seed_, credentials_);
    commandCursor_ = 0;
    eventCursor_ = 0;
    hidCursor_ = 0;
    lastResult_ = ErrorCode::Ok;
}

bool ScenarioRunner::fail(std::string message) {
    error_ = std::move(message);
    return false;
}

bool ScenarioRunner::run(std::istream &script, const std::string &name) {
    std::string line;
    int number = 0;
    while (std::getline(script, line)) {
        number++;
        const size_t hash = line.find('#');
        const std::string body = trim(hash == std::string::npos ? line : line.substr(0, hash));
        if (body.empty()) continue;
        if (trace_ != nullptr) *trace_ << name << ":" << number << ": " << body << "\n";
        if (!runLine(body)) {
            error_ = name + ":" + std::to_string(number) + ": " + error_;
            return false;
        }
    }
    return true;
}

bool ScenarioRunner::runLine(const std::string &raw) {
    const std::string line = trim(raw);
    std::istringstream in(line);
    std::string verb;
    in >> verb;
    std::string rest;
    std::getline(in, rest);
    rest = trim(rest);
    std::istringstream args(rest);
    std::vector<std::string> words;
    for (std::string w; args >> w;) words.push_back(w);

    auto button = [&](size_t i, remote::Button *out) {
        if (i >= words.size() || !remote::buttonFromName(words[i].c_str(), out)) {
            return fail("unknown button '" + (i < words.size() ? words[i] : "") + "'");
        }
        return true;
    };
    auto number = [&](size_t i, uint32_t fallback, uint32_t *out) {
        if (i >= words.size()) {
            *out = fallback;
            return true;
        }
        if (!parseUint(words[i], out)) return fail("bad number '" + words[i] + "'");
        return true;
    };

    SimHub &hub = *hub_;
    remote::Button b;
    uint32_t ms = 0;

    if (verb == "reset") {
        reset();
        return true;
    }
    if (verb == "reboot") {
        if (pinned_) return fail("reboot needs an unpinned hub (restart hub-sim instead)");
        reboot();
        return true;
    }
    if (verb == "activate") {
        if (words.size() != 1) return fail("usage: activate <activity_id>");
        lastResult_ = hub.activate(words[0]);
        return true;
    }
    if (verb == "off") {
        lastResult_ = hub.off();
        return true;
    }
    if (verb == "press" || verb == "release") {
        if (!button(0, &b)) return false;
        verb == "press" ? hub.press(b) : hub.release(b);
        return true;
    }
    if (verb == "tap" || verb == "hold") {
        if (!button(0, &b)) return false;
        if (verb == "hold" && words.size() < 2) return fail("usage: hold <button> <ms>");
        if (!number(1, 100, &ms)) return false;
        hub.tap(b, ms);
        return true;
    }
    if (verb == "advance" || verb == "silence") {
        if (words.size() != 1 || !number(0, 0, &ms)) return fail("usage: " + verb + " <ms>");
        hub.advance(ms, verb == "silence");
        return true;
    }
    if (verb == "ha") {
        if (words.empty()) return fail("usage: ha latency|fail|available|unavailable");
        if (words[0] == "latency") {
            if (!number(1, 0, &ms)) return false;
            hub.homeAssistant().setLatency(ms);
        } else if (words[0] == "fail" && words.size() >= 2) {
            ErrorCode code = ErrorCode::AdapterFailed;
            if (words.size() >= 3 && !errorCodeFromName(words[2].c_str(), &code)) {
                return fail("unknown error '" + words[2] + "'");
            }
            hub.homeAssistant().failNext(words[1], code);
        } else if (words[0] == "available" || words[0] == "unavailable") {
            hub.homeAssistant().setAvailable(words[0] == "available");
        } else {
            return fail("usage: ha latency <ms> | fail <command_id> [error] | available | unavailable");
        }
        return true;
    }
    if (verb == "ble") {
        if (words.size() != 1 || (words[0] != "connected" && words[0] != "disconnected")) {
            return fail("usage: ble connected|disconnected");
        }
        hub.hid().setConnected(words[0] == "connected");
        return true;
    }
    if (verb == "expect") {
        auto &log = hub.commands();
        if (commandCursor_ >= log.size()) return fail("expected '" + rest + "', but no more commands");
        const std::string &actual = log[commandCursor_].text;
        if (!matchesCommand(rest, actual)) return fail("expected '" + rest + "', got '" + actual + "'");
        commandCursor_++;
        return true;
    }
    if (verb == "expect_none") {
        auto &log = hub.commands();
        if (commandCursor_ < log.size()) return fail("expected no more commands, got '" + log[commandCursor_].text + "'");
        return true;
    }
    if (verb == "expect_event") {
        auto &log = hub.events();
        for (size_t i = eventCursor_; i < log.size(); i++) {
            if (normalize(log[i].text) == normalize(rest)) {
                eventCursor_ = i + 1;
                return true;
            }
        }
        return fail("expected event '" + rest + "' not found");
    }
    if (verb == "expect_no_event") {
        auto &log = hub.events();
        for (size_t i = eventCursor_; i < log.size(); i++) {
            if (normalize(log[i].text) == normalize(rest)) return fail("unexpected event '" + rest + "'");
        }
        return true;
    }
    if (verb == "expect_hid") {
        auto &log = hub.hid().reports();
        if (hidCursor_ >= log.size()) return fail("expected HID '" + rest + "', but no more reports");
        if (normalize(log[hidCursor_]) != normalize(rest)) {
            return fail("expected HID '" + rest + "', got '" + log[hidCursor_] + "'");
        }
        hidCursor_++;
        return true;
    }
    if (verb == "expect_active") {
        const std::string active = hub.engine().hasActiveActivity() ? hub.engine().activeActivityId() : "none";
        if (active != rest) return fail("expected active '" + rest + "', got '" + active + "'");
        return true;
    }
    if (verb == "expect_result") {
        if (rest != errorCodeName(lastResult_)) {
            return fail("expected result '" + rest + "', got '" + errorCodeName(lastResult_) + "'");
        }
        return true;
    }
    if (verb == "drain") {
        commandCursor_ = hub.commands().size();
        eventCursor_ = hub.events().size();
        hidCursor_ = hub.hid().reports().size();
        return true;
    }
    if (verb == "print") {
        if (trace_ != nullptr) {
            for (size_t i = commandCursor_; i < hub.commands().size(); i++) {
                *trace_ << "  command: " << hub.commands()[i].text << "\n";
            }
            for (size_t i = eventCursor_; i < hub.events().size(); i++) {
                *trace_ << "  event: " << hub.events()[i].text << "\n";
            }
        }
        return true;
    }
    return fail("unknown command '" + verb + "'");
}

}  // namespace hubbo::sim
