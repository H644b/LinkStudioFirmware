#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define LS_RECIPE_MAX 4096
typedef struct {
    const char* display_name;
    const char* trainer_name;
    const uint8_t* entity;
    const uint8_t* identity;
    const uint8_t* identity_tail;
    const uint8_t* ready;
    uint16_t identity_size, tail_size, ready_size;
} LsRecipe;
bool ls_recipe_parse(const uint8_t* data, size_t size, LsRecipe* recipe);
