/* Plain-text log in the app folder; each line is flushed so the last one
 * survives a crash. Capped so a chatty game cannot fill the disk.
 * Only with the Debug log setting on: then log.txt (app and menus, replaced on
 * each start) and logs/<game>.txt (replaced on each run of the game) are written;
 * switched on during a session, they start from there. Lines from before the
 * settings are read are kept in memory until then.
 * A watchdog thread logs what the main thread was doing when frames stop. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/thread.h>
#include <sys/systime.h>
#include <sys/mutex.h>
#include <rsx/rsx.h>
#include <sys/stat.h>
#include "common.h"

#define LOG_CAP (4 * 1024 * 1024)       /* bytes per file and run */
static FILE *lf, *main_lf, *game_lf;   /* lf: where lines go now */
static long w_main, w_game;
static int ready, on, main_made, game_made, in_game;
static char game_path[600];
static char early[16384];              /* lines before log_ready() */
static size_t early_n;
static sys_mutex_t lock;
volatile const char *app_where = "start";
volatile const char *app_sub;
extern const char *volatile psx_state_step;   /* core: SaveState/LoadState step */
static volatile unsigned frames;

void log_open(void)
{
    sys_mutex_attr_t a = { 0 };
    a.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
    a.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    sysMutexCreate(&lock, &a);
}

/* (lock held) the files open exactly while Debug log is on; log.txt only once a
   line outside a game is written */
static void sync_files(void)
{
    int want = ready && cfg.debug_log;
    if (want != on) {
        on = want;
        if (!on) {
            if (game_lf) { fclose(game_lf); game_lf = NULL; }
            if (main_lf) { fclose(main_lf); main_lf = NULL; }
            lf = NULL;
            return;
        }
        if (in_game && (game_lf = fopen(game_path, game_made ? "a" : "w"))) game_made = 1;
    }
    if (on && !in_game && !main_lf && (main_lf = fopen(APP_DIR "/log.txt", main_made ? "a" : "w"))) main_made = 1;
    lf = in_game && game_lf ? game_lf : main_lf;
}

/* the settings are read: from here the files follow the Debug log setting */
void log_ready(void)
{
    sysMutexLock(lock, 0);
    ready = 1;
    sync_files();
    if (lf && early_n) { w_main += (long)fwrite(early, 1, early_n, lf); fflush(lf); }
    early_n = 0;
    sysMutexUnlock(lock);
}

/* lines from here to log_game_end() go to logs/<name>.txt (overwritten per run) */
void log_game_begin(const char *name)   /* after the game's own settings are read */
{
    sysMutexLock(lock, 0);
    snprintf(game_path, sizeof game_path, APP_DIR "/logs/%s.txt", name);
    in_game = 1; game_made = 0; w_game = 0;
    sync_files();                      /* (opens the game log if it switches logging on) */
    if (on && !game_lf && (game_lf = fopen(game_path, "w"))) game_made = 1;
    lf = game_lf ? game_lf : main_lf;
    sysMutexUnlock(lock);
}

void log_game_end(void)
{
    sysMutexLock(lock, 0);
    if (game_lf) fclose(game_lf);
    game_lf = NULL;
    in_game = 0;
    lf = main_lf;
    sysMutexUnlock(lock);
}

void logf_(const char *fmt, ...)
{
    va_list ap;
    long *w;
    if (ready && !cfg.debug_log && !on) return;     /* logging off: nothing to do */
    sysMutexLock(lock, 0);
    if (!ready) {                                   /* before the settings: in memory */
        if (early_n + 600 < sizeof early) {
            int n = snprintf(early + early_n, sizeof early - early_n, "[%6.2f] ", sysGetSystemTime() / 1e6);
            if (n > 0) early_n += (size_t)n;
            va_start(ap, fmt);
            n = vsnprintf(early + early_n, sizeof early - early_n, fmt, ap);
            va_end(ap);
            if (n > 0) early_n += (size_t)n < sizeof early - early_n ? (size_t)n : sizeof early - 1 - early_n;
        }
        sysMutexUnlock(lock);
        return;
    }
    sync_files();
    w = lf == game_lf ? &w_game : &w_main;
    if (!lf || *w > LOG_CAP) { sysMutexUnlock(lock); return; }
    va_start(ap, fmt);
    *w += fprintf(lf, "[%6.2f] ", sysGetSystemTime() / 1e6);
    *w += vfprintf(lf, fmt, ap);
    va_end(ap);
    fflush(lf);
    sysMutexUnlock(lock);
}

/* pushes the log to the disk itself: lines written just before a hang that needs
   a forced power-off would otherwise be lost */
void log_sync(void)
{
    if (!lf) return;
    sysMutexLock(lock, 0);
    fflush(lf);
    fsync(fileno(lf));
    sysMutexUnlock(lock);
}

void watchdog_frame(void) { frames++; }

volatile int app_in_game;
volatile unsigned app_cb_counts[4];
void psx_debug_state(char *b, int n);     /* core, libpcsxcore/misc.c */

static void watchdog(void *arg)
{
    unsigned last = 0, still = 0;
    (void)arg;
    for (;;) {
        sleep(1);
        if (frames != last) { last = frames; still = 0; continue; }
        if (++still == 3 || still % 10 == 0) {
            gcmControlRegister *cr = gcmGetControlRegister();
            logf_("watchdog: no frame for %u s, main thread at: %s%s%s%s%s\n", still, (const char *)app_where,
                  app_sub ? " / " : "", app_sub ? (const char *)app_sub : "",
                  psx_state_step ? " / core " : "", psx_state_step ? psx_state_step : "");
            if (rsx_ctx && cr)
                logf_("watchdog: RSX put %x get %x ref %x; command buffer %x..%x, writing at %x\n",
                      (unsigned)cr->put, (unsigned)cr->get, (unsigned)cr->ref,
                      (unsigned)(u32)rsx_ctx->begin, (unsigned)(u32)rsx_ctx->end,
                      (unsigned)(u32)rsx_ctx->current);
            if (app_in_game && cfg.debug_log) {
                /* the emulated machine: two reports 7-10 s apart show whether the
                   PS1 CPU still runs (game waiting in a loop) or nothing moves
                   (stuck on the host side) */
                static char b[1400];
                psx_debug_state(b, sizeof b);
                logf_("watchdog: core %s\n", b);
                logf_("watchdog: core callbacks so far: video %u, audio %u, input %u, environment %u\n",
                      app_cb_counts[0], app_cb_counts[1], app_cb_counts[2], app_cb_counts[3]);
                if (still == 3) {          /* once: memory and the code being run, for a PC */
                    extern const char *app_game_name(void);
                    extern int psx_debug_dump(void *fp);
                    static char p[700];
                    FILE *df;
                    int ok = 0;
                    mkdir(APP_DIR "/dumps", 0777);
                    snprintf(p, sizeof p, APP_DIR "/dumps/%s_hang.bin", app_game_name());
                    /* flushed to the disk before the report says so: after a hang the
                       console gets switched off, and unflushed data is lost */
                    if ((df = fopen(p, "wb"))) {
                        ok = psx_debug_dump(df);
                        ok = fflush(df) == 0 && fsync(fileno(df)) == 0 && ok;
                        ok = fclose(df) == 0 && ok;
                    }
                    logf_("watchdog: hang dump %s: %s\n", p, ok ? "written" : "FAILED");
                }
            }
            log_sync();
        }
    }
}

void watchdog_start(void)
{
    sys_ppu_thread_t t;
    sysThreadCreate(&t, watchdog, NULL, 1500, 0x4000, 0, (char *)"watchdog");
}
