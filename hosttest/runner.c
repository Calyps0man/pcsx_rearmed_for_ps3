#include <signal.h>
#include <unistd.h>
/* Headless libretro front-end for testing the PCSX-ReARMed core off-console.
 * Built big-endian (powerpc64-linux) and run under qemu-ppc64, so the core goes
 * through the same byte-order paths as on the PS3.
 *
 * usage: runner <game.exe|cue|chd> <frames> [shot_frame,...] [key=value ...]
 *   writes shot_<frame>.ppm for each requested frame, prints a frame/audio summary
 *   and a per-frame checksum line every 60 frames (for comparing runs). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "libretro.h"

static unsigned fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static const char *opts_k[64], *opts_v[64];
static int nopts;
static uint32_t last_sum; static unsigned last_w, last_h, frames_seen, dupes;
static long audio_frames; static int16_t audio_peak;
static const void *last_data; static size_t last_pitch;
static char sysdir[512] = ".";

static void logcb(enum retro_log_level l, const char *f, ...)
{
    va_list ap;
    if (l < RETRO_LOG_WARN && !getenv("VERBOSE")) return;
    va_start(ap, f); vfprintf(stderr, f, ap); va_end(ap);
}

static bool env(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = logcb; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: fmt = *(const unsigned *)data; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = sysdir; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        for (int i = 0; i < nopts; i++) if (!strcmp(v->key, opts_k[i])) { v->value = opts_v[i]; return true; }
        v->value = NULL; return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
    default: return false;
    }
}

static uint32_t to_rgb(const void *row, unsigned x)
{
    if (fmt == RETRO_PIXEL_FORMAT_XRGB8888) return ((const uint32_t *)row)[x] & 0xffffff;
    uint16_t c = ((const uint16_t *)row)[x];
    if (fmt == RETRO_PIXEL_FORMAT_RGB565) {
        unsigned r = c >> 11, g = (c >> 5) & 63, b = c & 31;
        return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
    unsigned r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
    return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

static void video(const void *data, unsigned w, unsigned h, size_t pitch)
{
    frames_seen++;
    if (!data) { dupes++; return; }
    uint32_t s = 2166136261u;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) { s ^= to_rgb((const char *)data + y * pitch, x); s *= 16777619u; }
    last_sum = s; last_w = w; last_h = h; last_data = data; last_pitch = pitch;
}

static void shot(const char *name)
{
    FILE *f;
    if (!last_data) return;
    f = fopen(name, "wb");
    fprintf(f, "P6 %u %u 255\n", last_w, last_h);
    for (unsigned y = 0; y < last_h; y++)
        for (unsigned x = 0; x < last_w; x++) {
            uint32_t c = to_rgb((const char *)last_data + y * last_pitch, x);
            unsigned char p[3] = { c >> 16, c >> 8, c };
            fwrite(p, 1, 3, f);
        }
    fclose(f);
}

static size_t audio_batch(const int16_t *d, size_t n)
{
    audio_frames += n;
    for (size_t i = 0; i < n * 2; i++) { int16_t a = d[i] < 0 ? -d[i] : d[i]; if (a > audio_peak) audio_peak = a; }
    return n;
}
static void audio1(int16_t l, int16_t r) { int16_t b[2] = { l, r }; audio_batch(b, 1); }
static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) { return 0; }

/* INJECT=hang.bin: after the first frame, the PS1 memory, scratchpad and CPU
   registers from a console hang dump (psx_debug_dump) replace the running
   ones, and the emulation continues from there; a frame that takes longer than
   FRAME_TIMEOUT seconds is reported as a hang with the CPU state. */
