#include "fly_ui.h"

#include <stdio.h>
#include <string.h>

#include "fly_hud.h"

/* font sizes shared with the HUD */
#define FS_S FLY_FS_S
#define FS_B FLY_FS_VAL
#define FS_M FLY_FS_HEAD
#define FS_L FLY_FS_TITLE

void fly_ui_init(fly_ui *ui) {
    memset(ui, 0, sizeof *ui);
    ui->view = FLY_VIEW_SPLASH;
    ui->throttle = 0.0f;
    ui->ropts = fly_render_opts_default();
    /* Interactive frames buy occlusion on an instalment plan.
     *
     * The default budget is unlimited, which is right for a still: the frame is
     * then the same whether the window was warm or cold, and every gate and
     * gallery image depends on that. A session cannot afford it. Arriving
     * somewhere cold costs about 79k rays — a hundred milliseconds in one lump,
     * which is a visible stall — where flying only ever uncovers the strip of
     * lattice that just came over the horizon and costs nothing. So cap what a
     * frame may add: 2500 rays is a little over three milliseconds, converges a
     * cold arrival in about half a second, and leaves the steady state (which
     * traces nothing at all) untouched. Cells not yet filled read as open sky,
     * so the term fades in rather than popping. */
    ui->ropts.ao_budget = 2500;
    ui->view_w = 960;
    ui->view_h = 540;
}

/* ---------------- on-screen control zones (touch / mouse) ----------------
 * Layout in the play view (no overlay):
 *  - left ~55% below the tab strip: virtual stick; press anchors it, drag
 *    sets pitch/roll (drag down = pull back = nose up)
 *  - right edge strip: throttle slider (absolute)
 *  - right side pad above the radar: one button per weapon fitted, held
 *    rather than tapped — FIRE for a gun, LNCH for a launcher, DECY for a
 *    dispenser. An unarmed aeroplane gets no pane at all.
 */

#define FLY_STICK_RADIUS_FRAC 0.16f /* drag distance for full deflection, of h */

static int zone_slider(const fly_ui *ui, float x, float y) {
    return x > (float)ui->view_w - FLY_TOUCH_STRIP &&
           y > FLY_TOUCH_TOP(ui->view_h) && y < FLY_TOUCH_BOT(ui->view_h);
}

/* How many buttons the action column is carrying, and which is which.
 *
 * The column was built for one and documented as sized from the count; it now
 * has up to three, and the count depends on what is fitted. Everything — the
 * pane, the paint and the hit test — is derived from these two functions, so a
 * button that is drawn is a button you can press and one that is absent is not
 * a dead circle of glass. Fire first because it is always there when anything
 * is; then the rails, then the dispenser. */
static int act_count(const fly_game *g) {
    int n = 0;
    if (g->player.airframe.gun_dps > 0.0f) ++n;
    if (g->player.airframe.ord_kind) ++n;
    if (g->player.airframe.cm_kind) ++n;
    return n;
}

/* The index of one action in that column, or -1 when it is not fitted. */
static int act_slot(const fly_game *g, int which) {   /* 0 fire, 1 launch, 2 decoy */
    int n = 0;
    if (g->player.airframe.gun_dps > 0.0f) { if (which == 0) return n; ++n; }
    else if (which == 0) return -1;
    if (g->player.airframe.ord_kind) { if (which == 1) return n; ++n; }
    else if (which == 1) return -1;
    if (g->player.airframe.cm_kind) { if (which == 2) return n; }
    else if (which == 2) return -1;
    return which == 2 ? n : -1;
}

/* The button's own box, so the target and the paint cannot drift apart. */
static int zone_act(const fly_ui *ui, const fly_game *g, int which, float x, float y) {
    int n = act_count(g), i = act_slot(g, which);
    float bx, by, dx, dy, r = FLY_ACT_BTN * 0.5f + 6.0f;
    if (i < 0 || n <= 0) return 0;
    bx = FLY_ACT_BX((float)ui->view_w);
    by = FLY_ACT_BY((float)ui->view_h, n, i);
    dx = x - bx;
    dy = y - by;
    return dx * dx + dy * dy <= r * r;
}

static int zone_stick(const fly_ui *ui, float x, float y) {
    return x < ui->view_w * 0.55f && y > ui->view_h * 0.24f;
}

static float slider_value(const fly_ui *ui, float y) {
    float top = FLY_TOUCH_TOP(ui->view_h) + ui->view_h * 0.02f;
    float bot = FLY_TOUCH_BOT(ui->view_h) - ui->view_h * 0.02f;
    return fly_clampf((bot - y) / (bot - top), 0.0f, 1.0f);
}

/* ---------------- edit overlay model ---------------- */

typedef struct {
    /* long enough for a slot tag, the longest generated item name and its
     * rolls on one line; the row draw clips to the column anyway, and a
     * truncation here would cut a number in half rather than a word */
    char label[160];
    char right[28];  /* right-aligned column (price, status) */
    int enabled;
    int header;      /* section heading, not selectable */
    fly_icon icon;   /* what kind of thing this row is, read before the words */
    fly_cmd cmd;
    int is_cmd;
} fly__edit_row;

#define FLY_EDIT_MAX 128

/* clip `src` with an ellipsis so it fits `max_w` at size `px` */
static void fit_row(char *out, size_t n, float px, float max_w, const char *src) {
    size_t len = strlen(src);
    if (len >= n) len = n - 1;
    memcpy(out, src, len);
    out[len] = '\0';
    if (max_w <= 0.0f) { out[0] = '\0'; return; }
    while (len > 1 && fly_text_width(FLY_FONT_UI, px, out) > max_w) {
        --len;
        out[len] = '\0';
        if (len > 2) { out[len - 1] = '.'; out[len - 2] = '.'; }
    }
}

static fly__edit_row *fly__row(fly__edit_row *rows, int *n, int max) {
    if (*n >= max) return NULL;
    fly__edit_row *r = &rows[(*n)++];
    memset(r, 0, sizeof *r);
    return r;
}

static void fly__header(fly__edit_row *rows, int *n, int max, const char *title,
                        fly_icon icon) {
    fly__edit_row *r = fly__row(rows, n, max);
    if (!r) return;
    snprintf(r->label, sizeof r->label, "%s", title);
    r->header = 1;
    r->icon = icon;
}

/* Which sprite a module wears: the slot it goes in.
 *
 * Six slots, six shapes, and the same six wherever a module is listed — on the
 * rack, in the hold, on the shelf, on the surplus bench. A hold with four
 * engines and a wing in it used to be five rows of the same chip glyph and a
 * three-letter tag, so telling them apart meant reading; now the column sorts
 * itself and the tag is confirmation rather than the only signal. */
static fly_icon fly__slot_icon(int slot) {
    switch (slot) {
    case FLY_SLOT_ENGINE: return FLY_ICON_POWER;
    case FLY_SLOT_WINGS: return FLY_ICON_CRAFT;
    case FLY_SLOT_HARDPOINT: return FLY_ICON_ORD;
    case FLY_SLOT_BAY: return FLY_ICON_TASK;
    case FLY_SLOT_AVIONICS: return FLY_ICON_PART;
    case FLY_SLOT_HULL: return FLY_ICON_SHIELD;
    default: return FLY_ICON_PART;
    }
}

/* every row under a header inherits its icon unless it says otherwise */
static void fly__mark(fly__edit_row *rows, int from, int n, fly_icon icon) {
    int i;
    for (i = from; i < n; ++i)
        if (!rows[i].header && rows[i].icon == FLY_ICON_NONE) rows[i].icon = icon;
}

