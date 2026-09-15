#include "fly_faction.h"

#include <string.h>

#include "fly_rng.h"
#include "fly_world.h"

/* ---------------- identity ----------------
 *
 * One table, seven powers and the absence of one. Everything a power *is*
 * lives on this row: what it is called, what it flies in, and the two numbers
 * its politics come out of. Nothing else in the codebase is allowed a second
 * opinion about a faction's colour, which is why the renderer, the chart and
 * the HUD all read this and none of them keeps a palette.
 *
 * The liveries are picked to be told apart in the two conditions that matter
 * and are not the same condition: a silhouette against a bright sky, where
 * only value separates them, and a two-pixel dot on a chart, where only hue
 * does. So no two powers share a value band *and* a hue quadrant, and the two
 * that come closest — Foundry rust and Corsair crimson — are separated by
 * their trim, which is the ash-black fin the Corsairs paint and the Foundry
 * never do. */

typedef struct {
    /* No article. Every one of these is written "the %s" at the call sites
     * that want one — "sworn to the Concord", "the Meridian Choir board is
     * closed to you" — and a name that carried its own produced "the The
     * Concord" on four screens and in the gallery captions. */
    const char *name;
    const char *tag;    /* four letters, always: a column of tags sorts by eye */
    const char *creed;
    fly_v3 colour, trim;
    float law;   /* -1 outlaw .. +1 order */
    float make;  /* -1 mystic .. +1 industrial */
} fly__power;

static const fly__power FLY__POWERS[FLY_FACTION_COUNT] = {
    { "Free Reaches", "FREE", "Owes nobody a flag and pays for it in fuel",
      { 0.42f, 0.46f, 0.40f }, { 0.66f, 0.68f, 0.62f }, 0.0f, 0.0f },
    { "Concord", "CNCD", "One sky, one register, one law over it",
      { 0.80f, 0.84f, 0.90f }, { 0.16f, 0.42f, 0.86f }, 0.95f, 0.15f },
    { "Corsair Reach", "CORS", "The map belongs to whoever is holding it",
      { 0.62f, 0.10f, 0.11f }, { 0.09f, 0.08f, 0.09f }, -1.00f, -0.10f },
    { "Foundry", "FNDY", "Everything under the ground belongs above it",
      { 0.74f, 0.34f, 0.09f }, { 0.30f, 0.29f, 0.27f }, 0.35f, 0.95f },
    { "Lantern Compact", "LNTN", "Enough for everyone, and light to read by",
      { 0.42f, 0.86f, 0.68f }, { 0.92f, 0.76f, 0.30f }, 0.25f, -0.85f },
    { "Halcyon Line", "HLCN", "The mail moves or nothing does",
      { 0.16f, 0.55f, 0.62f }, { 0.86f, 0.88f, 0.90f }, 0.55f, 0.40f },
    { "Cinder Pact", "CNDR", "What the storm takes down, we take up",
      { 0.44f, 0.30f, 0.52f }, { 0.72f, 0.62f, 0.30f }, -0.60f, 0.50f },
    { "Meridian Choir", "MRDN", "The old world left instructions",
      { 0.24f, 0.26f, 0.62f }, { 0.90f, 0.84f, 0.56f }, -0.15f, -0.95f },
};

static int fly__fok(int f) { return f >= 0 && f < FLY_FACTION_COUNT; }

const char *fly_faction_name(int f) { return fly__fok(f) ? FLY__POWERS[f].name : "?"; }
const char *fly_faction_tag(int f) { return fly__fok(f) ? FLY__POWERS[f].tag : "????"; }
const char *fly_faction_creed(int f) { return fly__fok(f) ? FLY__POWERS[f].creed : ""; }
fly_v3 fly_faction_colour(int f) {
    return fly__fok(f) ? FLY__POWERS[f].colour : fly_v3mk(0.5f, 0.5f, 0.5f);
}
fly_v3 fly_faction_trim(int f) {
    return fly__fok(f) ? FLY__POWERS[f].trim : fly_v3mk(0.5f, 0.5f, 0.5f);
}
/* How much hue a colour has: the spread between its brightest and dimmest
 * channel. Grey and white are 0, a saturated primary is near 1. */
static float fly__sat(fly_v3 c) {
    float hi = c.x > c.y ? (c.x > c.z ? c.x : c.z) : (c.y > c.z ? c.y : c.z);
    float lo = c.x < c.y ? (c.x < c.z ? c.x : c.z) : (c.y < c.z ? c.y : c.z);
    return hi - lo;
}

