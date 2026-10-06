#include "keylight_policy.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    const char *valid[] = {"mqtt://broker", "mqtts://broker.local:8883", "mqtt://192.0.2.1:1883", "mqtt://a:65535"};
    const char *invalid[] = {"mqtt://", "mqtts://", "mqtt://user:secret@broker", "mqtt://user%40broker", "mqtt://broker/secret", "mqtt://broker?password=x", "mqtt://broker#x", "mqtt://broker:0", "mqtt://broker:65536", "mqtt://broker:9999999999999999999999", "mqtt://broker:", "mqtt://broker:1:2", "mqtt://:123", "mqtt://host\n", "http://host", "mqtt://host name", "mqtt://host@"};
    for (unsigned i = 0; i < sizeof(valid) / sizeof(*valid); i++) assert(kl_mqtt_uri_valid(valid[i], true));
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        assert(!kl_mqtt_uri_valid(invalid[i], true));
        assert(!kl_mqtt_uri_valid(invalid[i], false));
    }
    assert(kl_mqtt_uri_valid("", false)); assert(!kl_mqtt_uri_valid("", true));
    assert(!kl_mqtt_uri_valid(NULL, false));
    assert(kl_client_label_valid("Studio desk"));
    assert(!kl_client_label_valid("")); assert(!kl_client_label_valid("bad\nname"));
    assert(!kl_client_label_valid("012345678901234567890123456789012"));
    puts("Settings URI and client-label policy checks passed");
    return 0;
}