static int edit_build(const fly_ui *ui, const fly_game *g, fly__edit_row *rows, int max) {
    int n = 0;
    int docked = g->docked >= 0;
    fly__edit_row *r;
    (void)ui;

    /* Ruin collapses the overlay to one row. Everything else it could offer
     * needs an aircraft, money or cargo, and the point of the state is that
     * there is none of any of them. */
    if (g->ruin) {
        fly__header(rows, &n, max, "GROUNDED", FLY_ICON_CRAFT);
        if ((r = fly__row(rows, &n, max)) != NULL) {
            snprintf(r->label, sizeof r->label, "Sign on");
            snprintf(r->right, sizeof r->right, "run %d", g->operator_seq + 2);
            r->icon = FLY_ICON_CRAFT;
            r->enabled = 1;
            r->cmd.kind = FLY_CMD_RESTART;
            r->is_cmd = 1;
        }
        return n;
    }

    fly__header(rows, &n, max, "SERVICE", FLY_ICON_SERVICE);
    int sect = n;
    if ((r = fly__row(rows, &n, max)) != NULL) {
        snprintf(r->label, sizeof r->label, "Refuel");
        snprintf(r->right, sizeof r->right, "%.0f/%.0f", g->player.craft.fuel, g->player.airframe.fuel_cap);
        r->enabled = docked;
        r->cmd.kind = FLY_CMD_REFUEL;
        r->is_cmd = 1;
    }
    if ((r = fly__row(rows, &n, max)) != NULL) {
        r->icon = FLY_ICON_REPAIR;
        snprintf(r->label, sizeof r->label, "Repair");
        snprintf(r->right, sizeof r->right, "wear %.0f%%  hull %.0f%%",
                 g->player.craft.wear * 100.0f, g->player.craft.hp / g->player.airframe.structure * 100.0f);
        r->enabled = docked;
        r->cmd.kind = FLY_CMD_REPAIR;
        r->is_cmd = 1;
    }
    /* Rearm, and only for an aeroplane with something to rearm. A row offered
     * to an unarmed hauler would be a row that is greyed out for most of the
     * game with no explanation, which is the failure the pledge row below is
     * written to avoid. */
    if ((g->player.airframe.ord_kind || g->player.airframe.cm_kind) &&
        (r = fly__row(rows, &n, max)) != NULL) {
        const fly_ord_class *ok = fly_ord_class_get(g->player.airframe.ord_kind);
        const fly_ord_class *ck = fly_ord_class_get(g->player.airframe.cm_kind);
        r->icon = FLY_ICON_ORD;
        snprintf(r->label, sizeof r->label, "Rearm");
        if (ok && ck)
            snprintf(r->right, sizeof r->right, "%s %d/%d  %s %d/%d", ok->tag,
                     g->player.craft.ammo, g->player.airframe.ord_ammo, ck->tag,
                     g->player.craft.cm, g->player.airframe.cm_ammo);
        else if (ok)
            snprintf(r->right, sizeof r->right, "%s %d/%d", ok->tag,
                     g->player.craft.ammo, g->player.airframe.ord_ammo);
        else
            snprintf(r->right, sizeof r->right, "%s %d/%d", ck->tag,
                     g->player.craft.cm, g->player.airframe.cm_ammo);
        r->enabled = docked && (g->player.craft.ammo < g->player.airframe.ord_ammo ||
                                g->player.craft.cm < g->player.airframe.cm_ammo);
        r->cmd.kind = FLY_CMD_REARM;
        r->is_cmd = 1;
    }
    /* The droplet is the section's, and the fallback for anything in it that
     * has not said otherwise: a pad sells fuel, work and ordnance, and drawing
     * one drop against all three made them one thing with three labels. */
    fly__mark(rows, sect, n, FLY_ICON_SERVICE);

    /* --- the oath ---
     *
     * Only ever offered where it can actually be taken: at a field the power
     * holds. That is deliberate and it is the whole shape of the mechanic —
     * you go to them. A row that is offered but refused says *why* on its
     * right-hand column, because "swear to the Foundry" greyed out with no
     * explanation is indistinguishable from a bug.
     *
     * Resigning is offered wherever the operator is standing, because walking
     * away from a flag does not need anybody's permission. */
    {
        int owner = docked ? fly_game_site_owner(g, g->docked) : FLY_FACTION_FREE;
        int can_swear = fly_faction_is_power(owner) && owner != g->player.faction;
        int can_resign = g->player.faction != FLY_FACTION_FREE;
        /* One header for both rows. Standing at a rival's field while sworn to
         * somebody offers a swear *and* a resign, and two sections running
         * "ALLEGIANCE / ALLEGIANCE" is the sort of thing that only happens in
         * the one state nobody tests. */
        if (can_swear || can_resign) {
            fly__header(rows, &n, max, "ALLEGIANCE", FLY_ICON_FLAG);
            sect = n;
        }
        if (can_swear && (r = fly__row(rows, &n, max)) != NULL) {
            int ok = fly_game_pledge_ok(g, owner);
            snprintf(r->label, sizeof r->label, "Swear to the %s", fly_faction_name(owner));
            if (ok == 0)
                snprintf(r->right, sizeof r->right, "standing %d",
                         fly_game_standing(g, owner));
            else
                snprintf(r->right, sizeof r->right, "! need %d", FLY_STAND_PLEDGE);
            r->enabled = ok == 0;
            r->cmd.kind = FLY_CMD_PLEDGE;
            r->cmd.a = owner;
            r->is_cmd = 1;
        }
        if (can_resign && (r = fly__row(rows, &n, max)) != NULL) {
            snprintf(r->label, sizeof r->label, "Resign from the %s",
                     fly_faction_name(g->player.faction));
            snprintf(r->right, sizeof r->right, "-20 standing");
            r->enabled = 1;
            r->cmd.kind = FLY_CMD_PLEDGE;
            r->cmd.a = FLY_FACTION_FREE;
            r->is_cmd = 1;
        }
        if (can_swear || can_resign) fly__mark(rows, sect, n, FLY_ICON_FLAG);
    }

    int i, any = 0;
    for (i = 0; i < FLY_MAX_CONTRACTS; ++i)
        if (g->contracts[i].state == 1) any = 1;
    if (any) {
        fly__header(rows, &n, max, "TASKS", FLY_ICON_TASK);
        sect = n;
        for (i = 0; i < FLY_MAX_CONTRACTS; ++i) {
            const fly_contract *ct = &g->contracts[i];
            if (ct->state != 1) continue;
            if ((r = fly__row(rows, &n, max)) == NULL) break;
            snprintf(r->label, sizeof r->label, "%.0f %s > %s", ct->qty,
                     fly_resource_name((fly_resource)ct->res), g->world.loc[ct->to].name);
            /* What the destination demands, before the job is taken rather than
               after. Gated sites pay a premium — that is the point of them —
               but the premium was the only thing on the row, so the very first
               contract a new operator is offered can be one their aeroplane can
               never land at, and nothing said so until they got there. ROUTES
               has shown this all along; TASKS is where it decides anything. */
            {
                uint32_t gate = fly_world_gate_check(&g->world, ct->to, &g->player.airframe);
                if (gate) snprintf(r->right, sizeof r->right, "! %s", fly_gate_name(gate));
                else snprintf(r->right, sizeof r->right, "+%ld tk", ct->reward);
                r->enabled = docked && g->docked == ct->from && !gate;
            }
            r->cmd.kind = FLY_CMD_ACCEPT;
            r->cmd.a = i;
            r->is_cmd = 1;
        }
        fly__mark(rows, sect, n, FLY_ICON_TASK);
    }

    if (docked) {
        fly__header(rows, &n, max, "MARKET", FLY_ICON_TRADE);
        sect = n;
        for (i = 0; i < FLY_RES_COUNT && n < max - 1; ++i) {
            float px = fly_world_price(&g->world, g->docked, (fly_resource)i);
            if ((r = fly__row(rows, &n, max)) == NULL) break;
            snprintf(r->label, sizeof r->label, "Buy 5 %s", fly_resource_name((fly_resource)i));
            snprintf(r->right, sizeof r->right, "%.0f in  %.1f tk",
                     g->world.loc[g->docked].stock[i], px);
            r->enabled = g->world.loc[g->docked].stock[i] >= 5.0f;
            r->cmd.kind = FLY_CMD_BUY;
            r->cmd.a = i;
            r->cmd.f = 5.0f;
            r->is_cmd = 1;
            if ((r = fly__row(rows, &n, max)) == NULL) break;
            snprintf(r->label, sizeof r->label, "Sell 5 %s", fly_resource_name((fly_resource)i));
            snprintf(r->right, sizeof r->right, "%.0f held  %.1f tk", g->player.cargo[i], px * 0.92f);
            r->enabled = g->player.cargo[i] >= 5.0f;
            r->cmd.kind = FLY_CMD_SELL;
            r->cmd.a = i;
            r->cmd.f = 5.0f;
            r->is_cmd = 1;
        }
        fly__mark(rows, sect, n, FLY_ICON_TRADE);
    }

    /* What is bolted on. One row per occupied slot, because a build is read
     * slot by slot — six choices, not a list of everything that exists. */
    {
        const uint32_t *fit = fly_game_fitted(g);
        fly__header(rows, &n, max, "FITTED", FLY_ICON_PART);
        sect = n;
        for (i = 0; i < FLY_SLOT_COUNT; ++i) {
            const fly_item *it = fit ? fly_game_item(g, fit[i]) : NULL;
            char nm[64];
            if ((r = fly__row(rows, &n, max)) == NULL) break;
            if (!it) {
                snprintf(r->label, sizeof r->label, "  %s -", fly_slot_tag(i));
                snprintf(r->right, sizeof r->right, "empty");
                r->enabled = 0;
                r->is_cmd = 0;
                r->icon = fly__slot_icon(i);
                continue;
            }
            fly_item_name(it, nm, sizeof nm);
            {
                char rolls[64];
                fly_item_rolls(it, rolls, sizeof rolls);
                snprintf(r->label, sizeof r->label, "* %s %s%s%s", fly_slot_tag(i), nm,
                         rolls[0] ? "  " : "", rolls);
            }
            snprintf(r->right, sizeof r->right, "unfit");
            r->enabled = docked;
            r->cmd.kind = FLY_CMD_UNFIT_MODULE;
            r->cmd.a = i;
            r->cmd.data.fit.airframe = g->active_airframe;
            r->is_cmd = 1;
            r->icon = fly__slot_icon(i);
        }
        /* fly__mark only fills rows that have not chosen for themselves, so
         * this is the fallback for any future row here rather than an override */
        fly__mark(rows, sect, n, FLY_ICON_PART);
    }

    /* The hold: every loose item, one row each, because two of the same base
     * are two different items now and a player has to be able to tell which
     * one they are fitting. */
    {
        int held = 0;
        for (i = 0; i < g->item_count; ++i) {
            const fly_item *it = &g->items[i];
            const fly_module *u = fly_module_get(it->base);
            char nm[64];
            int kind_ok;
            if (!u || fly_game_item_fitted(g, it->uid)) continue;
            if (!held) {
                fly__header(rows, &n, max, "HOLD", FLY_ICON_PART);
                sect = n;
                held = 1;
            }
            kind_ok = u->craft_kind < 0 || (fly_craft_kind)u->craft_kind == g->player.airframe.kind;
            if ((r = fly__row(rows, &n, max)) == NULL) break;
            fly_item_name(it, nm, sizeof nm);
            {
                char rolls[64];
                fly_item_rolls(it, rolls, sizeof rolls);
                snprintf(r->label, sizeof r->label, "  %s %s%s%s", fly_slot_tag(u->slot), nm,
                         rolls[0] ? "  " : "", rolls);
            }
            if (!kind_ok) snprintf(r->right, sizeof r->right, "! wrong craft");
            else snprintf(r->right, sizeof r->right, "fit  %ld tk",
                          (long)((float)fly_game_item_price(g, g->docked >= 0 ? g->docked : 0, it)
                                 * FLY_RESALE));
            r->enabled = docked && kind_ok;
            r->cmd.kind = FLY_CMD_FIT_MODULE;
            r->cmd.a = it->base;
            r->cmd.data.fit.uid = it->uid;
            r->cmd.data.fit.airframe = g->active_airframe;
            r->is_cmd = 1;
            r->icon = fly__slot_icon(u->slot);
        }
        if (held) fly__mark(rows, sect, n, FLY_ICON_PART);
    }

    /* The shelf. A vendor sells particular items, so this is what is actually
     * on it today rather than a catalogue of everything that could be. */
    if (docked) {
        int any = 0;
        for (i = 0; i < FLY_SITE_STOCK_MAX; ++i) {
            const fly_item *it = &g->stock[g->docked][i];
            const fly_module *u = fly_module_get(it->base);
            char nm[64];
            if (!it->uid || !u) continue;
            if (!any) {
                fly__header(rows, &n, max, "SHELF", FLY_ICON_CASH);
                sect = n;
                any = 1;
            }
            if ((r = fly__row(rows, &n, max)) == NULL) break;
            fly_item_name(it, nm, sizeof nm);
            {
                char rolls[64];
                fly_item_rolls(it, rolls, sizeof rolls);
                snprintf(r->label, sizeof r->label, "  %s %s%s%s", fly_slot_tag(u->slot), nm,
                         rolls[0] ? "  " : "", rolls);
            }
            snprintf(r->right, sizeof r->right, "%ld tk", fly_game_item_price(g, g->docked, it));
            r->enabled = 1;
            r->cmd.kind = FLY_CMD_BUY_MODULE;
            r->cmd.a = i;
            r->is_cmd = 1;
            r->icon = fly__slot_icon(u->slot);
        }
        if (any) fly__mark(rows, sect, n, FLY_ICON_CASH);
    }

    /* What to do with the surplus. Listed apart from the shelf because it is
     * what a player reaches for once the hold has four engines in it and none
     * of them is the one on the aeroplane: sell it, break it for parts, pay to
     * redraw it, or eat three of its slot to push it a tier. */
    if (docked) {
        int any = 0;
        for (i = 0; i < g->item_count; ++i) {
            const fly_item *it = &g->items[i];
            char nm[64];
            float parts = 0.0f;
            long tk = 0;
            int fodder = 0;
            if (!fly_module_get(it->base) || fly_game_item_fitted(g, it->uid)) continue;
            if (!any) {
                fly__header(rows, &n, max, "SURPLUS", FLY_ICON_CASH);
                sect = n;
                any = 1;
            }
            fly_item_name(it, nm, sizeof nm);
            if ((r = fly__row(rows, &n, max)) == NULL) break;
            snprintf(r->label, sizeof r->label, "Sell %s", nm);
            snprintf(r->right, sizeof r->right, "+%ld tk",
                     (long)((float)fly_game_item_price(g, g->docked, it) * FLY_RESALE));
            r->enabled = 1;
            r->cmd.kind = FLY_CMD_SELL_MODULE;
            r->cmd.a = it->base;
            r->cmd.data.fit.uid = it->uid;
            r->is_cmd = 1;

            if ((r = fly__row(rows, &n, max)) == NULL) break;
            {
                int rc = fly_game_salvage_yield(g, it->uid, &parts, NULL);
                snprintf(r->label, sizeof r->label, "  break up for parts");
                if (rc == -4) snprintf(r->right, sizeof r->right, "hold full");
                else snprintf(r->right, sizeof r->right, "+%.0f parts", parts);
                r->enabled = rc == 0;
                r->cmd.kind = FLY_CMD_SALVAGE_MODULE;
                r->cmd.a = it->base;
                r->cmd.data.fit.uid = it->uid;
                r->is_cmd = 1;
            }

            if (it->naff) {
                int rc = fly_game_reroll_cost(g, it->uid, &parts, &tk);
                if ((r = fly__row(rows, &n, max)) == NULL) break;
                snprintf(r->label, sizeof r->label, "  redraw its rolls");
                snprintf(r->right, sizeof r->right, "%ld tk %.0f pt", tk, parts);
                r->enabled = rc == 0;
                r->cmd.kind = FLY_CMD_REROLL_MODULE;
                r->cmd.a = it->base;
                r->cmd.data.fit.uid = it->uid;
                r->is_cmd = 1;
            }

            if (it->rarity < FLY_UPRATE_CEILING) {
                int rc = fly_game_uprate_cost(g, it->uid, &fodder, &tk);
                if ((r = fly__row(rows, &n, max)) == NULL) break;
                snprintf(r->label, sizeof r->label, "  uprate to %s",
                         fly_item_rarity_name(it->rarity + 1));
                snprintf(r->right, sizeof r->right, "%ld tk %d/%d spares", tk, fodder,
                         FLY_UPRATE_FODDER);
                r->enabled = rc == 0;
                r->cmd.kind = FLY_CMD_UPRATE_MODULE;
                r->cmd.a = it->base;
                r->cmd.data.fit.uid = it->uid;
                r->is_cmd = 1;
            }
        }
        if (any) fly__mark(rows, sect, n, FLY_ICON_CASH);
    }

    if (docked) {
        fly__header(rows, &n, max, "HANGAR", FLY_ICON_CRAFT);
        sect = n;
        for (i = 0; i < g->owned_count; ++i) {
            const fly_owned_airframe *o = &g->owned[i];
            if ((r = fly__row(rows, &n, max)) == NULL) break;
            snprintf(r->label, sizeof r->label, "%s #%u %s",
                     i == g->active_airframe ? "*" : " ", o->instance_id, o->catalog_id);
            snprintf(r->right, sizeof r->right, "%s  life %.0f%%",
                     o->status == FLY_AIRFRAME_DESTROYED ? "wrecked" : "select",
                     (1.0f - o->fatigue) * 100.0f);
            r->enabled = o->status == FLY_AIRFRAME_USABLE && o->location == g->docked && i != g->active_airframe;
            r->cmd.kind = FLY_CMD_SELECT_AIRFRAME;
            r->cmd.a = i;
            r->is_cmd = 1;
        }
        for (i = 0; i < fly_game_airframe_catalog_count(); ++i) {
            const fly_airframe_catalog *c = fly_game_airframe_catalog_get(i);
            if ((r = fly__row(rows, &n, max)) == NULL) break;
            snprintf(r->label, sizeof r->label, "Buy %s", c->name);
            snprintf(r->right, sizeof r->right, "x%u  %ld tk",
                     (unsigned)g->airframe_stock[g->docked][i],
                     fly_game_airframe_price(g, g->docked, i));
            r->enabled = g->airframe_stock[g->docked][i] > 0;
            r->cmd.kind = FLY_CMD_BUY_AIRFRAME;
            r->cmd.a = i;
            r->is_cmd = 1;
            if ((r = fly__row(rows, &n, max)) == NULL) break;
            snprintf(r->label, sizeof r->label, "Build %s", c->name);
            snprintf(r->right, sizeof r->right, "%.0f alloy %.0f parts", c->build_alloy, c->build_parts);
            r->enabled = g->player.cargo[FLY_RES_ALLOY] >= c->build_alloy &&
                         g->player.cargo[FLY_RES_PARTS] >= c->build_parts;
            r->cmd.kind = FLY_CMD_BUILD_AIRFRAME;
            r->cmd.a = i;
            r->is_cmd = 1;
        }
        fly__mark(rows, sect, n, FLY_ICON_CRAFT);
    }

    fly__header(rows, &n, max, "ROUTES", FLY_ICON_ROUTE);
    sect = n;
    for (i = 0; i < g->world.nloc; ++i) {
        if (!g->world.loc[i].discovered || g->docked == i) continue;
        if ((r = fly__row(rows, &n, max)) == NULL) break;
        uint32_t gate = fly_world_gate_check(&g->world, i, &g->player.airframe);
        snprintf(r->label, sizeof r->label, "%s", g->world.loc[i].name);
        if (gate) snprintf(r->right, sizeof r->right, "! %s", fly_gate_name(gate));
        else snprintf(r->right, sizeof r->right, "%s", fly_loc_kind_name(g->world.loc[i].kind));
        r->enabled = 1;
        r->cmd.kind = FLY_CMD_AUTOPILOT;
        r->cmd.a = i;
        r->is_cmd = 1;
    }
    if ((r = fly__row(rows, &n, max)) != NULL) {
        snprintf(r->label, sizeof r->label, "%s", g->mode == FLY_MODE_WALK ? "Board" :
                 g->docked >= 0 ? "Launch" : "Cancel route");
        r->enabled = 1;
        if (g->mode == FLY_MODE_WALK) r->cmd.kind = FLY_CMD_INTERACT;
        else if (g->docked >= 0) r->cmd.kind = FLY_CMD_LAUNCH;
        else { r->cmd.kind = FLY_CMD_AUTOPILOT; r->cmd.a = -1; }
        r->is_cmd = 1;
    }
    if (g->mode == FLY_MODE_FLIGHT && g->docked >= 0 && (r = fly__row(rows, &n, max)) != NULL) {
        snprintf(r->label, sizeof r->label, "Disembark");
        r->enabled = 1;
        r->cmd.kind = FLY_CMD_DISEMBARK;
        r->is_cmd = 1;
    }
    fly__mark(rows, sect, n, FLY_ICON_ROUTE);
    return n;
}