fly_v3 fly_faction_mark(int f) {
    fly_v3 a = fly_faction_colour(f), b = fly_faction_trim(f);
    return fly__sat(b) > fly__sat(a) ? b : a;
}

fly_v3 fly_faction_countermark(int f) {
    fly_v3 a = fly_faction_colour(f), b = fly_faction_trim(f);
    return fly__sat(b) > fly__sat(a) ? a : b;
}

float fly_faction_law(int f) { return fly__fok(f) ? FLY__POWERS[f].law : 0.0f; }
float fly_faction_make(int f) { return fly__fok(f) ? FLY__POWERS[f].make : 0.0f; }
int fly_faction_is_power(int f) { return f > FLY_FACTION_FREE && f < FLY_FACTION_COUNT; }

/* ---------------- diplomacy ----------------
 *
 * Where a pair belongs, from their creeds and nothing else. Two powers that
 * want the same thing are near allies; two that differ on both axes at once
 * cannot be anything but at war. The weights make the law axis matter rather
 * more than the industrial one, because a power that tolerates piracy is a
 * threat to a power that polices it in a way that a difference of opinion
 * about strip-mining is not.
 *
 * FREE is nobody's enemy and nobody's ally: it is the ground everyone is
 * arguing over, not a party to the argument, and giving it a temper would have
 * made "unaligned" a thirteenth position rather than the absence of twelve. */
float fly_faction_temper(int a, int b) {
    float dl, dm, t;
    if (!fly__fok(a) || !fly__fok(b) || a == b) return 0.0f;
    if (a == FLY_FACTION_FREE || b == FLY_FACTION_FREE) return 0.0f;
    dl = FLY__POWERS[a].law - FLY__POWERS[b].law;
    dm = FLY__POWERS[a].make - FLY__POWERS[b].make;
    if (dl < 0) dl = -dl;
    if (dm < 0) dm = -dm;
    /* The constants were measured against the map rather than chosen. The
     * first set put fourteen of the twenty-one pairs at war on a fresh world,
     * which is not a political landscape, it is a free-for-all: with everybody
     * shooting everybody the liveries stop meaning anything and the only thing
     * a player can read off the sky is that it is dangerous. These put seven
     * pairs at war and leave nine cold — near enough to the line that the
     * seed's jitter and an evening's events decide them, which is what makes
     * two worlds different wars rather than the same one twice. */
    t = 0.55f - 0.55f * dl - 0.40f * dm;
    return fly_clampf(t, -1.0f, 1.0f);
}

/* The opening position: temper, plus a little of this particular world.
 *
 * The jitter is small — a fifth of the scale — and it is there so that two
 * seeds are not the same war. It cannot invert a creed: powers that belong at
 * each other's throats start there on every world, and the pairs it decides
 * are the ones temper leaves near a threshold, which are exactly the pairs
 * whose quarrel is a matter of circumstance. */
void fly_diplomacy_init(fly_diplomacy *d, uint32_t seed) {
    int a, b;
    memset(d, 0, sizeof *d);
    for (a = 0; a < FLY_FACTION_COUNT; ++a)
        for (b = a + 1; b < FLY_FACTION_COUNT; ++b) {
            float jitter = 0.0f;
            if (a != FLY_FACTION_FREE) {
                uint32_t h = fly_hash2(seed ^ 0x9c1fu, a, b);
                jitter = ((float)(h & 0xFFFFu) / 65535.0f - 0.5f) * 0.40f;
            }
            d->rel[a][b] = d->rel[b][a] =
                fly_clampf(fly_faction_temper(a, b) + jitter, -1.0f, 1.0f);
        }
}

float fly_diplomacy_rel(const fly_diplomacy *d, int a, int b) {
    if (!d || !fly__fok(a) || !fly__fok(b) || a == b) return 0.0f;
    return d->rel[a][b];
}

void fly_diplomacy_nudge(fly_diplomacy *d, int a, int b, float amount) {
    if (!d || !fly__fok(a) || !fly__fok(b) || a == b) return;
    if (a == FLY_FACTION_FREE || b == FLY_FACTION_FREE) return;
    d->rel[a][b] = d->rel[b][a] = fly_clampf(d->rel[a][b] + amount, -1.0f, 1.0f);
}

