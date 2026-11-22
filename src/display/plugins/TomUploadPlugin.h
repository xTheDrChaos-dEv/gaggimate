#ifndef TOMUPLOADPLUGIN_H
#define TOMUPLOADPLUGIN_H

#include <Arduino.h>
#include <display/core/Plugin.h>
#include <display/plugins/tom/TomInbox.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Sends each finished shot to TOM's Firestore shot inbox (households/<code>/inbox), where it waits for Keep or Skip.
// See TOM/README.md "Shot inbox". Signs in anonymously like the phones, keeps a small persistent queue while
// offline and does all network work on its own low-priority task, never while a brew/steam/grind is running.
class TomUploadPlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

    // One line for the web settings card, e.g. "Sent shot 42 at 14:03" or "Waiting for Wi-Fi, 2 shots queued".
    static String status();

  private:
    void onShotSaved(uint32_t shotId);
    void step();
    // Returns true when the queue head was handled (sent, already there, or dropped for good).
    bool uploadHead(uint32_t shotId, const std::string &household);
    bool ensureToken();
    bool signUp();
    bool refresh();
    void loadState();
    void saveQueue();
    void saveAuth();
    void setStatus(const String &text);
    void failed(const String &why);
    size_t logHeap(const char *when);

    static void taskMain(void *arg);

    Controller *controller = nullptr;
    tom::Endpoints endpoints;
    bool networkAvailable = true; // false in the simulator unless GAGGIMATE_TOM_EMULATOR points at a mock

    std::mutex queueLock;
    std::vector<uint32_t> queue; // shot ids waiting to be sent, oldest first
    bool queueDirty = false;

    tom::Token token;
    unsigned long tokenExpiresAt = 0; // millis()
    std::atomic<uint32_t> failures{0};   // also reset from the shot-history task
    std::atomic<unsigned long> nextTryAt{0};
    size_t lowestFreeSeen = 0; // lowest internal free heap sampled around the last upload
};

#endif // TOMUPLOADPLUGIN_H
