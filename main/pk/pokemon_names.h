#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    POKEMON_NAMES_OFFICIAL = 0,
    POKEMON_NAMES_GS_LEGACY = 1,
    POKEMON_NAMES_STYLE_COUNT
} pokemon_name_style_t;

// Display preference only, shared by every assets_species consumer. Runtime
// only: fresh hardware boots use official names; no species/save fields change.
bool pokemon_names_set_style(pokemon_name_style_t style);
pokemon_name_style_t pokemon_names_get_style(void);
const char *pokemon_names_style_label(void);

// Selected override, or NULL to retain the official asset's name. Returned
// storage is immutable for the lifetime of the program; length is UTF-8 bytes.
const char *pokemon_names_override(uint16_t species, uint8_t *length);
uint16_t pokemon_names_legacy_count(void);