/* The thresholds. Asymmetric on purpose — war is entered at -0.45 and left at
 * -0.35 — because a pair sitting exactly on one number would otherwise declare
 * and settle on alternate hours and fill the radio with it. Everything that
 * reads a state goes through here, so the hysteresis is in one place; the cost
 * is that a state is a function of the relation alone and the band between the
 * two numbers reads as COLD, which is a fair description of it. */
int fly_diplomacy_state(const fly_diplomacy *d, int a, int b) {
    float r = fly_diplomacy_rel(d, a, b);
    if (a == FLY_FACTION_FREE || b == FLY_FACTION_FREE) return FLY_REL_PEACE;
    if (a == b) return FLY_REL_PACT;
    if (r <= -0.45f) return FLY_REL_WAR;
    if (r < 0.10f) return FLY_REL_COLD;
    if (r < 0.60f) return FLY_REL_PEACE;
    return FLY_REL_PACT;
}

const char *fly_relation_name(int state) {
    static const char *n[FLY_REL_COUNT] = { "war", "cold", "peace", "pact" };
    return state >= 0 && state < FLY_REL_COUNT ? n[state] : "?";
}

int fly_faction_hostile(const struct fly_world *w, int a, int b) {
    if (!w || a == b) return 0;
    if (a == FLY_FACTION_FREE || b == FLY_FACTION_FREE) return 0;
    return fly_diplomacy_state(&w->dip, a, b) == FLY_REL_WAR;
}

float fly_faction_standing_spill(const fly_diplomacy *d, int deed_with, int other) {
    float r;
    if (deed_with == other) return 1.0f;
    if (!fly_faction_is_power(deed_with) || !fly_faction_is_power(other)) return 0.0f;
    r = fly_diplomacy_rel(d, deed_with, other);
    /* An ally takes half the credit; an enemy takes the whole insult. The
     * asymmetry is the design: it is easier to make an enemy than a friend,
     * and a player who tries to please every power ends up with nothing but
     * neutral ground and no kit. */
    return r >= 0.0f ? 0.5f * r : r;
}

/* ---------------- who starts with what ----------------
 *
 * Ownership at world-gen, and not one random number spent on it. The site
 * decides most of it — a mine is Foundry country and a ruin is where the Choir
 * digs — and a hash of the seed and the index decides the rest, so two worlds
 * with the same terrain still have different politics.
 *
 * The leaning is a weight per power, and the winner is the largest. That is a
 * deliberate choice over "roll on a table": a weight can say *how much* a
 * refinery is Foundry ground, which lets danger and the storm belt pull real
 * sites away from the obvious owner without special-casing anything. */
static void fly__site_weights(const struct fly_world *w, int li, float *wt) {
    const fly_location *L = &w->loc[li];
    float danger = fly_world_danger(w, L->pos.e, L->pos.n);
    float belt = 1.0f - fly_clampf(fabsf(L->pos.n - w->storm_belt_y) /
                                   (w->storm_belt_w > 1.0f ? w->storm_belt_w : 1.0f), 0.0f, 1.0f);
    int f;

    for (f = 0; f < FLY_FACTION_COUNT; ++f) wt[f] = 0.0f;
    /* The unaligned are a real answer, and a common one near home: most of the
     * world is somebody's market before it is anybody's territory. */
    wt[FLY_FACTION_FREE] = 0.55f - 0.35f * danger;
    switch (L->kind) {
    case FLY_LOC_CITY:
        wt[FLY_FACTION_CONCORD] += 0.85f; wt[FLY_FACTION_LANTERN] += 0.45f;
        wt[FLY_FACTION_HALCYON] += 0.40f; break;
    case FLY_LOC_OUTPOST:
        wt[FLY_FACTION_HALCYON] += 0.55f; wt[FLY_FACTION_CONCORD] += 0.30f;
        wt[FLY_FACTION_CINDER] += 0.25f; break;
    case FLY_LOC_MINE:
        wt[FLY_FACTION_FOUNDRY] += 0.90f; wt[FLY_FACTION_CINDER] += 0.35f; break;
    case FLY_LOC_REFINERY:
        wt[FLY_FACTION_FOUNDRY] += 0.80f; wt[FLY_FACTION_CONCORD] += 0.20f; break;
    case FLY_LOC_RUINS:
        wt[FLY_FACTION_MERIDIAN] += 0.85f; wt[FLY_FACTION_CORSAIR] += 0.55f; break;
    case FLY_LOC_SKYPORT:
        wt[FLY_FACTION_LANTERN] += 0.75f; wt[FLY_FACTION_HALCYON] += 0.45f; break;
    default: break;
    }
    /* Country tells as loudly as trade. Dangerous ground is outlaw ground, the
     * storm belt is where the Pact makes its living, and the law thins out the
     * further it has to fly to get there. */
    wt[FLY_FACTION_CORSAIR] += 0.95f * danger;
    wt[FLY_FACTION_CINDER] += 0.75f * belt + 0.25f * danger;
    wt[FLY_FACTION_CONCORD] -= 0.70f * danger;
    wt[FLY_FACTION_MERIDIAN] += 0.30f * danger;

    for (f = 0; f < FLY_FACTION_COUNT; ++f) {
        /* One hash per power per site: the tie-breaker, and wide enough to move
         * a close second into first without ever beating a strong lean. */
        uint32_t h = fly_hash2(w->seed ^ 0x4a37u, li * 16 + f, f * 7 + 3);
        wt[f] += ((float)(h & 0xFFFFu) / 65535.0f) * 0.62f;
    }
}

