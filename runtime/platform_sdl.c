/* SDL2 frontend: window/presentation, keyboard + game controller input (XInput/Xbox pads on
 * Windows, evdev on Linux), audio output and frame pacing.  Also implements the headless
 * test mode (scripted input + screenshots) used for automated verification. */
#include "psx.h"
#include "gpu.h"
#include <SDL.h>
#ifndef _WIN32
#include "sdl_dyn.h"
#endif
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

uint16_t g_pad_buttons[2];
bool g_pad_connected[2];   /* [0]: always (keyboard); [1]: a second controller is plugged in */

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_AudioDeviceID audio;
static int audio_ok;
static uint32_t *frame_px;
static int fullscreen;
static uint64_t perf_next;
static int shot_request;

/* slots 0/1 = players 1/2 (ports 1/2); further controllers wait for a free player slot */
#define MAX_PADS 4
static SDL_GameController *pads[MAX_PADS];

/* ------------------------------------------------------------------ input bindings */
/* PS buttons in a fixed order, then the inputs that are not buttons; each can be bound to
 * several keys and controller inputs. */
enum { NUM_BUTTONS = 16, B_LSTICK = 16 /* up down left right */, B_ANALOG = 20, NUM_BINDS = 21 };
static const struct { const char *name; uint16_t bit; } ps_buttons[NUM_BINDS] = {
    { "up", PAD_UP }, { "down", PAD_DOWN }, { "left", PAD_LEFT }, { "right", PAD_RIGHT },
    { "cross", PAD_CROSS }, { "circle", PAD_CIRCLE }, { "square", PAD_SQUARE }, { "triangle", PAD_TRIANGLE },
    { "l1", PAD_L1 }, { "r1", PAD_R1 }, { "l2", PAD_L2 }, { "r2", PAD_R2 },
    { "start", PAD_START }, { "select", PAD_SELECT }, { "l3", PAD_L3 }, { "r3", PAD_R3 },
    { "lstick_up", 0 }, { "lstick_down", 0 }, { "lstick_left", 0 }, { "lstick_right", 0 },   /* full left stick */
    { "analog", 0 },                                                                     /* ANALOG button */
};
#define MAX_BIND 6
static char bind_keys[NUM_BINDS][128] = {
    "Up", "Down", "Left", "Right",
    "X,Space,K", "Z,L", "S,J", "A,I",
    "Q", "W", "1", "2",
    "Return", "Right Shift,Backspace",
};
/* SDL game controller names: a b x y back start leftshoulder rightshoulder dpup ...;
 * "lefttrigger"/"righttrigger" are analog triggers.  Player 2 (pad2_*) uses the same layout. */
#define PAD_DEFAULTS {                                          \
    "dpup", "dpdown", "dpleft", "dpright",                      \
    "a", "b", "x", "y",                                         \
    "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", \
    "start", "back", "leftstick", "rightstick",                 \
}
static char bind_pad[2][NUM_BINDS][128] = { PAD_DEFAULTS, PAD_DEFAULTS };
static SDL_Scancode key_codes[NUM_BINDS][MAX_BIND];
static int pad_codes[2][NUM_BINDS][MAX_BIND];   /* SDL_GameControllerButton, or 1000+axis for triggers */
static int stick_as_dpad = 1;
static int rumble = 1;
static int deadzone = 15;                       /* analog sticks, percent of full deflection */

bool platform_set_binding(const char *key, const char *val) {
    if (!strcmp(key, "stick_as_dpad")) { stick_as_dpad = atoi(val); return true; }
    if (!strcmp(key, "rumble")) { rumble = atoi(val); return true; }
    if (!strcmp(key, "deadzone")) { int v = atoi(val); deadzone = v < 0 ? 0 : v > 90 ? 90 : v; return true; }
    if (!strcmp(key, "controller")) {
        if (!strcmp(val, "dualshock")) g_dualshock[0].type = g_dualshock[1].type = PAD_TYPE_DUALSHOCK;
        else if (!strcmp(val, "digital")) g_dualshock[0].type = g_dualshock[1].type = PAD_TYPE_DIGITAL;
        else LOGW("unknown controller type '%s' (dualshock or digital)", val);
        return true;
    }
    char (*table)[128];
    const char *name;
    if (!strncmp(key, "key_", 4)) { table = bind_keys; name = key + 4; }
    else if (!strncmp(key, "pad_", 4)) { table = bind_pad[0]; name = key + 4; }
    else if (!strncmp(key, "pad2_", 5)) { table = bind_pad[1]; name = key + 5; }
    else return false;
    for (int i = 0; i < NUM_BINDS; i++) {
        if (!strcmp(name, ps_buttons[i].name)) {
            snprintf(table[i], 128, "%s", val);
            return true;
        }
    }
    return false;
}

