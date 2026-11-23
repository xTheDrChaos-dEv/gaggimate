#include "TomInbox.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <display/models/shot_log_format.h>

namespace tom {

namespace {
constexpr uint32_t EPOCH_2020 = 1577836800;

bool validCodeChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

bool validCode(const std::string &code) {
    if (code.size() < 24 || code.size() > 64) {
        return false;
    }
    return std::all_of(code.begin(), code.end(), validCodeChar);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// JavaScript's \b word characters, so matches agree with TOM's regular expression.
bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Whole-word match on a lower-cased name.
bool hasWord(const std::string &name, const char *word) {
    const size_t n = strlen(word);
    for (size_t pos = name.find(word); pos != std::string::npos; pos = name.find(word, pos + 1)) {
        const bool startOk = pos == 0 || !isWordChar(name[pos - 1]);
        const bool endOk = pos + n >= name.size() || !isWordChar(name[pos + n]);
        if (startOk && endOk) {
            return true;
        }
    }
    return false;
}

std::string stringField(const char *name, const std::string &value) {
    return "\"" + std::string(name) + "\":{\"stringValue\":\"" + jsonEscape(value) + "\"}";
}

std::string integerField(const char *name, uint64_t value) {
    // Firestore REST carries 64-bit integers as JSON strings.
    return "\"" + std::string(name) + "\":{\"integerValue\":\"" + std::to_string(value) + "\"}";
}

std::string boolField(const char *name, bool value) {
    return "\"" + std::string(name) + "\":{\"booleanValue\":" + (value ? "true" : "false") + "}";
}
} // namespace

bool readSlogHeader(const uint8_t *bytes, size_t len, uint32_t shotId, ShotMeta &out) {
    if (bytes == nullptr || len < sizeof(ShotLogHeader)) {
        return false;
    }
    ShotLogHeader header;
    memcpy(&header, bytes, sizeof(header));
    if (header.magic != SHOT_LOG_MAGIC) {
        return false;
    }
    out.shotId = shotId;
    out.startEpoch = header.startEpoch;
    out.durationMs = header.durationMs;
    out.finalWeightG = static_cast<float>(header.finalWeight) / 10.0f;
    out.profileName = std::string(header.profileName, strnlen(header.profileName, sizeof(header.profileName)));
    out.size = len;
    return true;
}

bool isUtilityName(const std::string &profileName) {
    // Mirrors TOM's UTILITY_NAME: flush, back flush/backflush, clean(ing), descal*, rinse, purge.
    const std::string name = lower(profileName);
    static const char *const words[] = {"flush", "backflush", "clean", "cleaning", "rinse", "purge"};
    for (const char *word : words) {
        if (hasWord(name, word)) {
            return true;
        }
    }
    for (size_t pos = name.find("descal"); pos != std::string::npos; pos = name.find("descal", pos + 1)) {
        if (pos == 0 || !isWordChar(name[pos - 1])) {
            return true;
        }
    }
    return false;
}

Skip shouldSkip(const ShotMeta &meta, bool requireWeight) {
    if (meta.utility || isUtilityName(meta.profileName)) {
        return Skip::UTILITY;
    }
    if (meta.durationMs < MIN_DURATION_MS) {
        return Skip::TOO_SHORT;
    }
    if (meta.size > MAX_SLOG_BYTES) {
        return Skip::TOO_BIG;
    }
    if (requireWeight && meta.finalWeightG < MIN_WEIGHT_G) {
        return Skip::NO_WEIGHT;
    }
    return Skip::NONE;
}

const char *skipText(Skip skip) {
    switch (skip) {
    case Skip::NONE:
        return "sent";
    case Skip::NOT_A_SHOT:
        return "not a shot file";
    case Skip::UTILITY:
        return "cleaning or flush profile";
    case Skip::TOO_SHORT:
        return "shorter than 5 s";
    case Skip::TOO_BIG:
        return "file too large";
    case Skip::NO_WEIGHT:
        return "nothing on the scale";
    }
    return "";
}

std::string extractHouseholdCode(const std::string &input) {
    // Trim surrounding whitespace (pasted links often carry a newline).
    size_t begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = input.find_last_not_of(" \t\r\n");
    const std::string text = input.substr(begin, end - begin + 1);
    if (validCode(text)) {
        return text;
    }
    // A TOM link: take the value of the h= query parameter.
    for (size_t pos = text.find("h="); pos != std::string::npos; pos = text.find("h=", pos + 1)) {
        if (pos == 0 || (text[pos - 1] != '?' && text[pos - 1] != '&')) {
            continue;
        }
        size_t start = pos + 2;
        size_t stop = start;
        while (stop < text.size() && validCodeChar(text[stop])) {
            stop++;
        }
        const std::string code = text.substr(start, stop - start);
        const bool cleanEnd = stop == text.size() || text[stop] == '&' || text[stop] == '#';
        if (cleanEnd && validCode(code)) {
            return code;
        }
    }
    return "";
}

std::string base64(const uint8_t *bytes, size_t len) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += table[(v >> 6) & 63];
        out += table[v & 63];
    }
    if (i < len) {
        uint32_t v = bytes[i] << 16;
        if (i + 1 < len) {
            v |= bytes[i + 1] << 8;
        }
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < len ? table[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string jsonEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c); // UTF-8 bytes pass through unchanged
            }
        }
    }
    return out;
}