/* How much one power wants one site. Split out of the assignment because the
 * foothold pass below needs the runner-up's margin, not just the winner. */
static float fly__owner_weight(const struct fly_world *w, int li, int f) {
    float wt[FLY_FACTION_COUNT];
    if (f < 0 || f >= FLY_FACTION_COUNT) return -1e9f;
    fly__site_weights(w, li, wt);
    return wt[f];
}

static int fly__site_owner(const struct fly_world *w, int li) {
    float wt[FLY_FACTION_COUNT], best = -1e9f;
    int f, pick = FLY_FACTION_FREE;
    fly__site_weights(w, li, wt);
    for (f = 0; f < FLY_FACTION_COUNT; ++f)
        if (wt[f] > best) { best = wt[f]; pick = f; }
    return pick;
}

/* Give a power with nothing somewhere to stand.
 *
 * The weighted assignment above is honest and it is also perfectly capable of
 * handing one of the seven no ground at all — on seed 4242 the Cinder Pact
 * opened with zero sites, which does not make it a weak power, it makes it a
 * name in a table. A faction the player can never trade with, never fly for
 * and never see a pennant of is not in the game.
 *
 * So each empty power takes the one site it came closest to winning, off an
 * owner that can spare it. Closest and not best: the site it *nearly* held is
 * the one that fits its story — a mine for the Foundry, a storm-belt outpost
 * for the Pact — and taking it from a power with several leaves the map's
 * shape intact. Deterministic, and still not one random number. */
static void fly__seed_footholds(struct fly_world *w) {
    int f, i, guard;
    for (f = 1; f < FLY_FACTION_COUNT; ++f) {
        int best = -1;
        float best_gap = 1e9f;
        if (fly_faction_holdings(w, f) > 0) continue;
        for (i = 0; i < w->nloc; ++i) {
            int owner = w->loc[i].owner;
            float gap;
            /* Never off a power that would then be empty itself, and never the
             * field the player starts at — home changing hands before the
             * first take-off is a world that begins by moving under them. */
            if (i == 0) continue;
            if (owner != FLY_FACTION_FREE && fly_faction_holdings(w, owner) < 2) continue;
            gap = fly__owner_weight(w, i, owner) - fly__owner_weight(w, i, f);
            if (gap < best_gap) { best_gap = gap; best = i; }
        }
        if (best >= 0) w->loc[best].owner = (unsigned char)f;
    }
    /* Cheap belt and braces: the loop above can only fail if every site is
     * spoken for by a power holding exactly one, which needs more powers than
     * sites. Say so in the one way a header can — by leaving the state legal —
     * rather than looping for ever trying to fix it. */
    for (guard = 0, i = 0; i < w->nloc; ++i)
        if (w->loc[i].owner >= FLY_FACTION_COUNT) { w->loc[i].owner = FLY_FACTION_FREE; ++guard; }
    (void)guard;
}

void fly_faction_world_init(struct fly_world *w) {
    int i, f;
    fly_diplomacy_init(&w->dip, w->seed);
    for (i = 0; i < w->nloc; ++i) w->loc[i].owner = (unsigned char)fly__site_owner(w, i);
    fly__seed_footholds(w);
    for (i = 0; i < w->nloc; ++i) {
        fly_location *L = &w->loc[i];
        /* Unaligned ground is held loosely by definition — there is nobody
         * there to hold it — which is what makes the first site a power takes
         * a cheap one and the second an argument. */
        L->grip = L->owner == FLY_FACTION_FREE ? 0.25f : 0.55f +
                  (float)(fly_hash2(w->seed ^ 0x77b1u, i, 5) & 0xFFu) / 255.0f * 0.35f;
        for (f = 0; f < FLY_FACTION_COUNT; ++f) L->push[f] = 0.0f;
    }
}

