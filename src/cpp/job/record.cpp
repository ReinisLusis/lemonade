#include <lemon/job/record.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace lemon {
namespace job {

namespace {

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Howard Hinnant's civil-calendar algorithms rather than gmtime/timegm: the
// record can legitimately carry year 1 (Go's zero time), which is a negative
// time_t that gmtime_s rejects outright on Windows.
std::int64_t days_from_civil(std::int64_t y, int m, int d) {
    y -= (m <= 2) ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t& y, int& m, int& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    y = yoe + era * 400 + ((m <= 2) ? 1 : 0);
}

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

bool all_digits(const std::string& s, std::size_t from, std::size_t count) {
    if (from + count > s.size()) {
        return false;
    }
    for (std::size_t i = from; i < from + count; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

int digits_to_int(const std::string& s, std::size_t from, std::size_t count) {
    int v = 0;
    for (std::size_t i = from; i < from + count; ++i) {
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

void reject_unknown_keys(const Json& obj,
                         const std::vector<std::string>& known,
                         const std::string& where) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (std::find(known.begin(), known.end(), it.key()) == known.end()) {
            throw Invalid("unknown field \"" + it.key() + "\" in " + where +
                          " — refusing a record written by something newer");
        }
    }
}

TimePoint time_field(const Json& obj, const char* key) {
    if (!obj.contains(key) || obj.at(key).is_null()) {
        return TimePoint{};
    }
    if (!obj.at(key).is_string()) {
        throw Invalid(std::string("field \"") + key + "\" must be an RFC 3339 string");
    }
    return parse_rfc3339(obj.at(key).get<std::string>());
}

std::int64_t int_field(const Json& obj, const char* key) {
    if (!obj.contains(key) || obj.at(key).is_null()) {
        return 0;
    }
    if (!obj.at(key).is_number_integer()) {
        throw Invalid(std::string("field \"") + key + "\" must be an integer");
    }
    return obj.at(key).get<std::int64_t>();
}

std::string string_field(const Json& obj, const char* key) {
    if (!obj.contains(key) || obj.at(key).is_null()) {
        return "";
    }
    if (!obj.at(key).is_string()) {
        throw Invalid(std::string("field \"") + key + "\" must be a string");
    }
    return obj.at(key).get<std::string>();
}

}  // namespace

bool is_valid_want(const std::string& w) {
    return w == want::kRun || w == want::kPause || w == want::kCancel;
}

bool is_valid_state(const std::string& s) {
    return s == state::kPending || s == state::kRunning || s == state::kTransferred ||
           s == state::kComplete || s == state::kFailed || s == state::kCancelled;
}

bool is_terminal_state(const std::string& s) {
    return s == state::kComplete || s == state::kFailed || s == state::kCancelled;
}

std::string format_rfc3339(TimePoint t) {
    const std::int64_t micros =
        std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
    const std::int64_t secs = floor_div(micros, 1000000);
    const std::int64_t frac = micros - secs * 1000000;
    const std::int64_t days = floor_div(secs, 86400);
    const std::int64_t sod = secs - days * 86400;

    std::int64_t year = 0;
    int month = 0;
    int day = 0;
    civil_from_days(days, year, month, day);

    std::array<char, 64> buf{};
    std::snprintf(buf.data(), buf.size(), "%04lld-%02d-%02dT%02lld:%02lld:%02lld.%06lldZ",
                  static_cast<long long>(year), month, day,
                  static_cast<long long>(sod / 3600),
                  static_cast<long long>((sod / 60) % 60),
                  static_cast<long long>(sod % 60),
                  static_cast<long long>(frac));
    return std::string(buf.data());
}

TimePoint parse_rfc3339(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.empty()) {
        return TimePoint{};
    }
    // YYYY-MM-DDTHH:MM:SS is fixed-width; everything after it is optional.
    if (s.size() < 19 || !all_digits(s, 0, 4) || s[4] != '-' || !all_digits(s, 5, 2) ||
        s[7] != '-' || !all_digits(s, 8, 2) || (s[10] != 'T' && s[10] != 't' && s[10] != ' ') ||
        !all_digits(s, 11, 2) || s[13] != ':' || !all_digits(s, 14, 2) || s[16] != ':' ||
        !all_digits(s, 17, 2)) {
        throw Invalid("timestamp \"" + raw + "\" is not RFC 3339");
    }

    const std::int64_t year = digits_to_int(s, 0, 4);
    const int month = digits_to_int(s, 5, 2);
    const int day = digits_to_int(s, 8, 2);
    const int hour = digits_to_int(s, 11, 2);
    const int minute = digits_to_int(s, 14, 2);
    const int second = digits_to_int(s, 17, 2);

    std::size_t i = 19;
    std::int64_t frac_micros = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        int taken = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (taken < 6) {
                frac_micros = frac_micros * 10 + (s[i] - '0');
                ++taken;
            }
            ++i;
        }
        // Nanosecond precision from a Go writer is truncated, not rounded: a
        // resume point must never move forward because of a rounding rule.
        for (; taken < 6; ++taken) {
            frac_micros *= 10;
        }
    }

