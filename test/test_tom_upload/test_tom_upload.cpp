// TOM shot inbox upload: request building, filtering and response parsing (TomInbox.cpp, STL only).
#include <cstring>
#include <display/models/shot_log_format.h>
#include <unity.h>
#include <vector>

#include "../../src/display/plugins/tom/TomInbox.cpp"

using namespace tom;

void setUp() {}
void tearDown() {}

// finalWeightTenths: what the scale ended on, in 0.1 g (36.0 g unless a test says otherwise).
static std::vector<uint8_t> makeSlog(const char *profile, uint32_t durationMs, uint32_t startEpoch, size_t samples,
                                     uint16_t finalWeightTenths = 360) {
    ShotLogHeader h{};
    h.magic = SHOT_LOG_MAGIC;
    h.version = SHOT_LOG_VERSION;
    h.reserved0 = SHOT_LOG_SAMPLE_SIZE;
    h.headerSize = SHOT_LOG_HEADER_SIZE;
    h.sampleInterval = SHOT_LOG_SAMPLE_INTERVAL_MS;
    h.fieldsMask = SHOT_LOG_FIELDS_MASK_ALL;
    h.sampleCount = samples;
    h.durationMs = durationMs;
    h.startEpoch = startEpoch;
    h.finalWeight = finalWeightTenths;
    strncpy(h.profileName, profile, sizeof(h.profileName) - 1);
    std::vector<uint8_t> bytes(sizeof(h) + samples * SHOT_LOG_SAMPLE_SIZE, 0);
    memcpy(bytes.data(), &h, sizeof(h));
    for (size_t i = 0; i < samples; i++) {
        ShotLogSample s{};
        s.t = static_cast<uint32_t>(i * 250);
        s.cp = 90;
        memcpy(bytes.data() + sizeof(h) + i * SHOT_LOG_SAMPLE_SIZE, &s, sizeof(s));
    }
    return bytes;
}

static ShotMeta metaFor(const char *profile, uint32_t durationMs) {
    std::vector<uint8_t> bytes = makeSlog(profile, durationMs, 1795000000, 4);
    ShotMeta m;
    TEST_ASSERT_TRUE(readSlogHeader(bytes.data(), bytes.size(), 42, m));
    return m;
}

void test_reads_header() {
    std::vector<uint8_t> bytes = makeSlog("Classic 9 bar", 28500, 1795000000, 114);
    ShotMeta m;
    TEST_ASSERT_TRUE(readSlogHeader(bytes.data(), bytes.size(), 42, m));
    TEST_ASSERT_EQUAL_UINT32(42, m.shotId);
    TEST_ASSERT_EQUAL_UINT32(28500, m.durationMs);
    TEST_ASSERT_EQUAL_UINT32(1795000000, m.startEpoch);
    TEST_ASSERT_EQUAL_STRING("Classic 9 bar", m.profileName.c_str());
    TEST_ASSERT_EQUAL(512 + 114 * 30, m.size);
    bytes[0] = 'X';
    TEST_ASSERT_FALSE(readSlogHeader(bytes.data(), bytes.size(), 42, m));
    TEST_ASSERT_FALSE(readSlogHeader(bytes.data(), 100, 42, m));
}

void test_filters_like_tom() {
    TEST_ASSERT_TRUE(shouldSkip(metaFor("Classic 9 bar", 28000)) == Skip::NONE);
    TEST_ASSERT_TRUE(shouldSkip(metaFor("Classic 9 bar", 4999)) == Skip::TOO_SHORT);
    TEST_ASSERT_TRUE(shouldSkip(metaFor("Classic 9 bar", 5000)) == Skip::NONE);
    const char *utility[] = {"Flush", "Backflush", "Back flush", "Cleaning", "[Utility] Clean", "Descaling", "Descale",
                             "Rinse", "Group purge"};
    for (const char *name : utility) {
        TEST_ASSERT_TRUE_MESSAGE(shouldSkip(metaFor(name, 30000)) == Skip::UTILITY, name);
    }
    // Words that merely contain a utility word are real shots (TOM's \b boundaries agree).
    const char *shots[] = {"Flushed Blooming", "Turbo", "Cleanish Light Roast", "Blooming Espresso", "Flush2"};
    for (const char *name : shots) {
        TEST_ASSERT_TRUE_MESSAGE(shouldSkip(metaFor(name, 30000)) == Skip::NONE, name);
    }
    ShotMeta m = metaFor("Classic", 30000);
    m.utility = true; // BrewProcess::isUtility() at save time
    TEST_ASSERT_TRUE(shouldSkip(m) == Skip::UTILITY);
    m.utility = false;
    m.size = MAX_SLOG_BYTES + 1;
    TEST_ASSERT_TRUE(shouldSkip(m) == Skip::TOO_BIG);
}