static void resolve_pad_binding(int *codes, const char *spec, const char *what) {
    char buf[128], *save = NULL;
    snprintf(buf, sizeof buf, "%s", spec);
    int n = 0;
    for (char *t = strtok_r(buf, ",", &save); t && n < MAX_BIND; t = strtok_r(NULL, ",", &save)) {
        while (*t == ' ') t++;
        if (!*t) continue;
        if (!strcmp(t, "lefttrigger")) { codes[n++] = 1000 + SDL_CONTROLLER_AXIS_TRIGGERLEFT; continue; }
        if (!strcmp(t, "righttrigger")) { codes[n++] = 1000 + SDL_CONTROLLER_AXIS_TRIGGERRIGHT; continue; }
        SDL_GameControllerButton gb = SDL_GameControllerGetButtonFromString(t);
        if (gb == SDL_CONTROLLER_BUTTON_INVALID) LOGW("unknown controller button '%s' for %s", t, what);
        else codes[n++] = gb;
    }
}

static void resolve_bindings(void) {
    for (int i = 0; i < NUM_BINDS; i++) {
        for (int k = 0; k < MAX_BIND; k++) {
            key_codes[i][k] = SDL_SCANCODE_UNKNOWN;
            pad_codes[0][i][k] = pad_codes[1][i][k] = -1;
        }
        char buf[128], *save = NULL;
        snprintf(buf, sizeof buf, "%s", bind_keys[i]);
        int n = 0;
        for (char *t = strtok_r(buf, ",", &save); t && n < MAX_BIND; t = strtok_r(NULL, ",", &save)) {
            while (*t == ' ') t++;
            if (!*t) continue;
            SDL_Scancode sc = SDL_GetScancodeFromName(t);
            if (sc == SDL_SCANCODE_UNKNOWN) LOGW("unknown key name '%s' for %s", t, ps_buttons[i].name);
            else key_codes[i][n++] = sc;
        }
        resolve_pad_binding(pad_codes[0][i], bind_pad[0][i], ps_buttons[i].name);
        resolve_pad_binding(pad_codes[1][i], bind_pad[1][i], ps_buttons[i].name);
    }
}

/* ------------------------------------------------------------------ input script (testing) */
typedef struct { int frame, dur, end, period; uint16_t btn; int port; } ScriptEv;
static ScriptEv script[1024];
static int nscript;
static bool script_pad2;     /* the script uses the second controller: plug it in (headless) */

static uint16_t parse_buttons(const char *s) {
    static const struct { const char *n; uint16_t b; } names[] = {
        { "UP", PAD_UP }, { "DOWN", PAD_DOWN }, { "LEFT", PAD_LEFT }, { "RIGHT", PAD_RIGHT },
        { "CROSS", PAD_CROSS }, { "X", PAD_CROSS }, { "CIRCLE", PAD_CIRCLE }, { "O", PAD_CIRCLE },
        { "SQUARE", PAD_SQUARE }, { "TRIANGLE", PAD_TRIANGLE }, { "L1", PAD_L1 }, { "R1", PAD_R1 },
        { "L2", PAD_L2 }, { "R2", PAD_R2 }, { "START", PAD_START }, { "SELECT", PAD_SELECT },
        { "L3", PAD_L3 }, { "R3", PAD_R3 },
    };
    uint16_t b = 0;
    char tok[32];
    while (*s) {
        int n = 0;
        while (*s && *s != '+' && n < 31) tok[n++] = (char)toupper((unsigned char)*s++);
        tok[n] = 0;
        if (*s == '+') s++;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(tok, names[i].n)) b |= names[i].b;
    }
    return b;
}