    std::int64_t offset_seconds = 0;
    if (i < s.size()) {
        const char z = s[i];
        if (z == 'Z' || z == 'z') {
            ++i;
        } else if (z == '+' || z == '-') {
            if (!all_digits(s, i + 1, 2) || i + 3 >= s.size() || s[i + 3] != ':' ||
                !all_digits(s, i + 4, 2)) {
                throw Invalid("timestamp \"" + raw + "\" has a malformed UTC offset");
            }
            const std::int64_t oh = digits_to_int(s, i + 1, 2);
            const std::int64_t om = digits_to_int(s, i + 4, 2);
            offset_seconds = (oh * 3600 + om * 60) * (z == '-' ? -1 : 1);
            i += 6;
        }
    }

    const std::int64_t secs =
        days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second -
        offset_seconds;
    const std::chrono::microseconds since_epoch(secs * 1000000 + frac_micros);
    return TimePoint(std::chrono::duration_cast<TimePoint::duration>(since_epoch));
}

void Record::validate() const { validate_with(content, critical); }

void Record::validate_with(const std::vector<std::string>& check_content,
                           const std::vector<std::string>& check_critical) const {
    if (check_content.empty()) {
        throw Invalid("a record must say what it contains");
    }
    if (const std::string missing = first_unknown_critical(check_critical); !missing.empty()) {
        throw UnknownSchema("requires \"" + missing + "\"");
    }
    if (progress.step.has_value()) {
        const Step& st = *progress.step;
        // A step with no name tells a person nothing, which is the only thing a
        // step is for.
        if (st.name.find_first_not_of(" \t\r\n") == std::string::npos) {
            throw Invalid("a step needs a name");
        }
        if (st.ordinal < 1) {
            throw Invalid("step ordinal counts from one, got " + std::to_string(st.ordinal));
        }
        if (st.of > 0 && st.ordinal > st.of) {
            throw Invalid("step " + std::to_string(st.ordinal) + " of " + std::to_string(st.of));
        }
        if (st.done < 0 || st.total < 0) {
            throw Invalid("step progress cannot be negative");
        }
    }
    for (auto it = extensions.begin(); it != extensions.end(); ++it) {
        if (it.key().find_first_not_of(" \t\r\n") == std::string::npos) {
            throw Invalid("an extension needs a name saying who understands it");
        }
    }
    if (intent.has_value() && !is_valid_want(intent->want)) {
        // Refused rather than treated as run. Guessing here means carrying on
        // with a job somebody asked to stop, using a word this implementation
        // is too old to understand.
        throw Invalid("intent \"" + intent->want + "\"");
    }
    if (trim(id).empty()) {
        throw Invalid("id is required");
    }
    if (trim(kind).empty()) {
        throw Invalid("kind is required — an opaque spec nobody can identify is unusable");
    }
    if (!is_valid_state(state)) {
        throw Invalid("state \"" + state + "\"");
    }
    if (!spec.is_object()) {
        throw Invalid("spec must be present and be a JSON object");
    }
    if (progress.done < 0 || progress.total < 0) {
        throw Invalid("progress cannot be negative");
    }
    if (delegation) {
        if (trim(delegation->system).empty() || trim(delegation->external_id).empty()) {
            throw Invalid("delegation needs both a system and an external id");
        }
    }
}

