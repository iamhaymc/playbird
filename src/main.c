/* fly99 CLI: new/play/render/sim/market/help around the fly_* library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fly_app.h"
#include "fly_gpu.h"
#include "fly_render.h"

#ifdef FLY_WINDOW
int fly_win_run(fly_app *app); /* fly_win.c */
#endif

static void usage(void) {
    printf(
        "fly99 - retro-futurist frontier flight simulator\n\n"
        "usage: fly99 <command> [options]\n\n"
        "commands:\n"
        "  new                start a new game and save it\n"
        "  play               play (window if built with --window, else headless AP demo)\n"
        "  render             render a still of a view to PNG\n"
        "  sim                run a headless flight and dump telemetry CSV\n"
        "  market             print market prices at the docked location\n"
        "  help               this text\n\n"
        "common options:\n"
        "  --save FILE        save file (default fly99_save.yaml)\n"
        "  --seed N           world seed for new games (default: clock)\n"
        "  --craft plane|drone  starting airframe (default plane)\n"
        "  --size WxH         frame size (default 960x540)\n"
        "  --view V           render view: play|self|help|edit|map (default play)\n"
        "  --out FILE         output image (default shot.png)\n"
        "  --mode M           render mode: raster|pt|mix (default raster)\n"
        "  --pt-samples N     paths per pixel for pt/mix (default 2)\n"
        "  --quality Q        graphics level: low|medium|high|ultra (default medium)\n"
        "  --ssaa N           supersampling factor (overrides the level)\n"
        "  --msaa N           GPU edge multisampling: 1 off, 2/4/8 (default 4)\n"
        "  --steps N          headless steps to simulate (default 600)\n"
        "  --airframe FILE    load a custom YAML airframe asset\n"
        "  --ap LOC           engage autopilot to location index\n"
        "                     (`market` lists the charted ones and their indices)\n");
}

typedef struct {
    const char *save, *out, *view, *mode, *airframe, *craft, *quality;
    uint32_t seed;
    int w, h, steps, pt_samples, ssaa, msaa, ap;
    int have_seed, have_ssaa, have_msaa;
} fly_args;

static void args_default(fly_args *a) {
    memset(a, 0, sizeof *a);
    a->save = "fly99_save.yaml";
    a->out = "shot.png";
    a->view = "play";
    a->mode = "raster";
    a->craft = "plane";
    a->quality = "medium";
    a->w = 960;
    a->h = 540;
    a->steps = 600;
    a->pt_samples = 2;
    a->ssaa = 2;
    a->msaa = 4;
    a->ap = -1;
}

/* Every option here takes a value. Named so that one given without its value
 * can say so, instead of arriving at the bottom of the chain and being
 * reported as an option nobody has heard of. */
static int wants_value(const char *s) {
    static const char *opts[] = {
        "--save", "--out", "--view", "--mode", "--craft", "--quality",
        "--airframe", "--seed", "--size", "--steps", "--pt-samples",
        "--ssaa", "--msaa", "--ap"
    };
    size_t i;
    for (i = 0; i < sizeof opts / sizeof opts[0]; ++i)
        if (strcmp(s, opts[i]) == 0) return 1;
    return 0;
}

