#ifndef PORT_PAUSE_3DS_H
#define PORT_PAUSE_3DS_H

#include <stdbool.h>

#ifdef TMC_3DS
typedef enum {
    PORT_PAUSE3DS_MAIN_NONE,
    PORT_PAUSE3DS_MAIN_INVENTORY,
    PORT_PAUSE3DS_MAIN_MAP,
} PortPause3DSMainPage;

/* Read the original pause state on the game thread. */
bool Port_Pause3DS_Active(void);
bool Port_Pause3DS_MenuVisible(void);
PortPause3DSMainPage Port_Pause3DS_MainPage(void);
const char* Port_Pause3DS_PageName(void);
bool Port_Pause3DS_PageNavigationAvailable(void);
unsigned Port_Pause3DS_SelectedItemSlot(void);
int Port_Pause3DS_SelectedCollection(void); /* -1 when using the item grid */

/* Enqueue on the game thread at VBlank. Requests during transitions are ignored. */
void Port_Pause3DS_QueuePage(int direction); /* either nonzero direction switches main pages */
void Port_Pause3DS_QueueItem(unsigned slot); /* native item slots 0..15 */
void Port_Pause3DS_QueueEquip(unsigned button); /* 0 = A, 1 = B */
void Port_Pause3DS_QueueCollection(unsigned slot); /* layout slots 0..11 */
void Port_Pause3DS_QueueSave(void);
void Port_Pause3DS_QueueMapPoint(int nativeX, int nativeY); /* native 240x160 coordinates */
void Port_Pause3DS_QueueNativePoint(int nativeX, int nativeY); /* centered native detail or save screen */
void Port_Pause3DS_QueueBack(void);
#endif

#endif
