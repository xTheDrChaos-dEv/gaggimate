#include "TomHttp.h"

#ifndef GAGGIMATE_SIM

// esp_http_client.h first: Arduino's esp_crt_bundle.h uses esp_err_t without including esp_err.h.
#include <esp_http_client.h>
// clang-format off
#include <ca_bundle.h>
#include <esp_crt_bundle.h>
// clang-format on

namespace tom {

HttpResult httpPost(const std::string &url, const char *contentType, const std::string &bearer, const std::string &body,
                    size_t maxBody) {
    HttpResult result;
    if (url.compare(0, 8, "https://") != 0) {
        result.error = "refusing non-HTTPS URL";
        return result;
    }
    arduino_esp_crt_bundle_set(gm_ca_bundle); // same trust list as the OTA updater (GM-296)
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = arduino_esp_crt_bundle_attach;
    config.timeout_ms = 15000;
    config.buffer_size = 1536;
    config.buffer_size_tx = 1024;
    config.user_agent = "GaggiMate-TOM";
    config.disable_auto_redirect = true;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        result.error = "http client init failed";
        return result;
    }
    esp_http_client_set_header(client, "Content-Type", contentType);
    std::string auth;
    if (!bearer.empty()) {
        auth = "Bearer " + bearer;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    esp_err_t err = esp_http_client_open(client, static_cast<int>(body.size()));
    if (err != ESP_OK) {
        // "ESP_ERR_HTTP_CONNECT" alone covers DNS, TCP and TLS; the socket error narrows it down
        // (12 = out of memory, 113 = no route, 104 = connection reset, 11 = timed out).
        result.error = std::string(esp_err_to_name(err)) + ", errno " + std::to_string(esp_http_client_get_errno(client));
        esp_http_client_cleanup(client);
        return result;
    }
    size_t written = 0;
    while (written < body.size()) {
        const int n = esp_http_client_write(client, body.data() + written, static_cast<int>(body.size() - written));
        if (n <= 0) {
            result.error = "write failed after " + std::to_string(written) + " of " + std::to_string(body.size()) +
                           " bytes, errno " + std::to_string(esp_http_client_get_errno(client));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return result;
        }
        written += static_cast<size_t>(n);
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        result.error = "no response headers";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return result;
    }
    result.status = esp_http_client_get_status_code(client);
    char buf[256];
    while (result.body.size() < maxBody) {
        const int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        result.body.append(buf, static_cast<size_t>(n));
    }
    if (result.body.size() > maxBody) {
        result.body.resize(maxBody);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

} // namespace tom

#else // GAGGIMATE_SIM: plain HTTP over host sockets, for a local Firebase mock or the Firebase emulators

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace tom {

HttpResult httpPost(const std::string &url, const char *contentType, const std::string &bearer, const std::string &body,
                    size_t maxBody) {
    HttpResult result;
    if (url.compare(0, 7, "http://") != 0) {
        result.error = "simulator has no TLS; set GAGGIMATE_TOM_EMULATOR";
        return result;
    }
    const size_t hostStart = 7;
    const size_t pathStart = url.find('/', hostStart);
    const std::string hostPort = url.substr(hostStart, pathStart - hostStart);
    const std::string path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
    const size_t colon = hostPort.find(':');
    const std::string host = hostPort.substr(0, colon);
    const int port = colon == std::string::npos ? 80 : atoi(hostPort.c_str() + colon + 1);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    unsigned a = 0, b = 0, c = 0, d = 0;
    const std::string ip = host == "localhost" ? "127.0.0.1" : host;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        result.error = "simulator needs a numeric emulator address";
        return result;
    }
    addr.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
    const int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        result.error = "socket failed";
        return result;
    }
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        result.error = "connect failed";
        close(fd);
        return result;
    }
    std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + hostPort + "\r\nContent-Type: " + contentType +
                          "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n";
    if (!bearer.empty()) {
        request += "Authorization: Bearer " + bearer + "\r\n";
    }
    request += "\r\n";
    request += body;
    size_t sent = 0;
    while (sent < request.size()) {
        const int n = static_cast<int>(send(fd, request.data() + sent, static_cast<int>(request.size() - sent), 0));
        if (n <= 0) {
            result.error = "send failed";
            close(fd);
            return result;
        }
        sent += static_cast<size_t>(n);
    }
    std::string response;
    char buf[1024];
    while (true) {
        const int n = static_cast<int>(recv(fd, buf, sizeof(buf), 0));
        if (n <= 0) {
            break;
        }
        response.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    int status = 0;
    if (sscanf(response.c_str(), "HTTP/%*s %d", &status) != 1) {
        result.error = "bad response";
        return result;
    }
    result.status = status;
    const size_t split = response.find("\r\n\r\n");
    if (split != std::string::npos) {
        result.body = response.substr(split + 4, maxBody);
    }
    return result;
}

} // namespace tom

#endif
