#ifndef USERS_H
#define USERS_H

#include <Arduino.h>
#include <vector>

// People who share the machine. Each keeps a display theme and their own brew defaults; whoever is active
// "owns" the live settings, and a switch stores them for the person leaving and restores the other's.
// Everything else (Wi-Fi, calibration, hardware, plugins) stays shared.
struct MachineUser {
    String name;
    int theme = 0;            // Settings::themeMode value
    int background = 3;       // Settings::backgroundStyle value (default: drifting coffee beans)
    bool hasDefaults = false; // false until this person has been active once; their first wake keeps the live values
    String selectedProfile;
    std::vector<String> favorites;
    bool volumetricTarget = false;
    int steamTemp = 0;
    int waterTemp = 0;
    double grindVolume = 0.0;
    int grindDuration = 0;
    std::vector<String> privateProfiles; // profile ids only this person sees in the web profile list

    char letter() const; // upper-case initial for the "who's brewing" screen, '?' without a usable name
};

namespace users {

constexpr size_t MAX_USERS = 4;
constexpr size_t MAX_NAME_LENGTH = 16;

// Stored as one JSON array in the settings store (usr_cfg). Unknown or malformed input yields an empty list.
std::vector<MachineUser> parse(const String &json);
String serialize(const std::vector<MachineUser> &list);

// Merges a list edited in the web settings (names, themes, private profile ids) with the stored one, keeping each
// person's saved defaults by name. Blank names are dropped and the list is capped at MAX_USERS.
std::vector<MachineUser> merge(const std::vector<MachineUser> &stored, const std::vector<MachineUser> &edited);

int indexOf(const std::vector<MachineUser> &list, const String &name); // -1 when absent

} // namespace users

#endif // USERS_H
