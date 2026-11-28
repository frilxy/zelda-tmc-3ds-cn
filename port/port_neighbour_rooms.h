#pragma once
/* P4.9: the neighbouring room drawn beyond the room's edge in a wide view. Where the view reaches past
 * the room's rectangle (a room narrower or shorter than 400x240, centred with the rest black), the cells
 * come from the room that borders it in the area's room grid (RoomResInfo.map_x/map_y/pixel_*), not
 * from nothing. Only the two tile layers: no entities, no collision. A neighbour can be drawn only if
 * it uses the same tile set and metatile definitions as the current room, since those are what VRAM and
 * the layers hold. */
#include <stdbool.h>
#include <stdint.h>

/* Once a tick (Port_Widescreen_UpdateShadows): follows the room, rebuilds the window when it changes. */
void Port_NeighbourRooms_Update(void);
/* The 8x8 entry of a layer (topLayer 0 = the bottom map, 1 = the top one) at the current room's tile
 * (column, row from its origin) when that lies outside the room: the neighbour's, or `blank`. */
uint16_t Port_NeighbourRooms_Tile(int topLayer, int column, int row, uint16_t blank);
/* Where a view of `view` pixels along an axis (0 = x, 1 = y) should start, in world pixels, for room `room` of
 * the current area when the room is smaller than the view on that axis: of the positions that keep the whole
 * room in view (8-pixel steps), the one whose margin beside the room is most covered by rooms that can be drawn
 * there (same tile set); on a tie the one nearest `prefer` (where the camera already is: it then does not move for
 * nothing), or the centred one when `prefer` is PORT_NEIGHBOUR_NO_PREFERENCE. Without such rooms it is the centred
 * position, as before. */
#define PORT_NEIGHBOUR_NO_PREFERENCE (-0x7fffffff)
int Port_NeighbourRooms_Rest(unsigned room, int axis, int view, int prefer);
/* The line-up of a border scroll into `room` on `axis` (Port_Widescreen_BeginScroll): of the positions
 * Port_NeighbourRooms_Rest would score best for the incoming room, the one that leaves the least black in the
 * view while it lines up, still in the outgoing room `fromRoom` (the view there spans `otherStart` to
 * `otherStart + otherView` on the other axis); on a tie the one nearest `prefer`. */
int Port_NeighbourRooms_LineUp(unsigned room, int axis, int view, int prefer, unsigned fromRoom, int otherStart,
                               int otherView);
/* The rooms drawn beside the current one, each the part that shows in the 400x240 view (view pixels, given the
 * camera's scroll), the largest first, at most `max`: the blurred border of WIDE + BLUR (dropped on 2026-10-20, kept
 * for a cheaper way back) grew from each of them as from the room itself, and left them alone. */
typedef struct PortBlurRect {
    int x, y, w, h;
} PortBlurRect;
int Port_NeighbourRooms_DrawnRects(PortBlurRect* out, int max, int scrollX, int scrollY);
/* The frames the current room changed: the room's numbers are the new room's (and this module's list still the
 * old room's) while the frame shown is still drawn with the old room's registers, so a rectangle worked out
 * from them is wrong for that frame. */
bool Port_NeighbourRooms_RoomJustChanged(void);
/* For the probe's log and the debug overlay: what the last build found. */
typedef struct PortNeighbourStats {
    unsigned neighbours;     /* bordering rooms drawn */
    unsigned otherTileset;   /* bordering rooms left out: another tile set */
    unsigned unreadable;     /* bordering rooms left out: their map could not be read */
    unsigned apart;          /* rooms of the area left out: no chain of shared edges to the current one */
    unsigned cells;          /* cells filled from neighbours */
} PortNeighbourStats;
PortNeighbourStats Port_NeighbourRooms_Stats(void);