static int args_parse(fly_args *a, int argc, char **argv) {
    int i;
    for (i = 0; i < argc; ++i) {
        const char *s = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!next && wants_value(s)) {
            fprintf(stderr, "%s needs a value\n", s);
            return -1;
        }
#define ARG(name) (strcmp(s, name) == 0 && next && ++i)
        if (ARG("--save")) a->save = next;
        else if (ARG("--out")) a->out = next;
        else if (ARG("--view")) a->view = next;
        else if (ARG("--mode")) a->mode = next;
        else if (ARG("--craft")) a->craft = next;
        else if (ARG("--quality")) a->quality = next;
        else if (ARG("--airframe")) a->airframe = next;
        else if (ARG("--seed")) { a->seed = (uint32_t)strtoul(next, NULL, 10); a->have_seed = 1; }
        else if (ARG("--size")) {
            /* A frame has to have pixels in it: everything downstream sizes a
             * buffer off these two and a 0x0 or a mistyped "1920" allocates
             * nothing and renders into it. */
            int w = 0, h = 0;
            if (sscanf(next, "%dx%d", &w, &h) != 2 || w < 16 || h < 16 ||
                w > 16384 || h > 16384) {
                fprintf(stderr, "--size wants WxH in pixels, 16..16384 each (got %s)\n", next);
                return -1;
            }
            a->w = w;
            a->h = h;
        }
        else if (ARG("--steps")) a->steps = atoi(next);
        else if (ARG("--pt-samples")) a->pt_samples = atoi(next);
        else if (ARG("--ssaa")) { a->ssaa = atoi(next); a->have_ssaa = 1; }
        else if (ARG("--msaa")) { a->msaa = atoi(next); a->have_msaa = 1; }
        else if (ARG("--ap")) a->ap = atoi(next);
        else { fprintf(stderr, "unknown option: %s\n", s); return -1; }
#undef ARG
    }
    if (!a->have_seed) a->seed = (uint32_t)time(NULL);
    return 0;
}

static fly_render_quality quality_from_name(const char *s) {
    if (strcmp(s, "low") == 0) return FLY_QUALITY_LOW;
    if (strcmp(s, "high") == 0) return FLY_QUALITY_HIGH;
    if (strcmp(s, "ultra") == 0) return FLY_QUALITY_ULTRA;
    if (strcmp(s, "medium") != 0)
        fprintf(stderr, "unknown quality '%s', using medium\n", s);
    return FLY_QUALITY_MEDIUM;
}

/* Is there a file there at all? The difference between "no save yet" and "a
 * save this build could not read" is the difference between starting a world
 * and destroying one. */
static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Which save was opened, and whether it was there: status, not output, so it
 * goes to stderr like every other line this file says about itself. `sim`
 * writes a CSV to stdout and the README's own example redirects it to a file;
 * with these on stdout that file began "loaded run.yaml" and no reader of a
 * CSV expects a sentence in its header row. */
static int app_from_args(fly_app *app, const fly_args *a, int fresh) {
    fly_craft_kind kind = strcmp(a->craft, "drone") == 0 ? FLY_CRAFT_DRONE : FLY_CRAFT_PLANE;
    if (!fresh && fly_app_init_load(app, a->w, a->h, a->save) == 0) {
        fprintf(stderr, "loaded %s\n", a->save);
    } else if (!fresh && file_exists(a->save)) {
        /* The save is there and it did not load. Every command that gets this
         * far writes the file back when it is done, so carrying on with a new
         * world would overwrite the player's run with a different one — a
         * seed, a chart and an aeroplane they have never seen — and the only
         * sign of it would be that the market is in a town with another name.
         * That is a corrupt or foreign save, and it is the caller's to decide
         * what to do with; the one thing this must not do is stand on it. */
        fprintf(stderr,
                "cannot read save %s (corrupt, or written by a build with "
                "assets this one cannot find)\nrefusing to overwrite it; move "
                "it aside to start a new world here\n", a->save);
        return -1;
    } else if (fly_app_init(app, a->w, a->h, a->seed, kind) != 0) {
        fprintf(stderr, "init failed\n");
        return -1;
    } else if (!fresh) {
        fprintf(stderr, "no save at %s; started a new world (seed %u)\n", a->save, a->seed);
    }
    if (a->airframe && fly_app_load_airframe(app, a->airframe) != 0)
        fprintf(stderr, "warning: could not load airframe %s\n", a->airframe);
    app->ui.ropts.mode = strcmp(a->mode, "pt") == 0 ? FLY_RENDER_PT :
                         strcmp(a->mode, "mix") == 0 ? FLY_RENDER_MIX : FLY_RENDER_RASTER;
    app->ui.ropts.pt_samples = a->pt_samples > 0 ? a->pt_samples : 1;
    /* The graphics level picks the sampling as well as the detail — they are
     * one budget — so an explicit --ssaa/--msaa is an override on top of it
     * rather than a value the level then contradicts. */
    {
        fly_render_opts q = fly_render_opts_quality(quality_from_name(a->quality));
        app->ui.ropts.detail = q.detail;
        app->ui.ropts.ao_target = q.ao_target;
        app->ui.ropts.ssaa = a->have_ssaa ? (a->ssaa > 0 ? a->ssaa : 1) : q.ssaa;
        app->ui.ropts.msaa = a->have_msaa ? (a->msaa > 0 ? a->msaa : 1) : q.msaa;
    }
    return 0;
}