int fly_ui_edit_rows(const fly_ui *ui, const fly_game *g) {
    fly__edit_row rows[FLY_EDIT_MAX];
    return edit_build(ui, g, rows, FLY_EDIT_MAX);
}

/* first selectable row at/after `from` walking in `dir`; -1 if none */
static int edit_seek(const fly_ui *ui, const fly_game *g, int from, int dir) {
    fly__edit_row rows[FLY_EDIT_MAX];
    int n = edit_build(ui, g, rows, FLY_EDIT_MAX);
    int i;
    for (i = from; i >= 0 && i < n; i += dir)
        if (!rows[i].header && rows[i].is_cmd) return i;
    return -1;
}

/* edit overlay geometry: a centered window (shared by render + hit-testing) */
#define EDIT_Y 34.0f
/* the list starts below the title band: title pad, glyph, rule, then air */
#define EDIT_LIST_TOP (EDIT_Y + EDIT_PAD + 30.0f)
/* Row pitch comes from the type, not from a number that once looked right.
 * At 15 px for a 13 px face the glyphs of one row printed through the next,
 * which is most of what made the list unreadable at any size above 480p.
 *
 * It is now sized from the *sprite* rather than the type, because the sprite is
 * the bigger of the two and is what the row is read by. Four rows fewer on a
 * page of thirty is a cheap price for a list you can sort at a glance instead
 * of one you have to read word by word. */
#define EDIT_SPRITE 19.0f
#define EDIT_ROW_H (EDIT_SPRITE + 7.0f)
#define EDIT_ICON_W (EDIT_SPRITE + 11.0f) /* the gutter, constant so labels line up */
#define EDIT_PAD 14.0f

static float edit_width(const fly_ui *ui) {
    float w = ui->view_w * 0.68f;
    return w > 430.0f ? 430.0f : w;
}

static float edit_x(const fly_ui *ui) {
    return ((float)ui->view_w - edit_width(ui)) * 0.5f;
}

/* ---------------- events ---------------- */

static void edit_activate(fly_ui *ui, fly_game *g) {
    fly__edit_row rows[FLY_EDIT_MAX];
    int n = edit_build(ui, g, rows, FLY_EDIT_MAX);
    if (ui->sel < 0 || ui->sel >= n) return;
    if (!rows[ui->sel].enabled || !rows[ui->sel].is_cmd || rows[ui->sel].header) return;
    fly_game_apply(g, &rows[ui->sel].cmd);
}

void fly_ui_event(fly_ui *ui, fly_game *g, const fly_event *ev) {
    if (ev->type == FLY_EV_KEY_DOWN || ev->type == FLY_EV_KEY_UP) {
        if (ev->key > FLY_KEY_NONE && ev->key < FLY_KEY_COUNT)
            ui->held[ev->key] = ev->type == FLY_EV_KEY_DOWN;
    }

    if (ev->type == FLY_EV_AXIS) {
        if ((int)ev->axis >= 0 && (int)ev->axis < FLY_AXIS_COUNT) {
            ui->axis[ev->axis] = ev->axis == FLY_AXIS_THROTTLE
                                     ? fly_clampf(ev->value, 0.0f, 1.0f)
                                     : fly_clampf(ev->value, -1.0f, 1.0f);
            if (ev->axis == FLY_AXIS_THROTTLE) ui->axis_throttle_live = 1;
        }
        return;
    }

    if (ev->type == FLY_EV_POINTER_MOVE) {
        if (ui->look_active) {
            fly_cmd look;
            memset(&look, 0, sizeof look);
            look.kind = FLY_CMD_LOOK;
            look.data.look.yaw = -(ev->x - ui->look_last_x) * 0.004f;
            look.data.look.pitch = -(ev->y - ui->look_last_y) * 0.004f;
            fly_game_apply(g, &look);
            ui->look_last_x = ev->x;
            ui->look_last_y = ev->y;
        }
        if (ui->stick_active) {
            float rad = FLY_STICK_RADIUS_FRAC * (float)ui->view_h;
            ui->stick_px = fly_clampf((ev->y - ui->stick_oy) / rad, -1.0f, 1.0f); /* down = nose up */
            ui->stick_py = fly_clampf((ev->x - ui->stick_ox) / rad, -1.0f, 1.0f);
        }
        if (ui->slider_active) {
            ui->throttle = slider_value(ui, ev->y);
            ui->axis_throttle_live = 0; /* pointer takes throttle ownership */
        }
        return;
    }

    if (ev->type == FLY_EV_POINTER_UP) {
        ui->stick_active = 0;
        ui->stick_px = ui->stick_py = 0.0f;
        ui->slider_active = 0;
        ui->fire_touch = 0;
        ui->launch_touch = 0;
        ui->decoy_touch = 0;
        ui->look_active = 0;
        return;
    }

    if (ev->type != FLY_EV_KEY_DOWN && ev->type != FLY_EV_POINTER_DOWN) return;

    if (ev->type == FLY_EV_POINTER_DOWN) {
        if (ui->view == FLY_VIEW_SPLASH) {
            /* the Play button launches into the world */
            float dx = ev->x - (float)ui->view_w * 0.5f;
            float dy = ev->y - FLY_SPLASH_PLAY_Y(ui->view_h);
            if (dx * dx + dy * dy <= (FLY_SPLASH_PLAY_R + 8) * (FLY_SPLASH_PLAY_R + 8))
                ui->view = FLY_VIEW_PLAY;
            return;
        }
        /* top-right circular nav icons: self, help (tap active icon = back) */
        {
            float dsx = ev->x - FLY_NAV_SELF_X(ui->view_w), dsy = ev->y - FLY_NAV_Y;
            float dhx = ev->x - FLY_NAV_HELP_X(ui->view_w), dhy = ev->y - FLY_NAV_Y;
            if (dsx * dsx + dsy * dsy <= FLY_NAV_HIT_R * FLY_NAV_HIT_R) {
                ui->view = ui->view == FLY_VIEW_SELF ? FLY_VIEW_PLAY : FLY_VIEW_SELF;
                return;
            }
            if (dhx * dhx + dhy * dhy <= FLY_NAV_HIT_R * FLY_NAV_HIT_R) {
                ui->view = ui->view == FLY_VIEW_HELP ? FLY_VIEW_PLAY : FLY_VIEW_HELP;
                return;
            }
        }
        if (ui->view == FLY_VIEW_PLAY && ui->overlay == FLY_OVERLAY_EDIT) {
            int row = ui->edit_first + (int)((ev->y - EDIT_LIST_TOP) / EDIT_ROW_H);
            fly__edit_row rows[FLY_EDIT_MAX];
            int n = edit_build(ui, g, rows, FLY_EDIT_MAX);
            if (row >= 0 && row < n && !rows[row].header) {
                ui->sel = row;
                edit_activate(ui, g);
            }
            return;
        }
        if (ui->view == FLY_VIEW_PLAY && ui->overlay == FLY_OVERLAY_NONE &&
            g->mode == FLY_MODE_FLIGHT && g->docked < 0) {
            /* flight controls */
            if (zone_slider(ui, ev->x, ev->y)) {
                ui->slider_active = 1;
                ui->throttle = slider_value(ui, ev->y);
                ui->axis_throttle_live = 0; /* pointer takes throttle ownership */
                return;
            }
            if (zone_act(ui, g, 0, ev->x, ev->y)) { ui->fire_touch = 1; return; }
            if (zone_act(ui, g, 1, ev->x, ev->y)) { ui->launch_touch = 1; return; }
            if (zone_act(ui, g, 2, ev->x, ev->y)) { ui->decoy_touch = 1; return; }
            if (zone_stick(ui, ev->x, ev->y)) {
                ui->stick_active = 1;
                ui->stick_ox = ev->x;
                ui->stick_oy = ev->y;
                ui->stick_px = ui->stick_py = 0.0f;
                return;
            }
        }
        if (ui->view == FLY_VIEW_PLAY && ui->overlay == FLY_OVERLAY_NONE && g->mode != FLY_MODE_FLIGHT) {
            ui->look_active = 1;
            ui->look_last_x = ev->x;
            ui->look_last_y = ev->y;
        }
        return;
    }

    if (ui->view == FLY_VIEW_SPLASH &&
        (ev->key == FLY_KEY_SELECT || ev->key == FLY_KEY_VIEW_PLAY)) {
        ui->view = FLY_VIEW_PLAY;
        return;
    }

    switch (ev->key) {
    case FLY_KEY_VIEW_PLAY: ui->view = FLY_VIEW_PLAY; break;
    case FLY_KEY_VIEW_SELF: ui->view = FLY_VIEW_SELF; break;
    case FLY_KEY_VIEW_HELP: ui->view = FLY_VIEW_HELP; break;
    case FLY_KEY_EDIT:
        if (ui->view == FLY_VIEW_PLAY) {
            if (ui->overlay == FLY_OVERLAY_EDIT) {
                ui->overlay = FLY_OVERLAY_NONE;
            } else {
                ui->overlay = FLY_OVERLAY_EDIT;
                int first = edit_seek(ui, g, 0, 1);
                ui->sel = first >= 0 ? first : 0;
                ui->edit_first = 0;
            }
        }
        break;
    case FLY_KEY_MAP:
        if (ui->view == FLY_VIEW_PLAY)
            ui->overlay = ui->overlay == FLY_OVERLAY_MAP ? FLY_OVERLAY_NONE : FLY_OVERLAY_MAP;
        break;
    case FLY_KEY_BACK:
        if (ui->overlay != FLY_OVERLAY_NONE) ui->overlay = FLY_OVERLAY_NONE;
        else if (ui->view == FLY_VIEW_PLAY) ui->view = FLY_VIEW_SPLASH;
        else ui->view = FLY_VIEW_PLAY;
        break;
    case FLY_KEY_UP:
        if (ui->overlay == FLY_OVERLAY_EDIT) {
            int prev = edit_seek(ui, g, ui->sel - 1, -1);
            if (prev >= 0) ui->sel = prev;
        } else if (ui->view == FLY_VIEW_HELP && ui->help_page > 0) {
            --ui->help_page;
        }
        break;
    case FLY_KEY_DOWN:
        if (ui->overlay == FLY_OVERLAY_EDIT) {
            int next = edit_seek(ui, g, ui->sel + 1, 1);
            if (next >= 0) ui->sel = next;
        } else if (ui->view == FLY_VIEW_HELP && ui->help_page < 2) {
            ++ui->help_page;
        }
        break;
    case FLY_KEY_SELECT:
        if (ui->overlay == FLY_OVERLAY_EDIT) edit_activate(ui, g);
        break;
    case FLY_KEY_AUTOPILOT: {
        fly_cmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.kind = FLY_CMD_AUTOPILOT;
        cmd.a = g->autopilot ? -1 :
                fly_world_nearest(&g->world, fly_wpos_of(g->player.craft.pos), 1);
        fly_game_apply(g, &cmd);
        break;
    }
    case FLY_KEY_INTERACT: {
        fly_cmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.kind = FLY_CMD_INTERACT;
        fly_game_apply(g, &cmd);
        break;
    }
    default: break;
    }
}