static void parse_script(const char *s) {
    /* "frame:BUTTONS[:dur],..." or repeating "start-end/period:BUTTONS[:dur]" */
    static char buf[65536];
    snprintf(buf, sizeof buf, "%s", s);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && nscript < 1024; tok = strtok_r(NULL, ",", &save)) {
        ScriptEv e = { 0, 6, 0, 0, 0 };
        char *c1 = strchr(tok, ':');
        if (!c1) continue;
        *c1 = 0;
        char *dash = strchr(tok, '-'), *slash = strchr(tok, '/');
        e.frame = atoi(tok);
        if (dash && slash) { e.end = atoi(dash + 1); e.period = atoi(slash + 1); }
        char *c2 = strchr(c1 + 1, ':');
        if (c2) { *c2 = 0; e.dur = atoi(c2 + 1); }
        if (!strncmp(c1 + 1, "P2+", 3) || !strncmp(c1 + 1, "p2+", 3)) e.port = 1;   /* second controller */
        e.btn = parse_buttons(c1 + 1 + (e.port ? 3 : 0));
        if (e.port) script_pad2 = true;
        script[nscript++] = e;
    }
}

static uint16_t script_buttons(int frame);
/* testing: pseudo-random button presses that still let the game make progress */
static uint16_t fuzz_buttons(int frame) {
    static uint32_t rng;
    static uint16_t cur;
    static int hold;
    if (!g_cfg.fuzz_seed || frame < g_cfg.fuzz_start || (g_cfg.fuzz_end && frame >= g_cfg.fuzz_end)) return 0;
    if (!rng) rng = (uint32_t)g_cfg.fuzz_seed * 2654435761u + 1;
    if (--hold > 0) return cur;
    rng = rng * 1664525u + 1013904223u;
    uint32_t r = rng >> 8;
    static const uint16_t dirs[4] = { PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT };
    static const uint16_t acts[8] = { PAD_CROSS, PAD_CROSS, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1 };
    cur = 0;
    switch (r % 10) {
    case 0: case 1: case 2: case 3: cur = dirs[(r >> 4) & 3]; hold = 10 + (int)((r >> 8) % 50); break;
    case 4: case 5: case 6: cur = acts[(r >> 4) & 7]; hold = 3; break;
    case 7: cur = dirs[(r >> 4) & 3] | PAD_CROSS; hold = 8; break;
    case 8: cur = ((r >> 4) % 23 == 0) ? PAD_START : (((r >> 4) % 31 == 0) ? PAD_SELECT : 0); hold = 4; break;
    default: cur = 0; hold = 10; break;
    }
    return cur;
}

static uint16_t script_buttons_port(int frame, int port) {
    uint16_t b = port == 0 ? fuzz_buttons(frame) : 0;
    for (int i = 0; i < nscript; i++) {
        const ScriptEv *e = &script[i];
        if (e->port != port) continue;
        if (e->period > 0) {
            if (frame >= e->frame && frame <= e->end && (frame - e->frame) % e->period < e->dur) b |= e->btn;
        } else if (frame >= e->frame && frame < e->frame + e->dur) {
            b |= e->btn;
        }
    }
    return b;
}

static uint16_t script_buttons(int frame) { return script_buttons_port(frame, 0); }

/* ------------------------------------------------------------------ screenshots */
static void write_bmp(const char *path, const uint32_t *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t rowb = (uint32_t)w * 3, pad = (4 - rowb % 4) % 4, img = (rowb + pad) * (uint32_t)h;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t fsz = 54 + img;
    memcpy(hdr + 2, &fsz, 4);
    uint32_t v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    int32_t ww = w, hh = h;
    memcpy(hdr + 18, &ww, 4);
    memcpy(hdr + 22, &hh, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t row[1024 * 3 + 4] = { 0 };
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint32_t p = px[y * w + x];
            row[x * 3 + 0] = (uint8_t)p;
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)(p >> 16);
        }
        fwrite(row, 1, rowb + pad, f);
    }
    fclose(f);
}

