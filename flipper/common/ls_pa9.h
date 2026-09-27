#pragma once
#include <stdbool.h>
#include <stdint.h>
#define LS_PA9_SIZE 344
/* Structural integrity only; this is not an encounter-legality validator. */
bool ls_pa9_decode(const uint8_t encrypted[LS_PA9_SIZE], uint8_t plain[LS_PA9_SIZE]);