void fly_ui_pump_controls(fly_ui *ui, fly_game *g, float dt) {
    if (g->mode == FLY_MODE_WALK) {
        fly_cmd cmd;
        memset(&cmd, 0, sizeof cmd);
        cmd.kind = FLY_CMD_WALK;
        cmd.data.walk.forward = (ui->held[FLY_KEY_THROTTLE_UP] ? 1.0f : 0.0f) -
                                (ui->held[FLY_KEY_THROTTLE_DOWN] ? 1.0f : 0.0f) - ui->axis[FLY_AXIS_PITCH];
        cmd.data.walk.right = (ui->held[FLY_KEY_ROLL_RIGHT] ? 1.0f : 0.0f) -
                              (ui->held[FLY_KEY_ROLL_LEFT] ? 1.0f : 0.0f) + ui->axis[FLY_AXIS_ROLL];
        cmd.data.walk.jump = ui->held[FLY_KEY_JUMP];
        fly_game_apply(g, &cmd);
        if (fabsf(ui->axis[FLY_AXIS_LOOK_X]) > 0.02f || fabsf(ui->axis[FLY_AXIS_LOOK_Y]) > 0.02f) {
            memset(&cmd, 0, sizeof cmd);
            cmd.kind = FLY_CMD_LOOK;
            cmd.data.look.yaw = -ui->axis[FLY_AXIS_LOOK_X] * dt * 2.2f;
            cmd.data.look.pitch = -ui->axis[FLY_AXIS_LOOK_Y] * dt * 2.2f;
            fly_game_apply(g, &cmd);
        }
        return;
    }
    if (g->mode == FLY_MODE_RAIL) {
        if (fabsf(ui->axis[FLY_AXIS_LOOK_X]) > 0.02f || fabsf(ui->axis[FLY_AXIS_LOOK_Y]) > 0.02f) {
            fly_cmd cmd;
            memset(&cmd, 0, sizeof cmd);
            cmd.kind = FLY_CMD_LOOK;
            cmd.data.look.yaw = -ui->axis[FLY_AXIS_LOOK_X] * dt * 2.2f;
            cmd.data.look.pitch = -ui->axis[FLY_AXIS_LOOK_Y] * dt * 2.2f;
            fly_game_apply(g, &cmd);
        }
        return;
    }
    if (g->autopilot) return; /* AP owns the stick */
    fly_cmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.kind = FLY_CMD_CONTROLS;
    fly_controls *c = &cmd.controls;
    float kp = (ui->held[FLY_KEY_PITCH_UP] ? 1.0f : 0.0f) - (ui->held[FLY_KEY_PITCH_DOWN] ? 1.0f : 0.0f);
    float kr = (ui->held[FLY_KEY_ROLL_RIGHT] ? 1.0f : 0.0f) - (ui->held[FLY_KEY_ROLL_LEFT] ? 1.0f : 0.0f);
    float ky = (ui->held[FLY_KEY_YAW_RIGHT] ? 1.0f : 0.0f) - (ui->held[FLY_KEY_YAW_LEFT] ? 1.0f : 0.0f);
    c->pitch = fly_clampf(kp + ui->axis[FLY_AXIS_PITCH] + ui->stick_px, -1.0f, 1.0f);
    c->roll = fly_clampf(kr + ui->axis[FLY_AXIS_ROLL] + ui->stick_py, -1.0f, 1.0f);
    c->yaw = fly_clampf(ky + ui->axis[FLY_AXIS_YAW], -1.0f, 1.0f);
    if (ui->held[FLY_KEY_THROTTLE_UP]) { ui->throttle += 0.6f * dt; ui->axis_throttle_live = 0; }
    if (ui->held[FLY_KEY_THROTTLE_DOWN]) { ui->throttle -= 0.6f * dt; ui->axis_throttle_live = 0; }
    if (ui->axis_throttle_live) ui->throttle = ui->axis[FLY_AXIS_THROTTLE];
    ui->throttle = fly_clampf(ui->throttle, 0.0f, 1.0f);
    c->throttle = ui->throttle;
    /* Trim. Slow on purpose — a fifth of the range a second — because trim is
     * something you creep up on while watching the nose, and a trim wheel you
     * can slam from stop to stop is just a second elevator. */
    if (ui->held[FLY_KEY_TRIM_UP]) ui->trim += 0.2f * dt;
    if (ui->held[FLY_KEY_TRIM_DOWN]) ui->trim -= 0.2f * dt;
    ui->trim = fly_clampf(ui->trim, -1.0f, 1.0f);
    c->trim = ui->trim;
    c->brakes = ui->held[FLY_KEY_BRAKE];
    c->fire = ui->held[FLY_KEY_FIRE] || ui->fire_touch;
    /* Held, not toggled. A rocket you have to keep your hand on is a rocket
     * you cannot leave burning by accident, and the propellant is the one
     * consumable in the game that a pad cannot sell you back. */
    c->rocket = ui->held[FLY_KEY_ROCKET] || ui->rocket_touch;
    /* Held, and rate-limited by the rack rather than by the key. An edge would
     * be the obvious thing here and it is the wrong one: an AI pilot's controls
     * are recomputed from scratch every tick and never produce one, so a launch
     * gated on a key transition would be a weapon only the player could fire.
     * The reload on the launcher does the limiting for both of them. */
    c->launch = ui->held[FLY_KEY_LAUNCH] || ui->launch_touch;
    c->decoy = ui->held[FLY_KEY_DECOY] || ui->decoy_touch;
    fly_game_apply(g, &cmd);
}

/* ---------------- shared page furniture ---------------- */

/* The page surface itself is fly_ui_page, over in fly_hud with the rest of the
 * chrome: the ruin banner is a page too, and two files each mixing their own
 * version of the material is how the profile ended up a different colour from
 * the wreck screen it is read after. */

/* small circular nav buttons, top-right; non-invasive over the world view.
 * Play never appears here — launching lives on the splash. The active view's
 * icon is highlighted and tapping it returns to the world. */
static void draw_nav_icon(fly_img *im, float cx, float cy, int active) {
    uint32_t ring = active ? FLY_UI_ACCENT : FLY_UI_GLASS_EDGE;
    fly_img_soft_shadow(im, cx - FLY_NAV_ICON_R, cy - FLY_NAV_ICON_R + 1.0f,
                        2.0f * FLY_NAV_ICON_R, 2.0f * FLY_NAV_ICON_R,
                        FLY_NAV_ICON_R, 6.0f, FLY_RGBA(0, 0, 0, 110));
    /* One tint over the blur, the same glass as every other pane, one stop
     * lighter when the view is the one you are on. The near-opaque dark disc
     * that used to sit here was painted straight over the frost, which meant
     * the button paid for a blur it then hid. */
    fly_ui_glass_disc(im, cx, cy, FLY_NAV_ICON_R,
                      active ? FLY_RGBA(60, 132, 168, 128) : FLY_UI_GLASS);
    fly_img_aa_circle(im, cx, cy, FLY_NAV_ICON_R - 0.5f, active ? 1.6f : 1.1f, ring);
}

static void draw_nav(fly_img *im, fly_view active) {
    float sx = FLY_NAV_SELF_X(im->w), hx = FLY_NAV_HELP_X(im->w), y = FLY_NAV_Y;
    /* self: the operator, the same sprite that heads the page it opens */
    draw_nav_icon(im, sx, y, active == FLY_VIEW_SELF);
    fly_ui_sprite(im, FLY_ICON_PILOT, sx, y - 0.5f, FLY_NAV_ICON_R * 0.98f);
    /* help: a question mark, centred on its own marks.
     *
     * It used to be centred on the type size — `y - px * 0.5f` — and a '?' has
     * no descender, so the box the face reserves for one hung below the glyph
     * and pushed the glyph up. In a 20 px circle that was plainly a question
     * mark stuck to the top of its button, which is the sort of thing you
     * cannot stop seeing once you have seen it. fly_text_mid measures the ink. */
    draw_nav_icon(im, hx, y, active == FLY_VIEW_HELP);
    fly_text_mid(im, FLY_FONT_UI, hx, y, FLY_FS_HEAD,
                 active == FLY_VIEW_HELP ? FLY_UI_FG : FLY_UI_DIM, FLY_ALIGN_CENTER, "?");
}

/* Small-caps section header: a sprite, the name, and a hairline rule under
 * both. Returns content y.
 *
 * The sprite is the same one the thing wears everywhere else — a parcel over
 * CARGO HOLD, a planform over AIRFRAME, a chip over MODULES — so the page is
 * navigable by shape at a glance and so the profile and the actions menu are
 * plainly describing the same game. */
#define SECTION_SPRITE 15.0f
static float section(fly_img *im, float x, float y, float w, fly_icon ic,
                     const char *title) {
    float ty = y + FS_S * 0.5f;
    fly_ui_sprite(im, ic, x + SECTION_SPRITE * 0.5f, ty, SECTION_SPRITE);
    fly_text_mid(im, FLY_FONT_UI, x + SECTION_SPRITE + 8.0f, ty, FS_S, FLY_UI_ACCENT,
                 FLY_ALIGN_LEFT, title);
    fly_img_aa_line(im, x, y + 17, x + w, y + 17, 1.0f, FLY_RGBA(128, 172, 200, 110));
    return y + 25;
}

/* key/value row: dim label left, value right-aligned */
static float kv(fly_img *im, float x, float w, float y, const char *label,
                const char *value, uint32_t vcol) {
    fly_text_sh(im, FLY_FONT_UI, x, y, FS_B, FLY_UI_DIM, FLY_ALIGN_LEFT, label);
    fly_text_sh(im, FLY_FONT_MONO, x + w, y, FS_B, vcol, FLY_ALIGN_RIGHT, value);
    return y + 18;
}

/* ---------------- edit overlay ---------------- */

static void render_edit_overlay(fly_ui *ui, const fly_game *g, fly_img *im) {
    float w = edit_width(ui);
    float ex = edit_x(ui);
    float h = (float)im->h - 2.0f * EDIT_Y; /* vertically centered window */
    fly_ui_card(im, ex, EDIT_Y, w, h, FLY_UI_CARD);
    fly_text_sh(im, FLY_FONT_UI, ex + EDIT_PAD, EDIT_Y + EDIT_PAD, FS_S, FLY_UI_ACCENT,
             FLY_ALIGN_LEFT, "ACTIONS");
    char buf[64];
    snprintf(buf, sizeof buf, "%ld tk   cargo %.0f/%.0f kg", g->player.tokens,
             fly_game_cargo_mass(g), g->player.airframe.cargo_cap);
    fly_text_sh(im, FLY_FONT_MONO, ex + w - EDIT_PAD, EDIT_Y + EDIT_PAD, FS_S, FLY_UI_DIM,
             FLY_ALIGN_RIGHT, buf);
    fly_img_aa_line(im, ex + EDIT_PAD, EDIT_Y + EDIT_PAD + 19.0f, ex + w - EDIT_PAD,
                    EDIT_Y + EDIT_PAD + 19.0f, 1.0f, FLY_RGBA(96, 132, 150, 80));

    fly__edit_row rows[FLY_EDIT_MAX];
    int n = edit_build(ui, g, rows, FLY_EDIT_MAX);
    int visible = (int)((h - (EDIT_LIST_TOP - EDIT_Y) - EDIT_PAD) / EDIT_ROW_H);
    if (ui->sel < ui->edit_first) ui->edit_first = ui->sel;
    if (ui->sel >= ui->edit_first + visible) ui->edit_first = ui->sel - visible + 1;
    if (ui->edit_first < 0) ui->edit_first = 0;

    int i;
    float y = EDIT_LIST_TOP;
    for (i = ui->edit_first; i < n && i < ui->edit_first + visible; ++i, y += EDIT_ROW_H) {
        fly__edit_row *r = &rows[i];
        float lx = ex + EDIT_PAD + EDIT_ICON_W;
        float rx = ex + w - EDIT_PAD;
        float rw = r->right[0] ? fly_text_width(FLY_FONT_MONO, FS_S, r->right) + 14.0f : 0.0f;
        char label[64];
        uint32_t col, rc;
        if (r->header) {
            float ty = y + EDIT_ROW_H * 0.5f;
            fly_ui_sprite(im, r->icon, ex + EDIT_PAD + EDIT_SPRITE * 0.5f, ty,
                          EDIT_SPRITE * 0.86f);
            fly_text_mid(im, FLY_FONT_UI, lx, ty, FLY_FS_KEY, FLY_UI_ACCENT,
                         FLY_ALIGN_LEFT, r->label);
            fly_img_aa_line(im, ex + EDIT_PAD, y + EDIT_ROW_H - 2.0f, ex + w - EDIT_PAD,
                            y + EDIT_ROW_H - 2.0f, 1.0f, FLY_RGBA(122, 168, 196, 130));
            continue;
        }
        /* Banding, then selection. A list this long is read by running an eye
         * down it, and an eye needs something to hold onto between rows. */
        if ((i & 1) == 0)
            fly_img_round_rect(im, ex + EDIT_PAD - 6.0f, y - 1.0f, w - 2.0f * EDIT_PAD + 12.0f,
                               EDIT_ROW_H - 1.0f, 4.0f, FLY_RGBA(160, 200, 226, 20));
        if (i == ui->sel) {
            fly_img_round_rect(im, ex + EDIT_PAD - 6.0f, y - 1.0f, w - 2.0f * EDIT_PAD + 12.0f,
                               EDIT_ROW_H - 1.0f, 4.0f, FLY_RGBA(64, 150, 186, 150));
            fly_img_round_rect_line(im, ex + EDIT_PAD - 5.5f, y - 0.5f,
                                    w - 2.0f * EDIT_PAD + 11.0f, EDIT_ROW_H - 2.0f, 4.0f,
                                    1.0f, FLY_RGBA(158, 226, 250, 150));
            fly_img_round_rect(im, ex + EDIT_PAD - 6.0f, y - 1.0f, 3.0f, EDIT_ROW_H - 1.0f,
                               1.5f, FLY_UI_ACCENT);
        }
        col = !r->enabled ? FLY_RGBA(154, 172, 186, 190)
              : i == ui->sel ? FLY_UI_FG : FLY_RGBA(226, 240, 248, 250);
        /* The row is a band and its contents sit on the band's centre line —
         * sprite, label and value alike. Drawing the label at the band's top
         * edge and the value one pixel below it left every row looking like it
         * had settled a little.
         *
         * The sprite is the size of the row rather than a tenth of it. A menu
         * is read by shape first, and a 10 px monochrome glyph in a 21 px row
         * was not a shape — twenty rows of identical grey smudge down the left
         * margin. A row you cannot act on gets the same artwork in one muted
         * colour, which says "this one, but not now" without a second glyph. */
        {
            float ty = y - 1.0f + (EDIT_ROW_H - 1.0f) * 0.5f;
            float sx = ex + EDIT_PAD + EDIT_SPRITE * 0.5f;
            if (r->enabled) fly_ui_sprite(im, r->icon, sx, ty, EDIT_SPRITE);
            else fly_ui_icon(im, r->icon, sx, ty, EDIT_SPRITE * 0.9f,
                             FLY_RGBA(146, 166, 182, 170));
            /* The label is clipped to whatever the value column leaves. It used
             * to be drawn at full length over the top of it, so a long module
             * name and its price occupied the same pixels. */
            fit_row(label, sizeof label, FLY_FS_VAL, rx - rw - lx, r->label);
            fly_text_mid(im, FLY_FONT_UI, lx, ty, FLY_FS_VAL, col, FLY_ALIGN_LEFT, label);
            if (r->right[0]) {
                rc = !r->enabled ? FLY_RGBA(148, 166, 180, 180)
                     : r->right[0] == '!' ? FLY_UI_WARN : FLY_UI_DIM;
                fly_text_mid(im, FLY_FONT_MONO, rx, ty, FLY_FS_KEY, rc,
                             FLY_ALIGN_RIGHT, r->right);
            }
        }
    }
    /* scroll hints */
    if (ui->edit_first > 0)
        fly_text_sh(im, FLY_FONT_UI, ex + w * 0.5f, EDIT_LIST_TOP - 14.0f, FS_S, FLY_UI_DIM,
                 FLY_ALIGN_CENTER, "^");
    if (ui->edit_first + visible < n)
        fly_text_sh(im, FLY_FONT_UI, ex + w * 0.5f, EDIT_Y + h - EDIT_PAD - 8.0f, FS_S, FLY_UI_DIM,
                 FLY_ALIGN_CENTER, "v");
}

/* ---------------- map overlay ---------------- */

/* The largest chart the layout will draw, in pixels a side. */
#define FLY_MAP_MAX 1024
/* How far a power's colours reach from a field it holds, in metres. Roughly
 * the spacing between sites, so holdings read as territory with seams between
 * them rather than as dots or as a partition of the whole chart. */
