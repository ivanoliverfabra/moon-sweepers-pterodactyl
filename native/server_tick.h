#ifndef MOON_SERVER_TICK_H
#define MOON_SERVER_TICK_H
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>

/* Dedicated, headless server only: measures every tick, raises the tick rate and drains the cleaning queue
   while the world has a backlog, and turns GPU-only engine calls into no-ops. */
bool MoonTick_Install(HMODULE game_module,const void *game_image,void (*log_line)(const char *));
/* Once per Unity frame, on the Unity thread. */
void MoonTick_Frame(void);
/* Every host tick (250 ms): picks and applies the tick rate, warns when the server falls behind.
   Returns false when the module is not installed, so the caller keeps its own frame limit. */
bool MoonTick_Govern(ULONGLONG now);
/* Appends ,"tick":{...} to the state JSON. */
void MoonTick_WriteJson(FILE *f);
/* The perf console command. */
bool MoonTick_Command(const char *verb);
#endif
