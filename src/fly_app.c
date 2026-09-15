#include "fly_app.h"

#include <stdio.h>

/* --- finding data/ -------------------------------------------------------
 *
 * The assets are loaded by relative path, so where they are depends on where
 * the process was started, and there are only a few places anybody starts it:
 * the app directory, the repository root, and the build directory the
 * executable itself sits in. These are those places, in that order.
 *
 * They are one list because they were not: the module registry and the
 * airframe catalogue each carried their own chain, and both chains named
 * `apps/fly99/` — a directory that does not exist, because the project is
 * fly99 and its directory is `apps/fly`. That left exactly one working
 * prefix, and starting the game from anywhere but the app directory quietly
 * dropped nine of the eleven airframes and every YAML-tuned handling number
 * off the other two. Silently: the catalogue falls back to its built-in
 * defaults, so the game still ran, sold two aeroplanes instead of eleven, and
 * refused to load any save that named one of the missing nine — which sent
 * the CLI down its "start a new world instead" path over the top of it.
 *
 * fly_font.c has the same list for the two typefaces, and it has to: it is
 * below this module and loads its own file. The two must agree — they are the
 * same question about the same `data/` directory.
 */
static const char *FLY__DATA_ROOTS[] = { "", "apps/fly/", "../", "../../" };

/* Load `relative` through the first prefix that works, using `load` (which
 * returns negative on failure). Returns that loader's result, or -1. */
static int fly__load_data(int (*load)(const char *), const char *relative) {
    size_t i;
    for (i = 0; i < sizeof FLY__DATA_ROOTS / sizeof FLY__DATA_ROOTS[0]; ++i) {
        char path[256];
        int rc, n = snprintf(path, sizeof path, "%s%s", FLY__DATA_ROOTS[i], relative);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        rc = load(path);
        if (rc >= 0) return rc;
    }
    return -1;
}

static void fly__load_module_data(void) {
    static int tried = 0;
    if (tried) return;
    tried = 1;
    /* optional data-driven registry; the built-in table mirrors this file */
    fly__load_data(fly_modules_load_yaml, "data/modules.yaml");
}

static void fly__load_airframe_data(void) {
    static int tried = 0;
    /* The four anybody sells, then the one each power builds for itself. The
     * order is the catalogue's order and the catalogue's order is what a save
     * refers to by id rather than by index, so appending here is safe. */
    static const char *names[] = {
        "skylark", "condor", "dragonfly", "wasp",
        "marshal", "vulture", "slagback", "pilgrim", "courser", "ashjack", "antiphon"
    };
    int i, missing = 0;
    if (tried) return;
    tried = 1;
    for (i = 0; i < (int)(sizeof names / sizeof names[0]); ++i) {
        char rel[64];
        snprintf(rel, sizeof rel, "data/airframes/%s.yaml", names[i]);
        if (fly__load_data(fly_game_load_airframe_catalog, rel) < 0) ++missing;
    }
    /* A catalogue short of its assets is a different game — fewer aeroplanes
     * on every shelf, and saves that name a missing one will not load — so it
     * is said out loud rather than discovered later. */
    if (missing)
        fprintf(stderr,
                "warning: %d of %d airframe assets not found; run from the app "
                "directory or the repository root so data/airframes is reachable\n",
                missing, (int)(sizeof names / sizeof names[0]));
}

int fly_app_init(fly_app *app, int w, int h, uint32_t seed, fly_craft_kind kind) {
    if (fly_img_init(&app->frame, w, h) != 0) return -1;
    fly__load_module_data();
    fly__load_airframe_data();
    fly_game_init(&app->game, seed, kind);
    fly_ui_init(&app->ui);
    app->accum = 0.0f;
    return 0;
}

int fly_app_init_load(fly_app *app, int w, int h, const char *save_path) {
    if (fly_img_init(&app->frame, w, h) != 0) return -1;
    fly__load_module_data();
    fly__load_airframe_data();
    if (fly_game_load(&app->game, save_path) != 0) {
        fly_img_free(&app->frame);
        return -1;
    }
    fly_ui_init(&app->ui);
    app->accum = 0.0f;
    return 0;
}

void fly_app_free(fly_app *app) {
    fly_img_free(&app->frame);
}

void fly_app_event(fly_app *app, const fly_event *ev) {
    fly_ui_event(&app->ui, &app->game, ev);
}

void fly_app_step(fly_app *app, float elapsed) {
    app->accum += elapsed;
    /* clamp runaway frames so slow renders do not spiral */
    if (app->accum > 0.5f) app->accum = 0.5f;
    while (app->accum >= FLY_APP_DT) {
        fly_ui_pump_controls(&app->ui, &app->game, FLY_APP_DT);
        fly_game_step(&app->game, FLY_APP_DT);
        app->accum -= FLY_APP_DT;
    }
}

void fly_app_render(fly_app *app) {
    fly_ui_render(&app->ui, &app->game, &app->frame);
}

int fly_app_shot(fly_app *app, const char *path) {
    fly_app_render(app);
    return fly_img_write_png(&app->frame, path);
}

int fly_app_load_airframe(fly_app *app, const char *yaml_path) {
    int catalog = fly_game_load_airframe_catalog(yaml_path);
    if (catalog < 0) return -1;
    fly_game *g = &app->game;
    if (g->active_airframe < 0 || g->active_airframe >= g->owned_count) return -2;
    snprintf(g->owned[g->active_airframe].catalog_id,
             sizeof g->owned[g->active_airframe].catalog_id, "%s",
             fly_game_airframe_catalog_get(catalog)->id);
    fly_game_rebuild_airframe(g);
    if (g->player.craft.fuel > g->player.airframe.fuel_cap) g->player.craft.fuel = g->player.airframe.fuel_cap;
    if (g->player.craft.hp > g->player.airframe.structure) g->player.craft.hp = g->player.airframe.structure;
    fly_game_log(g, "Airframe set: %s", g->player.airframe.name);
    return 0;
}