#define FLY_MAP_REACH 9000.0f

/* Where the chart itself lands in a frame of this size: its left edge, its top
 * edge and its side. A function because three things want it — the drawing, the
 * label placement, and the probe a test asks for the placement through.
 *
 * The chart is one heightfield sample per pixel and the terrain sampler is the
 * most expensive function in the engine, so the side is bounded rather than
 * left to whatever frame size arrives. Clamped and re-centred rather than
 * truncated: a chart that quietly stopped drawing its bottom rows would read as
 * a rendering fault instead of as a limit. */
static void chart_rect(int w, int h, float *x0, float *y0, float *size) {
    float margin = 16.0f;
    float card_y = margin, card_h = (float)h - 2.0f * margin;
    float sz = card_h - 2.0f * EDIT_PAD - 42.0f;
    if (sz > (float)FLY_MAP_MAX) sz = (float)FLY_MAP_MAX;
    *size = sz;
    *x0 = ((float)w - sz) * 0.5f;
    *y0 = card_y + EDIT_PAD + 21.0f;
}

/* --- where the names go ---------------------------------------------------
 *
 * Every label used to be drawn seven pixels right of its dot and six above it,
 * which is fine for a lone outpost and wrong for the thing a chart is mostly
 * made of: sites cluster, because settlements cluster. On seed 4242 the five
 * charted fields around the start sit inside a fifth of the chart and
 * `Taldine`, `Ororell` and `Hexdan` printed through each other.
 *
 * So the offset is chosen rather than fixed. Eight positions round the dot in
 * the order a cartographer tries them — right of it first, because that is
 * where a name is expected and where the eye looks — and the first that
 * collides with nothing already placed, with nobody else's dot, or with the
 * chart's own edge is the one used. Greedy and in index order, so it is
 * deterministic for a seed the way everything else here is.
 *
 * If nothing is clear, the name is drawn at the first position that at least
 * stays on the chart. A chart with a nameless dot on it is worse than a chart
 * with two names close together — the dot is the thing you are trying to
 * identify — and a name in the margin prints over the card's own frame, which
 * is worse than both. */
typedef struct {
    float x, y, w, h;   /* the name's own box */
    float dx, dy;       /* the dot it belongs to */
    int align, lead;    /* and whether it is far enough out to need a line */
} fly__chart_label;

static int chart_labels(const fly_game *g, float x0, float y0, float size,
                        fly__chart_label *out, int max) {
    /* dx, dy and how the text sits against them, nearest first: the two
     * flanks at the dot's own line, then above and below it, then the same
     * eight again a row further out. Anything past the first four gets a
     * leader line, because a name that is not beside its dot has to be joined
     * to it or it belongs to whichever dot it landed nearest. */
    static const struct { float dx, dy; int align; } cand[] = {
        {  7.0f,  -6.0f, FLY_ALIGN_LEFT }, {  -7.0f,  -6.0f, FLY_ALIGN_RIGHT },
        {  7.0f,   3.0f, FLY_ALIGN_LEFT }, {  -7.0f,   3.0f, FLY_ALIGN_RIGHT },
        {  7.0f, -16.0f, FLY_ALIGN_LEFT }, {  -7.0f, -16.0f, FLY_ALIGN_RIGHT },
        {  7.0f,  13.0f, FLY_ALIGN_LEFT }, {  -7.0f,  13.0f, FLY_ALIGN_RIGHT },
        { 18.0f,  -6.0f, FLY_ALIGN_LEFT }, { -18.0f,  -6.0f, FLY_ALIGN_RIGHT },
        { 18.0f, -26.0f, FLY_ALIGN_LEFT }, { -18.0f, -26.0f, FLY_ALIGN_RIGHT },
        { 18.0f,  23.0f, FLY_ALIGN_LEFT }, { -18.0f,  23.0f, FLY_ALIGN_RIGHT },
        {  2.0f, -27.0f, FLY_ALIGN_CENTER }, {  2.0f,  24.0f, FLY_ALIGN_CENTER },
        { 30.0f, -16.0f, FLY_ALIGN_LEFT }, { -30.0f, -16.0f, FLY_ALIGN_RIGHT },
        { 30.0f,  13.0f, FLY_ALIGN_LEFT }, { -30.0f,  13.0f, FLY_ALIGN_RIGHT },
    };
    const int ncand = (int)(sizeof cand / sizeof cand[0]);
    const float dot = 5.5f;      /* keep clear of every dot, not just your own */
    int i, k, n = 0;
    for (i = 0; i < g->world.nloc && n < max; ++i) {
        const fly_location *L = &g->world.loc[i];
        float px, py, tw, th = FS_S + 3.0f;
        int best = -1, fallback = -1;
        if (!L->discovered) continue;
        px = x0 + ((L->pos.e / (2.0f * FLY_WORLD_HALF)) + 0.5f) * size;
        py = y0 + (0.5f - L->pos.n / (2.0f * FLY_WORLD_HALF)) * size;
        tw = fly_text_width(FLY_FONT_UI, FS_S, L->name);
        for (k = 0; k < ncand; ++k) {
            float lx = cand[k].align == FLY_ALIGN_LEFT ? px + cand[k].dx
                     : cand[k].align == FLY_ALIGN_RIGHT ? px + cand[k].dx - tw
                     : px + cand[k].dx - tw * 0.5f;
            float ly = py + cand[k].dy;
            int clash = 0, j;
            if (lx < x0 || lx + tw > x0 + size || ly < y0 || ly + th > y0 + size) continue;
            /* the first one that at least stays on the chart, in case nothing
               is clear: a name in the margin prints over the card's own frame */
            if (fallback < 0) fallback = k;
            for (j = 0; j < n && !clash; ++j)
                clash = !(lx + tw < out[j].x || out[j].x + out[j].w < lx ||
                          ly + th < out[j].y || out[j].y + out[j].h < ly);
            for (j = 0; j < g->world.nloc && !clash; ++j) {
                float qx, qy;
                if (!g->world.loc[j].discovered) continue;
                qx = x0 + ((g->world.loc[j].pos.e / (2.0f * FLY_WORLD_HALF)) + 0.5f) * size;
                qy = y0 + (0.5f - g->world.loc[j].pos.n / (2.0f * FLY_WORLD_HALF)) * size;
                clash = !(lx + tw < qx - dot || qx + dot < lx ||
                          ly + th < qy - dot || qy + dot < ly);
            }
            if (!clash) { best = k; break; }
        }
        if (best < 0) best = fallback >= 0 ? fallback : 0;
        out[n].x = cand[best].align == FLY_ALIGN_LEFT ? px + cand[best].dx
                 : cand[best].align == FLY_ALIGN_RIGHT ? px + cand[best].dx - tw
                 : px + cand[best].dx - tw * 0.5f;
        out[n].y = py + cand[best].dy;
        out[n].w = tw;
        out[n].h = th;
        out[n].align = cand[best].align;
        out[n].dx = px;
        out[n].dy = py;
        out[n].lead = best >= 4;
        ++n;
    }
    return n;
}

int fly_ui_chart_labels(int w, int h, const fly_game *g, fly_rectf *out, int max) {
    fly__chart_label lab[FLY_MAX_LOC];
    float x0, y0, size;
    int i, n;
    if (!g || !out || max <= 0) return 0;
    chart_rect(w, h, &x0, &y0, &size);
    n = chart_labels(g, x0, y0, size, lab, FLY_MAX_LOC);
    if (n > max) n = max;
    for (i = 0; i < n; ++i) {
        out[i].x = lab[i].x; out[i].y = lab[i].y;
        out[i].w = lab[i].w; out[i].h = lab[i].h;
    }
    return n;
}

