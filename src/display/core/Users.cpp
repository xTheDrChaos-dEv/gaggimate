#include "Users.h"

#include <ArduinoJson.h>
#include <cctype>

char MachineUser::letter() const {
    for (size_t i = 0; i < name.length(); i++) {
        const char c = name.charAt(i);
        if (isalpha(static_cast<unsigned char>(c))) {
            return static_cast<char>(toupper(static_cast<unsigned char>(c)));
        }
    }
    return '?';
}

namespace users {

static String cleanName(const String &raw) {
    String name = raw;
    name.trim();
    if (name.length() > MAX_NAME_LENGTH) {
        name = name.substring(0, MAX_NAME_LENGTH);
    }
    return name;
}

std::vector<MachineUser> parse(const String &json) {
    std::vector<MachineUser> list;
    if (json.length() == 0) {
        return list;
    }
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok || !doc.is<JsonArray>()) {
        return list;
    }
    for (JsonObject item : doc.as<JsonArray>()) {
        if (list.size() >= MAX_USERS) {
            break;
        }
        MachineUser user;
        user.name = cleanName(item["name"] | "");
        if (user.name.length() == 0) {
            continue;
        }
        user.theme = item["theme"] | 0;
        user.hasDefaults = item["set"] | false;
        user.selectedProfile = item["sp"] | "";
        for (JsonVariant id : item["fp"].as<JsonArray>()) {
            user.favorites.push_back(id.as<String>());
        }
        user.volumetricTarget = item["vt"] | false;
        user.steamTemp = item["ts"] | 0;
        user.waterTemp = item["tw"] | 0;
        user.grindVolume = item["tgv"] | 0.0;
        user.grindDuration = item["tgd"] | 0;
        for (JsonVariant id : item["private"].as<JsonArray>()) {
            user.privateProfiles.push_back(id.as<String>());
        }
        list.push_back(user);
    }
    return list;
}

String serialize(const std::vector<MachineUser> &list) {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    for (const MachineUser &user : list) {
        JsonObject item = array.add<JsonObject>();
        item["name"] = user.name;
        item["theme"] = user.theme;
        item["set"] = user.hasDefaults;
        if (user.hasDefaults) {
            item["sp"] = user.selectedProfile;
            JsonArray favorites = item["fp"].to<JsonArray>();
            for (const String &id : user.favorites) {
                favorites.add(id);
            }
            item["vt"] = user.volumetricTarget;
            item["ts"] = user.steamTemp;
            item["tw"] = user.waterTemp;
            item["tgv"] = user.grindVolume;
            item["tgd"] = user.grindDuration;
        }
        JsonArray privateIds = item["private"].to<JsonArray>();
        for (const String &id : user.privateProfiles) {
            privateIds.add(id);
        }
    }
    String out;
    serializeJson(doc, out);
    return out;
}

int indexOf(const std::vector<MachineUser> &list, const String &name) {
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].name.equalsIgnoreCase(name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<MachineUser> merge(const std::vector<MachineUser> &stored, const std::vector<MachineUser> &edited) {
    std::vector<MachineUser> result;
    for (const MachineUser &incoming : edited) {
        if (result.size() >= MAX_USERS) {
            break;
        }
        const String name = cleanName(incoming.name);
        if (name.length() == 0 || indexOf(result, name) >= 0) {
            continue; // blank or a second entry with the same name
        }
        MachineUser user;
        const int known = indexOf(stored, name);
        if (known >= 0) {
            user = stored[known]; // keeps this person's saved defaults
        }
        user.name = name;
        user.theme = incoming.theme;
        user.privateProfiles = incoming.privateProfiles;
        result.push_back(user);
    }
    return result;
}

} // namespace users