static void save_screenshot(const uint32_t *px, int w, int h, uint64_t frame) {
    char path[1400];
#ifdef _WIN32
    _mkdir(g_cfg.screenshot_dir);
#else
    mkdir(g_cfg.screenshot_dir, 0755);
#endif
    snprintf(path, sizeof path, "%s/frame_%06llu.bmp", g_cfg.screenshot_dir, (unsigned long long)frame);
    write_bmp(path, px, w, h);
    LOGI("screenshot %s (%dx%d)", path, w, h);
}

/* ------------------------------------------------------------------ init */
static void open_pad(int index) {
    if (!SDL_IsGameController(index)) return;
    SDL_GameController *c = SDL_GameControllerOpen(index);
    if (!c) return;
    /* SDL also reports the controllers present at startup as added: opening one again
     * returns the same handle with one more reference */
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] == c) { SDL_GameControllerClose(c); return; }
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i]) {
            pads[i] = c;
            LOGI("controller connected: %s (%s)", SDL_GameControllerName(c),
                 i == 0 ? "player 1" : i == 1 ? "player 2" : "spare");
            return;
        }
    }
    SDL_GameControllerClose(c);
}

static void close_pad(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++)
        if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == id) {
            SDL_GameControllerClose(pads[i]);
            pads[i] = NULL;
            LOGI("controller disconnected (%s)", i == 0 ? "player 1" : i == 1 ? "player 2" : "spare");
        }
    /* a spare controller takes over a free player slot */
    for (int i = 0; i < 2; i++)
        for (int j = 2; j < MAX_PADS && !pads[i]; j++)
            if (pads[j]) { pads[i] = pads[j]; pads[j] = NULL; LOGI("spare controller is now player %d", i + 1); }
}

/* Open the default output device.  Skipped when the system has no output device at all
 * (WASAPI would otherwise stall for seconds); a device plugged in later is picked up via
 * SDL_AUDIODEVICEADDED. */
static void open_audio(void) {
    if (audio_ok) return;
    if (SDL_GetNumAudioDevices(0) <= 0) {
        LOGW("no audio output device found - running without sound until one is connected");
        return;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio) {
        audio_ok = 1;
        SDL_PauseAudioDevice(audio, 0);
        LOGI("audio: %s", SDL_GetCurrentAudioDriver());
    } else {
        LOGW("cannot open audio device: %s", SDL_GetError());
    }
}

int platform_init(void) {
    frame_px = calloc(1024 * 512, sizeof(uint32_t));
    if (g_cfg.input_script[0]) parse_script(g_cfg.input_script);
    if (g_cfg.headless) return 0;
#ifndef _WIN32
    const char *err = sdl_dyn_load();
    if (err) { fprintf(stderr, "%s\n", err); return 1; }
#endif
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    resolve_bindings();
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");          /* pacing is done by the emulated video clock */
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    int sc = g_cfg.scale > 0 ? g_cfg.scale : 3;
    win = SDL_CreateWindow("Yu-Gi-Oh! Forbidden Memories", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           320 * sc, 240 * sc, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_cfg.smooth ? "1" : "0");
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 1024, 512);
    if (g_cfg.fullscreen) { SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP); fullscreen = 1; }

    open_audio();

    for (int i = 0; i < SDL_NumJoysticks(); i++) open_pad(i);
    perf_next = SDL_GetPerformanceCounter();
    LOGI("platform init took %.0f ms", (double)(perf_next - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency());
    return 0;
}

void platform_shutdown(void) {
    if (g_cfg.headless) return;
    if (audio) SDL_CloseAudioDevice(audio);
    SDL_Quit();
}

void platform_error_box(const char *title, const char *msg) {
    if (g_cfg.headless || !win) return;
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, win);
}

static FILE *wav_file;
static uint32_t wav_frames;

static void wav_finish(void) {
    if (!wav_file) return;
    uint32_t data = wav_frames * 4, riff = 36 + data;
    fseek(wav_file, 4, SEEK_SET); fwrite(&riff, 4, 1, wav_file);
    fseek(wav_file, 40, SEEK_SET); fwrite(&data, 4, 1, wav_file);
    fclose(wav_file);
    wav_file = NULL;
}

