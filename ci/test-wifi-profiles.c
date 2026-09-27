#include "wifi_profiles.h"
#include "ls_networks.h"
#include "ls_actions.h"
#include "ls_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    WifiProfiles profiles = {0};
    assert(wifi_profiles_valid(&profiles));
    assert(wifi_profiles_put(&profiles, "Test WPA2", "12345678") == 0);
    assert(wifi_profiles_put(&profiles, "Test WPA2", "replacement") == 0);
    assert(!strcmp(profiles.entries[0].password, "replacement"));
    assert(wifi_profiles_put(&profiles, "Open network", "") == 1);
    assert(wifi_profiles_put(&profiles, "Bad", "short") == -1);
    for (int i = 2; i < WIFI_PROFILE_MAX; ++i) {
        char name[16]; snprintf(name, sizeof(name), "Network %d", i);
        assert(wifi_profiles_put(&profiles, name, "abcdefgh") == i);
    }
    assert(wifi_profiles_put(&profiles, "Full", "abcdefgh") == -1);
    assert(wifi_profiles_forget(&profiles, 3));
    for (unsigned i = 0; i < sizeof(WifiProfile); ++i)
        assert(((uint8_t *)&profiles.entries[3])[i] == 0);
    assert(wifi_profiles_put(&profiles, "New", "12345678") == 3);
    WifiProfiles roundtrip;
    memcpy(&roundtrip, &profiles, sizeof(profiles));
    assert(wifi_profiles_valid(&roundtrip));
    roundtrip.entries[1] = roundtrip.entries[0];
    assert(!wifi_profiles_valid(&roundtrip));
    memset(&roundtrip, 'A', sizeof(roundtrip));
    assert(!wifi_profiles_valid(&roundtrip));
    assert(!wifi_profiles_forget(&profiles, WIFI_PROFILE_MAX));
    LsNetworks networks;
    uint8_t packet[8 + LS_NETWORK_MAX * 36] = {1, 0, 1};
    memcpy(packet + 8, "Example network", 16);
    packet[8 + 33] = (uint8_t)-42; packet[8 + 34] = 1; packet[8 + 35] = 1;
    assert(ls_networks_parse(packet, 44, &networks));
    assert(networks.count == 1 && networks.entries[0].rssi == -42 && networks.entries[0].saved_slot == 1);
    for (unsigned n = 0; n < 44; ++n) assert(!ls_networks_parse(packet, n, &networks));
    packet[2] = 2; memcpy(packet + 44, packet + 8, 36);
    assert(!ls_networks_parse(packet, 80, &networks)); /* duplicate saved slot */
    packet[44 + 35] = 0; assert(ls_networks_parse(packet, 80, &networks));
    unsigned idle = ls_actions(true, true, true, true, false, false, LsStateIdle, LsFlagRecipe);
    assert((idle & LsCanStart) && !(idle & (LsCanStop | LsCanOffer | LsCanCancel)));
    unsigned offered = ls_actions(true, true, true, true, false, false, LsStateOffered, LsFlagRecipe | LsFlagPeer);
    assert((offered & LsCanCancel) && (offered & LsCanStop) && !(offered & (LsCanStart | LsCanWifi | LsCanLoad)));
    unsigned saving = ls_actions(true, true, true, true, false, false, LsStateSaving, LsFlagRecipe | LsFlagPeer);
    assert((saving & LsCanStop) && !(saving & (LsCanCancel | LsCanOffer | LsCanWifi)));
    assert(!ls_actions(true, true, true, true, true, false, LsStateIdle, LsFlagRecipe));
    assert(!ls_actions(true, true, true, true, false, true, LsStateIdle, LsFlagRecipe));
    unsigned unsupported = ls_actions(true, false, false, true, false, false, LsStateIdle, LsFlagRecipe);
    assert((unsupported & LsCanWifi) && !(unsupported & (LsCanStart | LsCanOffer | LsCanCancel | LsCanStop)));
    uint32_t rng = 789123;
    for (unsigned i = 0; i < 20000; ++i) {
        for (size_t j = 0; j < sizeof(packet); ++j) {
            rng = rng * 1664525 + 1013904223; packet[j] = rng >> 24;
        }
        ls_networks_parse(packet, i % (sizeof(packet) + 1), &networks);
        memcpy(&roundtrip, packet, sizeof(roundtrip));
        wifi_profiles_valid(&roundtrip);
    }
    puts("Saved profiles, network parser, action guards and 20000 malformed inputs passed");
    return 0;
}
