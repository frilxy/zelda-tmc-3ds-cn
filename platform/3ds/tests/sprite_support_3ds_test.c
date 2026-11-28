/* Host test of the stereo 3D support groups (platform/3ds/source/sprite_support_3ds.h): an entity's entries share the
 * highest support; its shadow takes the surface under it, never more than its entity (on a roof, on the ground, an
 * entity jumping over the roof's edge, a shadow whose entity is not drawn); entries with no group, or not eligible, take
 * nothing; the group counter wraps within 1..127.
 *
 *   cc -std=c11 -Wall -Wextra -Iplatform/3ds/source platform/3ds/tests/sprite_support_3ds_test.c -o /tmp/sprite_support_3ds_test
 *   /tmp/sprite_support_3ds_test
 */
#include "sprite_support_3ds.h"

#include <stdio.h>

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr);        \
            return 1;                                                      \
        }                                                                  \
    } while (0)

enum { SHADOW = SPRITE_SUPPORT_3DS_SHADOW, ROOF = 8, GROUND = 0 };

int main(void) {
    /* Entries: 0-1 the chick (group 5) on the roof; 2 its shadow on the roof; 3 Link (group 6) on the ground; 4 his
     * shadow; 5 a DrawDirect sprite (no group) over the roof; 6 a HUD sprite (not eligible) with a group. */
    {
        const uint8_t groups[7] = { 5, 5, 5 | SHADOW, 6, 6 | SHADOW, 0, 7 };
        const uint8_t own[7] = { ROOF, GROUND, ROOF, GROUND, GROUND, ROOF, ROOF };
        const bool eligible[7] = { true, true, true, true, true, true, false };
        uint8_t out[7];
        SpriteSupport3DS_Resolve(groups, own, eligible, 7, out);
        CHECK(out[0] == ROOF && out[1] == ROOF); /* one object: the highest of its entries */
        CHECK(out[2] == ROOF);                   /* the chick's shadow on the roof: the roof's depth */
        CHECK(out[3] == GROUND && out[4] == GROUND); /* Link and his shadow on the ground, unchanged */
        CHECK(out[5] == 0 && out[6] == 0);
    }
    /* A shadow never stands in front of its entity: the entity samples the ground (it jumped off the roof's edge, its
     * tiles over the ground), its shadow still over the roof's edge: the shadow is held to the entity's. */
    {
        const uint8_t groups[2] = { 9, 9 | SHADOW };
        const uint8_t own[2] = { 4, ROOF };
        const bool eligible[2] = { true, true };
        uint8_t out[2];
        SpriteSupport3DS_Resolve(groups, own, eligible, 2, out);
        CHECK(out[0] == 4 && out[1] == 4);
    }
    /* The other way: the entity over the roof (jumping in front of it), its shadow on the ground below: the ground.
     * The shadow does not raise its entity either (it is not one of its entries). */
    {
        const uint8_t groups[2] = { 9, 9 | SHADOW };
        const uint8_t own[2] = { ROOF, GROUND };
        const bool eligible[2] = { true, true };
        uint8_t out[2];
        SpriteSupport3DS_Resolve(groups, own, eligible, 2, out);
        CHECK(out[0] == ROOF && out[1] == GROUND);
        const uint8_t own2[2] = { GROUND, ROOF };
        SpriteSupport3DS_Resolve(groups, own2, eligible, 2, out);
        CHECK(out[0] == GROUND && out[1] == GROUND);
    }
    /* A shadow whose entity drew nothing eligible (its owner off screen or the HUD's): nothing. */
    {
        const uint8_t groups[1] = { 12 | SHADOW };
        const uint8_t own[1] = { ROOF };
        const bool eligible[1] = { true };
        uint8_t out[1];
        SpriteSupport3DS_Resolve(groups, own, eligible, 1, out);
        CHECK(out[0] == 0);
    }
    /* A bare shadow mark (group 0 with the bit): nothing. */
    {
        const uint8_t groups[1] = { SHADOW };
        const uint8_t own[1] = { ROOF };
        const bool eligible[1] = { true };
        uint8_t out[1];
        SpriteSupport3DS_Resolve(groups, own, eligible, 1, out);
        CHECK(out[0] == 0);
    }
    /* Groups: 1..127, never 0 or the shadow bit. */
    uint8_t g = 0;
    for (int i = 0; i < 400; ++i) {
        g = SpriteSupport3DS_NextGroup(g);
        CHECK(g >= 1 && g <= SPRITE_SUPPORT_3DS_GROUP_MASK);
    }
    CHECK(SpriteSupport3DS_NextGroup(127) == 1 && SpriteSupport3DS_NextGroup(0) == 1 && SpriteSupport3DS_NextGroup(5) == 6);
    printf("sprite_support_3ds_test: ok\n");
    return 0;
}