std::string Record::encode() const {
    // Derived here rather than remembered, so the declaration cannot drift from
    // the data, and so this stays const: every caller holds the record by value
    // or by const reference and none of them should have to remember a step.
    std::vector<std::string> derived_content;
    std::vector<std::string> derived_critical;
    derive_models(*this, derived_content, derived_critical);

    validate_with(derived_content, derived_critical);

    // Key order and omissions match the Go struct tags exactly. Go decodes with
    // DisallowUnknownFields, so an extra key here is not cosmetic — it makes
    // the record unreadable to the other implementations.
    // What this implementation writes, it writes as its own version. A record
    // read at 3 and written back at 3 while carrying an intent would tell an
    // older reader it is safe to ignore fields it does not know, which is
    // exactly the risk the schema check refuses.
    Json d = Json::object();
    d["content"] = derived_content;
    if (!derived_critical.empty()) {
        d["critical"] = derived_critical;
    }
    d["id"] = id;
    d["kind"] = kind;
    d["state"] = state;
    d["spec"] = spec;
    if (checkpoint) {
        d["checkpoint"] = *checkpoint;
    }

    Json p = Json::object();
    p["done"] = progress.done;
    if (progress.total != 0) {
        p["total"] = progress.total;
    }
    p["updated_at"] = format_rfc3339(progress.updated_at);
    if (progress.step.has_value()) {
        Json st = Json::object();
        st["name"] = progress.step->name;
        st["ordinal"] = progress.step->ordinal;
        if (progress.step->of != 0) {
            st["of"] = progress.step->of;
        }
        if (progress.step->done != 0) {
            st["done"] = progress.step->done;
        }
        if (progress.step->total != 0) {
            st["total"] = progress.step->total;
        }
        p["step"] = std::move(st);
    }
    d["progress"] = std::move(p);

    Json l = Json::object();
    l["owner"] = lease.owner;
    l["epoch"] = lease.epoch;
    l["expires_at"] = format_rfc3339(lease.expires_at);
    d["lease"] = std::move(l);

    if (delegation) {
        Json g = Json::object();
        g["system"] = delegation->system;
        g["external_id"] = delegation->external_id;
        if (delegation->delivered) {
            g["delivered"] = true;
        }
        d["delegation"] = std::move(g);
    }
    if (!requires_capabilities.empty()) {
        d["requires"] = requires_capabilities;
    }
    if (!error.empty()) {
        d["error"] = error;
    }
    if (intent) {
        Json it = Json::object();
        it["want"] = intent->want;
        if (!intent->by.empty()) {
            it["by"] = intent->by;
        }
        if (intent->at.has_value()) {
            it["at"] = format_rfc3339(*intent->at);
        }
        d["intent"] = std::move(it);
    }
    if (!extensions.empty()) {
        // The ordered_json specialisation keeps insertion order, and the other
        // implementations sort by key, so this must be sorted too: the output is
        // compared byte for byte.
        std::vector<std::string> names;
        for (auto it2 = extensions.begin(); it2 != extensions.end(); ++it2) {
            names.push_back(it2.key());
        }
        std::sort(names.begin(), names.end());
        Json ext = Json::object();
        for (const std::string& n : names) {
            ext[n] = extensions.at(n);
        }
        d["extensions"] = std::move(ext);
    }
    d["created_at"] = format_rfc3339(created_at);
    d["updated_at"] = format_rfc3339(updated_at);

    return d.dump(2) + "\n";
}