/* ---------------- pressure and the grip ---------------- */

void fly_faction_push(struct fly_world *w, int loc, int faction, float amount) {
    if (!w || loc < 0 || loc >= w->nloc) return;
    if (!fly_faction_is_power(faction) || !(amount > 0.0f)) return;
    /* Bounded rather than accumulating without limit: a power that parks six
     * aircraft on one pad for a day should take the site, not build a reserve
     * of pressure that then flips four more the moment they leave. */
    w->loc[loc].push[faction] = fly_clampf(w->loc[loc].push[faction] + amount, 0.0f, 3.0f);
}

int fly_faction_challenger(const struct fly_world *w, int loc, float *pressure) {
    int f, best = FLY_FACTION_FREE;
    float bp = 0.0f;
    if (!w || loc < 0 || loc >= w->nloc) { if (pressure) *pressure = 0.0f; return FLY_FACTION_FREE; }
    for (f = 1; f < FLY_FACTION_COUNT; ++f) {
        float p = w->loc[loc].push[f];
        if (f == w->loc[loc].owner) continue;
        /* A power at pact with the owner is not challenging it: it is trading
         * there. Without this an alliance reads as a slow takeover of exactly
         * the partner you are allied with, which is the opposite of the deal. */
        if (fly_faction_is_power(w->loc[loc].owner) &&
            fly_diplomacy_state(&w->dip, f, w->loc[loc].owner) >= FLY_REL_PACT) continue;
        if (p > bp) { bp = p; best = f; }
    }
    if (pressure) *pressure = bp;
    return bp > 0.0f ? best : FLY_FACTION_FREE;
}

/* Contested means "the grip is actually moving", not "somebody has been seen
 * here": a single courier's turnaround should not put a second pennant up over
 * a city. The number is the same one the grip arithmetic uses below, so the
 * banner and the outcome cannot disagree. */
#define FLY_CONTEST_MIN 0.12f

int fly_faction_contested(const struct fly_world *w, int loc) {
    float p = 0.0f;
    int c = fly_faction_challenger(w, loc, &p);
    return c != FLY_FACTION_FREE && p >= FLY_CONTEST_MIN;
}

int fly_faction_holdings(const struct fly_world *w, int f) {
    int i, n = 0;
    if (!w) return 0;
    for (i = 0; i < w->nloc; ++i)
        if (w->loc[i].owner == f) ++n;
    return n;
}

const char *fly_faction_grip_name(float grip) {
    return grip < 0.20f ? "slipping" : grip < 0.45f ? "shaky"
         : grip < 0.75f ? "held" : "entrenched";
}

/* How fast the argument moves. One site changing hands every few hours of
 * world is the target: fast enough that a player who flies for an evening sees
 * the map redraw itself, slow enough that a chart is worth reading. */
#define FLY_PUSH_DECAY 0.55f   /* fraction of pressure lost per hour */
#define FLY_GRIP_RATE 0.30f    /* grip per hour at one unit of net pressure */
#define FLY_GRIP_SETTLE 0.09f  /* per hour, back toward what quiet ground sits at */
#define FLY_REL_DRIFT 0.045f   /* per hour, back toward temper */

