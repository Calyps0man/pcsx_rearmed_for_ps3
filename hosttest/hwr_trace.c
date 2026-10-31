/* Records what the hardware renderer would send to the RSX while a game runs in
 * the big-endian test runner, plus the software renderer's VRAM at chosen
 * frames, so hwr_replay (x86 + Mesa, same shaders) can compare the two.
 *
 * Trace (big-endian 32-bit integers):
 *   'S' + 1 MiB    VRAM snapshot
 *   'B' nops nv, ops (8 words each), vertices (8 floats as bit patterns, 16 integers)
 *   'F' frame x y w h, then 1 MiB software VRAM      (compared frames only)
 *   'P' x y w h                                       (other presents) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hwr.h"
#include "gpu_hooks.h"

static FILE *tr;
static int frame_no, every = 30;

static void w32(uint32_t v) { unsigned char b[4] = { v >> 24, v >> 16, v >> 8, v }; fwrite(b, 1, 4, tr); }

static int rec_snapshot(hwr_dev *d, const uint16_t *vram) { (void)d; fputc('S', tr); fwrite(vram, 1, 1024 * 512 * 2, tr); return 1; }

static void rec_submit(hwr_dev *d, const hwr_op *ops, int nops, const hwr_vtx *v, int nv)
{
    int i, k;
    (void)d;
    fputc('B', tr); w32(nops); w32(nv);
    for (i = 0; i < nops; i++) {
        w32(ops[i].blend); w32(ops[i].prog);
        w32((uint32_t)(int32_t)ops[i].sx0); w32((uint32_t)(int32_t)ops[i].sy0);
        w32((uint32_t)(int32_t)ops[i].sx1); w32((uint32_t)(int32_t)ops[i].sy1);
        w32(ops[i].first); w32(ops[i].count);
    }
    for (i = 0; i < nv; i++) {
        const uint32_t *f = (const uint32_t *)&v[i]; const int16_t *s = &v[i].tpx;
        for (k = 0; k < 8; k++) w32(f[k]);
        for (k = 0; k < 16; k++) w32((uint32_t)(int32_t)s[k]);
    }
}

static int rec_present(hwr_dev *d, int x, int y, int w, int h)
{
    (void)d;
    frame_no++;
    if (frame_no == 1 || frame_no % every == 0) {
        fputc('F', tr); w32(frame_no); w32(x); w32(y); w32(w); w32(h);
        fwrite(gpulib_get_vram(), 1, 1024 * 512 * 2, tr);
    } else { fputc('P', tr); w32(x); w32(y); w32(w); w32(h); }
    return 0;               /* keep the software picture flowing to the runner too */
}

static hwr_dev rec = { rec_snapshot, rec_submit, rec_present };
static int present_hook(int x, int y, int w, int h, int bgr24) { return hwr_present(x, y, w, h, bgr24); }

/* called by the runner after retro_load_game when HWR_TRACE is set */
void hwr_trace_start(const char *path)
{
    if (getenv("HWR_EVERY")) every = atoi(getenv("HWR_EVERY"));
    tr = fopen(path, "wb");
    if (!tr) return;
    hwr_attach(&rec, gpulib_get_vram(), NULL);
    hwr_sync_ecmds(gpulib_get_ex_regs());
    gpu_frontend_hooks.cmd = hwr_cmd;
    gpu_frontend_hooks.vram_written = hwr_vram_written;
    gpu_frontend_hooks.sync_ecmds = hwr_sync_ecmds;
    gpu_frontend_hooks.present = present_hook;
}

void hwr_trace_stop(void)
{
    if (!tr) return;
    memset(&gpu_frontend_hooks, 0, sizeof gpu_frontend_hooks);
    hwr_detach();
    fprintf(stderr, "hwr: frames %u prims %u snapshots %u (max %u/frame) overflow %u\n",
            hwr_stats.frames, hwr_stats.prims, hwr_stats.snapshots, hwr_stats.max_snaps, hwr_stats.overflow_frames);
    fclose(tr);
    tr = NULL;
}
