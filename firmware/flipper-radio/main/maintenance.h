#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool maintenance_command(uint8_t command, const uint8_t *payload, size_t length, bool idle);
bool maintenance_busy(void);
void maintenance_abort(void);
