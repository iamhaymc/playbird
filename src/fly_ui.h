/* fly_ui: page-like view system with a shared input abstraction.
 * Views: play (world + HUD), self (pilot profile & loadout), help (guide).
 * Overlays on play: edit (in-game actions: contracts, market, upgrades)
 * and map (discovered world chart). Input arrives as abstract events so
 * keyboard, mouse and touch all funnel through the same path. */
#ifndef FLY_UI_H
#define FLY_UI_H

#include "fly_game.h"
#include "fly_hud.h"
#include "fly_img.h"
#include "fly_render.h"

typedef enum {
    FLY_VIEW_SPLASH, /* title screen; the Play button lives here */
    FLY_VIEW_PLAY,
    FLY_VIEW_SELF,
    FLY_VIEW_HELP
} fly_view;

/* splash play button center/radius */
#define FLY_SPLASH_PLAY_Y(h) ((float)(h) * 0.62f)
#define FLY_SPLASH_PLAY_R 30.0f

typedef enum {
    FLY_OVERLAY_NONE,
    FLY_OVERLAY_EDIT,
    FLY_OVERLAY_MAP
} fly_overlay;

/* abstract keys (the platform layer maps real keys/gestures onto these) */
typedef enum {
    FLY_KEY_NONE = 0,
    FLY_KEY_PITCH_UP, FLY_KEY_PITCH_DOWN,
    FLY_KEY_ROLL_LEFT, FLY_KEY_ROLL_RIGHT,
    FLY_KEY_YAW_LEFT, FLY_KEY_YAW_RIGHT,
    FLY_KEY_THROTTLE_UP, FLY_KEY_THROTTLE_DOWN,
    FLY_KEY_BRAKE, FLY_KEY_FIRE, FLY_KEY_ROCKET,
    /* Release what is on the rails, and shed a decoy. Two keys rather than a
     * weapon-select and one, because in the four seconds a seeker gives you
     * there is no time to cycle a selector — and because the two are almost
     * never wanted at the same moment. */
    FLY_KEY_LAUNCH, FLY_KEY_DECOY,
    FLY_KEY_TRIM_UP, FLY_KEY_TRIM_DOWN,
    FLY_KEY_UP, FLY_KEY_DOWN, FLY_KEY_SELECT, FLY_KEY_BACK,
    FLY_KEY_VIEW_PLAY, FLY_KEY_VIEW_SELF, FLY_KEY_VIEW_HELP,
    FLY_KEY_EDIT, FLY_KEY_MAP, FLY_KEY_AUTOPILOT,
    FLY_KEY_JUMP, FLY_KEY_INTERACT,
    FLY_KEY_COUNT
} fly_key;

typedef enum {
    FLY_EV_KEY_DOWN,
    FLY_EV_KEY_UP,
    FLY_EV_POINTER_DOWN, /* mouse click or touch tap; x,y in frame pixels */
    FLY_EV_POINTER_UP,
    FLY_EV_POINTER_MOVE, /* drag; drives the virtual stick / throttle slider */
    FLY_EV_AXIS          /* analog input (joypad); axis + value */
} fly_event_type;

/* analog axes for joypads and other continuous devices.
 * pitch/roll/yaw are -1..1 (pitch +1 = stick pulled back = nose up),
 * throttle is absolute 0..1 */
typedef enum {
    FLY_AXIS_PITCH,
    FLY_AXIS_ROLL,
    FLY_AXIS_YAW,
    FLY_AXIS_THROTTLE,
    FLY_AXIS_LOOK_X,
    FLY_AXIS_LOOK_Y,
    FLY_AXIS_COUNT
} fly_axis;

typedef struct {
    fly_event_type type;
    fly_key key;
    float x, y;
    fly_axis axis;   /* FLY_EV_AXIS only */
    float value;     /* FLY_EV_AXIS only */
} fly_event;

typedef struct {
    fly_view view;
    fly_overlay overlay;
    int sel;         /* highlighted row in the edit overlay */
    int help_page;
    int held[FLY_KEY_COUNT];
    float throttle;  /* sticky throttle setting */
    /* Pitch trim, and it is sticky for the same reason the throttle is: the
     * whole point of trim is that you set it once and take your hand off.
     * It lives here rather than on the craft because it is a thing the pilot
     * does, not a thing the aeroplane has — the AI never touches it, and a
     * replayed command stream carries the setting in the controls it already
     * journals. */
    float trim;
    fly_render_opts ropts;
    /* analog state: joypad axes + on-screen touch/mouse controls */
    float axis[FLY_AXIS_COUNT];
    int axis_throttle_live; /* a device is driving throttle absolutely */
    int stick_active;       /* virtual stick engaged by pointer drag */
    float stick_ox, stick_oy; /* stick anchor, frame pixels */
    float stick_px, stick_py; /* stick pitch/roll output, -1..1 */
    int slider_active;      /* pointer riding the throttle slider */
    int fire_touch;         /* on-screen fire pad held */
    int rocket_touch;       /* on-screen rocket pad held */
    int launch_touch;       /* on-screen ordnance pad held */
    int decoy_touch;        /* on-screen countermeasure pad held */
    int look_active;
    float look_last_x, look_last_y;
    int view_w, view_h;     /* last rendered frame size (hit-testing) */
    int edit_first;         /* first visible row of the edit overlay */
} fly_ui;

void fly_ui_init(fly_ui *ui);
/* feed one input event; may apply commands to the game */
void fly_ui_event(fly_ui *ui, fly_game *g, const fly_event *ev);
/* translate held keys into a CONTROLS command (call once per frame) */
void fly_ui_pump_controls(fly_ui *ui, fly_game *g, float dt);
/* draw the active view into out (out defines the resolution) */
void fly_ui_render(fly_ui *ui, const fly_game *g, fly_img *out);

/* number of rows in the current edit overlay (for input wrap + tests) */
int fly_ui_edit_rows(const fly_ui *ui, const fly_game *g);

/* Where the world chart puts each discovered site's name, in frame pixels, in
 * world-index order. Exposed for the reason fly_hud_layout is: "no two names
 * overprint" is a measurement rather than a promise, and the placement it
 * measures is the one the chart is drawn from. Returns how many were written,
 * up to `max`. */
int fly_ui_chart_labels(int w, int h, const fly_game *g, fly_rectf *out, int max);

#endif /* FLY_UI_H */