void platform_audio_push(const int16_t *stereo, int frames) {
    if (g_cfg.wav_path[0]) {
        if (!wav_file) {
            wav_file = fopen(g_cfg.wav_path, "wb");
            if (wav_file) {
                static const uint8_t hdr[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                    0x44,0xAC,0,0,0x10,0xB1,2,0,4,0,16,0,'d','a','t','a',0,0,0,0 };
                fwrite(hdr, 1, 44, wav_file);
                atexit(wav_finish);
            }
        }
        if (wav_file) { fwrite(stereo, 4, (size_t)frames, wav_file); wav_frames += (uint32_t)frames; }
    }
    if (audio_ok && !g_cfg.headless && !g_cfg.unthrottled) SDL_QueueAudio(audio, stereo, (Uint32)frames * 4);
}

/* ------------------------------------------------------------------ per-frame */
/* binding i of player pl held on the keyboard (ks, player 1 only) or on the controller c */
static bool binding_down(int pl, int i, SDL_GameController *c, const Uint8 *ks) {
    if (ks)
        for (int k = 0; k < MAX_BIND && key_codes[i][k] != SDL_SCANCODE_UNKNOWN; k++)
            if (ks[key_codes[i][k]]) return true;
    if (c)
        for (int k = 0; k < MAX_BIND && pad_codes[pl][i][k] >= 0; k++) {
            int code = pad_codes[pl][i][k];
            if (code >= 1000 ? SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)(code - 1000)) > 16000
                             : SDL_GameControllerGetButton(c, (SDL_GameControllerButton)code) != 0)
                return true;
        }
    return false;
}

/* SDL stick position -> DualShock bytes (00h..FFh, centre 80h, down = FFh): radial dead zone,
 * rescaled so full deflection is still reached, limited to the circle like the real sticks */
static void stick_bytes(int x, int y, uint8_t *ox, uint8_t *oy) {
    double fx = x / 32767.0, fy = y / 32767.0, m = sqrt(fx * fx + fy * fy), dz = deadzone / 100.0;
    double scale = m <= dz ? 0.0 : (m - dz) / (1.0 - dz) / m;
    if (m * scale > 1.0) scale = 1.0 / m;
    fx *= scale;
    fy *= scale;
    *ox = (uint8_t)(fx < 0 ? 128.0 + fx * 128.0 + 0.5 : 128.0 + fx * 127.0 + 0.5);
    *oy = (uint8_t)(fy < 0 ? 128.0 + fy * 128.0 + 0.5 : 128.0 + fy * 127.0 + 0.5);
}

/* one player's buttons (active high); also sets the DualShock stick bytes for that port */
static uint16_t read_player(int pl, const Uint8 *ks) {
    static bool analog_held[2];
    SDL_GameController *c = pads[pl];
    DualShock *d = &g_dualshock[pl];
    uint16_t b = 0;
    for (int i = 0; i < NUM_BUTTONS; i++)
        if (binding_down(pl, i, c, ks)) b |= ps_buttons[i].bit;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    if (c) {
        lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
        ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
        ry = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    }
    /* lstick_* bindings push the left stick fully */
    int kx = binding_down(pl, B_LSTICK + 3, c, ks) - binding_down(pl, B_LSTICK + 2, c, ks);
    int ky = binding_down(pl, B_LSTICK + 1, c, ks) - binding_down(pl, B_LSTICK + 0, c, ks);
    if (kx) lx = kx * 32767;
    if (ky) ly = ky * 32767;
    /* in analog mode the left stick is only the analog stick, or the game would read it twice */
    if (stick_as_dpad && !d->analog) {
        const int dz = 16000;
        if (lx < -dz) b |= PAD_LEFT;
        if (lx > dz) b |= PAD_RIGHT;
        if (ly < -dz) b |= PAD_UP;
        if (ly > dz) b |= PAD_DOWN;
    }
    stick_bytes(rx, ry, &d->axis[0], &d->axis[1]);
    stick_bytes(lx, ly, &d->axis[2], &d->axis[3]);
    bool ab = binding_down(pl, B_ANALOG, c, ks);
    if (ab && !analog_held[pl]) pad_analog_button(pl);
    analog_held[pl] = ab;
    return b;
}

/* DualShock motors -> controller rumble: large motor (00h..FFh) = low-frequency motor,
 * small motor (on/off) = high-frequency motor.  Refreshed every frame with a short duration,
 * so it also stops by itself when the emulation stalls. */
