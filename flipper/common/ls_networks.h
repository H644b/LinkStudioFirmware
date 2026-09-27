#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define LS_NETWORK_MAX 24
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t security, saved_slot;
} LsNetwork;
typedef struct {
    uint8_t state, count;
    int32_t error;
    LsNetwork entries[LS_NETWORK_MAX];
} LsNetworks;
bool ls_networks_parse(const uint8_t* data, size_t size, LsNetworks* networks);