static void set_view(fly_app *app, const char *view) {
    if (strcmp(view, "self") == 0) app->ui.view = FLY_VIEW_SELF;
    else if (strcmp(view, "help") == 0) app->ui.view = FLY_VIEW_HELP;
    else {
        app->ui.view = FLY_VIEW_PLAY;
        if (strcmp(view, "edit") == 0) app->ui.overlay = FLY_OVERLAY_EDIT;
        else if (strcmp(view, "map") == 0) app->ui.overlay = FLY_OVERLAY_MAP;
    }
}

/* Every site the autopilot will take, with the index `--ap` wants.
 *
 * `--ap` is an index into a world the caller cannot see. The chart is
 * generated, only sites already charted are legal, and which ones those are is
 * different on every seed — so the one number this option takes is the one
 * thing the CLI never told anybody. The refusal was a single line with a
 * question mark in it and no way to answer the question, and the README's own
 * example is `--ap 3`, which on most seeds is a site nobody has been to yet.
 *
 * Printed with the same three facts the in-game ROUTES list carries, and for
 * the same reason it carries them: what it is, how far, and whether this
 * airframe can get in at all. A gate is shown rather than hidden, because a
 * route you cannot fly is still a place you know about. */
static void print_routes(const fly_game *g, FILE *to) {
    int i, n = 0;
    fprintf(to, "charted routes (--ap takes the index):\n");
    for (i = 0; i < g->world.nloc; ++i) {
        const fly_location *L = &g->world.loc[i];
        uint32_t gate;
        if (!L->discovered) continue;
        gate = fly_world_gate_check(&g->world, i, &g->player.airframe);
        fprintf(to, "  %3d  %-14s %-9s ", i, L->name, fly_loc_kind_name(L->kind));
        if (g->docked == i) fprintf(to, "docked here\n");
        else fprintf(to, "%6.1f km%s%s\n",
                     fly_world_dist(fly_wpos_of(g->player.craft.pos), L->pos) / 1000.0f,
                     gate ? "   ! needs " : "", gate ? fly_gate_name(gate) : "");
        ++n;
    }
    if (!n) fprintf(to, "  (nothing charted yet)\n");
}

/* Engage the autopilot, and say something a caller can act on when it will
 * not go. The four refusals are four different problems and they were one
 * message. All of it to stderr: `sim`'s stdout is a CSV, and a CSV with a
 * route table in the middle of it is not one. */
static void engage_autopilot(fly_game *g, int loc) {
    fly_cmd cmd;
    int rc;
    /* Out of range first, and without applying it: the command's own answer to
       an index it does not have is to switch the autopilot *off* and report
       success, which is the right answer to "AP off" and the wrong one to a
       typed number. `--ap` is only read at all when it is >= 0, so the only way
       to arrive here out of range is a mistake. */
    if (loc < 0 || loc >= g->world.nloc) {
        fprintf(stderr, "no location %d: this world has %d\n", loc, g->world.nloc);
        print_routes(g, stderr);
        return;
    }
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = FLY_CMD_AUTOPILOT;
    cmd.a = loc;
    rc = fly_game_apply(g, &cmd);
    if (rc == 0) return;
    /* Why, asked of the world rather than of the return code. `fly_game_apply`
       documents "0 ok, negative error" and nothing finer, and two of these
       refusals do in fact share a code — a route the airframe cannot enter and
       a player who is not in an aeroplane are both -3. Reading the same three
       facts the ROUTES list reads gives an answer that stays right whatever
       the codes do next. */
    {
        uint32_t gate = fly_world_gate_check(&g->world, loc, &g->player.airframe);
        const char *name = g->world.loc[loc].name;
        if (!g->world.loc[loc].discovered)
            fprintf(stderr, "%s is not charted yet\n", name);
        else if (gate)
            fprintf(stderr, "%s will not have this airframe: needs %s\n",
                    name, fly_gate_name(gate));
        else if (g->mode != FLY_MODE_FLIGHT)
            fprintf(stderr, "not flying: the autopilot needs an aeroplane under you\n");
        else
            fprintf(stderr, "the autopilot will not take %s (%d)\n", name, rc);
    }
    print_routes(g, stderr);
}

