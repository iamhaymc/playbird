/* fly_app: application glue — owns the game, the UI and the framebuffer,
 * advances everything with a fixed-step loop, and captures stills.
 * The optional realtime window backend (fly_win.c) drives exactly this API,
 * so headless and windowed sessions behave identically. */
#ifndef FLY_APP_H
#define FLY_APP_H

#include "fly_game.h"
#include "fly_img.h"
#include "fly_ui.h"

#define FLY_APP_DT (1.0f / 60.0f)

typedef struct {
    fly_game game;
    fly_ui ui;
    fly_img frame;
    float accum;
} fly_app;

int fly_app_init(fly_app *app, int w, int h, uint32_t seed, fly_craft_kind kind);
int fly_app_init_load(fly_app *app, int w, int h, const char *save_path);
void fly_app_free(fly_app *app);

void fly_app_event(fly_app *app, const fly_event *ev);
/* advance by real elapsed seconds (internally fixed-stepped) */
void fly_app_step(fly_app *app, float elapsed);
/* draw the active view into app->frame */
void fly_app_render(fly_app *app);
/* render + write a PNG still of the current view */
int fly_app_shot(fly_app *app, const char *path);

/* load a custom airframe asset over the current craft (data yaml file) */
int fly_app_load_airframe(fly_app *app, const char *yaml_path);

#endif /* FLY_APP_H */
