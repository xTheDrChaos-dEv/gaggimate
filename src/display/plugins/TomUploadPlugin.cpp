#include "TomUploadPlugin.h"

#include <LittleFS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <display/core/Controller.h>
#include <display/plugins/tom/TomHttp.h>
#include <esp_heap_caps.h>

// A firmware built on the owner's own PC may carry the TOM project in an untracked header. It is copied into
// the settings store once, so firmware published without it (and wireless updates) keeps working on that screen.
#if __has_include(<display/plugins/tom/TomLocalConfig.h>)
#include <display/plugins/tom/TomLocalConfig.h>
#endif

static constexpr const char *TAG = "TomUpload";
static constexpr const char *STATE_DIR = "/tom";
static constexpr const char *QUEUE_PATH = "/tom/queue.txt";
static constexpr const char *AUTH_PATH = "/tom/auth.txt"; // refresh token only; LittleFS, never the removable SD card
static constexpr unsigned long POLL_MS = 2000;
// mbedTLS needs roughly 40-50 KB of internal RAM for one handshake; below this, wait instead of failing mid-way.
static constexpr size_t MIN_INTERNAL_BLOCK = 36 * 1024;
static constexpr uint32_t TASK_STACK = 10 * 1024;

namespace {
std::mutex statusLock;
String statusText = "Starts after the next restart";

std::string readFile(fs::FS &fs, const char *path, size_t limit, size_t *sizeOut = nullptr) {
    std::string out;
    File f = fs.open(path, FILE_READ);
    if (!f) {
        return out;
    }
    const size_t size = f.size();
    if (sizeOut != nullptr) {
        *sizeOut = size;
    }
    if (size <= limit) {
        out.resize(size);
        const size_t got = f.read(reinterpret_cast<uint8_t *>(&out[0]), size);
        out.resize(got);
    }
    f.close();
    return out;
}

bool writeFile(fs::FS &fs, const char *path, const std::string &text) {
    if (!fs.exists(STATE_DIR)) {
        fs.mkdir(STATE_DIR);
    }
    File f = fs.open(path, FILE_WRITE);
    if (!f) {
        return false;
    }
    const size_t n = f.write(reinterpret_cast<const uint8_t *>(text.data()), text.size());
    f.close();
    return n == text.size();
}

String clockText() {
    time_t now = time(nullptr);
    if (now < 1577836800) {
        return "";
    }
    struct tm local {};
    localtime_r(&now, &local);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &local);
    return String(" at ") + buf;
}

// Shown after a send so heap use can be read off the web UI without a serial console.
String heapText() {
#ifndef GAGGIMATE_SIM
    return " (heap: " + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024) + " KB free, lowest since boot " +
           String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024) + " KB)";
#else
    return "";
#endif
}
} // namespace

String TomUploadPlugin::status() {
    std::lock_guard<std::mutex> guard(statusLock);
    return statusText;
}

void TomUploadPlugin::setStatus(const String &text) {
    std::lock_guard<std::mutex> guard(statusLock);
    statusText = text;
}

void TomUploadPlugin::seedLocalConfig(Settings &settings) {
#if defined(TOM_LOCAL_PROJECT_ID) && defined(TOM_LOCAL_WEB_API_KEY)
    if (settings.getTomProject().length() == 0 || settings.getTomApiKey().length() == 0) {
        settings.setTomProject(TOM_LOCAL_PROJECT_ID);
        settings.setTomApiKey(TOM_LOCAL_WEB_API_KEY);
        ESP_LOGI(TAG, "TOM project stored in settings from the local build");
    }
#else
    (void)settings;
#endif
}

void TomUploadPlugin::setup(Controller *c, PluginManager *pm) {
    controller = c;
#ifdef GAGGIMATE_SIM
    // No TLS in the simulator: GAGGIMATE_TOM_EMULATOR=127.0.0.1:9199 sends to a local Firebase mock/emulator.
    const char *emulator = getenv("GAGGIMATE_TOM_EMULATOR");
    networkAvailable = emulator != nullptr && emulator[0] != '\0';
    if (networkAvailable) {
        endpoints = tom::Endpoints::local(emulator);
    }
#endif
    loadState();
    setStatus(queue.empty() ? "Waiting for the next shot" : "Sending queued shots");
    // evt:history-shot-saved fires once the .slog is closed and indexed, and only for shots ShotHistory keeps.
    // (history:shot:recorded fires a moment earlier with no shot id, right before shots under 7.5 s are deleted.)
    pm->on("evt:history-shot-saved", [this](Event const &event) { onShotSaved(static_cast<uint32_t>(event.getInt("id"))); });
#ifndef GAGGIMATE_SIM
    xTaskCreatePinnedToCore(taskMain, "TomUpload", TASK_STACK, this, 1, nullptr, 0);
#endif
    ESP_LOGI(TAG, "TOM upload ready, %u shot(s) queued", static_cast<unsigned>(queue.size()));
}

void TomUploadPlugin::loop() {
#ifdef GAGGIMATE_SIM
    // The simulator runs no FreeRTOS tasks; it pumps plugin loops on its main thread instead.
    static unsigned long last = 0;
    if (millis() - last >= POLL_MS) {
        last = millis();
        step();
    }
#endif
}