static int cmd_new(const fly_args *a) {
    fly_app app;
    if (app_from_args(&app, a, 1) != 0) return 1;
    if (fly_game_save(&app.game, a->save) != 0) {
        fprintf(stderr, "cannot write %s\n", a->save);
        fly_app_free(&app);
        return 1;
    }
    printf("new game (seed %u, %s) saved to %s\n", app.game.seed, app.game.player.airframe.name, a->save);
    printf("renderer: %s\n",
           fly_gpu_init() == 0 ? fly_gpu_renderer() : "CPU (no GPU backend)");
    printf("docked at %s; %d locations out there\n",
           app.game.world.loc[0].name, app.game.world.nloc);
    fly_app_free(&app);
    return 0;
}

static int cmd_render(const fly_args *a) {
    fly_app app;
    if (app_from_args(&app, a, 0) != 0) return 1;
    set_view(&app, a->view);
    if (a->ap >= 0) engage_autopilot(&app.game, a->ap);
    int i;
    for (i = 0; i < a->steps; ++i) fly_app_step(&app, FLY_APP_DT);
    if (fly_app_shot(&app, a->out) != 0) {
        fprintf(stderr, "cannot write %s\n", a->out);
        fly_app_free(&app);
        return 1;
    }
    printf("wrote %s (%dx%d, view %s, mode %s)\n", a->out, a->w, a->h, a->view, a->mode);
    fly_app_free(&app);
    return 0;
}

static int cmd_sim(const fly_args *a) {
    fly_app app;
    if (app_from_args(&app, a, 0) != 0) return 1;
    fly_game *g = &app.game;
    if (a->ap >= 0) engage_autopilot(g, a->ap);
    printf("t,x,y,z,speed,alt_agl,fuel,hp,wear,stalled,docked\n");
    int i;
    for (i = 0; i < a->steps; ++i) {
        fly_app_step(&app, FLY_APP_DT);
        if (i % 30 == 0) {
            float gz = fly_world_ground(&g->world, g->player.craft.pos.x, g->player.craft.pos.y);
            printf("%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%d,%d\n",
                   i * FLY_APP_DT, g->player.craft.pos.x, g->player.craft.pos.y, g->player.craft.pos.z,
                   fly_craft_airspeed(&g->player.craft, &g->weather), g->player.craft.pos.z - gz,
                   g->player.craft.fuel, g->player.craft.hp, g->player.craft.wear, g->player.craft.stalled, g->docked);
        }
    }
    if (fly_game_save(g, a->save) != 0)
        fprintf(stderr, "warning: could not write %s\n", a->save);
    fly_app_free(&app);
    return 0;
}

