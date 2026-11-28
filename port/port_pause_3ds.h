#ifndef PORT_PAUSE_3DS_H
#define PORT_PAUSE_3DS_H

#include <stdbool.h>

#ifdef TMC_3DS
/* Sample on the game thread, together with the original pause-menu frame. */
bool Port_Pause3DS_Active(void);
bool Port_Pause3DS_MenuVisible(void);
const char* Port_Pause3DS_PageName(void);
bool Port_Pause3DS_QuestPage(void);
bool Port_Pause3DS_PageNavigationAvailable(void);

/* The selected quest action, when the footer should highlight a button. */
enum {
    PORT_PAUSE3DS_ACTION_NONE,
    PORT_PAUSE3DS_ACTION_SETTINGS,
    PORT_PAUSE3DS_ACTION_UPDATES,
};
unsigned Port_Pause3DS_QuestAction(void);

/* Enqueue on the game thread at VBlank. Negative is previous, positive next. */
void Port_Pause3DS_QueuePage(int direction);
void Port_Pause3DS_QueueAction(bool updates);
#endif

#endif
