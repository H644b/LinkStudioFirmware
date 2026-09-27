#include "update_contract.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HASH "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
static const char release_json[] = "{\"tag_name\":\"fw-2-abcd1234\",\"draft\":false,\"prerelease\":false,\"assets\":[{\"name\":\"firmware.json\",\"id\":12,\"size\":100,\"state\":\"uploaded\",\"digest\":\"sha256:" HASH "\"},{\"name\":\"pokeldn_radio.bin\",\"id\":13,\"size\":1024,\"state\":\"uploaded\",\"digest\":\"sha256:" HASH "\"}]}";
static const char manifest_json[] = "{\"schema_version\":1,\"repository\":\"H644b/LinkStudioFirmware\",\"version\":\"fw-2-abcd1234\",\"target\":\"esp32s2\",\"variant\":\"usb-midi\",\"layout\":\"ota-v1\",\"flash_bytes\":4194304,\"capabilities\":[\"uart-v1\",\"wifi-update-v1\",\"ota-v1\"],\"images\":[{\"file\":\"pokeldn_radio.bin\",\"address\":65536,\"bytes\":1024,\"sha256\":\"" HASH "\"}]}";
static void release_reject(cJSON *j) {
    char *text = cJSON_PrintUnformatted(j); UpdateRelease r;
    assert(!update_release_parse(text, strlen(text), &r)); free(text); cJSON_Delete(j);
}
static void manifest_reject(cJSON *j, const UpdateRelease *r) {
    char *text = cJSON_PrintUnformatted(j);
    assert(!update_manifest_check(text, strlen(text), r)); free(text); cJSON_Delete(j);
}
int main(void) {
    UpdateRelease r;
    assert(update_release_parse(release_json, strlen(release_json), &r));
    assert(r.image.id == 13 && r.image.size == 1024 && r.image.sha256[0] == 0xaa);
    assert(update_manifest_check(manifest_json, strlen(manifest_json), &r));
    cJSON *j = cJSON_Parse(release_json);
    cJSON_ReplaceItemInObjectCaseSensitive(j,"draft",cJSON_CreateTrue()); release_reject(j);
    j = cJSON_Parse(release_json);
    cJSON_ReplaceItemInObjectCaseSensitive(j,"tag_name",cJSON_CreateString("../evil")); release_reject(j);
    j = cJSON_Parse(release_json);
    cJSON *assets = cJSON_GetObjectItem(j,"assets");
    cJSON_AddItemToArray(assets,cJSON_Duplicate(cJSON_GetArrayItem(assets,1),1)); release_reject(j);
    j = cJSON_Parse(release_json);
    cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItem(j,"assets"),1),"size",cJSON_CreateNumber(UPDATE_IMAGE_MAX+1)); release_reject(j);
    j = cJSON_Parse(release_json);
    cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItem(j,"assets"),1),"id",cJSON_CreateNumber(12.5)); release_reject(j);
    j = cJSON_Parse(manifest_json);
    cJSON_ReplaceItemInObjectCaseSensitive(j,"target",cJSON_CreateString("esp32")); manifest_reject(j,&r);
    j = cJSON_Parse(manifest_json);
    cJSON_ReplaceItemInObjectCaseSensitive(j,"layout",cJSON_CreateString("legacy")); manifest_reject(j,&r);
    j = cJSON_Parse(manifest_json);
    cJSON_ReplaceItemInObjectCaseSensitive(j,"repository",cJSON_CreateString("elsewhere/firmware")); manifest_reject(j,&r);
    j = cJSON_Parse(manifest_json);
    cJSON_DeleteItemFromArray(cJSON_GetObjectItem(j,"capabilities"),0); manifest_reject(j,&r);
    j = cJSON_Parse(manifest_json);
    cJSON_ReplaceItemInObjectCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItem(j,"images"),0),"address",cJSON_CreateNumber(0)); manifest_reject(j,&r);
    UpdateRelease bad = r; bad.image.sha256[4] ^= 1;
    assert(!update_manifest_check(manifest_json, strlen(manifest_json), &bad));
    bad = r; bad.image.size++;
    assert(!update_manifest_check(manifest_json, strlen(manifest_json), &bad));
    assert(!update_release_parse("{} trailing", 11, &r));
    assert(update_download_url_allowed("https://api.github.com/repos/a/b"));
    assert(update_download_url_allowed("https://release-assets.githubusercontent.com/a?sig=123"));
    assert(!update_download_url_allowed("http://github.com/a"));
    assert(!update_download_url_allowed("https://api.github.com.evil.example/a"));
    assert(!update_download_url_allowed("https://github.com@evil.example/a"));
    assert(!update_download_url_allowed("https://github.com:80/a"));
    uint8_t credentials[] = {4, 8, 'h','o','m','e','p','a','s','s','w','o','r','d'};
    assert(update_credentials_valid(credentials,sizeof(credentials)));
    assert(!update_credentials_valid(credentials,sizeof(credentials)-1));
    credentials[1]=0;
    assert(update_credentials_valid(credentials,6));
    credentials[0]=0; assert(!update_credentials_valid(credentials,6));
    credentials[0]=4; credentials[1]=7; assert(!update_credentials_valid(credentials,13));
    credentials[1]=8; credentials[8]=0; assert(!update_credentials_valid(credentials,14));
    /* Truncated documents and arbitrary serial bytes must fail without out-of-bounds reads. */
    for (size_t i=0;i<strlen(release_json);++i) assert(!update_release_parse(release_json,i,&r));
    for (size_t i=0;i<strlen(manifest_json);++i) assert(!update_manifest_check(manifest_json,i,&bad));
    unsigned seed=0xabc123;
    for (unsigned round=0;round<20000;++round) {
        uint8_t bytes[120];
        for(size_t i=0;i<sizeof(bytes);++i) { seed=seed*1664525u+1013904223u;bytes[i]=seed>>24; }
        (void)update_credentials_valid(bytes,round%sizeof(bytes));
        (void)update_release_parse((char*)bytes,round%sizeof(bytes),&r);
    }
    puts("Firmware update contract: release, layout, hashes, redirects, credentials, 20,000 malformed inputs passed");
}
