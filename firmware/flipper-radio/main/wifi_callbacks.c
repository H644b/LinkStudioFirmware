#include "wifi_callbacks.h"
#include <stdlib.h>
#include <string.h>
bool wifi_callbacks_init(WifiCallbacks *tables, const void *stock, size_t size) {
    if (!tables || !stock || !size) return false;
    *tables = (WifiCallbacks){0};
    tables->stock = malloc(size); tables->local = malloc(size);
    if (!tables->stock || !tables->local) { wifi_callbacks_free(tables); return false; }
    memcpy(tables->stock, stock, size); memcpy(tables->local, stock, size);
    tables->size = size;
    return true;
}
void wifi_callbacks_free(WifiCallbacks *tables) {
    if (!tables) return;
    free(tables->stock); free(tables->local); *tables = (WifiCallbacks){0};
}
void *wifi_callbacks_activate(const WifiCallbacks *tables, bool local, int (*install)(void *)) {
    if (!tables || !tables->stock || !tables->local || !tables->size || !install) return NULL;
    void *active = malloc(tables->size);
    if (!active) return NULL;
    memcpy(active, local ? tables->local : tables->stock, tables->size);
    if (install(active) != 0) { free(active); return NULL; }
    return active;
}
