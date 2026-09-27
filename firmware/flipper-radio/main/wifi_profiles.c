#include "wifi_profiles.h"
#include <string.h>

static bool valid(const WifiProfile *p) {
    const char *end = memchr(p->ssid, 0, sizeof(p->ssid));
    const char *pass = memchr(p->password, 0, sizeof(p->password));
    if (!end || !pass) return false;
    size_t n = (size_t)(pass - p->password);
    if (!p->ssid[0]) return !n;
    if (n && n < 8) return false;
    for (size_t i = 0; i < n; ++i)
        if ((uint8_t)p->password[i] < 32 || (uint8_t)p->password[i] > 126) return false;
    return true;
}
bool wifi_profiles_valid(const WifiProfiles *profiles) {
    if (!profiles) return false;
    for (size_t i = 0; i < WIFI_PROFILE_MAX; ++i) {
        if (!valid(&profiles->entries[i])) return false;
        for (size_t j = 0; j < i; ++j)
            if (profiles->entries[i].ssid[0] &&
                !strcmp(profiles->entries[i].ssid, profiles->entries[j].ssid)) return false;
    }
    return true;
}
int wifi_profiles_find(const WifiProfiles *profiles, const char *ssid) {
    if (!profiles || !ssid || !ssid[0]) return -1;
    for (int i = 0; i < WIFI_PROFILE_MAX; ++i)
        if (!strcmp(profiles->entries[i].ssid, ssid)) return i;
    return -1;
}
int wifi_profiles_put(WifiProfiles *profiles, const char *ssid, const char *password) {
    if (!profiles || !ssid || !password || !ssid[0] || strlen(ssid) > 32 ||
        strlen(password) > 63 || !wifi_profiles_valid(profiles)) return -1;
    WifiProfile candidate = {0};
    memcpy(candidate.ssid, ssid, strlen(ssid));
    memcpy(candidate.password, password, strlen(password));
    if (!valid(&candidate)) return -1;
    int slot = wifi_profiles_find(profiles, ssid);
    if (slot < 0)
        for (int i = 0; i < WIFI_PROFILE_MAX; ++i)
            if (!profiles->entries[i].ssid[0]) { slot = i; break; }
    if (slot >= 0) profiles->entries[slot] = candidate;
    /* The owning caller clears its temporary credentials after persistence. */
    volatile char *p = (volatile char *)&candidate;
    for (size_t i = 0; i < sizeof(candidate); ++i) p[i] = 0;
    return slot;
}
bool wifi_profiles_forget(WifiProfiles *profiles, unsigned slot) {
    if (!profiles || slot >= WIFI_PROFILE_MAX) return false;
    volatile char *p = (volatile char *)&profiles->entries[slot];
    for (size_t i = 0; i < sizeof(WifiProfile); ++i) p[i] = 0;
    return true;
}