Record Record::decode(const std::string& text) {
    Json d;
    try {
        d = Json::parse(text);
    } catch (const Json::exception& e) {
        throw Invalid(std::string("not valid JSON: ") + e.what());
    }
    if (!d.is_object()) {
        throw Invalid("record must be a JSON object");
    }
    reject_unknown_keys(d,
                        {"schema", "content", "critical", "id", "kind", "state", "spec",
                         "checkpoint", "progress", "lease", "delegation", "requires", "error",
                         "intent", "extensions", "created_at", "updated_at"},
                        "record");

    Record r;
    if (d.contains("content") && d.at("content").is_array()) {
        r.content = d.at("content").get<std::vector<std::string>>();
    }
    if (d.contains("critical") && d.at("critical").is_array()) {
        r.critical = d.at("critical").get<std::vector<std::string>>();
    }
    if (r.content.empty() && d.contains("schema")) {
        // A legacy record. The mapping is exact, not a guess: those versions are
        // frozen and it is known what each one could contain.
        if (!d.at("schema").is_number_integer()) {
            throw UnknownSchema("schema " + d.at("schema").dump());
        }
        bool ok = false;
        const int legacy = d.at("schema").get<int>();
        r.content = models_for_legacy_schema(legacy, &ok);
        if (!ok) {
            throw UnknownSchema("legacy schema " + std::to_string(legacy));
        }
        r.critical.clear();
        for (const std::string& m : r.content) {
            if (m != model::kStep) {
                r.critical.push_back(m);
            }
        }
        if (d.contains("delegation") && !d.at("delegation").is_null()) {
            r.content.push_back(model::kDelegation);
            r.critical.push_back(model::kDelegation);
        }
    }
    if (const std::string missing = first_unknown_critical(r.critical); !missing.empty()) {
        throw UnknownSchema("this record requires \"" + missing +
                            "\", which this implementation cannot read");
    }
    if (d.contains("extensions") && d.at("extensions").is_object()) {
        r.extensions = d.at("extensions");
    }
    r.id = string_field(d, "id");
    r.kind = string_field(d, "kind");
    r.state = string_field(d, "state");
    if (!d.contains("spec")) {
        throw Invalid("spec must be present and be a JSON object");
    }
    r.spec = d.at("spec");
    if (d.contains("checkpoint") && !d.at("checkpoint").is_null()) {
        r.checkpoint = d.at("checkpoint");
    }

    if (d.contains("progress")) {
        const Json& p = d.at("progress");
        if (!p.is_object()) {
            throw Invalid("progress must be an object");
        }
        reject_unknown_keys(p, {"done", "total", "updated_at", "step"}, "progress");
        r.progress.done = int_field(p, "done");
        r.progress.total = int_field(p, "total");
        r.progress.updated_at = time_field(p, "updated_at");
        if (p.contains("step") && p.at("step").is_object()) {
            const Json& sp = p.at("step");
            reject_unknown_keys(sp, {"name", "ordinal", "of", "done", "total"}, "step");
            Step st;
            st.name = string_field(sp, "name");
            st.ordinal = static_cast<int>(int_field(sp, "ordinal"));
            st.of = static_cast<int>(int_field(sp, "of"));
            st.done = int_field(sp, "done");
            st.total = int_field(sp, "total");
            r.progress.step = st;
        }
    }

    if (d.contains("lease")) {
        const Json& l = d.at("lease");
        if (!l.is_object()) {
            throw Invalid("lease must be an object");
        }
        reject_unknown_keys(l, {"owner", "epoch", "expires_at"}, "lease");
        r.lease.owner = string_field(l, "owner");
        r.lease.epoch = int_field(l, "epoch");
        r.lease.expires_at = time_field(l, "expires_at");
    }

    if (d.contains("delegation") && !d.at("delegation").is_null()) {
        const Json& g = d.at("delegation");
        if (!g.is_object()) {
            throw Invalid("delegation must be an object");
        }
        reject_unknown_keys(g, {"system", "external_id", "delivered"}, "delegation");
        Delegation deleg;
        deleg.system = string_field(g, "system");
        deleg.external_id = string_field(g, "external_id");
        if (g.contains("delivered") && !g.at("delivered").is_null()) {
            if (!g.at("delivered").is_boolean()) {
                throw Invalid("delegation.delivered must be a boolean");
            }
            deleg.delivered = g.at("delivered").get<bool>();
        }
        r.delegation = deleg;
    }

    if (d.contains("requires") && !d.at("requires").is_null()) {
        if (!d.at("requires").is_array()) {
            throw Invalid("requires must be an array of strings");
        }
        for (const auto& cap : d.at("requires")) {
            if (!cap.is_string()) {
                throw Invalid("requires must be an array of strings");
            }
            r.requires_capabilities.push_back(cap.get<std::string>());
        }
    }

    r.error = string_field(d, "error");

    if (d.contains("intent") && !d.at("intent").is_null()) {
        const Json& it = d.at("intent");
        if (!it.is_object()) {
            throw Invalid("intent must be an object");
        }
        reject_unknown_keys(it, {"want", "by", "at"}, "intent");
        Intent in;
        in.want = string_field(it, "want");
        in.by = string_field(it, "by");
        if (it.contains("at") && !it.at("at").is_null()) {
            in.at = time_field(it, "at");
        }
        r.intent = in;
    }

    r.created_at = time_field(d, "created_at");
    r.updated_at = time_field(d, "updated_at");

    r.validate();
    return r;
}


