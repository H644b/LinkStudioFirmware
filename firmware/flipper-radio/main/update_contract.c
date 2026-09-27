/* Only repository-controlled release assets can become an OTA candidate. */
#include "update_contract.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>

static const cJSON *get(const cJSON *j, const char *name) { return cJSON_GetObjectItemCaseSensitive(j, name); }
static bool string_is(const cJSON *j, const char *s) { return cJSON_IsString(j) && !strcmp(j->valuestring, s); }
static bool integer(const cJSON *j, double low, double high) {
    return cJSON_IsNumber(j) && isfinite(j->valuedouble) && j->valuedouble >= low &&
           j->valuedouble <= high && floor(j->valuedouble) == j->valuedouble;
}
static bool tag_valid(const char *s) {
    size_t n = strlen(s);
    if (!n || n >= UPDATE_TAG_SIZE) return false;
    for (size_t i = 0; i < n; ++i) {
        bool alpha = (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z');
        bool digit = s[i] >= '0' && s[i] <= '9';
        if (!alpha && !digit && (!i || (s[i] != '-' && s[i] != '_' && s[i] != '.'))) return false;
    }
    return true;
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static bool digest(const cJSON *j, bool prefix, uint8_t out[32]) {
    if (!cJSON_IsString(j)) return false;
    const char *s = j->valuestring;
    if (prefix) { if (strncmp(s, "sha256:", 7)) return false; s += 7; }
    if (strlen(s) != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        int a = hex(s[i * 2]), b = hex(s[i * 2 + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)((a << 4) | b);
    }
    return true;
}
static bool asset(const cJSON *j, size_t max, UpdateAsset *a) {
    const cJSON *id = get(j, "id"), *size = get(j, "size");
    if (!integer(id, 1, 9007199254740991.0) || !integer(size, 1, max) ||
        !string_is(get(j, "state"), "uploaded") || !digest(get(j, "digest"), true, a->sha256)) return false;
    a->id = (uint64_t)id->valuedouble;
    a->size = (size_t)size->valuedouble;
    return true;
}
static cJSON *parse(const char *text, size_t size) {
    if (!text || !size || memchr(text, 0, size)) return NULL;
    const char *end = NULL;
    cJSON *j = cJSON_ParseWithLengthOpts(text, size, &end, false);
    if (!j) return NULL;
    while (end < text + size && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
    if (end != text + size || !cJSON_IsObject(j)) { cJSON_Delete(j); return NULL; }
    return j;
}
bool update_release_parse(const char *json, size_t size, UpdateRelease *release) {
    cJSON *j = parse(json, size);
    if (!j) return false;
    UpdateRelease out = {0};
    const cJSON *tag = get(j, "tag_name"), *assets = get(j, "assets");
    bool ok = cJSON_IsFalse(get(j, "draft")) && cJSON_IsFalse(get(j, "prerelease")) &&
              cJSON_IsString(tag) && tag_valid(tag->valuestring) && cJSON_IsArray(assets);
    int manifests = 0, images = 0;
    if (ok) {
        strcpy(out.tag, tag->valuestring);
        const cJSON *item;
        cJSON_ArrayForEach(item, assets) {
            if (string_is(get(item, "name"), "firmware.json")) {
                ++manifests; ok &= asset(item, UPDATE_MANIFEST_MAX, &out.manifest);
            } else if (string_is(get(item, "name"), "pokeldn_radio.bin")) {
                ++images; ok &= asset(item, UPDATE_IMAGE_MAX, &out.image);
            }
        }
    }
    ok &= manifests == 1 && images == 1;
    if (ok) *release = out;
    cJSON_Delete(j);
    return ok;
}
bool update_manifest_check(const char *json, size_t size, const UpdateRelease *release) {
    cJSON *j = parse(json, size);
    if (!j) return false;
    bool ok = integer(get(j, "schema_version"), 1, 1) &&
        string_is(get(j, "repository"), UPDATE_REPOSITORY) &&
        string_is(get(j, "version"), release->tag) && string_is(get(j, "target"), "esp32s2") &&
        string_is(get(j, "variant"), "usb-midi") && string_is(get(j, "layout"), "ota-v1") &&
        integer(get(j, "flash_bytes"), 0x400000, 0x400000);
    bool uart = false, wifi = false, ota = false;
    const cJSON *item, *caps = get(j, "capabilities");
    if (cJSON_IsArray(caps)) cJSON_ArrayForEach(item, caps) {
        uart |= string_is(item, "uart-v1"); wifi |= string_is(item, "wifi-update-v1");
        ota |= string_is(item, "ota-v1");
    }
    ok &= uart && wifi && ota;
    int images = 0;
    const cJSON *list = get(j, "images");
    if (cJSON_IsArray(list)) cJSON_ArrayForEach(item, list) {
        if (string_is(get(item, "file"), "pokeldn_radio.bin")) {
            ++images;
            uint8_t hash[32];
            ok &= integer(get(item, "address"), 0x10000, 0x10000) &&
                  integer(get(item, "bytes"), release->image.size, release->image.size) &&
                  digest(get(item, "sha256"), false, hash) && !memcmp(hash, release->image.sha256, 32);
        }
    }
    cJSON_Delete(j);
    return ok && images == 1;
}
bool update_credentials_valid(const uint8_t *p, size_t n) {
    if (!p || n < 3 || p[0] < 1 || p[0] > 32 || p[1] > 63 || (p[1] && p[1] < 8) ||
        n != (size_t)2 + p[0] + p[1] || memchr(p + 2, 0, n - 2)) return false;
    for (size_t i = 2 + p[0]; i < n; ++i) if (p[i] < 32 || p[i] > 126) return false;
    return true;
}
bool update_download_url_allowed(const char *url) {
    static const char *hosts[] = {"api.github.com", "github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com"};
    if (strncmp(url, "https://", 8)) return false;
    for (size_t i = 0; i < sizeof(hosts) / sizeof(*hosts); ++i) {
        size_t n = strlen(hosts[i]);
        if (!strncmp(url + 8, hosts[i], n) && url[8 + n] == '/') return true;
    }
    return false;
}