static void render_map_overlay(const fly_game *g, fly_img *im) {
    /* Title band, chart, legend band — one pad outside all three, so the card
     * has the same air at the top as at the bottom. It used to be ten pixels
     * at the sides and thirty-two under the legend, with the title riding on
     * the card's own edge. */
    float margin = 16.0f;
    float card_y = margin, card_h = (float)im->h - 2.0f * margin;
    float size, x0, my0;
    float ty = card_y + EDIT_PAD;
    int i, j;
    chart_rect(im->w, im->h, &x0, &my0, &size);
    fly_ui_card(im, x0 - EDIT_PAD, card_y, size + 2.0f * EDIT_PAD, card_h, FLY_UI_CARD);
    {
        float tw = fly_text_width(FLY_FONT_UI, FS_S, "WORLD CHART");
        float tc = ty + FS_S * 0.5f;
        fly_ui_sprite(im, FLY_ICON_ROUTE, x0 + size * 0.5f - tw * 0.5f - 11.0f, tc, 14.0f);
        fly_text_mid(im, FLY_FONT_UI, x0 + size * 0.5f + 9.0f, tc, FS_S, FLY_UI_ACCENT,
                     FLY_ALIGN_CENTER, "WORLD CHART");
    }
    /* --- the country ---
     *
     * A chart is read for its shape: where the coast runs, which way the
     * ground rises, where the passes are. It used to be read for none of
     * those, because the tone curve was `hgt / 2600 * 200 + 22` and no seed
     * puts 2600 m inside the sixty-kilometre chart. Seed 4242 tops out at
     * 816 m and seed 777 at 1349, so `v` never left the bottom third of its
     * range — and the colour it drives is (v/3, v/2, v/3), which puts the
     * whole of the land between luminance 9 and 33. A black rectangle with a
     * slightly-less-black rectangle in it, under a wash of faction colour that
     * was the only thing on the chart anybody could see. Measured off the
     * frame: neighbouring land pixels differed by 3.5 levels, and 3.5 of those
     * were the quantisation of a channel divided by three.
     *
     * Two changes, and the first is the one that matters. Relief is drawn by
     * *shading* rather than by absolute tone: the slope between neighbouring
     * samples, lit from the north-west, which is how every paper chart in
     * existence says "this is a ridge and that is a valley" and which works
     * whatever the seed's relief happens to be. It costs no extra terrain
     * samples — the row above is kept and the sample to the left is already
     * in hand — which matters, because fly_world_ground is the most expensive
     * function in the engine and this loop runs it a million times.
     *
     * The second is a hypsometric tint over the top, green through tan, and
     * bathymetry under the water instead of one flat navy: the shelf reads
     * apart from the deep, so a coastline is a coast rather than a cut. The
     * band stays dark enough that the territory wash and the site dots are
     * still the loudest things on the chart. */
    {
        float prev[FLY_MAP_MAX];
        int n = (int)size;
        float cell = 2.0f * FLY_WORLD_HALF / (size > 1.0f ? size : 1.0f);
        for (j = 0; j < n; ++j) {
            float left = 0.0f;
            for (i = 0; i < n; ++i) {
                float wx = ((float)i / size - 0.5f) * 2.0f * FLY_WORLD_HALF;
                float wy = (0.5f - (float)j / size) * 2.0f * FLY_WORLD_HALF;
                float hgt = fly_world_ground(&g->world, wx, wy);
                float dzdx = i ? (hgt - left) / cell : 0.0f;
                /* rows run north to south, so a row's predecessor is to the
                 * north and dz/dy is negated to keep north up */
                float dzdy = j ? (prev[i] - hgt) / cell : 0.0f;
                float r, gg, b, shade;
                uint32_t c;
                left = hgt;
                prev[i] = hgt;
                /* Lambert against a light out of the north-west at 45 degrees,
                 * with the gradient exaggerated: real terrain at 60 m a pixel
                 * is nearly flat to a light, and a chart is a diagram. */
                {
                    float gx = dzdx * 9.0f, gy = dzdy * 9.0f;
                    float inv = 1.0f / sqrtf(gx * gx + gy * gy + 1.0f);
                    shade = fly_clampf((0.60f * -gx + 0.60f * gy + 0.53f) * inv * 1.75f,
                                       0.42f, 1.28f);
                }
                /* Water is water, wherever it stands. The sea reads by its
                 * depth; a river or a lake three hundred metres up is drawn in
                 * the same blue at the shallow end of the same ramp, which is
                 * what puts on the chart the one landmark a pilot can also
                 * follow home from inside the cockpit. */
                if (hgt < fly_world_water_at(&g->world, wx, wy, hgt)) {
                    /* Depth, not a silhouette. The bed is real down to three
                     * kilometres, so the shelf can be told from the basin and
                     * an island reads as an island. */
                    float d = fly_clampf(-hgt / 900.0f, 0.0f, 1.0f);
                    r = fly_lerpf(20.0f, 7.0f, d);
                    gg = fly_lerpf(40.0f, 15.0f, d);
                    b = fly_lerpf(62.0f, 30.0f, d);
                } else {
                    /* Shore green to upland tan over the first kilometre, which
                     * is where the ground this game is flown over lives; higher
                     * than that keeps going pale rather than clipping. */
                    float t = fly_clampf(hgt / 950.0f, 0.0f, 1.0f);
                    r = fly_lerpf(30.0f, 88.0f, t);
                    gg = fly_lerpf(50.0f, 80.0f, t);
                    b = fly_lerpf(30.0f, 62.0f, t);
                    r *= shade; gg *= shade; b *= shade;
                }
                /* The belt is weather, and weather has no edge. It used to be
                 * `fabsf(wy - belt_y) < belt_w`, which drew two hard
                 * horizontal lines the full width of the chart and read as a
                 * rendering fault rather than as a place. */
                {
                    float k = 0.40f * (1.0f - fly_smoothstepf(g->world.storm_belt_w * 0.35f,
                                                              g->world.storm_belt_w,
                                                              fabsf(wy - g->world.storm_belt_y)));
                    if (k > 0.004f) {
                        r = fly_lerpf(r, 86.0f, k);
                        gg = fly_lerpf(gg, 68.0f, k);
                        b = fly_lerpf(b, 52.0f, k);
                    }
                }
                c = FLY_RGB((int)fly_clampf(r, 0, 255), (int)fly_clampf(gg, 0, 255),
                            (int)fly_clampf(b, 0, 255));
                fly_img_set(im, (int)x0 + i, (int)my0 + j, c);
            }
        }
    }
#define MAPX(wx) (x0 + (((wx) / (2.0f * FLY_WORLD_HALF)) + 0.5f) * size)
#define MAPY(wy) (my0 + (0.5f - (wy) / (2.0f * FLY_WORLD_HALF)) * size)

    /* --- territory ---
     *
     * Each field a power holds washes the country around it in that power's
     * colours, fading out by FLY_MAP_REACH. Overlapping holdings blend, so a
     * cluster of one flag reads as a region and a lone outpost reads as a
     * lone outpost.
     *
     * A nearest-site Voronoi was tried first and is wrong twice over. It cuts
     * the whole sixty-kilometre chart into a handful of enormous wedges, which
     * looks like a rendering fault; and it *claims* something the simulation
     * never says, because control is a property of fields and nothing in the
     * model decides who owns the empty ground between them. A halo says the
     * true and smaller thing: somebody holds this place, and their reach
     * shades off with distance from it.
     *
     * Undiscovered fields wash nothing. A chart is what the operator knows,
     * and colouring the far half of the world by powers they have never met
     * would hand them the map for free. */
    for (i = 0; i < g->world.nloc; ++i) {
        const fly_location *L = &g->world.loc[i];
        fly_v3 fc;
        float cx, cy, rad;
        int px, py;
        if (!L->discovered || !fly_faction_is_power(L->owner)) continue;
        fc = fly_faction_mark((int)L->owner);
        cx = MAPX(L->pos.e); cy = MAPY(L->pos.n);
        rad = FLY_MAP_REACH / (2.0f * FLY_WORLD_HALF) * size;
        for (py = (int)(cy - rad); py <= (int)(cy + rad); ++py) {
            if (py < (int)my0 || py >= (int)(my0 + size)) continue;
            for (px = (int)(cx - rad); px <= (int)(cx + rad); ++px) {
                float dx = (float)px + 0.5f - cx, dy = (float)py + 0.5f - cy;
                float d = sqrtf(dx * dx + dy * dy), k;
                uint32_t c;
                int cr, cg, cb;
                if (px < (int)x0 || px >= (int)(x0 + size)) continue;
                if (d >= rad) continue;
                /* A dome from the field outward rather than a plateau with an
                 * edge: a flat core reads as a light source sitting on the
                 * chart instead of as country belonging to somebody. */
                k = 0.30f * (1.0f - fly_smoothstepf(0.0f, rad, d));
                if (k < 0.004f) continue;
                c = fly_img_get(im, px, py);
                cr = (int)(c & 0xFF); cg = (int)((c >> 8) & 0xFF); cb = (int)((c >> 16) & 0xFF);
                cr = (int)fly_clampf((float)cr * (1.0f - k) + fc.x * 255.0f * k, 0, 255);
                cg = (int)fly_clampf((float)cg * (1.0f - k) + fc.y * 255.0f * k, 0, 255);
                cb = (int)fly_clampf((float)cb * (1.0f - k) + fc.z * 255.0f * k, 0, 255);
                fly_img_set(im, px, py, FLY_RGB(cr, cg, cb));
            }
        }
    }
    /* --- the roads ---
     *
     * Drawn under the sites and over the country they cross, because that is
     * what they are: the thing that says which of these places are joined to
     * each other and which are only reachable by air. A road whose two ends
     * have not both been found is not drawn — a chart is what the operator
     * knows, and a network handed over whole would be the map for free. */
    for (i = 0; i < g->roads.count; ++i) {
        const fly_road *rd = &g->roads.road[i];
        int k;
        if (!g->world.loc[rd->from].discovered || !g->world.loc[rd->to].discovered) continue;
        for (k = 0; k + 1 < rd->point_count; ++k)
            fly_img_aa_line(im, MAPX(rd->points[k].pos.x), MAPY(rd->points[k].pos.y),
                            MAPX(rd->points[k + 1].pos.x), MAPY(rd->points[k + 1].pos.y),
                            1.0f, FLY_RGBA(232, 214, 178, 120));
    }
    /* --- and the lanes ---
     *
     * The same claim one medium over, and drawn the same way and under the
     * same rule: a lane joins two harbours and is only on the chart once the
     * operator has found both ends of it. Dashed rather than solid, because
     * that is the difference a chart draws between a thing built on the ground
     * and a course somebody keeps across water — and because at this scale a
     * second solid line of the same weight would read as another road. */
    for (i = 0; i < g->lanes.count; ++i) {
        const fly_lane *ln = &g->lanes.lane[i];
        int k;
        if (!g->world.loc[ln->from].discovered || !g->world.loc[ln->to].discovered) continue;
        for (k = 0; k + 1 < ln->point_count; ++k) {
            if ((k & 3) >= 2) continue;
            fly_img_aa_line(im, MAPX(ln->points[k].pos.x), MAPY(ln->points[k].pos.y),
                            MAPX(ln->points[k + 1].pos.x), MAPY(ln->points[k + 1].pos.y),
                            1.0f, FLY_RGBA(196, 224, 236, 110));
        }
    }
    for (i = 0; i < FLY_MAX_CONTRACTS; ++i)
        if (g->contracts[i].state == 2) {
            const fly_location *T = &g->world.loc[g->contracts[i].to];
            fly_img_aa_line(im, MAPX(g->player.craft.pos.x), MAPY(g->player.craft.pos.y),
                            MAPX(T->pos.e), MAPY(T->pos.n), 1.2f, FLY_RGBA(255, 220, 120, 140));
        }
    fly__chart_label lab[FLY_MAX_LOC];
    int nlab = chart_labels(g, x0, my0, size, lab, FLY_MAX_LOC), ilab = 0;
    for (i = 0; i < g->world.nloc; ++i) {
        const fly_location *L = &g->world.loc[i];
        if (!L->discovered) continue;
        float px = MAPX(L->pos.e), py = MAPY(L->pos.n);
        /* The dot is the flag now. Site *kind* keeps only the one distinction
         * a route actually turns on — a service field, ringed in white, is
         * somewhere you can buy and repair — because with a war on, "who holds
         * it" is what a player opens this chart to find out, and the dot is
         * the only mark on it big enough to carry a hue. */
        uint32_t c = fly_hud_faction_rgba((int)L->owner, 255);
        fly_img_aa_disc(im, px, py, 2.6f, c);
        if (L->kind == FLY_LOC_CITY || L->kind == FLY_LOC_SKYPORT)
            fly_img_aa_circle(im, px, py, 4.6f, 1.0f, FLY_RGBA(255, 255, 255, 150));
        /* Somebody is taking this one. The ring pulses in the challenger's
         * colours, so a glance at the chart says where the front line is
         * without reading a single word. */
        if (fly_faction_contested(&g->world, i)) {
            int ch = fly_faction_challenger(&g->world, i, NULL);
            float ph = 0.5f + 0.5f * sinf((float)g->time_s * 2.4f + (float)i);
            fly_img_aa_circle(im, px, py, 6.0f + ph * 2.0f, 1.4f,
                              fly_hud_faction_rgba(ch, (int)(120.0f + 135.0f * ph)));
        }
        /* The name goes where chart_labels put it, which is not always seven
         * pixels right of the dot — see that function for why it used to be. */
        if (ilab < nlab) {
            const fly__chart_label *b = &lab[ilab++];
            float ax = b->align == FLY_ALIGN_LEFT ? b->x
                     : b->align == FLY_ALIGN_RIGHT ? b->x + b->w : b->x + b->w * 0.5f;
            /* A name pushed off its own flank is joined back to its dot. It is
             * the difference between a chart and a word cloud: without the
             * line the name belongs to whichever dot it happens to have landed
             * nearest, which in a cluster is not its own. */
            if (b->lead) {
                float tx = b->dx < b->x ? b->x - 2.0f
                         : b->dx > b->x + b->w ? b->x + b->w + 2.0f
                         : b->x + b->w * 0.5f;
                fly_img_aa_line(im, b->dx, b->dy, tx, b->y + b->h * 0.5f, 1.0f,
                                FLY_RGBA(226, 240, 248, 90));
            }
            fly_text_sh(im, FLY_FONT_UI, ax, b->y, FS_S, FLY_UI_FG, b->align, L->name);
        }
    }
    {
        float px = MAPX(g->player.craft.pos.x), py = MAPY(g->player.craft.pos.y);
        float roll, pitch, yaw;
        fly_qto_euler(g->player.craft.ori, &roll, &pitch, &yaw);
        fly_img_aa_circle(im, px, py, 3.5f, 1.3f, FLY_UI_FG);
        fly_img_aa_line(im, px, py, px + cosf(yaw) * 9, py - sinf(yaw) * 9, 1.3f, FLY_UI_FG);
    }
#undef MAPX
#undef MAPY
    /* The legend is the scoreboard.
     *
     * Every power that holds anything, in its own colours, with the count of
     * fields it holds and — once the operator has sworn to somebody — what it
     * is to them. It replaces a legend that explained three dot colours, which
     * is a thing a player learns once and then never looks at again; a running
     * tally of who is winning is a thing they will look at every time. */
    {
        const char *key = "ring: contested";
        float ly = my0 + size + 10.0f, lx = x0;
        /* The key is drawn last and right-aligned, so the scoreboard has to
         * stop before it rather than at a constant. Sixty pixels was that
         * constant, and at seven powers with two-figure holdings the tally ran
         * under the key and the two overprinted — read off a 960x540 frame,
         * where `MRDN 4` and `contested` occupied the same pixels. Measure the
         * thing being avoided instead. */
        float keep = fly_text_width(FLY_FONT_UI, FS_S, key) + 14.0f;
        int f, pf = g->player.faction;
        for (f = 1; f < FLY_FACTION_COUNT; ++f) {
            char cell[40];
            int held = fly_faction_holdings(&g->world, f);
            uint32_t tc = FLY_UI_DIM;
            if (!held) continue;
            if (pf != FLY_FACTION_FREE && f != pf) {
                int st = fly_diplomacy_state(&g->world.dip, pf, f);
                tc = st == FLY_REL_WAR ? FLY_UI_BAD : st == FLY_REL_COLD ? FLY_UI_WARN
                   : st >= FLY_REL_PACT ? FLY_UI_GOOD : FLY_UI_DIM;
            } else if (f == pf) {
                tc = FLY_UI_FG;
            }
            snprintf(cell, sizeof cell, "%s %d", fly_faction_tag(f), held);
            /* Ask before drawing. Stopping *after* the cell that overran is
             * the same overlap one entry later. */
            if (lx + 9.0f + fly_text_width(FLY_FONT_UI, FS_S, cell) > x0 + size - keep) break;
            fly_img_aa_disc(im, lx + 3, ly + 5, 2.6f, fly_hud_faction_rgba(f, 255));
            lx += 9;
            lx += fly_text_sh(im, FLY_FONT_UI, lx, ly, FS_S, tc, FLY_ALIGN_LEFT, cell) + 11;
        }
        fly_text_sh(im, FLY_FONT_UI, x0 + size, ly, FS_S, FLY_UI_FAINT, FLY_ALIGN_RIGHT, key);
    }
}

/* ---------------- self view ---------------- */

