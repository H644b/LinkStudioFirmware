#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_PROFILE_MAX 8
typedef struct {
    char ssid[33];
    char password[64];
} WifiProfile;
typedef struct { WifiProfile entries[WIFI_PROFILE_MAX]; } WifiProfiles;
/* Fixed-size NVS v1 blob. Empty slots are zero; never sent over the wire. */
bool wifi_profiles_valid(const WifiProfiles *profiles);
int wifi_profiles_find(const WifiProfiles *profiles, const char *ssid);
int wifi_profiles_put(WifiProfiles *profiles, const char *ssid, const char *password);
bool wifi_profiles_forget(WifiProfiles *profiles, unsigned slot);
