/* fly_win: realtime hardware-accelerated window backend using vendored TIGR.
 * Compiled only with make.py --window (-DFLY_WINDOW). TIGR gives us an
 * OpenGL-backed window with keyboard, mouse and touch input on
 * Windows/macOS/Linux; everything else (game, UI, rendering) is the same
 * code the headless build runs, so this file is just an event/blit shim. */
#ifdef FLY_WINDOW

#include <string.h>

#include "tigr.h"

#include "fly_app.h"

typedef struct {
    int tk;      /* tigr key */
    fly_key key; /* abstract key */
} fly__keymap;

static const fly__keymap keymap[] = {
    { TK_UP, FLY_KEY_PITCH_UP },     { TK_DOWN, FLY_KEY_PITCH_DOWN },
    { TK_LEFT, FLY_KEY_ROLL_LEFT },  { TK_RIGHT, FLY_KEY_ROLL_RIGHT },
    { 'Q', FLY_KEY_YAW_LEFT },       { 'E', FLY_KEY_YAW_RIGHT },
    { 'W', FLY_KEY_THROTTLE_UP },    { 'S', FLY_KEY_THROTTLE_DOWN },
    { 'B', FLY_KEY_BRAKE },          { 'F', FLY_KEY_FIRE },
    { 'R', FLY_KEY_ROCKET },         { 'C', FLY_KEY_LAUNCH },
    { 'V', FLY_KEY_DECOY },
    { 'Z', FLY_KEY_TRIM_DOWN },      { 'X', FLY_KEY_TRIM_UP },
    { ' ', FLY_KEY_JUMP },           { 'G', FLY_KEY_INTERACT },
    { 'A', FLY_KEY_AUTOPILOT },      { TK_TAB, FLY_KEY_EDIT },
    { 'M', FLY_KEY_MAP },            { TK_ESCAPE, FLY_KEY_BACK },
    { TK_RETURN, FLY_KEY_SELECT },
    { '1', FLY_KEY_VIEW_PLAY },      { '2', FLY_KEY_VIEW_SELF },
    { '3', FLY_KEY_VIEW_HELP },
    { TK_PAGEUP, FLY_KEY_UP },       { TK_PAGEDN, FLY_KEY_DOWN },
    { 'K', FLY_KEY_UP },             { 'J', FLY_KEY_DOWN },
};

int fly_win_run(fly_app *app);

/* ---------------- joypad ----------------
 * Linux: read /dev/input/js0 (kernel joystick API, no extra deps).
 * Axes 0/1 = roll/pitch, 2 = yaw (or twist), 3 = throttle;
 * buttons: 0 fire, 1 brake, 2 autopilot, 3 edit overlay.
 * Other platforms: stub (add a backend here when porting). */
#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <linux/joystick.h>

static int fly__js_fd = -2; /* -2 = not tried yet, -1 = unavailable */

static void fly_win_poll_joypad(fly_app *app) {
    if (fly__js_fd == -2) {
        fly__js_fd = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
        if (fly__js_fd >= 0) fly_game_log(&app->game, "Joypad connected");
    }
    if (fly__js_fd < 0) return;
    struct js_event je;
    while (read(fly__js_fd, &je, sizeof je) == (ssize_t)sizeof je) {
        fly_event ev;
        ev.key = FLY_KEY_NONE;
        ev.x = ev.y = 0;
        if ((je.type & ~JS_EVENT_INIT) == JS_EVENT_AXIS) {
            float v = (float)je.value / 32767.0f;
            ev.type = FLY_EV_AXIS;
            switch (je.number) {
            case 0: ev.axis = FLY_AXIS_ROLL; ev.value = v; break;
            case 1: ev.axis = FLY_AXIS_PITCH; ev.value = v; break; /* stick back = +1 = nose up */
            case 2: ev.axis = FLY_AXIS_YAW; ev.value = v; break;
            case 3: ev.axis = FLY_AXIS_THROTTLE; ev.value = (1.0f - v) * 0.5f; break;
            default: continue;
            }
            fly_app_event(app, &ev);
        } else if ((je.type & ~JS_EVENT_INIT) == JS_EVENT_BUTTON) {
            static const fly_key btns[] = { FLY_KEY_FIRE, FLY_KEY_LAUNCH,
                                            FLY_KEY_DECOY, FLY_KEY_BRAKE,
                                            FLY_KEY_AUTOPILOT, FLY_KEY_EDIT };
            if (je.number >= sizeof btns / sizeof btns[0]) continue;
            ev.type = je.value ? FLY_EV_KEY_DOWN : FLY_EV_KEY_UP;
            ev.key = btns[je.number];
            fly_app_event(app, &ev);
        }
    }
}
#else
static void fly_win_poll_joypad(fly_app *app) { (void)app; }
#endif