static void render_self(const fly_game *g, fly_img *im) {
    fly_ui_page(im);
    float w = (float)im->w, h = (float)im->h;
    float cw = fminf(w - 48.0f, 640.0f);
    float x = (w - cw) * 0.5f;
    float colw = (cw - 28.0f) * 0.5f;
    float xr = x + colw + 28.0f;
    char buf[96];

    /* Page header: the operator's own sprite, the same one on the button that
     * opened the page, then the title on its centre line. */
    fly_ui_sprite(im, FLY_ICON_PILOT, x + 15.0f, 26.0f + FS_L * 0.5f, 30.0f);
    fly_text_mid(im, FLY_FONT_UI, x + 40.0f, 26.0f + FS_L * 0.5f, FS_L, FLY_UI_FG,
                 FLY_ALIGN_LEFT, "Pilot Profile");
    snprintf(buf, sizeof buf, "day %d  %02d:%02d", (int)(g->time_s / 86400.0),
             (int)(fmod(g->time_s, 86400.0) / 3600.0), (int)(fmod(g->time_s, 3600.0) / 60.0));
    fly_text_sh(im, FLY_FONT_MONO, x + cw, 36, FS_B, FLY_UI_DIM, FLY_ALIGN_RIGHT, buf);
    fly_img_aa_line(im, x, 58, x + cw, 58, 1.0f, FLY_RGBA(90, 120, 135, 90));
    float top = 68;

    /* left column: profile (incl. vitals), cargo table */
    float y = section(im, x, top, colw, FLY_ICON_PILOT, "PROFILE");
    snprintf(buf, sizeof buf, "%ld", g->player.tokens);
    y = kv(im, x, colw, y, "tokens", buf, FLY_UI_FG);
    snprintf(buf, sizeof buf, "%d", g->xp);
    y = kv(im, x, colw, y, "experience", buf, FLY_UI_FG);
    snprintf(buf, sizeof buf, "%d", g->reputation);
    y = kv(im, x, colw, y, "reputation", buf, FLY_UI_FG);
    snprintf(buf, sizeof buf, "%ld", fly_game_score(g));
    y = kv(im, x, colw, y, "score", buf, FLY_UI_ACCENT);
    y += 6;

    /* --- the powers ---
     *
     * Seven rows: who they are, how much of the world they are holding, and
     * what each of them makes of the operator. It is the only page in the game
     * that shows the whole political picture at once, and it is on the profile
     * rather than the chart because the numbers here are all about *you* — the
     * chart says where the war is, this says where you stand in it.
     *
     * The colour of a row is the relation, not the standing: a power that
     * likes you is still a power at war with the one you swore to, and which
     * of those two facts matters when you land there is the first one. */
    y = section(im, x, y, colw, FLY_ICON_FLAG, "POWERS");
    {
        int f, pf = g->player.faction;
        fly_text_sh(im, FLY_FONT_UI, x, y, FS_S, FLY_UI_FAINT, FLY_ALIGN_LEFT, "power");
        fly_text_sh(im, FLY_FONT_UI, x + colw - 62, y, FS_S, FLY_UI_FAINT, FLY_ALIGN_RIGHT, "held");
        fly_text_sh(im, FLY_FONT_UI, x + colw, y, FS_S, FLY_UI_FAINT, FLY_ALIGN_RIGHT, "standing");
        y += 14;
        for (f = 1; f < FLY_FACTION_COUNT; ++f) {
            int st = fly_game_standing(g, f);
            int held = fly_faction_holdings(&g->world, f);
            uint32_t c = FLY_UI_DIM;
            if (f == pf) c = FLY_UI_FG;
            else if (pf != FLY_FACTION_FREE) {
                int rel = fly_diplomacy_state(&g->world.dip, pf, f);
                c = rel == FLY_REL_WAR ? FLY_UI_BAD : rel == FLY_REL_COLD ? FLY_UI_WARN
                  : rel >= FLY_REL_PACT ? FLY_UI_GOOD : FLY_UI_DIM;
            }
            if ((f & 1) == 0)
                fly_img_round_rect(im, x - 4, y - 2, colw + 8, 15, 3.0f,
                                   FLY_RGBA(255, 255, 255, 10));
            fly_img_aa_disc(im, x + 3, y + 5, 2.6f, fly_hud_faction_rgba(f, 255));
            fly_text_sh(im, FLY_FONT_UI, x + 10, y, FS_S, c, FLY_ALIGN_LEFT,
                        f == pf ? "* " : "");
            fly_text_sh(im, FLY_FONT_UI, x + (f == pf ? 20 : 10), y, FS_S, c, FLY_ALIGN_LEFT,
                        fly_faction_name(f));
            snprintf(buf, sizeof buf, "%d", held);
            fly_text_sh(im, FLY_FONT_MONO, x + colw - 62, y, FS_S, c, FLY_ALIGN_RIGHT, buf);
            snprintf(buf, sizeof buf, "%+d", st);
            fly_text_sh(im, FLY_FONT_MONO, x + colw, y, FS_S,
                        st >= FLY_STAND_KIT ? FLY_UI_GOOD
                      : st <= FLY_STAND_HOSTILE ? FLY_UI_BAD : c,
                        FLY_ALIGN_RIGHT, buf);
            y += 15;
        }
        y += 4;
    }

    y = section(im, x, y, colw, FLY_ICON_TASK, "CARGO HOLD");
    {
        fly_text_sh(im, FLY_FONT_UI, x, y, FS_S, FLY_UI_FAINT, FLY_ALIGN_LEFT, "resource");
        fly_text_sh(im, FLY_FONT_UI, x + colw - 60, y, FS_S, FLY_UI_FAINT, FLY_ALIGN_RIGHT, "qty");
        fly_text_sh(im, FLY_FONT_UI, x + colw, y, FS_S, FLY_UI_FAINT, FLY_ALIGN_RIGHT, "base tk");
        y += 14;
        int r, row = 0;
        float row_h = (h - 40.0f - y) / (float)FLY_RES_COUNT;
        if (row_h > 15.0f) row_h = 15.0f;
        if (row_h < 12.0f) row_h = 12.0f;
        for (r = 0; r < FLY_RES_COUNT; ++r, ++row) {
            if (y > h - 30) break;
            if (row & 1)
                fly_img_round_rect(im, x - 4, y - 1, colw + 8, row_h, 3.0f,
                                   FLY_RGBA(255, 255, 255, 10));
            uint32_t c = g->player.cargo[r] > 0 ? FLY_UI_FG : FLY_RGBA(152, 172, 188, 180);
            float ty = y - 1.0f + row_h * 0.5f - FLY_FS_KEY * 0.5f; /* on the band's line */
            fly_text_sh(im, FLY_FONT_UI, x, ty, FS_S, c, FLY_ALIGN_LEFT,
                     fly_resource_name((fly_resource)r));
            snprintf(buf, sizeof buf, "%.1f", g->player.cargo[r]);
            fly_text_sh(im, FLY_FONT_MONO, x + colw - 60, ty, FS_S, c, FLY_ALIGN_RIGHT, buf);
            snprintf(buf, sizeof buf, "%.0f", fly_resource_base_price((fly_resource)r));
            fly_text_sh(im, FLY_FONT_MONO, x + colw, ty, FS_S, FLY_UI_DIM, FLY_ALIGN_RIGHT, buf);
            y += row_h;
        }
        snprintf(buf, sizeof buf, "%.0f / %.0f kg", fly_game_cargo_mass(g), g->player.airframe.cargo_cap);
        fly_text_sh(im, FLY_FONT_MONO, x + colw, y + 2, FS_S, FLY_UI_DIM, FLY_ALIGN_RIGHT, buf);
    }

    /* right column: airframe, modules, log */
    y = section(im, xr, top, colw, FLY_ICON_CRAFT, "AIRFRAME");
    fly_text_sh(im, FLY_FONT_UI, xr, y, FS_M, FLY_UI_ACCENT, FLY_ALIGN_LEFT, g->player.airframe.name);
    fly_text_sh(im, FLY_FONT_UI, xr + colw, y + 3, FS_S, FLY_UI_DIM, FLY_ALIGN_RIGHT,
             g->player.airframe.kind == FLY_CRAFT_DRONE ? "quad drone" : "fixed wing");
    y += 24;
    snprintf(buf, sizeof buf, "%.0f kg", g->player.airframe.mass);
    y = kv(im, xr, colw, y, "empty mass", buf, FLY_UI_FG);
    snprintf(buf, sizeof buf, "%.0f m", g->player.airframe.ceiling);
    y = kv(im, xr, colw, y, "ceiling", buf, FLY_UI_FG);
    snprintf(buf, sizeof buf, "%.0f m/s", g->player.airframe.vne);
    y = kv(im, xr, colw, y, "never-exceed", buf, FLY_UI_FG);
    snprintf(buf, sizeof buf, "%.0f / %.0f", g->player.craft.fuel, g->player.airframe.fuel_cap);
    y = kv(im, xr, colw, y, g->player.airframe.kind == FLY_CRAFT_DRONE ? "charge" : "fuel", buf, FLY_UI_FG);
    snprintf(buf, sizeof buf, "%.0f / %.0f", g->player.craft.hp, g->player.airframe.structure);
    y = kv(im, xr, colw, y, "hull", buf, FLY_UI_FG);
    snprintf(buf, sizeof buf, "%.0f %%  /  %.2f", g->player.craft.wear * 100.0f, g->player.airframe.weather_rating);
    y = kv(im, xr, colw, y, "wear / storm rating", buf,
           g->player.craft.wear > 0.5f ? FLY_UI_WARN : FLY_UI_FG);

    y = section(im, xr, y + 6, colw, FLY_ICON_PART, "MODULES");
    {
        int i, anym = 0;
        float mx = xr;
        for (i = 0; i < FLY_SLOT_COUNT; ++i) {
            const uint32_t *fitted = fly_game_fitted(g);
            const fly_item *it = fitted ? fly_game_item(g, fitted[i]) : NULL;
            char pill[80], nm[64];
            if (!it) continue;
            anym = 1;
            fly_item_name(it, nm, sizeof nm);
            snprintf(pill, sizeof pill, "%s: %s", fly_slot_name(i), nm);
            float tw = fly_text_width(FLY_FONT_UI, FS_S, pill) + 16;
            if (mx + tw > xr + colw) { mx = xr; y += 20; }
            fly_img_round_rect(im, mx, y - 2, tw, 16, 8.0f, FLY_RGBA(60, 130, 160, 60));
            fly_text_sh(im, FLY_FONT_UI, mx + tw * 0.5f, y, FS_S, FLY_UI_FG, FLY_ALIGN_CENTER, pill);
            mx += tw + 8;
        }
        if (!anym)
            fly_text_sh(im, FLY_FONT_UI, xr, y, FS_B, FLY_UI_FAINT, FLY_ALIGN_LEFT,
                     "stock airframe - all six slots open");
        y += 26;
    }

    /* --- what the build actually flies like ---
     *
     * A loadout screen that lists its parts tells a player what they bought.
     * This tells them what they are going to fly, which is the thing they are
     * actually choosing between: two builds worth the same money can be a
     * freighter and an interceptor, and nothing in a list of module names says
     * so. Every figure comes off the effective airframe the sim is using, so
     * it cannot describe an aeroplane that does not exist. */
    y = section(im, xr, y, colw, FLY_ICON_POWER, "HANDLING");
    {
        const fly_airframe *a = &g->player.airframe;
        float w_n = (a->mass + fly_game_cargo_mass(g)) * 9.80665f;
        /* best rate of climb at the reference speed: what is left of the
           thrust once the drag at that speed is paid for, over the weight */
        float q = 0.5f * 1.225f * a->vref * a->vref;
        float drag = q * a->wing_area * (a->cd0 + a->k_ind * 0.35f * 0.35f);
        float thrust = a->kind == FLY_CRAFT_DRONE ? a->rotor_thrust : a->thrust_max;
        float climb = w_n > 1.0f ? (thrust - drag) * a->vref / w_n : 0.0f;
        float stall = a->cl_max > 0.01f && a->wing_area > 0.01f
                          ? sqrtf(2.0f * w_n / (1.225f * a->wing_area * a->cl_max)) : 0.0f;
        float endur = a->burn_rate > 1e-4f ? a->fuel_cap / a->burn_rate / 3600.0f : 0.0f;
        char line[120];
        struct { const char *k; char v[24]; } cell[9];
        int ci, cx2, ncell = 6;
        snprintf(cell[0].v, sizeof cell[0].v, "%.0f m/s", climb > 0.0f ? climb : 0.0f);
        cell[0].k = "climb";
        snprintf(cell[1].v, sizeof cell[1].v, "%.0f m/s", stall);
        cell[1].k = "stall";
        snprintf(cell[2].v, sizeof cell[2].v, "%.0f m/s", a->vne);
        cell[2].k = "Vne";
        snprintf(cell[3].v, sizeof cell[3].v, "%.1f h", endur);
        cell[3].k = "endurance";
        snprintf(cell[4].v, sizeof cell[4].v, "%.0f kg", a->cargo_cap);
        cell[4].k = "hold";
        if (a->beam_heat > 0.0f)
            snprintf(cell[5].v, sizeof cell[5].v, "%.0f dps", a->gun_dps);
        else if (a->gun_dps > 0.0f)
            snprintf(cell[5].v, sizeof cell[5].v, "%.0f dps", a->gun_dps);
        else
            snprintf(cell[5].v, sizeof cell[5].v, "unarmed");
        cell[5].k = a->beam_heat > 0.0f ? "beam" : "guns";
        /* The rails, and only when there are any. A row of "0 rounds" on every
         * unarmed hauler in the game would be six cells of nothing to say the
         * same thing the "unarmed" above already says. */
        if (a->ord_kind) {
            const fly_ord_class *k = fly_ord_class_get(a->ord_kind);
            snprintf(cell[ncell].v, sizeof cell[ncell].v, "%d %s",
                     a->ord_ammo, k ? k->tag : "?");
            cell[ncell].k = "rails";
            ++ncell;
            snprintf(cell[ncell].v, sizeof cell[ncell].v, "%.1f km",
                     (double)(a->ord_range / 1000.0f));
            cell[ncell].k = "envelope";
            ++ncell;
        }
        if (a->cm_kind) {
            const fly_ord_class *k = fly_ord_class_get(a->cm_kind);
            snprintf(cell[ncell].v, sizeof cell[ncell].v, "%d %s",
                     a->cm_ammo, k ? k->tag : "?");
            cell[ncell].k = "decoys";
            ++ncell;
        }
        for (ci = 0, cx2 = 0; ci < ncell; ++ci) {
            float px2 = xr + (float)(ci % 3) * (colw / 3.0f);
            float py = y + (float)(ci / 3) * 17.0f;
            snprintf(line, sizeof line, "%s %s", cell[ci].v, cell[ci].k);
            fly_text_sh(im, FLY_FONT_UI, px2, py, FS_S, FLY_UI_FG, FLY_ALIGN_LEFT, line);
            (void)cx2;
        }
        /* The block is as tall as the rows it actually drew: three cells to a
         * row, and a third row appears only on an armed aeroplane. A fixed
         * height here put the LOG heading through the last line of stats the
         * moment the stats grew. */
        y += 6.0f + 17.0f * (float)((ncell + 2) / 3);
    }

    if (y < h - 64) {
        y = section(im, xr, y, colw, FLY_ICON_ROUTE, "LOG");
        int i, nlog = g->log_count < 3 ? g->log_count : 3;
        for (i = 0; i < nlog && y < h - 16; ++i, y += 14) {
            char line[96];
            snprintf(line, sizeof line, "%s", g->log[g->log_count - nlog + i]);
            fly_text_sh(im, FLY_FONT_UI, xr, y, FS_S,
                     i == nlog - 1 ? FLY_UI_FG : FLY_UI_DIM, FLY_ALIGN_LEFT, line);
        }
    }
}

/* ---------------- help view ---------------- */

static const char *help_titles[3] = { "Frontier Courier", "Controls", "Trade & Risk" };

static const char *help_paras[3] =
{
    "The world runs on airframes, and you can own a hangar full. Buy modules "
    "into inventory, fit them to independent aircraft, and haul cargo between "
    "settlements, keep your machine flying, and buy the "
    "upgrades that open the far places: storm belts, high skyports, ruins "
    "with VTOL pads.\n\n"
    "Tokens buy fuel, parts and modules. Earn them with delivery "
    "contracts, trade margins and combat bounties. Lose your hull "
    "and recovery begins on foot at your launch site. Scattered fitted modules "
    "can be collected, and recovery credit guarantees a replacement airframe.",
    NULL, /* rendered as a key table */
    "Prices follow scarcity: a mine drowning in ore sells it cheap, while a "
    "city starved of alloy pays fortunes. Watch stock levels, fill contracts, "
    "and learn the routes.\n\n"
    "Weather wears the airframe and storms tear it. Repair with spare parts, "
    "refuel early, and fit armor or a gun before flying pirate "
    "country.\n\n"
    "A gun arrives the instant you press it; everything else is an object in "
    "the air with seconds of life in it, and those seconds work both ways. "
    "Carry flares. When one is inbound the glass gives you the bearing and "
    "the count: put it on your wingtip, get low, come off the power, and shed "
    "a flare at about three. All four, not one of them.\n\n"
    "Some sites only accept certain airframes. Explore to find them; "
    "upgrade to reach them.",
};

