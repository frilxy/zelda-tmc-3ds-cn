#ifndef TMC_BOOT_SCREEN_3DS_H
#define TMC_BOOT_SCREEN_3DS_H

/* Called after gfx and RomFS setup. Stop before a fatal screen or Citro3D takes over. */
void BootScreen3DS_Start(void);
void BootScreen3DS_Stop(void);

#endif
