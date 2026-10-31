/* Runs the front-end's menu code on the host: video/audio/pad replaced by stubs,
 * pad input scripted from argv, overlay dumped to a PPM on request. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "common.h"
#include "core_hooks.h"

static uint32_t ovl[OVL_W * OVL_H];
static const char *script; static int frame;
static uint32_t pressed_now, prev_buttons;
static pad_state_t pads[2];

volatile int app_exit_requested;
void app_poll_system(void) {}
#include <stdarg.h>
void log_open(void) {}
volatile const char *app_where;
void watchdog_frame(void) {}
void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fprintf(stdout, "LOG: "); vfprintf(stdout, fmt, ap); va_end(ap); }
/* video stubs */
uint32_t *video_overlay(void) { return ovl; }
void video_overlay_clear(void) { memset(ovl, 0, sizeof ovl); }
void video_overlay_clear_rows(int a, int b) { memset(ovl + a * OVL_W, 0, (b - a) * OVL_W * 4); }
unsigned video_overlay_generation(void) { return 0; }
void video_begin(void) {}
void video_clear(uint32_t c) { (void)c; }
void video_draw_overlay(void) {}
void video_borders_dirty(void) {}
void video_draw_overlay_rows(int a, int b) { (void)a; (void)b; }
static void dump(const char *n) { FILE *f = fopen(n, "wb"); int i; fprintf(f, "P6 %d %d 255\n", OVL_W, OVL_H);
    for (i = 0; i < OVL_W * OVL_H; i++) { unsigned char p[3] = { ovl[i] >> 16, ovl[i] >> 8, ovl[i] }; fwrite(p, 1, 3, f); } fclose(f); }
void video_present(void)
{
    frame++;
    if (frame % 5 == 0) {                      /* one scripted step every 5 frames */
        char c = *script ? *script++ : 'q';
        uint32_t b = 0;
        switch (c) {
        case 'U': b = PAD_UP; break;   case 'D': b = PAD_DOWN; break;
        case 'L': b = PAD_LEFT; break; case 'R': b = PAD_RIGHT; break;
        case 'X': b = PAD_CROSS; break; case 'O': b = PAD_CIRCLE; break;
        case 'T': b = PAD_TRIANGLE; break; case 'S': b = PAD_SQUARE; break;
        case 'p': { char n[32]; snprintf(n, sizeof n, "shot_%03d.ppm", frame); dump(n); fprintf(stderr, "shot %s\n", n); break; }
        case 'q': fprintf(stderr, "script done at frame %d\n", frame); app_exit_requested = 1; break;
        }
        pads[0].buttons = b;
    } else pads[0].buttons = 0;
}
/* input stubs */
void input_init(void) {}
void input_rumble(int p, int l, unsigned s) { (void)p; (void)l; (void)s; }
void input_rumble_stop(void) {}
void input_poll(void) { pressed_now = pads[0].buttons & ~prev_buttons; prev_buttons = pads[0].buttons; }
const pad_state_t *input_pad(int p) { return &pads[p]; }
uint32_t input_pressed(void) { return pressed_now; }
const char *input_names(uint32_t b, char *out, size_t n) { snprintf(out, n, "%#x", b); return out; }
/* hooks */
void hooks_redraw_last_frame(void) {}
const char *hooks_bios_status(void) { return getenv("BIOS") ? "scph1001.bin found" : "none (HLE)"; }
int hooks_bios_present(void) { return getenv("BIOS") != NULL; }
int game_save_state(int slot) { return slot != 3; } int game_load_state(int slot) { return slot == 1; }
const char *game_state_info(int slot, char *o, size_t n) { snprintf(o, n, slot == 1 ? "28 Oct 19:40" : "empty"); return o; }
int video_overlay_border(void) { return 160; }
void video_redraw_quick(int on) { (void)on; }
void hooks_gpu_scale(int *sx, float *sy) { *sx = 0; *sy = 0; }
void hwr_shown_size(int *w, int *h) { *w = getenv("SHOWN") ? 368 : 0; *h = getenv("SHOWN") ? 480 : 0; }
int game_dump_vram(void) { return 1; }
void log_game_begin(const char *n) { (void)n; } void log_game_end(void) {}
void game_reset(void) {} void game_settings_changed(void) {}

static void on_alarm(int s) { (void)s; fprintf(stderr, "HANG: still running at frame %d (script left: '%s')\n", frame, script); _exit(3); }

int main(int argc, char **argv)
{
    const char *g;
    script = argc > 1 ? argv[1] : "";
    signal(SIGALRM, on_alarm); alarm(10);
    config_load();
    if (getenv("INGAME")) { config_game_begin("Test Game"); fprintf(stderr, "menu_ingame returned %d\n", menu_ingame()); config_save(); config_game_end(); return 0; }
    g = menu_run();
    fprintf(stderr, "menu_run returned %s, frames %d\n", g ? g : "NULL", frame);
    config_save();
    return 0;
}
/* disc.c: DISC=<label> pretends a disc is in the drive */
int disc_needs_cfw;
int disc_probe(char *label, size_t n) { const char *d = getenv("DISC"); if (getenv("HEN")) { disc_needs_cfw = 1; return 0; } if (!d) return 0; snprintf(label, n, "%s", d); return 1; }
/* disc.c identification: DISC=<label> means a disc, DISCTITLE=<its name> */
void disc_identify_start(void) {}
void disc_identify_wait(void) {}
int disc_identify_result(char *t, size_t n) { const char *d = getenv("DISC"), *tt = getenv("DISCTITLE");
    if (getenv("DISCBUSY")) return -1; if (!d) return 0; if (t) snprintf(t, n, "%s", tt ? tt : ""); return 1; }
/* disc.c copy: COPYFAIL=1 fails at the start, otherwise a 600 MB disc copied 2 MB per step */
struct disc_copy { unsigned done; };
static struct disc_copy fake_copy;
struct disc_copy *disc_copy_begin(const char *dir, const char *name, char *err, size_t n)
{ (void)dir; if (getenv("COPYFAIL")) { snprintf(err, n, "Already on the HDD: %s.cue", name); return NULL; } fake_copy.done = 0; return &fake_copy; }
int disc_copy_step(struct disc_copy *c, unsigned ms, uint32_t *done, uint32_t *total)
{ (void)ms; c->done += 900; if (c->done > 267000) c->done = 267000; *done = c->done; *total = 267000; return c->done < 267000; }
const char *disc_copy_error(struct disc_copy *c) { (void)c; return "x"; }
const char *disc_copy_cue(struct disc_copy *c) { (void)c; return "usrdir/roms/x.cue"; }
void disc_copy_end(struct disc_copy *c) { (void)c; }
