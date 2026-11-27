/* Hand-curated stereo 3D "zone depth fixes" (docs/stereo-3d.md, "Zone depth fixes").
 *
 *   PORT_ZONE_DEPTH(area, room, bg, x, y, w, h, toBg)
 *
 *   area, room  the room the fix belongs to: the game's area id and room id (gRoomControls.area / .room), numbers,
 *               hex ok (0x02, 0x00)
 *   bg          1, 2 or 3: the layer whose pixels inside the rectangle are moved
 *   x, y, w, h  the rectangle, in ROOM pixel coordinates (from the room's origin, the coordinates the layer workbench
 *               shows for the room; multiples of 8 in practice, 16 for a whole metatile)
 *   toBg        1, 2 or 3, not bg: the layer whose stereo plane the rectangle takes
 *
 * Meaning: the pixels of layer bg inside the rectangle are drawn moved as layer toBg is, for each eye; nothing else about
 * the layering changes. The copy is drawn in bg's own place in the drawing order, so a layer in front of bg keeps its
 * opaque pixels on top and only its transparent ones show the moved copy: the rectangle may be bigger than the hole it
 * fixes (a generous one is better: where the hole runs past the rectangle's edge, the layer's picture breaks by a pixel).
 * Only in the world scene with 3D on (the slider up, 3D WORLD on), only while the current room is area/room, and only
 * when the two layers have different depths. Several lines per room are fine (16 at most per room). The software
 * renderer is flat and ignores them. The file is optional.
 *
 * Edited by hand or by tools/layer-workbench; one entry per line, comments directly above an entry belong to it.
 */
/* Hyrule Town (MAIN), the log hut: its roof is BG1 (raised), but BG1 is transparent at x 272..296, y 856..896 (the
 * beaded log pillar and the ladder's top, the little chick on it, all BG2, the ground's layer), and in a few pixels of
 * the tiles around it, so that part of the roof sat on the ground's plane. The rectangle covers the hole and those
 * neighbours; BG1's opaque pixels stay on top. The thin gaps of the roof's sign above (y 840..848, BG2's red letters) and
 * most of the shutters (x 304..328; their stripes look the same moved sideways) and the door (x 304..320, y 880..896)
 * are left at the ground's plane. The hole is open at its bottom edge (y 896), where the ladder meets the ground. */
PORT_ZONE_DEPTH(0x02, 0x00, 2, 264, 848, 40, 48, 1)
