#include "ls_networks.h"
#include "ls_control.h"
#include <string.h>
bool ls_networks_parse(const uint8_t* data, size_t size, LsNetworks* networks) {
    if (!data || !networks || size < 8 || data[0] != 1 || data[1] > 2 || data[2] > LS_NETWORK_MAX ||
        data[3] || size != 8u + data[2] * 36u)
        return false;
    unsigned saved_slots = 0;
    for (unsigned i = 0; i < data[2]; ++i) {
        const uint8_t* entry = data + 8 + i * 36;
        if (!entry[0] || !memchr(entry, 0, 33) || entry[34] > 2 || entry[35] > 8)
            return false;
        if (entry[35]) {
            unsigned bit = 1u << entry[35];
            if (saved_slots & bit)
                return false;
            saved_slots |= bit;
        }
    }
    memset(networks, 0, sizeof(*networks));
    networks->state = data[1];
    networks->count = data[2];
    networks->error = (int32_t)ls_read32(data + 4);
    for (unsigned i = 0; i < data[2]; ++i) {
        const uint8_t* entry = data + 8 + i * 36;
        memcpy(networks->entries[i].ssid, entry, 33);
        networks->entries[i].rssi = (int8_t)entry[33];
        networks->entries[i].security = entry[34];
        networks->entries[i].saved_slot = entry[35];
    }
    return true;
}