static ShotMeta weighing(uint16_t tenths, uint32_t durationMs = 30000) {
    std::vector<uint8_t> bytes = makeSlog("Classic 9 bar", durationMs, 1795000000, 4, tenths);
    ShotMeta m;
    TEST_ASSERT_TRUE(readSlogHeader(bytes.data(), bytes.size(), 42, m));
    return m;
}

// A shot that put nothing on the scale (test run, choked or aborted shot) stays home unless the owner brews
// without a scale and has said so.
void test_empty_cup_is_not_sent() {
    TEST_ASSERT_EQUAL_FLOAT(38.1f, weighing(381).finalWeightG);
    TEST_ASSERT_TRUE(shouldSkip(weighing(381)) == Skip::NONE);
    TEST_ASSERT_TRUE(shouldSkip(weighing(0)) == Skip::NO_WEIGHT);
    TEST_ASSERT_TRUE(shouldSkip(weighing(9)) == Skip::NO_WEIGHT); // 0.9 g: drips, not a shot
    TEST_ASSERT_TRUE(shouldSkip(weighing(10)) == Skip::NONE);
    TEST_ASSERT_TRUE(shouldSkip(weighing(0), false) == Skip::NONE); // "also send shots with nothing on the scale"
    // The 9.8 s shot with no weight from the bug report, and a long one with no weight.
    TEST_ASSERT_TRUE(shouldSkip(weighing(0, 9800)) == Skip::NO_WEIGHT);
    TEST_ASSERT_TRUE(shouldSkip(weighing(0, 74800)) == Skip::NO_WEIGHT);
    // Earlier reasons still win, so the status line names the more basic one.
    TEST_ASSERT_TRUE(shouldSkip(weighing(0, 3000)) == Skip::TOO_SHORT);
    TEST_ASSERT_EQUAL_STRING("nothing on the scale", skipText(Skip::NO_WEIGHT));
}

void test_household_code() {
    const std::string code = "abcdefghijkmnpqrstuvwxyz2345";
    TEST_ASSERT_EQUAL_STRING(code.c_str(), extractHouseholdCode(code).c_str());
    TEST_ASSERT_EQUAL_STRING(code.c_str(), extractHouseholdCode("  " + code + "\n").c_str());
    TEST_ASSERT_EQUAL_STRING(code.c_str(), extractHouseholdCode("https://example.web.app/?h=" + code).c_str());
    TEST_ASSERT_EQUAL_STRING(code.c_str(), extractHouseholdCode("https://x.web.app/?a=1&h=" + code + "#top").c_str());
    TEST_ASSERT_EQUAL_STRING("", extractHouseholdCode("short").c_str());
    TEST_ASSERT_EQUAL_STRING("", extractHouseholdCode("ABCDEFGHIJKMNPQRSTUVWXYZ2345").c_str()); // upper case
    TEST_ASSERT_EQUAL_STRING("", extractHouseholdCode("https://x/?h=" + code + "!").c_str());
    TEST_ASSERT_EQUAL_STRING("", extractHouseholdCode("https://x/?xh=" + code).c_str());
    TEST_ASSERT_EQUAL_STRING("", extractHouseholdCode("../" + code).c_str()); // no path tricks into Firestore
}