std::string isoTime(uint32_t epochSeconds, uint32_t extraMs) {
    if (epochSeconds < EPOCH_2020) {
        return "";
    }
    const uint64_t totalMs = static_cast<uint64_t>(epochSeconds) * 1000 + extraMs;
    const uint64_t secs = totalMs / 1000;
    const uint32_t daySecs = static_cast<uint32_t>(secs % 86400);
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm), so no libc time zone state is involved.
    int64_t z = static_cast<int64_t>(secs / 86400) + 719468;
    const int64_t era = z / 146097;
    const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    const unsigned day = doy - (153 * mp + 2) / 5 + 1;
    const unsigned month = mp < 10 ? mp + 3 : mp - 9;
    const int64_t year = static_cast<int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ", static_cast<int>(year), month, day, daySecs / 3600,
             (daySecs / 60) % 60, daySecs % 60, static_cast<unsigned>(totalMs % 1000));
    return buf;
}

std::string padShotId(uint32_t shotId) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%06u", static_cast<unsigned>(shotId));
    return buf;
}

Endpoints Endpoints::local(const std::string &hostPort) {
    // Same path layout as the Firebase emulators, so the mock or the real emulators both work.
    Endpoints ep;
    ep.identityToolkit = "http://" + hostPort + "/identitytoolkit.googleapis.com";
    ep.secureToken = "http://" + hostPort + "/securetoken.googleapis.com";
    ep.firestore = "http://" + hostPort;
    return ep;
}

std::string signUpUrl(const Endpoints &ep) {
    return ep.identityToolkit + "/v1/accounts:signUp?key=" + ep.apiKey;
}

std::string refreshUrl(const Endpoints &ep) { return ep.secureToken + "/v1/token?key=" + ep.apiKey; }

std::string inboxDocumentId(const ShotMeta &meta) {
    // The start time keeps ids unique when the machine's shot counter restarts after a history wipe.
    return "gm-" + padShotId(meta.shotId) + "-" + std::to_string(meta.startEpoch);
}

std::string createDocumentUrl(const Endpoints &ep, const std::string &household, const ShotMeta &meta) {
    return ep.firestore + "/v1/projects/" + ep.projectId + "/databases/(default)/documents/households/" + household +
           "/inbox?documentId=" + inboxDocumentId(meta);
}

std::string signUpBody() { return "{\"returnSecureToken\":true}"; }

std::string refreshBody(const std::string &refreshToken) {
    // Refresh tokens are URL-safe ([A-Za-z0-9_-]), so they need no percent-encoding.
    return "grant_type=refresh_token&refresh_token=" + refreshToken;
}

