/* Hand-curated stereo 3D depth corrections per kind of entity (docs/stereo-3d.md, "Sprite depth corrections").
 *
 * PORT_SPRITE_DEPTH(kind, id, type, mode)
 *   kind  the entity's kind (ENEMY, OBJECT, NPC...)
 *   id    its id inside that kind (BOSS_DOOR, CHUCHU_BOSS...)
 *   type  its type, or PORT_ANY_TYPE
 *   mode  where the sprite meets the ground, and so which way its depth grows (nil at the ground, rising across the
 *         sprite by the same rule as a standing one's, capped):
 *         PORT_DEPTH_UP      the normal rule (what an entity without an entry gets): ground at the bottom edge, the
 *                            depth grows upwards
 *         PORT_DEPTH_DOWN    (or PORT_DEPTH_INVERT) the sprite hangs from its top edge (a door in a south wall): the
 *                            depth grows downwards
 *         PORT_DEPTH_LEFT    the sprite hangs on a wall to its left (a curtain on a west wall): ground at its right
 *                            edge, the depth grows towards the left of the screen, column of tiles by column
 *         PORT_DEPTH_RIGHT   the mirror: on a wall to its right, ground at its left edge, the depth grows to the right
 *         PORT_DEPTH_FLAT    no lean at all
 *         PORT_DEPTH_RAISED  no lean, the whole sprite at one raised depth: the one the normal rule gives its top edge
 *                            (the entity's own depth plus the rise of its whole height, feet to top; 20 quarters at most)
 *         PORT_DEPTH_FIXED(n) no lean, the whole sprite at n quarter pixels (0..20) over the entity's own depth, for sprites
 *                            that must sit on a surface of a known height
 *         PORT_DEPTH_WALL    picked from where the entity stands in its room: within 24 px of the left, right or bottom
 *                            edge it is LEFT, RIGHT or DOWN (the nearest wins), anywhere else UP; for door-like sprites
 *                            that sit in a wall whichever way they face
 *
 * The HUD's sprites (priority 0) ignore every mode. A side lean (LEFT, RIGHT) gives all of its shift to one eye, so its
 * columns never part (docs/stereo-3d.md).
 *
 * The file is optional: without it every sprite follows the normal rule.
 */
PORT_SPRITE_DEPTH(OBJECT, BOSS_DOOR, PORT_ANY_TYPE, PORT_DEPTH_INVERT)
/* The big Chuchu's shadow is an entity of its own (the same graphics as its body with a dark palette): flat on the ground. */
PORT_SPRITE_DEPTH(OBJECT, CHUCHU_BOSS_START_PARTICLE, PORT_ANY_TYPE, PORT_DEPTH_FLAT)
/* The curtain on the left wall of a room (area 0x20, room 00): drawn as a standing sprite, but it hangs on the wall, so
 * its depth grows from the room (its right edge, on the floor's plane) towards the wall on the left. */
PORT_SPRITE_DEPTH(OBJECT, GENTARI_CURTAIN, PORT_ANY_TYPE, PORT_DEPTH_LEFT)
/* The small archway left of that curtain (area 0x20, room 00, x 8 y 80): part of the wall, so it stays flat with the
 * rest of the scenery. Raised (one constant depth at its top's height) was tried and looked odd on the console. Type 2
 * only: type 1 of the same object sits at the bottom of the room and keeps the normal rule. */
PORT_SPRITE_DEPTH(OBJECT, MINISH_SIZED_ARCHWAY, 2, PORT_DEPTH_FLAT)
/* The side archways of the Gentari exit room (area 0x20, room 01): the left one hangs on the left wall, the right one on
 * the right wall, so the depth follows the wall they are on. */
PORT_SPRITE_DEPTH(OBJECT, MINISH_SIZED_ARCHWAY, 3, PORT_DEPTH_LEFT)
/* Every other archway sits in a wall of its room and faces it whichever way: the wall it stands in decides (the bottom
 * archway of a room hangs from its top edge, the same fix as the south boss doors). Listed after the exact types above,
 * the first match wins. */
PORT_SPRITE_DEPTH(OBJECT, MINISH_SIZED_ARCHWAY, PORT_ANY_TYPE, PORT_DEPTH_WALL)
PORT_SPRITE_DEPTH(OBJECT, MINISH_LIGHT, 1, PORT_DEPTH_DOWN)
PORT_SPRITE_DEPTH(ENEMY, DUST, 0, PORT_DEPTH_FLAT)
/* The leaves lying on the Crenel Minish paths' floor (area 0x12, GIANT_TWIG types 2 and 3: a tall multi-piece sprite
 * that leant out of the ground with the world's body rule, owner on the New 3DS 2026-10-30): flat on the ground. */
PORT_SPRITE_DEPTH(OBJECT, GIANT_TWIG, 2, PORT_DEPTH_FLAT)
PORT_SPRITE_DEPTH(OBJECT, GIANT_TWIG, 3, PORT_DEPTH_FLAT)
/* The stone Minish portal (a 3x3-tile slab in the floor, Cave of Flames area 0x50 room 07): with the body rule its top
 * edge stood out over Link standing on it (owner on the New 3DS, 2026-11-19). Flat on the ground. */
PORT_SPRITE_DEPTH(OBJECT, MINISH_PORTAL_STONE, PORT_ANY_TYPE, PORT_DEPTH_FLAT)
/* The minecart rails that turn (RAILTRACK, Cave of Flames 0x50/04): a piece of track in the floor, which stood a pixel
 * behind the floor around it when lying across (owner on the New 3DS, 2026-11-19). Flat on the ground. */
PORT_SPRITE_DEPTH(OBJECT, RAILTRACK, PORT_ANY_TYPE, PORT_DEPTH_FLAT)
/* Things hung on a wall, drawn as standing sprites: the weapon racks of the Swiftblade dojos (FURNITURE type 7: three on
 * each of the top, left and right walls) and the archways of every room (the door frames on all four walls). The wall
 * they stand in decides (PORT_DEPTH_WALL): left wall LEFT, right wall RIGHT, bottom wall DOWN, top wall the normal rule. */
PORT_SPRITE_DEPTH(OBJECT, FURNITURE, 7, PORT_DEPTH_WALL)
/* The Cave of Flames' archways (type 13): only the wooden joints where a door meets the wall, which stood out of the
 * flat wall around them (owner on the New 3DS, 2026-11-19). Flat with the wall. */
PORT_SPRITE_DEPTH(OBJECT, ARCHWAY, 13, PORT_DEPTH_FLAT)
PORT_SPRITE_DEPTH(OBJECT, ARCHWAY, PORT_ANY_TYPE, PORT_DEPTH_WALL)
/* Rem, the shoemaker (area 0x23 room 02): his hands and the boots he works on are drawn over his counter, which stands out;
 * they take its height (14 quarters, tuned on the console) instead of the floor's. */
PORT_SPRITE_DEPTH(NPC, REM, PORT_ANY_TYPE, PORT_DEPTH_FIXED(14))

/* The carpenters' planks (area 0x03 room 0x04): the third part of a carpenter of type 0 and 1 (the palette-1 piece, the
 * plank standing at his side) lies on the ground, so it stays flat and does not count in the box that body, head and saw
 * lean from. PORT_PART_FLAT(kind, id, part): part 0-3 of an entity drawn as several parts. */
PORT_PART_FLAT(NPC, CARPENTER, 2)