#include "../core/libpcsxcore/r3000a.h"
void psx_debug_state(char *b, int n);
static void on_hang(int sig)
{
    char b[700];
    (void)sig;
    psx_debug_state(b, sizeof b);
    printf("HANG: %s\n", b);
    fflush(stdout);
    _exit(3);
}
static void inject_hang(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d; long n; size_t o = 12; int w, k;
    if (!f) { fprintf(stderr, "no %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) exit(1); fclose(f);
#define LE32(p) ((uint32_t)(p)[0] | (uint32_t)(p)[1] << 8 | (uint32_t)(p)[2] << 16 | (uint32_t)(p)[3] << 24)
    psxRegs.pc = LE32(d + 8);
    {   /* version 1: 2 code sections after pc; version 2: trace pc, count, 3 sections */
        int ver = (int)LE32(d + 4), nsec = ver >= 2 ? 3 : 2;
        o = ver >= 2 ? 20 : 12;
        for (w = 0; w < nsec; w++) { uint32_t size = LE32(d + o + 8); o += 20 + size; }
    }
    for (k = 0; k < 34; k++) psxRegs.GPR.r[k] = LE32(d + o + k * 4);
    o += 136;
    psxRegs.CP0.n.SR = LE32(d + o); psxRegs.CP0.n.Cause = LE32(d + o + 4); psxRegs.CP0.n.EPC = LE32(d + o + 8);
    o += 12;
    memcpy(psxRegs.ptrs.psxM, d + o, 0x200000); o += 0x200000;
    memcpy(psxRegs.ptrs.psxH, d + o, 0x400);
    psxCpu->Notify(R3000ACPU_NOTIFY_AFTER_LOAD, NULL);
    fprintf(stderr, "injected %s: pc %08x sp %08x ra %08x\n", path, psxRegs.pc, psxRegs.GPR.n.sp, psxRegs.GPR.n.ra);
    signal(SIGALRM, on_hang);
    free(d);
}

int main(int argc, char **argv)
{
    struct retro_game_info gi = { 0 };
    struct retro_system_av_info av;
    int frames, shots[32], nshots = 0, i;
    if (argc < 3) { fprintf(stderr, "usage: runner game frames [shots] [k=v...]\n"); return 2; }
    frames = atoi(argv[2]);
    for (i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) { *eq = 0; opts_k[nopts] = argv[i]; opts_v[nopts++] = eq + 1; }
        else for (char *t = strtok(argv[i], ","); t && nshots < 32; t = strtok(NULL, ",")) shots[nshots++] = atoi(t);
    }
    if (getenv("SYSDIR")) snprintf(sysdir, sizeof sysdir, "%s", getenv("SYSDIR"));
    if (getenv("TRACE")) { extern int psx_debug_trace; psx_debug_trace = 1; }
    retro_set_environment(env);
    retro_set_video_refresh(video);
    retro_set_audio_sample(audio1);
    retro_set_audio_sample_batch(audio_batch);
    retro_set_input_poll(poll);
    retro_set_input_state(input);
    retro_init();
    gi.path = argv[1];
    if (!retro_load_game(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
    retro_get_system_av_info(&av);
    if (getenv("HWR_TRACE")) { extern void hwr_trace_start(const char *); hwr_trace_start(getenv("HWR_TRACE")); }
#ifdef SPU_MDEC
    if (getenv("SPU_MDEC")) {   /* FMV decoding through the SPU dispatcher (SPUs run as plain calls) */
        extern int spu_mdec_init(int); extern void spu_mdec_enable(int);
        if (spu_mdec_init(5)) spu_mdec_enable(1);
    }
#endif
#ifdef HWR_GL
    if (getenv("HWR_GL")) { extern void hwr_gl_start(int); hwr_gl_start(atoi(getenv("HWR_GL"))); }
#endif
    for (i = 1; i <= frames; i++) {
        if (getenv("TOGGLE_THREAD")) {   /* switch the GPU thread on/off every 30 frames */
            extern void gpulib_set_thread_rendering(int);
            if (i % 30 == 0) gpulib_set_thread_rendering((i / 30) & 1);
        }
        if (getenv("INJECT")) alarm(atoi(getenv("FRAME_TIMEOUT") ? getenv("FRAME_TIMEOUT") : "60"));
        retro_run();
        if (getenv("INJECT") && i == 1) inject_hang(getenv("INJECT"));
        if (getenv("INJECT") && getenv("RUN_AT") && i == 1) {
            /* compile and run the block at RUN_AT (hex) with the dump's registers */
            psxRegs.pc = (uint32_t)strtoul(getenv("RUN_AT"), NULL, 16);
            psxRegs.CP0.n.SR &= ~1;   /* no interrupt first: run the block itself */
            psxRegs.next_interupt = psxRegs.cycle + 100000;
            psxCpu->Notify(R3000ACPU_NOTIFY_AFTER_LOAD, NULL);
            fprintf(stderr, "running the block at %08x\n", psxRegs.pc);
            alarm(atoi(getenv("FRAME_TIMEOUT") ? getenv("FRAME_TIMEOUT") : "60"));
            psxCpu->ExecuteBlock(&psxRegs, EXEC_CALLER_OTHER);
            fprintf(stderr, "block done: pc now %08x\n", psxRegs.pc);
            exit(0);
        }
        if (getenv("INJECT")) { char b[1400]; psx_debug_state(b, sizeof b); printf("frame %d: %s\n", i, b); fflush(stdout); }
        if (getenv("STATE_AT") && i == atoi(getenv("STATE_AT"))) {   /* save + load a state mid-run */
            size_t sz = retro_serialize_size(); void *b = malloc(sz);
            if (!b || !retro_serialize(b, sz) || (!getenv("STATE_SAVE_ONLY") && !retro_unserialize(b, sz)))
                fprintf(stderr, "state round trip failed\n");
            free(b);
        }
        for (int k = 0; k < nshots; k++) if (shots[k] == i) {
            char n[64]; snprintf(n, sizeof n, "shot_%d.ppm", i); shot(n);
#ifdef HWR_GL
            if (getenv("HWR_GL")) { extern void hwr_gl_dump(const char *); snprintf(n, sizeof n, "gl_%d.ppm", i); hwr_gl_dump(n); }
#endif
            if (getenv("VRAM_DUMP")) {   /* core VRAM (brought up to date from the GPU model) */
                extern void *gpulib_get_vram(void);
                FILE *f;
#ifdef HWR_GL
                if (getenv("HWR_GL")) { extern void hwr_gl_sync_all(void); hwr_gl_sync_all(); }
#endif
                snprintf(n, sizeof n, "vram_%d.bin", i);
                if ((f = fopen(n, "wb"))) { fwrite(gpulib_get_vram(), 2, 1024 * 512, f); fclose(f); }
            }
        }
        if (i % 60 == 0) printf("frame %d: %ux%u sum %08x\n", i, last_w, last_h, last_sum);
    }
    printf("done: %d runs, %u video calls (%u dupes), last %ux%u, audio %ld frames (%.1f s) peak %d, fps %.2f\n",
           frames, frames_seen, dupes, last_w, last_h, audio_frames, audio_frames / 44100.0, audio_peak, av.timing.fps);
    if (getenv("HWR_TRACE")) { extern void hwr_trace_stop(void); hwr_trace_stop(); }
#ifdef HWR_GL
    if (getenv("HWR_GL")) { extern void hwr_gl_stats(void); hwr_gl_stats(); }
#endif
#ifdef SPU_MDEC
    if (getenv("SPU_MDEC")) {
        extern void spu_mdec_stats(unsigned *, unsigned *, unsigned *, double *);
        unsigned a, b, c; double w;
        spu_mdec_stats(&a, &b, &c, &w);
        fprintf(stderr, "spu mdec: %u streams, %u macroblocks, %u misses\n", a, b, c);
    }
#endif
    retro_unload_game();
    retro_deinit();
    return 0;
}