void test_base64_and_time() {
    const uint8_t bytes[] = {'M', 'a', 'n'};
    TEST_ASSERT_EQUAL_STRING("", base64(bytes, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("TQ==", base64(bytes, 1).c_str());
    TEST_ASSERT_EQUAL_STRING("TWE=", base64(bytes, 2).c_str());
    TEST_ASSERT_EQUAL_STRING("TWFu", base64(bytes, 3).c_str());
    const uint8_t high[] = {0xff, 0xfe, 0x00};
    TEST_ASSERT_EQUAL_STRING("//4A", base64(high, 3).c_str());
    TEST_ASSERT_EQUAL_STRING("2026-11-22T14:03:12.000Z", isoTime(1795356192, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("2026-11-22T14:03:40.500Z", isoTime(1795356192, 28500).c_str());
    TEST_ASSERT_EQUAL_STRING("2024-02-29T23:59:59.000Z", isoTime(1709251199, 0).c_str());
    TEST_ASSERT_EQUAL_STRING("", isoTime(12345, 0).c_str()); // clock never set
    TEST_ASSERT_EQUAL_STRING("000042", padShotId(42).c_str());
}

void test_urls() {
    Endpoints ep;
    ep.projectId = "demo-project";
    ep.apiKey = "demo-key";
    TEST_ASSERT_TRUE(ep.configured());
    TEST_ASSERT_FALSE(Endpoints().configured());
    ShotMeta m = metaFor("Classic", 30000);
    const std::string code = "abcdefghijkmnpqrstuvwxyz2345";
    TEST_ASSERT_EQUAL_STRING("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=demo-key",
                             signUpUrl(ep).c_str());
    TEST_ASSERT_EQUAL_STRING("https://securetoken.googleapis.com/v1/token?key=demo-key",
                             refreshUrl(ep).c_str());
    TEST_ASSERT_EQUAL_STRING("https://firestore.googleapis.com/v1/projects/demo-project/databases/(default)/documents/households/"
                             "abcdefghijkmnpqrstuvwxyz2345/inbox?documentId=gm-000042-1795000000",
                             createDocumentUrl(ep, code, m).c_str());
    Endpoints local = Endpoints::local("127.0.0.1:9199");
    local.apiKey = "demo-key";
    TEST_ASSERT_EQUAL_STRING("http://127.0.0.1:9199/identitytoolkit.googleapis.com/v1/accounts:signUp?key="
                             "demo-key",
                             signUpUrl(local).c_str());
    TEST_ASSERT_EQUAL_STRING("{\"returnSecureToken\":true}", signUpBody().c_str());
    TEST_ASSERT_EQUAL_STRING("grant_type=refresh_token&refresh_token=AMf-vBx_1", refreshBody("AMf-vBx_1").c_str());
}

void test_document_body() {
    std::vector<uint8_t> bytes = makeSlog("Ana's \"9 bar\"", 28500, 1795356192, 2);
    ShotMeta m;
    TEST_ASSERT_TRUE(readSlogHeader(bytes.data(), bytes.size(), 42, m));
    const std::string body = documentBody(m, bytes.data(), bytes.size(), "gaggimate");
    const std::string expectedStart =
        "{\"fields\":{\"source\":{\"stringValue\":\"gaggimate\"},\"fileName\":{\"stringValue\":\"000042.slog\"},"
        "\"size\":{\"integerValue\":\"572\"},\"shotId\":{\"stringValue\":\"42\"},"
        "\"capturedAt\":{\"stringValue\":\"2026-11-22T14:03:40.500Z\"},"
        "\"profile\":{\"stringValue\":\"Ana's \\\"9 bar\\\"\"},\"utility\":{\"booleanValue\":false},"
        "\"durationMs\":{\"integerValue\":\"28500\"},\"device\":{\"stringValue\":\"gaggimate\"},"
        "\"data\":{\"stringValue\":\"";
    TEST_ASSERT_EQUAL_STRING(expectedStart.c_str(), body.substr(0, expectedStart.size()).c_str());
    const std::string encoded = base64(bytes.data(), bytes.size());
    TEST_ASSERT_EQUAL_STRING((encoded + "\"}}}").c_str(), body.substr(expectedStart.size()).c_str());
    TEST_ASSERT_EQUAL_STRING("U0hPVA", encoded.substr(0, 6).c_str()); // "SHOT", what TOM's isSlog checks

    // No clock: capturedAt is left out rather than sent as 1970.
    ShotMeta noClock = m;
    noClock.startEpoch = 50;
    TEST_ASSERT_EQUAL(std::string::npos, documentBody(noClock, bytes.data(), bytes.size(), "gm").find("capturedAt"));
}

void test_token_parsing() {
    Token t;
    const char *signUp = "{\n  \"kind\": \"identitytoolkit#SignupNewUserResponse\",\n  \"idToken\": \"eyJhbGc.abc.def\",\n"
                         "  \"refreshToken\": \"AMf-vBx_99\",\n  \"expiresIn\": \"3600\",\n  \"localId\": \"u1\"\n}";
    TEST_ASSERT_TRUE(parseToken(signUp, t));
    TEST_ASSERT_EQUAL_STRING("eyJhbGc.abc.def", t.idToken.c_str());
    TEST_ASSERT_EQUAL_STRING("AMf-vBx_99", t.refreshToken.c_str());
    TEST_ASSERT_EQUAL_UINT32(3600, t.expiresInSec);
    const char *refreshed = "{\"access_token\":\"x\",\"expires_in\":\"1800\",\"token_type\":\"Bearer\","
                            "\"refresh_token\":\"AMf-new\",\"id_token\":\"eyJnew\",\"user_id\":\"u1\"}";
    TEST_ASSERT_TRUE(parseToken(refreshed, t));
    TEST_ASSERT_EQUAL_STRING("eyJnew", t.idToken.c_str());
    TEST_ASSERT_EQUAL_STRING("AMf-new", t.refreshToken.c_str());
    TEST_ASSERT_EQUAL_UINT32(1800, t.expiresInSec);
    TEST_ASSERT_FALSE(parseToken("{\"error\":{\"code\":400,\"message\":\"ADMIN_ONLY_OPERATION\"}}", t));
    TEST_ASSERT_EQUAL_STRING("eyJnew", t.idToken.c_str()); // a failed parse leaves the old token alone
    TEST_ASSERT_EQUAL_STRING("400", jsonField("{\"error\":{\"code\": 400}}", "code").c_str());
}

void test_outcomes() {
    TEST_ASSERT_TRUE(classifyCreate(200) == Outcome::SENT);
    TEST_ASSERT_TRUE(classifyCreate(409) == Outcome::SENT); // the same shot was already created
    TEST_ASSERT_TRUE(classifyCreate(401) == Outcome::REAUTH);
    TEST_ASSERT_TRUE(classifyCreate(400) == Outcome::DROP);
    TEST_ASSERT_TRUE(classifyCreate(403) == Outcome::RETRY);
    TEST_ASSERT_TRUE(classifyCreate(429) == Outcome::RETRY);
    TEST_ASSERT_TRUE(classifyCreate(503) == Outcome::RETRY);
    TEST_ASSERT_TRUE(classifyCreate(-1) == Outcome::RETRY);
    TEST_ASSERT_EQUAL_UINT32(0, retryDelayMs(0));
    TEST_ASSERT_EQUAL_UINT32(30000, retryDelayMs(1));
    TEST_ASSERT_EQUAL_UINT32(60000, retryDelayMs(2));
    TEST_ASSERT_EQUAL_UINT32(5 * 60 * 1000, retryDelayMs(50));
}

void test_queue() {
    std::vector<uint32_t> q;
    TEST_ASSERT_TRUE(enqueue(q, 7));
    TEST_ASSERT_FALSE(enqueue(q, 7)); // never queued (or sent) twice
    TEST_ASSERT_TRUE(enqueue(q, 8));
    TEST_ASSERT_EQUAL_STRING("7\n8\n", serializeQueue(q).c_str());
    std::vector<uint32_t> back = parseQueue("7\r\n8\n\ngarbage\n8\n12");
    TEST_ASSERT_EQUAL(3, back.size());
    TEST_ASSERT_EQUAL_UINT32(12, back[2]);
    for (uint32_t i = 100; i < 130; i++) {
        enqueue(q, i);
    }
    TEST_ASSERT_EQUAL(MAX_QUEUE, q.size());
    TEST_ASSERT_EQUAL_UINT32(110, q.front()); // oldest dropped first
}

// A profile name cut in the middle of a character (48-byte field) must not make Firestore refuse the shot.
void test_clean_utf8() {
    TEST_ASSERT_EQUAL_STRING("Classic 9 bar", cleanUtf8("Classic 9 bar").c_str());
    TEST_ASSERT_EQUAL_STRING("Caf\xC3\xA9 \xE2\x80\x93 \xF0\x9F\x98\x80", cleanUtf8("Caf\xC3\xA9 \xE2\x80\x93 \xF0\x9F\x98\x80").c_str());
    TEST_ASSERT_EQUAL_STRING("Caf", cleanUtf8("Caf\xC3").c_str());           // cut after the first byte of two
    TEST_ASSERT_EQUAL_STRING("9 bar ", cleanUtf8("9 bar \xE2\x80").c_str()); // cut inside a dash
    TEST_ASSERT_EQUAL_STRING("ab", cleanUtf8("a\xFF\x80" "b").c_str());      // bytes UTF-8 never uses
    TEST_ASSERT_EQUAL_STRING("", cleanUtf8("\xED\xA0\x80").c_str());        // a UTF-16 surrogate
    std::vector<uint8_t> bytes = makeSlog("Caf\xC3", 30000, 1795000000, 4);
    ShotMeta m;
    TEST_ASSERT_TRUE(readSlogHeader(bytes.data(), bytes.size(), 42, m));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, documentBody(m, bytes.data(), bytes.size(), "gm").find("\"profile\":{\"stringValue\":\"Caf\"}"));
}

void test_send_again_and_activity_list() {
    std::vector<uint32_t> q = {258, 259, 260};
    enqueueFirst(q, 260); // already waiting: moves to the front, no second copy
    TEST_ASSERT_EQUAL_STRING("260\n258\n259\n", serializeQueue(q).c_str());
    enqueueFirst(q, 12); // long gone from the queue
    TEST_ASSERT_EQUAL_STRING("12\n260\n258\n259\n", serializeQueue(q).c_str());
    for (uint32_t i = 300; i < 330; i++) {
        enqueue(q, i);
    }
    enqueueFirst(q, 5);
    TEST_ASSERT_EQUAL(MAX_QUEUE, q.size());
    TEST_ASSERT_EQUAL_UINT32(5, q.front());

    std::string log;
    log = appendLogLine(log, "Shot 1 sent");
    log = appendLogLine(log, "Shot 2 not sent yet:\nOffline");
    TEST_ASSERT_EQUAL_STRING("Shot 1 sent\nShot 2 not sent yet: Offline\n", log.c_str());
    for (int i = 3; i < 30; i++) {
        log = appendLogLine(log, "Shot " + std::to_string(i) + " sent");
    }
    TEST_ASSERT_EQUAL(MAX_LOG_LINES, std::count(log.begin(), log.end(), '\n'));
    TEST_ASSERT_EQUAL(0, log.find("Shot 20 sent\n")); // the oldest lines went
    TEST_ASSERT_NOT_EQUAL(std::string::npos, log.find("Shot 29 sent\n"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_reads_header);
    RUN_TEST(test_filters_like_tom);
    RUN_TEST(test_empty_cup_is_not_sent);
    RUN_TEST(test_household_code);
    RUN_TEST(test_base64_and_time);
    RUN_TEST(test_urls);
    RUN_TEST(test_document_body);
    RUN_TEST(test_token_parsing);
    RUN_TEST(test_outcomes);
    RUN_TEST(test_queue);
    RUN_TEST(test_clean_utf8);
    RUN_TEST(test_send_again_and_activity_list);
    return UNITY_END();
}
