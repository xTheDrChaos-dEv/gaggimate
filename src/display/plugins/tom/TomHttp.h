#ifndef TOM_HTTP_H
#define TOM_HTTP_H

#include <string>

namespace tom {

struct HttpResult {
    int status = -1;      // HTTP status, or -1 when no response arrived (offline, DNS, TLS)
    std::string body;     // response body, cut at maxBody bytes
    std::string error;    // short reason when status is -1
};

// One blocking HTTPS POST. On the device it uses esp_http_client with the firmware's reviewed CA list
// (lib/OTA/certs/ca_roots.pem, which already holds the Google Trust Services roots Firebase uses).
// The simulator has no TLS, so there it only speaks plain http:// to a local Firebase mock or emulator.
HttpResult httpPost(const std::string &url, const char *contentType, const std::string &bearer, const std::string &body,
                    size_t maxBody = 4096);

} // namespace tom

#endif // TOM_HTTP_H