static void update_rumble(int pl) {
    static uint32_t last_polls[2];
    static int idle[2];
    static bool active[2];
    SDL_GameController *c = pads[pl];
    const DualShock *d = &g_dualshock[pl];
    /* a pad the game no longer polls would be reset by the DualShock's watchdog after ~1 s */
    if (d->polls != last_polls[pl]) { last_polls[pl] = d->polls; idle[pl] = 0; }
    else if (idle[pl] < 60) idle[pl]++;
    Uint16 lo = 0, hi = 0;
    if (rumble && idle[pl] < 60 && g_pad_connected[pl]) {
        lo = (Uint16)(d->motor_large * 257);
        hi = d->motor_small ? 0xFFFF : 0;
    }
    if (!c) { active[pl] = false; return; }
    if (lo || hi) SDL_GameControllerRumble(c, lo, hi, 100);
    else if (active[pl]) SDL_GameControllerRumble(c, 0, 0, 0);
    active[pl] = lo || hi;
}

static double t_upload, t_present;
static int t_n;

static void present(int w, int h) {
    uint64_t f = SDL_GetPerformanceFrequency(), t0 = SDL_GetPerformanceCounter();
    int ww, wh;
    SDL_GetRendererOutputSize(ren, &ww, &wh);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (w > 0 && h > 0) {
        SDL_Rect src = { 0, 0, w, h };
        SDL_UpdateTexture(tex, &src, frame_px, w * 4);
        double aspect = g_cfg.aspect_4_3 ? 4.0 / 3.0 : (double)w / h;
        int dw = ww, dh = (int)(ww / aspect);
        if (dh > wh) { dh = wh; dw = (int)(wh * aspect); }
        SDL_Rect dst = { (ww - dw) / 2, (wh - dh) / 2, dw, dh };
        SDL_RenderCopy(ren, tex, &src, &dst);
    }
    uint64_t t1 = SDL_GetPerformanceCounter();
    SDL_RenderPresent(ren);
    uint64_t t2 = SDL_GetPerformanceCounter();
    t_upload += (double)(t1 - t0) * 1000.0 / (double)f;
    t_present += (double)(t2 - t1) * 1000.0 / (double)f;
    if (++t_n == 120) {
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(ren, &ri);
        static uint64_t last;
        uint64_t nowt = SDL_GetPerformanceCounter();
        double fps = last ? 120.0 * (double)f / (double)(nowt - last) : 0;
        last = nowt;
        LOGD("present: renderer=%s upload %.2f ms, present %.2f ms (avg), %.2f fps", ri.name, t_upload / t_n, t_present / t_n, fps);
        t_upload = t_present = 0;
        t_n = 0;
    }
}

static void dump_ram(uint64_t frame) {
    if (g_cfg.trace_from && (uint64_t)g_cfg.trace_from == frame) {
        g_trace_calls = 2;
        LOGI("call tracing enabled at frame %llu", (unsigned long long)frame);
    }
    for (int k = 0; k < 8; k++) {
        if (g_cfg.dump_frame[k] && (uint64_t)g_cfg.dump_frame[k] == frame) {
            char path[1400];
#ifdef _WIN32
            _mkdir(g_cfg.screenshot_dir);
#else
            mkdir(g_cfg.screenshot_dir, 0755);
#endif
            snprintf(path, sizeof path, "%s/ram_%06llu.bin", g_cfg.screenshot_dir, (unsigned long long)frame);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(g_ram, 1, sizeof g_ram, f); fwrite(g_scratch, 1, sizeof g_scratch, f); fclose(f); }
            LOGI("RAM dump %s", path);
            /* full VRAM as a 1024x512 image */
            static uint32_t vpx[1024 * 512];
            const uint16_t *vr = gpu_vram();
            for (int i = 0; i < 1024 * 512; i++) {
                uint16_t c = vr[i];
                uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
                vpx[i] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
            }
            snprintf(path, sizeof path, "%s/vram_%06llu.bmp", g_cfg.screenshot_dir, (unsigned long long)frame);
            write_bmp(path, vpx, 1024, 512);
        }
    }
}

/* Wait until the next emulated frame is due (59.29 Hz NTSC).  When an audio device is
 * present the queue depth is used as the clock instead, so audio never drifts. */