std::string documentBody(const ShotMeta &meta, const uint8_t *slog, size_t len, const std::string &device) {
    const std::string encoded = base64(slog, len);
    std::string body;
    body.reserve(encoded.size() + 512);
    body += "{\"fields\":{";
    body += stringField("source", "gaggimate") + ",";
    body += stringField("fileName", padShotId(meta.shotId) + ".slog") + ",";
    body += integerField("size", len) + ",";
    body += stringField("shotId", std::to_string(meta.shotId)) + ",";
    const std::string captured = isoTime(meta.startEpoch, meta.durationMs);
    if (!captured.empty()) {
        body += stringField("capturedAt", captured) + ",";
    }
    body += stringField("profile", meta.profileName) + ",";
    body += boolField("utility", meta.utility || isUtilityName(meta.profileName)) + ",";
    body += integerField("durationMs", meta.durationMs) + ",";
    body += stringField("device", device) + ",";
    body += "\"data\":{\"stringValue\":\"" + encoded + "\"}";
    body += "}}";
    return body;
}

std::string jsonField(const std::string &json, const std::string &key) {
    const std::string quoted = "\"" + key + "\"";
    for (size_t pos = json.find(quoted); pos != std::string::npos; pos = json.find(quoted, pos + 1)) {
        size_t i = pos + quoted.size();
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            i++;
        }
        if (i >= json.size() || json[i] != ':') {
            continue; // the key text appeared as a value, not a key
        }
        i++;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) {
            i++;
        }
        if (i < json.size() && json[i] == '"') {
            std::string value;
            for (i++; i < json.size() && json[i] != '"'; i++) {
                if (json[i] == '\\' && i + 1 < json.size()) {
                    i++;
                }
                value += json[i];
            }
            return value;
        }
        size_t end = i;
        while (end < json.size() && (std::isalnum(static_cast<unsigned char>(json[end])) || json[end] == '.' ||
                                     json[end] == '-')) {
            end++;
        }
        return json.substr(i, end - i);
    }
    return "";
}

bool parseToken(const std::string &json, Token &out) {
    Token t;
    t.idToken = jsonField(json, "idToken");
    if (t.idToken.empty()) {
        t.idToken = jsonField(json, "id_token");
    }
    t.refreshToken = jsonField(json, "refreshToken");
    if (t.refreshToken.empty()) {
        t.refreshToken = jsonField(json, "refresh_token");
    }
    std::string expires = jsonField(json, "expiresIn");
    if (expires.empty()) {
        expires = jsonField(json, "expires_in");
    }
    t.expiresInSec = expires.empty() ? 3600 : static_cast<uint32_t>(strtoul(expires.c_str(), nullptr, 10));
    if (t.idToken.empty() || t.refreshToken.empty()) {
        return false;
    }
    out = t;
    return true;
}

Outcome classifyCreate(int httpStatus) {
    if (httpStatus == 200 || httpStatus == 409) {
        return Outcome::SENT; // 409: this shot's document already exists from an earlier try
    }
    if (httpStatus == 401) {
        return Outcome::REAUTH;
    }
    if (httpStatus == 400 || httpStatus == 404 || httpStatus == 413) {
        return Outcome::DROP; // the request itself is wrong; retrying the same bytes cannot help
    }
    return Outcome::RETRY; // network error (<0), 403 (rules/key), 429, 5xx
}

std::string serializeQueue(const std::vector<uint32_t> &ids) {
    std::string out;
    for (uint32_t id : ids) {
        out += std::to_string(id);
        out += '\n';
    }
    return out;
}

std::vector<uint32_t> parseQueue(const std::string &text) {
    std::vector<uint32_t> ids;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty() && std::all_of(line.begin(), line.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
            line.size() <= 9) {
            enqueue(ids, static_cast<uint32_t>(strtoul(line.c_str(), nullptr, 10)));
        }
        pos = end + 1;
    }
    return ids;
}

bool enqueue(std::vector<uint32_t> &ids, uint32_t shotId) {
    if (std::find(ids.begin(), ids.end(), shotId) != ids.end()) {
        return false;
    }
    ids.push_back(shotId);
    while (ids.size() > MAX_QUEUE) {
        ids.erase(ids.begin());
    }
    return true;
}

uint32_t retryDelayMs(uint32_t failures) {
    const uint32_t base = 30000;
    const uint32_t cap = 5 * 60 * 1000; // a kitchen machine is often switched off again within the half hour
    if (failures == 0) {
        return 0;
    }
    uint32_t delay = base;
    for (uint32_t i = 1; i < failures && delay < cap; i++) {
        delay *= 2;
    }
    return delay < cap ? delay : cap;
}

} // namespace tom
