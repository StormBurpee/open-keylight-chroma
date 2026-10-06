#include "keylight_policy.h"
#include <stddef.h>
#include <string.h>

bool kl_mqtt_uri_valid(const char *uri, bool enabled) {
    if (!uri) return false;
    if (!*uri) return !enabled;
    const char *host;
    if (!strncmp(uri, "mqtt://", 7)) host = uri + 7;
    else if (!strncmp(uri, "mqtts://", 8)) host = uri + 8;
    else return false;
    if (!*host) return false;
    /* Deliberately accept only DNS/IPv4 authorities and an optional TCP port. */
    const char *port = NULL;
    for (const char *p = host; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ':' && !port && p != host) { port = p + 1; continue; }
        if (port) { if (c < '0' || c > '9') return false; }
        else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    if (port) {
        size_t length = strlen(port);
        if (!length || length > 5) return false;
        unsigned value = 0;
        for (const char *p = port; *p; p++) value = value * 10 + (unsigned)(*p - '0');
        if (!value || value > 65535) return false;
    }
    return true;
}

bool kl_client_label_valid(const char *label) {
    if (!label || !*label || strlen(label) > 32) return false;
    for (const unsigned char *p = (const unsigned char *)label; *p; p++)
        if (*p < 32 || *p == 127) return false;
    return true;
}
