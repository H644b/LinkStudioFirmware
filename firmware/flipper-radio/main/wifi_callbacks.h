#pragma once
#include <stdbool.h>
#include <stddef.h>
typedef struct { void *stock, *local; size_t size; } WifiCallbacks;
/* Registration destroys the previously registered table and owns the new table
   on success. Retain independent templates, never pointers owned by the driver. */
bool wifi_callbacks_init(WifiCallbacks *tables, const void *stock, size_t size);
void wifi_callbacks_free(WifiCallbacks *tables);
void *wifi_callbacks_activate(const WifiCallbacks *tables, bool local, int (*install)(void *));
