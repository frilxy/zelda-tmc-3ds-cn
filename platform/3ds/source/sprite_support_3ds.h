#ifndef SPRITE_SUPPORT_3DS_H
#define SPRITE_SUPPORT_3DS_H

/* Stereo 3D "support" (docs/stereo-3d.md, "Support"): the depth a sprite takes from the background it rests on. Each OAM
 * entry an entity writes carries the entity's group (virtuappu_mode1_obj_group, written by port/port_draw.c); the GPU model
 * samples what lies under each entry (its own support) and this header resolves the groups:
 *
 *  - an entity's entries share one support, the highest of theirs (an object that overhangs a roof's edge stays one
 *    object);
 *  - an entity's shadow (the deferred shadow pass of port_draw.c) is marked with the owner's group plus
 *    SPRITE_SUPPORT_3DS_SHADOW: it takes the support of the surface under it, never the entity's height (its own OAM
 *    depth stays 0), and never more than its owner's support, so a shadow can never stand in front of its entity's feet
 *    (the eyes' base / front split would then draw it over them).
 *
 * Header only, no dependencies: port/port_draw.c, the GPU model (port_ppu_gpu_3ds_model.c) and the host test
 * (platform/3ds/tests/sprite_support_3ds_test.c) include it. */

#include <stdbool.h>
#include <stdint.h>

enum {
    SPRITE_SUPPORT_3DS_SHADOW = 0x80u,     /* bit 7 of an entry's group: the shadow of the entity of the low bits */
    SPRITE_SUPPORT_3DS_GROUP_MASK = 0x7fu, /* entity groups run 1..127 (0: no entity, no support) */
};

/* The next entity group after `last` (1..127, wrapping, never 0). */
static inline uint8_t SpriteSupport3DS_NextGroup(uint8_t last) {
    const unsigned next = ((unsigned)last & SPRITE_SUPPORT_3DS_GROUP_MASK) + 1u;
    return (uint8_t)(next > SPRITE_SUPPORT_3DS_GROUP_MASK ? 1u : next);
}

/* The support of every entry, quarter pixels. `groups[i]` is entry i's group (0: none: it takes nothing), `own[i]` the
 * support sampled under it (only read for entries with `eligible[i]`; the others, the HUD's, affine and OBJ-window ones,
 * take nothing). `out` gets each entry's support. */
static inline void SpriteSupport3DS_Resolve(const uint8_t* groups, const uint8_t* own, const bool* eligible, unsigned count,
                                            uint8_t* out) {
    uint8_t group[SPRITE_SUPPORT_3DS_GROUP_MASK + 1u] = { 0 };
    for (unsigned i = 0; i < count; ++i) {
        const unsigned g = groups[i];
        if (!eligible[i] || g == 0u || (g & SPRITE_SUPPORT_3DS_SHADOW) != 0u) continue;
        if (own[i] > group[g]) group[g] = own[i];
    }
    for (unsigned i = 0; i < count; ++i) {
        const unsigned g = groups[i];
        if (!eligible[i] || (g & SPRITE_SUPPORT_3DS_GROUP_MASK) == 0u) {
            out[i] = 0;
        } else if (g & SPRITE_SUPPORT_3DS_SHADOW) {
            const uint8_t owner = group[g & SPRITE_SUPPORT_3DS_GROUP_MASK];
            out[i] = own[i] < owner ? own[i] : owner;
        } else {
            out[i] = group[g];
        }
    }
}

#endif