void TomUploadPlugin::taskMain(void *arg) {
    auto *self = static_cast<TomUploadPlugin *>(arg);
    while (true) {
        self->step();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void TomUploadPlugin::onShotSaved(uint32_t shotId) {
    // Runs on the shot-history task: only note the id, the upload task does the rest.
    // Utility profiles (BrewProcess::isUtility) never reach this event: ShotHistoryPlugin::startRecording skips
    // them. Asking the controller for its last process here would be wrong, as a flush started during the
    // extended weight recording can already have replaced the shot. Cleaning-like names are filtered at upload.
    std::lock_guard<std::mutex> guard(queueLock);
    if (tom::enqueue(queue, shotId)) {
        queueDirty = true;
        nextTryAt = 0; // a new shot is worth an immediate try even after earlier failures
        failures = 0;
    }
}

void TomUploadPlugin::step() {
    Settings &settings = controller->getSettings();
    {
        std::lock_guard<std::mutex> guard(queueLock);
        if (queueDirty) {
            queueDirty = false;
            saveQueue();
        }
    }
    if (!settings.isTomUploadActive()) {
        setStatus("Off");
        return;
    }
    const std::string household = tom::extractHouseholdCode(settings.getTomHousehold().c_str());
    if (household.empty()) {
        setStatus("Paste your TOM link to start sending shots");
        return;
    }
    endpoints.projectId = settings.getTomProject().c_str();
    endpoints.apiKey = settings.getTomApiKey().c_str();
    if (!endpoints.configured()) {
        setStatus("This screen has no TOM project stored yet");
        return;
    }
    uint32_t head = 0;
    size_t pending = 0;
    {
        std::lock_guard<std::mutex> guard(queueLock);
        pending = queue.size();
        if (pending == 0) {
            return;
        }
        head = queue.front();
    }
    if (!networkAvailable) {
        setStatus("Simulator: no network (set GAGGIMATE_TOM_EMULATOR), " + String(pending) + " queued");
        return;
    }
    if (static_cast<long>(millis() - nextTryAt) < 0) {
        return;
    }
    // Brewing, steaming, grinding or updating comes first; send once the machine is idle again.
    if (controller->isActive() || controller->isUpdating()) {
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        setStatus("Waiting for Wi-Fi, " + String(pending) + (pending == 1 ? " shot" : " shots") + " queued");
        return;
    }
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (largest < MIN_INTERNAL_BLOCK) {
        failed("Low memory (" + String(largest / 1024) + " KB block free)");
        return;
    }
    if (uploadHead(head, household)) {
        std::lock_guard<std::mutex> guard(queueLock);
        if (!queue.empty() && queue.front() == head) {
            queue.erase(queue.begin());
        }
        saveQueue();
        queueDirty = false;
    }
}

bool TomUploadPlugin::uploadHead(uint32_t shotId, const std::string &household) {
    fs::FS &historyFs = controller->isSDCard() ? static_cast<fs::FS &>(SD_MMC) : static_cast<fs::FS &>(LittleFS);
    const std::string path = "/h/" + tom::padShotId(shotId) + ".slog";
    size_t fileSize = 0;
    std::string bytes = readFile(historyFs, path.c_str(), tom::MAX_SLOG_BYTES, &fileSize);
    if (fileSize > tom::MAX_SLOG_BYTES) {
        setStatus("Shot " + String(shotId) + " not sent: " + tom::skipText(tom::Skip::TOO_BIG));
        return true;
    }
    if (bytes.empty()) {
        ESP_LOGW(TAG, "Shot %u is gone from history, dropped", shotId);
        setStatus("Shot " + String(shotId) + " was deleted before it could be sent");
        return true;
    }
    tom::ShotMeta meta;
    if (!tom::readSlogHeader(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), shotId, meta)) {
        setStatus("Shot " + String(shotId) + " skipped: not a shot file");
        return true;
    }
    const tom::Skip skip = tom::shouldSkip(meta);
    if (skip != tom::Skip::NONE) {
        ESP_LOGI(TAG, "Shot %u not sent: %s", shotId, tom::skipText(skip));
        setStatus("Shot " + String(shotId) + " not sent: " + tom::skipText(skip));
        return true;
    }
    std::string body = tom::documentBody(meta, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(),
                                         controller->getSettings().getMdnsName().c_str());
    std::string().swap(bytes);

    lowestFreeSeen = logHeap("before sign-in");
    if (!ensureToken()) {
        return false;
    }
    lowestFreeSeen = std::min(lowestFreeSeen, logHeap("before upload"));
    tom::HttpResult res =
        tom::httpPost(tom::createDocumentUrl(endpoints, household, meta), "application/json", token.idToken, body, 512);
    lowestFreeSeen = std::min(lowestFreeSeen, logHeap("after upload"));
    switch (tom::classifyCreate(res.status)) {
    case tom::Outcome::SENT:
        ESP_LOGI(TAG, "Shot %u sent to TOM (%u bytes, HTTP %d)", shotId, static_cast<unsigned>(meta.size), res.status);
        failures = 0;
        nextTryAt = 0;
        setStatus("Sent shot " + String(shotId) + clockText() + heapText());
        return true;
    case tom::Outcome::REAUTH:
        token.idToken.clear(); // expired or revoked; sign in again on the next pass
        failed("Sign-in expired, retrying");
        return false;
    case tom::Outcome::DROP:
        ESP_LOGE(TAG, "TOM rejected shot %u: HTTP %d %s", shotId, res.status, res.body.c_str());
        failures = 0;
        setStatus("TOM rejected shot " + String(shotId) + " (HTTP " + String(res.status) + ")");
        return true;
    case tom::Outcome::RETRY:
    default:
        failed(res.status < 0 ? String("Offline: ") + res.error.c_str() : "TOM answered HTTP " + String(res.status));
        return false;
    }
}

bool TomUploadPlugin::ensureToken() {
    if (!token.idToken.empty() && static_cast<long>(tokenExpiresAt - millis()) > 60000) {
        return true;
    }
    if (!token.refreshToken.empty()) {
        if (refresh()) {
            return true;
        }
        if (!token.refreshToken.empty()) {
            return false; // offline or a server error: keep this sign-in and retry later, no new anonymous user
        }
    }
    return signUp(); // first run, or the refresh token was refused
}

bool TomUploadPlugin::refresh() {
    tom::HttpResult res = tom::httpPost(tom::refreshUrl(endpoints), "application/x-www-form-urlencoded", "",
                                        tom::refreshBody(token.refreshToken), 4096);
    tom::Token fresh;
    if (res.status == 200 && tom::parseToken(res.body, fresh)) {
        token = fresh;
        tokenExpiresAt = millis() + token.expiresInSec * 1000UL;
        saveAuth();
        return true;
    }
    if (res.status >= 400 && res.status < 500) {
        ESP_LOGW(TAG, "Refresh token refused (HTTP %d), signing in again", res.status);
        token = tom::Token();
        saveAuth();
    } else {
        failed(res.status < 0 ? String("Offline: ") + res.error.c_str() : "Token refresh HTTP " + String(res.status));
    }
    return false;
}

bool TomUploadPlugin::signUp() {
    tom::HttpResult res = tom::httpPost(tom::signUpUrl(endpoints), "application/json", "", tom::signUpBody(), 4096);
    tom::Token fresh;
    if (res.status == 200 && tom::parseToken(res.body, fresh)) {
        token = fresh;
        tokenExpiresAt = millis() + token.expiresInSec * 1000UL;
        saveAuth();
        ESP_LOGI(TAG, "Signed in to TOM anonymously");
        return true;
    }
    if (res.status > 0) {
        // e.g. 400 ADMIN_ONLY_OPERATION when anonymous sign-in is off, 403 when the API key refuses this client
        ESP_LOGE(TAG, "Anonymous sign-in failed: HTTP %d %s", res.status, res.body.c_str());
        failed("Sign-in refused (HTTP " + String(res.status) + ")");
    } else {
        failed(String("Offline: ") + res.error.c_str());
    }
    return false;
}

void TomUploadPlugin::failed(const String &why) {
    failures++;
    const uint32_t wait = tom::retryDelayMs(failures);
    nextTryAt = millis() + wait;
    size_t pending;
    {
        std::lock_guard<std::mutex> guard(queueLock);
        pending = queue.size();
    }
    ESP_LOGW(TAG, "%s; retry in %u s", why.c_str(), static_cast<unsigned>(wait / 1000));
    setStatus(why + ", " + String(pending) + " queued, retry in " + String(wait / 60000 ? wait / 60000 : wait / 1000) +
              (wait / 60000 ? " min" : " s"));
}

void TomUploadPlugin::loadState() {
    queue = tom::parseQueue(readFile(LittleFS, QUEUE_PATH, 1024));
    token.refreshToken = tom::jsonField(readFile(LittleFS, AUTH_PATH, 2048), "refreshToken");
}

void TomUploadPlugin::saveQueue() {
    // Caller holds queueLock.
    if (!writeFile(LittleFS, QUEUE_PATH, tom::serializeQueue(queue))) {
        ESP_LOGE(TAG, "Could not save the upload queue");
    }
}

void TomUploadPlugin::saveAuth() {
    const std::string text =
        token.refreshToken.empty() ? std::string("{}") : "{\"refreshToken\":\"" + token.refreshToken + "\"}";
    writeFile(LittleFS, AUTH_PATH, text);
}

size_t TomUploadPlugin::logHeap(const char *when) {
    const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#ifndef GAGGIMATE_SIM
    const size_t lowest = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const unsigned stackLeft = uxTaskGetStackHighWaterMark(nullptr);
#else
    const size_t lowest = 0; // the host shim has no allocator statistics
    const unsigned stackLeft = 0;
#endif
    ESP_LOGI(TAG, "Heap %s: internal free %u, largest block %u, lowest since boot %u, task stack left %u", when,
             static_cast<unsigned>(freeInternal), static_cast<unsigned>(largest), static_cast<unsigned>(lowest), stackLeft);
    return freeInternal;
}