static const char *help_keys[][2] = {
    { "arrows", "pitch / roll" },
    { "q  e", "yaw" },
    { "w  s", "throttle" },
    { "b", "brakes" },
    { "f", "fire gun" },
    { "c", "release ordnance" },
    { "v", "shed a flare / chaff" },
    { "r", "light the rocket" },
    { "a", "autopilot toggle" },
    { "tab", "actions overlay" },
    { "m", "world chart" },
    { "1 2 3", "play / self / help" },
    { "enter / esc", "select / back" },
    { "drag left half", "virtual stick (down = pull back)" },
    { "right edge", "throttle slider" },
    { "joypad", "sticks fly, buttons: fire, launch, decoy, brake, AP" },
    { "w a s d / arrows", "walk on the ground" },
    { "space / g", "jump / interact, board, rail" },
    { "drag", "look while walking or riding" },
};

/* greedy word-wrap */
static float wrap_text(fly_img *im, float x, float y, float maxw, float px,
                       uint32_t color, const char *s) {
    char line[128];
    int ll = 0;
    const char *p = s;
    while (*p) {
        if (*p == '\n') {
            line[ll] = 0;
            fly_text_sh(im, FLY_FONT_UI, x, y, px, color, FLY_ALIGN_LEFT, line);
            y += fly_line_height(px) * (p[1] == '\n' ? 1.6f : 1.0f);
            if (p[1] == '\n') ++p;
            ll = 0;
            ++p;
            continue;
        }
        /* take next word */
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n') ++e;
        int wl = (int)(e - p);
        char probe[128];
        int pl = ll ? ll + 1 : 0;
        if (pl + wl >= (int)sizeof probe) wl = (int)sizeof probe - pl - 1;
        memcpy(probe, line, (size_t)ll);
        if (ll) probe[ll] = ' ';
        memcpy(probe + pl, p, (size_t)wl);
        probe[pl + wl] = 0;
        if (ll && fly_text_width(FLY_FONT_UI, px, probe) > maxw) {
            line[ll] = 0;
            fly_text_sh(im, FLY_FONT_UI, x, y, px, color, FLY_ALIGN_LEFT, line);
            y += fly_line_height(px);
            ll = 0;
            continue;
        }
        memcpy(line, probe, (size_t)(pl + wl + 1));
        ll = pl + wl;
        p = e;
        while (*p == ' ') ++p;
    }
    if (ll) {
        line[ll] = 0;
        fly_text_sh(im, FLY_FONT_UI, x, y, px, color, FLY_ALIGN_LEFT, line);
        y += fly_line_height(px);
    }
    return y;
}

static void render_help(const fly_ui *ui, fly_img *im) {
    fly_ui_page(im);
    float w = (float)im->w, h = (float)im->h;
    float cw = fminf(w - 72.0f, 520.0f);
    float x = (w - cw) * 0.5f;

    /* one sprite per page, so the three pages are told apart before they are
     * read: what the world is, what the keys do, what will kill you */
    {
        static const fly_icon PAGE[3] = { FLY_ICON_ROUTE, FLY_ICON_PART, FLY_ICON_SHIELD };
        float ty = 30.0f + FS_L * 0.5f;
        fly_ui_sprite(im, PAGE[ui->help_page % 3], x + 15.0f, ty, 30.0f);
        fly_text_mid(im, FLY_FONT_UI, x + 40.0f, ty, FS_L, FLY_UI_FG, FLY_ALIGN_LEFT,
                     help_titles[ui->help_page]);
    }
    fly_img_aa_line(im, x, 68, x + cw, 68, 1.0f, FLY_RGBA(128, 172, 200, 120));

    float y = 78;
    if (ui->help_page == 1) {
        size_t i;
        for (i = 0; i < sizeof help_keys / sizeof help_keys[0]; ++i) {
            if (y > h - 46) break;
            fly_text_sh(im, FLY_FONT_MONO, x + 118, y, FS_B, FLY_UI_ACCENT, FLY_ALIGN_RIGHT,
                     help_keys[i][0]);
            fly_text_sh(im, FLY_FONT_UI, x + 134, y, FS_B, FLY_UI_FG, FLY_ALIGN_LEFT, help_keys[i][1]);
            y += 17;
        }
    } else {
        wrap_text(im, x, y, cw, FS_B, FLY_RGBA(232, 242, 250, 245), help_paras[ui->help_page]);
    }

    /* page dots */
    {
        int i;
        for (i = 0; i < 3; ++i)
            fly_img_aa_disc(im, w * 0.5f + (float)(i - 1) * 14.0f, h - 22.0f,
                            i == ui->help_page ? 3.2f : 2.2f,
                            i == ui->help_page ? FLY_UI_ACCENT : FLY_UI_FAINT);
        fly_text_sh(im, FLY_FONT_UI, w * 0.5f, h - 40, FS_S, FLY_UI_FAINT, FLY_ALIGN_CENTER,
                 "up / down to turn pages");
    }
}

/* ---------------- splash view ---------------- */

/* the splash sits on a live frame, so its text carries its own wash, padded
 * like every other group */
#define SPLASH_PAD 11.0f

static void render_splash(fly_ui *ui, const fly_game *g, fly_img *out) {
    /* living world as the backdrop */
    fly_cam cam = fly_cam_player(g);
    fly_render_frame_aa(out, g, &cam, &ui->ropts);
    float w = (float)out->w, h = (float)out->h;
    /* No vignette. The splash is the one screen whose whole job is to show the
     * world you are about to fly into, and it was showing it through a 55%
     * wash of near-black plus a soft band over the top third. Every piece of
     * type on this screen already sits on its own pane of glass — the
     * wordmark, the caption under the Play button, the world facts — so the
     * contrast is paid for locally and the sky can be the sky. */

    /* wordmark */
    {
        const char *tag = "survival courier of the retro-future skies";
        float tw = fly_text_width(FLY_FONT_UI, FS_B, tag);
        float mw = fly_text_width(FLY_FONT_UI, 44.0f, "FLY99");
        float bw = (tw > mw ? tw : mw) + 2.0f * SPLASH_PAD;
        fly_ui_scrim(out, w * 0.5f - bw * 0.5f, h * 0.16f - SPLASH_PAD, bw,
                     52.0f + FS_B + 2.0f * SPLASH_PAD);
        fly_text_sh(out, FLY_FONT_UI, w * 0.5f, h * 0.16f, 44.0f, FLY_UI_FG,
                    FLY_ALIGN_CENTER, "FLY99");
        fly_text_sh(out, FLY_FONT_UI, w * 0.5f, h * 0.16f + 52.0f, FS_B, FLY_UI_DIM,
                    FLY_ALIGN_CENTER, tag);
    }

    /* the Play button */
    {
        float cx = w * 0.5f, cy = FLY_SPLASH_PLAY_Y(h), r = FLY_SPLASH_PLAY_R;
        fly_img_soft_shadow(out, cx - r, cy - r, 2 * r, 2 * r, r, 12.0f, FLY_RGBA(0, 0, 0, 130));
        fly_ui_glass_disc(out, cx, cy, r, FLY_RGBA(46, 122, 158, 128));
        fly_img_aa_circle(out, cx, cy, r, 1.6f, FLY_UI_ACCENT);
        fly_img_aa_tri(out, cx - 7, cy - 11, cx - 7, cy + 11, cx + 12, cy, FLY_UI_ACCENT);
        /* the caption lands on the pad's white stripe, which is the brightest
         * thing in the game — dim text on it was invisible */
        {
            const char *cap = "tap or press enter to launch";
            float cw = fly_text_width(FLY_FONT_UI, FS_S, cap) + 2.0f * SPLASH_PAD;
            fly_ui_scrim(out, cx - cw * 0.5f, cy + r + 10.0f - SPLASH_PAD, cw,
                         FS_S + 2.0f * SPLASH_PAD);
            fly_text_sh(out, FLY_FONT_UI, cx, cy + r + 10, FS_S, FLY_UI_FG,
                        FLY_ALIGN_CENTER, cap);
        }
    }

    /* world facts, bottom center */
    {
        char buf[96];
        int i, known = 0;
        for (i = 0; i < g->world.nloc; ++i) known += g->world.loc[i].discovered;
        snprintf(buf, sizeof buf, "world %u   -   %d/%d sites charted   -   day %d",
                 g->seed, known, g->world.nloc, (int)(g->time_s / 86400.0));
        float fw = fly_text_width(FLY_FONT_MONO, FS_S, buf) + 2.0f * SPLASH_PAD;
        fly_ui_scrim(out, w * 0.5f - fw * 0.5f, h - 26.0f - SPLASH_PAD, fw,
                     FS_S + 2.0f * SPLASH_PAD);
        fly_text_sh(out, FLY_FONT_MONO, w * 0.5f, h - 26.0f, FS_S, FLY_UI_DIM,
                    FLY_ALIGN_CENTER, buf);
    }
}

/* ---------------- touch affordances ---------------- */

static void draw_touch_controls(const fly_ui *ui, const fly_game *g, fly_img *im) {
    float w = (float)im->w, h = (float)im->h;
    /* throttle slider */
    {
        /* drawn a touch inside the hit zone, so the ends of the travel are
         * still grabbable without aiming */
        float x = FLY_TOUCH_X(w);
        float top = FLY_TOUCH_TOP(h) + h * 0.02f, bot = FLY_TOUCH_BOT(h) - h * 0.02f;
        fly_img_aa_line(im, x + 1, top + 1, x + 1, bot + 1, 1.2f, FLY_UI_SHADOW);
        fly_img_aa_line(im, x, top, x, bot, 1.2f, FLY_UI_FAINT);
        float ky = bot - (bot - top) * ui->throttle;
        fly_img_aa_disc(im, x + 1, ky + 1, 5.0f, FLY_UI_SHADOW);
        fly_img_aa_disc(im, x, ky, 5.0f, ui->slider_active ? FLY_UI_ACCENT : FLY_UI_DIM);
        fly_text_sh(im, FLY_FONT_UI, x, bot + 6, FS_S, FLY_UI_FAINT, FLY_ALIGN_CENTER, "THR");
    }
    /* The action column: a pane of glass with its buttons stacked and centred
     * in it, inboard of the throttle strip. The box is sized from the count, so
     * an aeroplane with a rack and a dispenser grows the pane downward and one
     * with neither draws a single FIRE exactly where it always was. */
    {
        int nbtn = act_count(g), which;
        static const char *LABEL[3] = { "FIRE", "LNCH", "DECY" };
        if (nbtn > 0)
            fly_ui_scrim(im, FLY_ACT_X(w), FLY_ACT_Y(h, nbtn), FLY_ACT_W, FLY_ACT_H(nbtn));
        for (which = 0; which < 3; ++which) {
            int i = act_slot(g, which);
            float bx, by, r = FLY_ACT_BTN * 0.5f;
            int held = which == 0 ? ui->fire_touch
                     : which == 1 ? ui->launch_touch : ui->decoy_touch;
            /* Empty reads as empty. A launch button on a rack with nothing left
             * on it is a control that does nothing, and saying so on the button
             * is cheaper than making the player work it out from the corner. */
            int empty = which == 1 ? g->player.craft.ammo <= 0
                      : which == 2 ? g->player.craft.cm <= 0 : 0;
            uint32_t c;
            if (i < 0) continue;
            c = held ? FLY_UI_WARN : empty ? FLY_UI_FAINT : FLY_UI_FG;
            bx = FLY_ACT_BX(w);
            by = FLY_ACT_BY(h, nbtn, i);
            if (held) fly_ui_glass_disc(im, bx, by, r, FLY_RGBA(96, 54, 16, 150));
            fly_img_aa_circle(im, bx, by, r - 0.5f, held ? 1.6f : 1.1f,
                              held ? c : FLY_UI_GLASS_EDGE);
            fly_text_mid(im, FLY_FONT_UI, bx, by, FS_S, c, FLY_ALIGN_CENTER, LABEL[which]);
        }
    }
    /* virtual stick, visible while engaged */
    if (ui->stick_active) {
        float rad = FLY_STICK_RADIUS_FRAC * h;
        fly_img_aa_circle(im, ui->stick_ox, ui->stick_oy, rad, 1.3f, FLY_UI_FAINT);
        fly_img_aa_disc(im, ui->stick_ox + ui->stick_py * rad + 1,
                        ui->stick_oy + ui->stick_px * rad + 1, 6.0f, FLY_UI_SHADOW);
        fly_img_aa_disc(im, ui->stick_ox + ui->stick_py * rad,
                        ui->stick_oy + ui->stick_px * rad, 6.0f, FLY_UI_ACCENT);
    }
}

/* ---------------- top level ---------------- */

void fly_ui_render(fly_ui *ui, const fly_game *g, fly_img *out) {
    ui->view_w = out->w;
    ui->view_h = out->h;
    switch (ui->view) {
    case FLY_VIEW_SPLASH:
        render_splash(ui, g, out);
        return; /* no nav on the splash: Play is the only door */
    case FLY_VIEW_SELF:
        render_self(g, out);
        break;
    case FLY_VIEW_HELP:
        render_help(ui, out);
        break;
    default: {
        fly_cam cam = fly_cam_player(g);
        fly_render_frame_aa(out, g, &cam, &ui->ropts);
        if (ui->overlay == FLY_OVERLAY_EDIT) {
            /* defocus the scene rather than dimming it; the overlay is the
             * focus, and the same glass carries that without turning the world
             * into a flat 65%-opaque wash of near-black */
            fly_ui_page(out);
            render_edit_overlay(ui, g, out);
        } else if (ui->overlay == FLY_OVERLAY_MAP) {
            fly_ui_page(out);
            render_map_overlay(g, out);
        } else {
            fly_hud_draw(out, g);
            if (g->mode == FLY_MODE_FLIGHT && g->docked < 0 && !g->autopilot)
                draw_touch_controls(ui, g, out);
        }
        break;
    }
    }
    draw_nav(out, ui->view);
}
