#ifndef KEYLIGHT_JSON_H
#define KEYLIGHT_JSON_H
#include "keylight_core.h"
#include "cJSON.h"
bool kl_json_parse_patch(const cJSON *json, kl_patch *patch, uint32_t *expected, bool *has_expected);
bool kl_json_parse_revision(const cJSON *json, uint32_t *expected, bool *has_expected);
cJSON *kl_json_light(const kl_state *state);
#endif
