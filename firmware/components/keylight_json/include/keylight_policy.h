#ifndef KEYLIGHT_POLICY_H
#define KEYLIGHT_POLICY_H
#include <stdbool.h>

/* MQTT credentials belong in separate write-only fields, never in a URI. */
bool kl_mqtt_uri_valid(const char *uri, bool enabled);
bool kl_client_label_valid(const char *label);
#endif
