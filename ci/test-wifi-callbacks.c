#include "wifi_callbacks.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { int (*connect)(void); int (*receive)(void); unsigned cookie; } Table;
static Table *driver;
static int stock_connect(void) { return 11; }
static int local_connect(void) { return 22; }
static int receive(void) { return 33; }
static int install(void *table) { free(driver); driver = table; return 0; }
static int reject(void *table) { (void)table; return -1; }
int main(void) {
    driver = malloc(sizeof(*driver));
    *driver = (Table){stock_connect, receive, 1234};
    WifiCallbacks templates;
    assert(wifi_callbacks_init(&templates, driver, sizeof(*driver)));
    ((Table *)templates.local)->connect = local_connect;
    for (unsigned i = 0; i < 2000; ++i) {
        bool local = (i & 1) == 0;
        Table *active = wifi_callbacks_activate(&templates, local, install);
        assert(active && driver == active);
        assert(active != templates.stock && active != templates.local);
        assert(active->connect() == (local ? 22 : 11));
        assert(active->receive() == 33 && active->cookie == 1234);
        assert(((Table *)templates.stock)->connect() == 11);
        assert(((Table *)templates.local)->connect() == 22);
    }
    assert(!wifi_callbacks_activate(&templates, true, reject));
    assert(driver->receive() == 33);
    wifi_callbacks_free(&templates);
    assert(driver->connect() == 11);
    free(driver);
    puts("2000 driver-owned callback replacements passed without freed-table reuse");
    return 0;
}
