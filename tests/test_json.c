#include "keylight_json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static bool parse(const char *text, kl_patch *patch) {
    cJSON *json = cJSON_Parse(text); uint32_t revision; bool has_revision;
    bool result = kl_json_parse_patch(json, patch, &revision, &has_revision);
    cJSON_Delete(json); return result;
}
int main(void) {
    const char *invalid[] = {"null", "[]", "{}", "{\"power\":1}", "{\"power\":true,\"power\":false}",
        "{\"brightness\":101}", "{\"brightness\":-1}", "{\"brightness\":0.5}", "{\"temperature_k\":2999}",
        "{\"temperature_k\":7001}", "{\"transition_ms\":10001}", "{\"mode\":\"mixed\"}", "{\"effect\":\"unknown\"}",
        "{\"rgb\":{\"r\":0,\"g\":0,\"b\":256}}", "{\"rgb\":{\"r\":0,\"g\":0}}", "{\"power\":true,\"typo\":1}",
        "{\"rgb\":{\"r\":0,\"g\":0,\"b\":0,\"r\":0}}", "{\"power\":true,\"expected_revision\":-1}",
        "{\"power\":true,\"expected_revision\":4294967296}", "{\"power\":true,\"expected_revision\":0.1}"};
    kl_patch patch;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) assert(!parse(invalid[i], &patch));
    char text[128];
    for (int value = -20; value < 280; value++) {
        snprintf(text, sizeof(text), "{\"brightness\":%d}", value);
        assert(parse(text, &patch) == (value >= 0 && value <= 100));
        snprintf(text, sizeof(text), "{\"rgb\":{\"r\":%d,\"g\":0,\"b\":0}}", value);
        assert(parse(text, &patch) == (value >= 0 && value <= 255));
    }
    assert(parse("{\"power\":true,\"expected_revision\":4294967295}", &patch));
    kl_state state = kl_state_default(); cJSON *json = kl_json_light(&state);
    uint32_t expected; bool has_expected;
    assert(kl_json_parse_patch(json, &patch, &expected, &has_expected));
    assert(patch.fields == KL_ALL_FIELDS && !has_expected);
    kl_state decoded;
    assert(kl_state_patch(&state, &patch, &decoded) == KL_OK);
    assert(decoded.temperature_k == state.temperature_k && decoded.rgb.r == state.rgb.r);
    cJSON_Delete(json);
    const char *revisions[] = {"{}", "{\"expected_revision\":0}", "{\"expected_revision\":4294967295}",
        "{\"expected_revision\":-1}", "{\"expected_revision\":4294967296}", "{\"expected_revision\":0.1}",
        "{\"expected_revision\":0,\"expected_revision\":1}", "{\"power\":true}", "null"};
    for (unsigned i = 0; i < sizeof(revisions) / sizeof(*revisions); i++) {
        json = cJSON_Parse(revisions[i]);
        assert(kl_json_parse_revision(json, &expected, &has_expected) == (i < 3));
        cJSON_Delete(json);
    }
    puts("json: strict types, duplicates, limits, revisions and state round-trip passed");
}