static int fast_forward;

static void throttle(void) {
    if (g_cfg.unthrottled) return;
    if (fast_forward) { perf_next = SDL_GetPerformanceCounter(); if (audio_ok) SDL_ClearQueuedAudio(audio); return; }
    uint64_t freq = SDL_GetPerformanceFrequency();
    double period = (double)freq / 59.2898;          /* NTSC PS1 frame rate */
    if (audio_ok) {
        /* keep ~3-5 frames of audio queued: nudge the frame period by up to 1% so the
           emulated clock follows the sound card's clock without audible gaps */
        double queued = (double)SDL_GetQueuedAudioSize(audio) / 4.0 / 735.0;   /* in frames */
        if (queued > 5.0) period *= 1.0 + 0.002 * (queued - 5.0 > 5.0 ? 5.0 : queued - 5.0);
        else if (queued < 3.0) period *= 1.0 - 0.003 * (3.0 - queued);
    }
    perf_next += (uint64_t)period;
    uint64_t now = SDL_GetPerformanceCounter();
    if (now > perf_next + (uint64_t)period * 4 || perf_next > now + (uint64_t)period * 4) perf_next = now;   /* resync after a stall */
    for (;;) {
        now = SDL_GetPerformanceCounter();
        if (now >= perf_next) break;
        uint64_t us = (perf_next - now) * 1000000u / freq;
        if (us > 2000) SDL_Delay((Uint32)(us / 1000 - 1));   /* coarse sleep, then spin the rest */
    }
}

void platform_frame(void) {
    uint64_t frame = frame_count();
    dump_ram(frame);
    int w = 0, h = 0;
    bool need_pixels = !g_cfg.headless || (g_cfg.screenshot_every && frame % (uint64_t)g_cfg.screenshot_every == 0) || shot_request;
    if (need_pixels) gpu_render_display(frame_px, &w, &h);
    if (g_cfg.screenshot_every && frame % (uint64_t)g_cfg.screenshot_every == 0) save_screenshot(frame_px, w, h, frame);

    if (g_cfg.headless) {
        g_pad_buttons[0] = script_buttons((int)frame);
        g_pad_buttons[1] = script_buttons_port((int)frame, 1);
        g_pad_connected[1] = script_pad2;
        if (g_cfg.max_frames && frame >= (uint64_t)g_cfg.max_frames) g_quit = 1;
        return;
    }

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT: g_quit = 1; break;
        case SDL_CONTROLLERDEVICEADDED: open_pad(ev.cdevice.which); break;
        case SDL_AUDIODEVICEADDED: if (!ev.adevice.iscapture) open_audio(); break;
        case SDL_CONTROLLERDEVICEREMOVED: close_pad(ev.cdevice.which); break;
        case SDL_KEYDOWN:
            if (ev.key.repeat) break;
            if (ev.key.keysym.scancode == SDL_SCANCODE_F11 ||
                (ev.key.keysym.scancode == SDL_SCANCODE_RETURN && (ev.key.keysym.mod & KMOD_ALT))) {
                fullscreen = !fullscreen;
                SDL_SetWindowFullscreen(win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            } else if (ev.key.keysym.scancode == SDL_SCANCODE_F12) {
                shot_request = 1;
            } else if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE && fullscreen) {
                fullscreen = 0;
                SDL_SetWindowFullscreen(win, 0);
            }
            break;
        }
    }
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    fast_forward = ks[SDL_SCANCODE_TAB] && g_cfg.allow_fast_forward;
    /* player 1: keyboard + first controller; player 2: second controller */
    uint16_t b = read_player(0, ks);
    if (!(SDL_GetModState() & KMOD_ALT)) g_pad_buttons[0] = b | script_buttons((int)frame);
    g_pad_buttons[1] = read_player(1, NULL);
    g_pad_connected[1] = pads[1] != NULL;
    update_rumble(0);
    update_rumble(1);
    if (shot_request) { save_screenshot(frame_px, w, h, frame); shot_request = 0; }
    present(w, h);
    throttle();
    if (g_cfg.max_frames && frame >= (uint64_t)g_cfg.max_frames) g_quit = 1;
}