int fly_win_run(fly_app *app) {
    int w = app->frame.w, h = app->frame.h;
    Tigr *win = tigrWindow(w, h, "fly99", TIGR_AUTO);
    if (!win) return 1;

    int held[sizeof keymap / sizeof keymap[0]] = { 0 };
    int mouse_was_down = 0, mouse_last_x = -1, mouse_last_y = -1;
    tigrTime();

    while (!tigrClosed(win)) {
        float dt = tigrTime();

        /* Follow the window.
         *
         * TIGR_AUTO resizes the window's bitmap with the window, and the frame
         * is a buffer of its own that the game renders into; blitting one into
         * the other on the *old* dimensions indexes `win->pix` by a stride and
         * a height that are no longer its own, so dragging a corner inwards
         * wrote past the end of it. Resizing the frame to match is not merely
         * the safe version of that, it is the right behaviour twice over: the
         * game gets the resolution the player asked for, and a pointer event —
         * which TIGR reports in bitmap pixels — lands where it was aimed
         * instead of at a scaled offset from it.
         *
         * A minimised window can report a zero dimension, which is not a
         * framebuffer; hold the last good size until it comes back. */
        if (win->w > 0 && win->h > 0 && (win->w != w || win->h != h)) {
            fly_img next;
            if (fly_img_init(&next, win->w, win->h) == 0) {
                fly_img_free(&app->frame);
                app->frame = next;
                w = win->w;
                h = win->h;
            }
        }

        /* keys: edge-detect into events so held state stays in the UI */
        unsigned i;
        for (i = 0; i < sizeof keymap / sizeof keymap[0]; ++i) {
            int down = tigrKeyHeld(win, keymap[i].tk);
            if (down != held[i]) {
                fly_event ev;
                ev.type = down ? FLY_EV_KEY_DOWN : FLY_EV_KEY_UP;
                ev.key = keymap[i].key;
                ev.x = ev.y = 0;
                fly_app_event(app, &ev);
                held[i] = down;
            }
        }

        /* One pointer, whatever the platform calls it: down/up edges plus
         * move-while-held drags (the virtual stick, the throttle slider).
         *
         * The mouse and the touch screen used to be read as two devices feeding
         * the same abstract pointer, and on a desktop they are one. TIGR's touch
         * API is the mouse on every backend that has no touch screen — X11's
         * `tigrTouch` calls `tigrMouse` and reports a contact whenever a button
         * is down, and Android sets `mouseButtons` from its contact count — so a
         * single click arrived at the UI as two FLY_EV_POINTER_DOWNs and its
         * release as two UPs.
         *
         * Doubled input is not a doubled effect on a control that is *held*,
         * which is why the flight stick and the throttle never showed it, and it
         * is exactly a doubled effect on everything that toggles or commits: the
         * two nav icons and the map switched to their page and straight back, so
         * clicking them did nothing at all, and a click on a row of the actions
         * list ran that row's command *twice* — bought twice, refuelled twice,
         * launched twice — while the player saw one click and one line in the
         * log.
         *
         * So the two sources are coalesced into one before any edge is taken.
         * The mouse leads, because on the desktop backends it is the real
         * reading; touch only answers when no button is down, which is what a
         * platform with a genuine touch screen and no mouse reports. */
        {
            int mx, my, buttons, n;
            int px, py, down;
            TigrTouchPoint pts[4];
            fly_event ev;
            tigrMouse(win, &mx, &my, &buttons);
            px = mx;
            py = my;
            down = (buttons & 1) != 0;
            n = tigrTouch(win, pts, 4);
            if (!down && n > 0) {
                px = pts[0].x;
                py = pts[0].y;
                down = 1;
            }
            ev.key = FLY_KEY_NONE;
            /* A release carries the last position the press was at, not
             * wherever the pointer has since been reported: a UP at (0,0) would
             * end a stick drag somewhere the finger never was. */
            ev.x = (float)(down ? px : (mouse_last_x >= 0 ? mouse_last_x : px));
            ev.y = (float)(down ? py : (mouse_last_y >= 0 ? mouse_last_y : py));
            if (down != mouse_was_down) {
                ev.type = down ? FLY_EV_POINTER_DOWN : FLY_EV_POINTER_UP;
                fly_app_event(app, &ev);
                mouse_was_down = down;
            } else if (down && (px != mouse_last_x || py != mouse_last_y)) {
                ev.type = FLY_EV_POINTER_MOVE;
                fly_app_event(app, &ev);
            }
            if (down) {
                mouse_last_x = px;
                mouse_last_y = py;
            }
        }

        fly_win_poll_joypad(app);

        fly_app_step(app, dt);
        fly_app_render(app);

        /* Blit fly_img into tigr's pixel buffer.
         *
         * A fly_img word is 0xAABBGGRR and a TPixel is the bytes r,g,b,a, so on
         * a little-endian machine the two are the same four bytes in the same
         * order and the whole thing is one copy per row rather than a
         * shift-and-store per pixel — at 1080p that unpack was two million
         * iterations of work a memcpy already does. Checked once at runtime
         * rather than assumed, so a big-endian build takes the loop and is
         * still correct. The frame's own alpha is already 255 everywhere —
         * the resolve stamps it and fly_img_blend writes it back on every
         * composite — so copying it is the same byte the loop stored. */
        {
            static int direct = -1;
            /* Whatever the two agree on, in case the resize above could not
             * get the memory it wanted and the frame is still the old size. */
            int bw = w < win->w ? w : win->w;
            int bh = h < win->h ? h : win->h;
            int x, y;
            if (direct < 0) {
                direct = 0;
                if (sizeof(TPixel) == 4) {
                    uint32_t word = 0xFF030201u;
                    TPixel probe;
                    memcpy(&probe, &word, 4);
                    direct = probe.r == 1 && probe.g == 2 && probe.b == 3 &&
                             probe.a == 0xFF;
                }
            }
            if (direct) {
                for (y = 0; y < bh; ++y)
                    memcpy(&win->pix[y * win->w], &app->frame.px[y * w],
                           sizeof(TPixel) * (size_t)bw);
            } else {
                for (y = 0; y < bh; ++y)
                    for (x = 0; x < bw; ++x) {
                        uint32_t c = app->frame.px[y * w + x];
                        TPixel p;
                        p.r = (unsigned char)(c & 0xFF);
                        p.g = (unsigned char)((c >> 8) & 0xFF);
                        p.b = (unsigned char)((c >> 16) & 0xFF);
                        p.a = 255;
                        win->pix[y * win->w + x] = p;
                    }
            }
        }
        tigrUpdate(win);
    }
    tigrFree(win);
    return 0;
}

#endif /* FLY_WINDOW */