static int cmd_market(const fly_args *a) {
    fly_app app;
    if (app_from_args(&app, a, 0) != 0) return 1;
    fly_game *g = &app.game;
    if (g->docked < 0) {
        printf("not docked; market data needs a pad under your wheels\n");
        fly_app_free(&app);
        return 0;
    }
    const fly_location *L = &g->world.loc[g->docked];
    printf("market at %s [%s]  (tokens: %ld)\n", L->name, fly_loc_kind_name(L->kind), g->player.tokens);
    printf("%-10s %8s %8s %8s\n", "resource", "price", "stock", "cargo");
    int r;
    for (r = 0; r < FLY_RES_COUNT; ++r)
        printf("%-10s %8.1f %8.0f %8.1f\n", fly_resource_name((fly_resource)r),
               fly_world_price(&g->world, g->docked, (fly_resource)r), L->stock[r], g->player.cargo[r]);
    printf("\nshelf:\n");
    for (r = 0; r < FLY_SITE_STOCK_MAX; ++r) {
        const fly_item *it = &g->stock[g->docked][r];
        char nm[64];
        if (!it->uid) continue;
        fly_item_name(it, nm, sizeof nm);
        printf("  [%d] %-40s %6ld tk\n", r, nm, fly_game_item_price(g, g->docked, it));
    }
    printf("\nhold (%d/%d):\n", g->item_count, FLY_ITEM_MAX);
    for (r = 0; r < g->item_count; ++r) {
        const fly_item *it = &g->items[r];
        char nm[64];
        fly_item_name(it, nm, sizeof nm);
        printf("  %-8u %-40s %s\n", it->uid, nm,
               fly_game_item_fitted(g, it->uid) ? "fitted" : "");
    }
    printf("\nairframes:\n");
    for (r = 0; r < fly_game_airframe_catalog_count(); ++r) {
        const fly_airframe_catalog *c = fly_game_airframe_catalog_get(r);
        printf("  %-24s %6ld tk  stock %u\n", c->name,
               fly_game_airframe_price(g, g->docked, r),
               (unsigned)g->airframe_stock[g->docked][r]);
    }
    printf("\ncontract offers:\n");
    int i;
    for (i = 0; i < FLY_MAX_CONTRACTS; ++i) {
        const fly_contract *ct = &g->contracts[i];
        if (ct->state != 1) continue;
        printf("  [%d] %.0f %s to %s  +%ld tk\n", i, ct->qty,
               fly_resource_name((fly_resource)ct->res), g->world.loc[ct->to].name, ct->reward);
    }
    /* And where you can take them. A contract names a destination and the
       autopilot wants that destination's index; without this the two halves of
       one decision were in different commands, and one of them was in no
       command at all. */
    printf("\n");
    print_routes(g, stdout);
    fly_app_free(&app);
    return 0;
}

static int cmd_play(const fly_args *a) {
    fly_app app;
    if (app_from_args(&app, a, 0) != 0) return 1;
#ifdef FLY_WINDOW
    int rc = fly_win_run(&app);
    if (fly_game_save(&app.game, a->save) != 0) {
        fprintf(stderr, "cannot write %s\n", a->save);
        rc = rc ? rc : 1;
    }
    fly_app_free(&app);
    return rc;
#else
    /* headless play: engage autopilot on the nearest contract-worthy site and
     * let the world run; this is the AFK mode without a display */
    fly_game *g = &app.game;
    printf("headless session (build with --window for realtime graphics)\n");
    if (a->ap >= 0) engage_autopilot(g, a->ap);
    int i;
    for (i = 0; i < a->steps; ++i) {
        fly_app_step(&app, FLY_APP_DT);
    }
    int li;
    for (li = 0; li < g->log_count; ++li) printf("| %s\n", g->log[li]);
    printf("t=%.0fs pos=(%.0f,%.0f,%.0f) fuel=%.0f hp=%.0f tokens=%ld\n",
           a->steps * FLY_APP_DT, g->player.craft.pos.x, g->player.craft.pos.y, g->player.craft.pos.z,
           g->player.craft.fuel, g->player.craft.hp, g->player.tokens);
    if (fly_game_save(g, a->save) != 0) {
        fprintf(stderr, "cannot write %s\n", a->save);
        fly_app_free(&app);
        return 1;
    }
    printf("saved %s\n", a->save);
    fly_app_free(&app);
    return 0;
#endif
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 0; }
    const char *cmd = argv[1];
    fly_args a;
    args_default(&a);
    if (args_parse(&a, argc - 2, argv + 2) != 0) return 1;

    if (strcmp(cmd, "new") == 0) return cmd_new(&a);
    if (strcmp(cmd, "play") == 0) return cmd_play(&a);
    if (strcmp(cmd, "render") == 0) return cmd_render(&a);
    if (strcmp(cmd, "sim") == 0) return cmd_sim(&a);
    if (strcmp(cmd, "market") == 0) return cmd_market(&a);
    usage();
    return strcmp(cmd, "help") == 0 ? 0 : 1;
}