int fly_faction_step(struct fly_world *w, float dt_hours,
                     fly_faction_event *out, int max) {
    int i, f, a, b, n = 0;
    int held[FLY_FACTION_COUNT];
    float decay;
    if (!w || dt_hours <= 0.0f) return 0;
    decay = 1.0f - FLY_PUSH_DECAY * dt_hours;
    if (decay < 0.0f) decay = 0.0f;
    /* A snapshot, taken before any of this step's seizures. Deliberately: how
     * loosely a power holds its ground this hour is a fact about how much it
     * held when the hour began, and recounting as sites change hands would
     * make the outcome depend on the order the site array happens to be in. */
    for (f = 0; f < FLY_FACTION_COUNT; ++f) held[f] = fly_faction_holdings(w, f);

    for (i = 0; i < w->nloc; ++i) {
        fly_location *L = &w->loc[i];
        float mine = fly_faction_is_power(L->owner) ? L->push[L->owner] : 0.0f;
        float rival = 0.0f;
        int challenger = fly_faction_challenger(w, i, &rival);
        float net;

        /* Two terms, and they say different things. The net pressure is the
         * argument being had right now. The settle term is what happens when
         * nobody is having it: a power left alone entrenches, and ground
         * nobody holds stays loosely held rather than slowly becoming
         * impossible to take — which is what a single baseline for both would
         * have made of the unaligned half of the map.
         *
         * Where a power entrenches *to* falls with how much it already holds,
         * and that term is the only thing standing between this system and a
         * dead map. Pressure is roughly proportional to traffic and traffic
         * follows territory, so the leader compounds: six hours of seed 4242
         * took one power from five sites to nine and the Concord to none, and
         * another evening of that is a world with one flag on it and nothing
         * left to fight over. An empire holding a dozen fields holds each of
         * them at 0.45 rather than 0.85, so the edges of it are always coming
         * loose and somebody is always taking one back. */
        float settle = fly_faction_is_power(L->owner)
                           ? 0.85f - 0.40f * fly_clampf(((float)held[L->owner] - 3.0f) / 8.0f,
                                                        0.0f, 1.0f)
                           : 0.25f;
        net = fly_faction_is_power(L->owner) ? mine - rival : -rival;
        L->grip = fly_clampf(L->grip + net * FLY_GRIP_RATE * dt_hours +
                             (settle - L->grip) * FLY_GRIP_SETTLE * dt_hours,
                             0.0f, 1.0f);

        if (L->grip <= 0.0f && challenger != FLY_FACTION_FREE && rival >= FLY_CONTEST_MIN) {
            int from = L->owner;
            L->owner = (unsigned char)challenger;
            /* Taken, not consolidated. A site changes hands weakly held, which
             * is what makes a front line move back and forth over an evening
             * instead of ratcheting one way. */
            L->grip = 0.22f;
            for (f = 0; f < FLY_FACTION_COUNT; ++f) L->push[f] *= 0.35f;
            if (n < max && out) {
                out[n].kind = FLY_FEVT_SEIZED;
                out[n].a = challenger;
                out[n].b = from;
                out[n].loc = i;
                ++n;
            }
            /* Taking ground off somebody is the single largest thing that
             * happens to a relation, and it is why the map and the diplomacy
             * are one system rather than two: powers fall out *because* of
             * where the front line went. */
            if (fly_faction_is_power(from))
                fly_diplomacy_nudge(&w->dip, challenger, from, -0.16f);
        }
        for (f = 0; f < FLY_FACTION_COUNT; ++f) L->push[f] *= decay;
    }

    /* Relations. Drift home, then let the world argue with it.
     *
     * The one non-local term is the enemy of my enemy: powers that share a war
     * warm to each other, which is what turns seven quarrels into two blocs
     * over a long enough session and is the only reason a seven-power map
     * reads as a war rather than as noise. */
    for (a = 1; a < FLY_FACTION_COUNT; ++a)
        for (b = a + 1; b < FLY_FACTION_COUNT; ++b) {
            float r = w->dip.rel[a][b];
            float temper = fly_faction_temper(a, b);
            int before = fly_diplomacy_state(&w->dip, a, b), after;
            float d = (temper - r) * FLY_REL_DRIFT * dt_hours;
            int c;
            for (c = 1; c < FLY_FACTION_COUNT; ++c) {
                if (c == a || c == b) continue;
                if (fly_diplomacy_state(&w->dip, a, c) == FLY_REL_WAR &&
                    fly_diplomacy_state(&w->dip, b, c) == FLY_REL_WAR)
                    d += 0.020f * dt_hours;
            }
            w->dip.rel[a][b] = w->dip.rel[b][a] = fly_clampf(r + d, -1.0f, 1.0f);
            after = fly_diplomacy_state(&w->dip, a, b);
            if (after == before) continue;
            if (after == FLY_REL_WAR && n < max && out) {
                out[n].kind = FLY_FEVT_WAR; out[n].a = a; out[n].b = b; out[n].loc = -1;
                ++n;
            } else if (after == FLY_REL_PACT && n < max && out) {
                out[n].kind = FLY_FEVT_PACT; out[n].a = a; out[n].b = b; out[n].loc = -1;
                ++n;
            }
        }
    return n;
}