bool is_known_model(const std::string& name) {
    return name == model::kBase || name == model::kIntent || name == model::kDelegation ||
           name == model::kStep;
}

std::string first_unknown_critical(const std::vector<std::string>& critical) {
    for (const std::string& name : critical) {
        if (!is_known_model(name)) {
            return name;
        }
    }
    return "";
}

std::vector<std::string> models_for_legacy_schema(int schema, bool* ok) {
    if (ok != nullptr) {
        *ok = true;
    }
    switch (schema) {
        case 3:
            return {model::kBase};
        case 4:
            return {model::kBase, model::kIntent};
        case 5:
            return {model::kBase, model::kIntent, model::kStep};
        default:
            if (ok != nullptr) {
                *ok = false;
            }
            return {};
    }
}

void derive_models(const Record& r, std::vector<std::string>& out_content,
                   std::vector<std::string>& out_critical) {
    out_content = {model::kBase};
    out_critical = {model::kBase};

    if (r.intent.has_value()) {
        out_content.push_back(model::kIntent);
        out_critical.push_back(model::kIntent);
    }
    if (r.delegation.has_value()) {
        out_content.push_back(model::kDelegation);
        out_critical.push_back(model::kDelegation);
    }
    // Advisory, and deliberately not critical: a reader that ignores a step is
    // correct about everything that matters.
    if (r.progress.step.has_value()) {
        out_content.push_back(model::kStep);
    }

    std::vector<std::string> names;
    for (auto it = r.extensions.begin(); it != r.extensions.end(); ++it) {
        names.push_back(it.key());
    }
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        out_content.push_back(n);
    }

    // Whatever the caller marked critical that this layer did not derive -- an
    // extension, or a model a newer writer knows about -- stays marked.
    for (const std::string& name : r.critical) {
        const bool already =
            std::find(out_critical.begin(), out_critical.end(), name) != out_critical.end();
        const bool present =
            std::find(out_content.begin(), out_content.end(), name) != out_content.end();
        if (!already && present) {
            out_critical.push_back(name);
        }
    }
}

void Record::describe() { derive_models(*this, content, critical); }

}  // namespace job
}  // namespace lemon
