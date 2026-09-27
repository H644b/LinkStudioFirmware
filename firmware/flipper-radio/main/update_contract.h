#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define UPDATE_REPOSITORY "H644b/LinkStudioFirmware"
#define UPDATE_TAG_SIZE 48
#define UPDATE_MANIFEST_MAX 12288
#define UPDATE_IMAGE_MAX 0x1e0000

typedef struct { uint64_t id; size_t size; uint8_t sha256[32]; } UpdateAsset;
typedef struct {
    char tag[UPDATE_TAG_SIZE];
    UpdateAsset manifest, image;
} UpdateRelease;
bool update_release_parse(const char *json, size_t size, UpdateRelease *release);
bool update_manifest_check(const char *json, size_t size, const UpdateRelease *release);
bool update_credentials_valid(const uint8_t *payload, size_t size);
bool update_download_url_allowed(const char *url);
