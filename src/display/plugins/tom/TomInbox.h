#ifndef TOM_INBOX_H
#define TOM_INBOX_H

// Request building, filtering and response parsing for the TOM shot inbox upload (see TomUploadPlugin).
// Plain C++/STL with no Arduino or ESP-IDF types, so the native Unity suite (test/test_tom_upload) covers it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tom {

constexpr uint32_t MIN_DURATION_MS = 5000;   // TOM ignores shorter shots, so they are never sent
constexpr float MIN_WEIGHT_G = 1.0f;         // less than this on the scale counts as an empty cup
constexpr size_t MAX_SLOG_BYTES = 256 * 1024; // keeps the document far below Firestore's 1 MB limit
constexpr size_t MAX_QUEUE = 20;              // pending shots kept while offline; the oldest drop first

struct ShotMeta {
    uint32_t shotId = 0;
    uint32_t startEpoch = 0; // seconds; 0 when the clock was not set
    uint32_t durationMs = 0;
    float finalWeightG = 0.0f; // highest weight the scale showed (what Shot History lists); 0 without a scale reading
    std::string profileName;
    size_t size = 0;
    bool utility = false; // the brew process ran a utility (flush/backflush/descale) profile
};

enum class Skip { NONE, NOT_A_SHOT, UTILITY, TOO_SHORT, TOO_BIG, NO_WEIGHT };

// Reads the fixed .slog header (shot_log_format.h). False when the bytes are not a shot log.
bool readSlogHeader(const uint8_t *bytes, size_t len, uint32_t shotId, ShotMeta &out);

// Same rules as TOM's inboxIgnored(): utility profiles, cleaning-like names and anything under 5 s stay home.
// With requireWeight, so does a shot that put nothing on the scale: a test run, a choked or aborted shot, or
// water run through an empty basket is not coffee anyone wants to log.
Skip shouldSkip(const ShotMeta &meta, bool requireWeight = true);
bool isUtilityName(const std::string &profileName);
const char *skipText(Skip skip);

// Accepts the bare household code or a TOM link containing ?h=<code>; "" when neither is valid.
std::string extractHouseholdCode(const std::string &input);

std::string base64(const uint8_t *bytes, size_t len);
std::string jsonEscape(const std::string &text);
// "2026-11-22T14:03:12.000Z"; "" for clocks that were never set (before 2020).
std::string isoTime(uint32_t epochSeconds, uint32_t extraMs);
std::string padShotId(uint32_t shotId); // "000042", like ShotHistoryPlugin's file names

// Endpoints. base is "https://" for the real services; the simulator points everything at one local mock.
struct Endpoints {
    std::string identityToolkit = "https://identitytoolkit.googleapis.com";
    std::string secureToken = "https://securetoken.googleapis.com";
    std::string firestore = "https://firestore.googleapis.com";
    // Which TOM Firebase project to talk to. Not compiled in: the plugin reads both from the settings store
    // (tom_p, tom_k), so published firmware carries no project details.
    std::string projectId;
    std::string apiKey;
    bool configured() const { return !projectId.empty() && !apiKey.empty(); }
    static Endpoints local(const std::string &hostPort); // emulator-style paths on http://hostPort
};

std::string signUpUrl(const Endpoints &ep);
std::string refreshUrl(const Endpoints &ep);
// Deterministic id per shot, so a retry after a lost response cannot create a second inbox entry (409 instead).
std::string inboxDocumentId(const ShotMeta &meta);
std::string createDocumentUrl(const Endpoints &ep, const std::string &household, const ShotMeta &meta);

std::string signUpBody();                                   // {"returnSecureToken":true}
std::string refreshBody(const std::string &refreshToken);   // form-encoded grant_type=refresh_token
std::string documentBody(const ShotMeta &meta, const uint8_t *slog, size_t len, const std::string &device);

struct Token {
    std::string idToken;
    std::string refreshToken;
    uint32_t expiresInSec = 0;
};
// Parses accounts:signUp ({idToken, refreshToken, expiresIn}) or securetoken ({id_token, refresh_token, expires_in}).
bool parseToken(const std::string &json, Token &out);
// First string (or bare number) value for "key" in a flat JSON response; "" when absent.
std::string jsonField(const std::string &json, const std::string &key);

// What to do after a createDocument response.
enum class Outcome { SENT, REAUTH, RETRY, DROP };
Outcome classifyCreate(int httpStatus);

// Offline queue file: one shot id per line.
std::string serializeQueue(const std::vector<uint32_t> &ids);
std::vector<uint32_t> parseQueue(const std::string &text);
// Appends unless already queued; drops the oldest beyond MAX_QUEUE. False when it was already there.
bool enqueue(std::vector<uint32_t> &ids, uint32_t shotId);

// Wait before the next try after `failures` failures in a row: 30 s doubling to 5 min.
uint32_t retryDelayMs(uint32_t failures);

} // namespace tom

#endif // TOM_INBOX_H
