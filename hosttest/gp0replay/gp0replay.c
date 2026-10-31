/* Replays a GP0T trace (written by the PS3 VRAM check) through the core's
 * software GPU, or through hwr_core + the GL model of the RSX backend
 * (HWR_GL=<scale>), and writes the final VRAM as raw little-endian halfwords.
 * usage: gp0replay trace.bin out.vram */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gpu_hooks.h"

long GPUinit(void);
void GPUwriteDataMem(uint32_t *mem, int count);
void GPUwriteStatus(uint32_t data);
void hwr_gl_start(int scale);
void GPUrearmedCallbacks(const void *cbs);
#include <sys/mman.h>
#include <stddef.h>
#include "../../core/frontend/plugin_lib.h"
static void *map_cb(unsigned int size) { void *p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); return p; }
static void unmap_cb(void *p, unsigned int size) { munmap(p, size); }
static void cbs_mmap_patch(uint64_t *b) { struct rearmed_cbs *c = (struct rearmed_cbs *)b; c->mmap = map_cb; c->munmap = unmap_cb;
    { static unsigned fc, hc; c->gpu_frame_count = &fc; c->gpu_hcnt = &hc; } }
void hwr_gl_sync_all(void);
void hwr_gl_stats(void);
void hwr_flush(void);
void hwr_gl_dump(const char *name);

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    long n; uint32_t *w; size_t o; int i, gl = getenv("HWR_GL") ? atoi(getenv("HWR_GL")) : 0;
    uint16_t *vram, last = 0;
    int tpx = -1, tpy = 0; unsigned ncmd = 0, e6 = 0;
    if (!f || argc < 3) return 1;
    fseek(f, 0, SEEK_END); n = ftell(f) / 4; fseek(f, 0, SEEK_SET);
    w = malloc(n * 4); if (fread(w, 4, n, f) != (size_t)n) return 1; fclose(f);
    if (w[0] != 0x54305047) { fprintf(stderr, "not GP0T\n"); return 1; }
    GPUinit();
    {   /* rearmed callbacks: all zero, VRAM through the mmap callback */
        static uint64_t cbs[1024];
        cbs_mmap_patch(cbs);
        GPUrearmedCallbacks(cbs);
    }
    GPUwriteStatus(0x00000000);   /* reset */
    GPUwriteStatus(0x03000000);   /* display on */
    vram = (uint16_t *)gpulib_get_vram();
    memcpy(vram, w + 2, 1024 * 1024);
    o = 2 + 256 * 1024;
    for (i = 1; i <= 6; i++) { uint32_t e = (w[o + i] & 0x00ffffff) | ((0xe0u + i) << 24); GPUwriteDataMem(&e, 1); }
    o += 8;
    if (gl) hwr_gl_start(gl);
    if (getenv("TRACE_PX")) { sscanf(getenv("TRACE_PX"), "%d,%d", &tpx, &tpy); last = vram[tpy * 1024 + tpx]; }
    while (o < (size_t)n) {
        uint32_t t = w[o++];
        if (t == 1) { uint32_t k = w[o++]; GPUwriteDataMem(w + o, (int)k);
            if (tpx >= 0) {   /* TRACE_PX=x,y: every command that changes that pixel */
                uint16_t now;
                if (gl) { hwr_flush(); hwr_gl_sync_all(); }
                now = vram[tpy * 1024 + tpx];
                if (now != last) { printf("cmd %5u %08x (%u words, E6 %x): %04x -> %04x\n", ncmd, w[o], k, e6, last, now); last = now; }
            }
            if ((w[o] >> 24) == 0xe6) e6 = w[o] & 3;
            ncmd++; o += k; }
        else if (t == 2) {
            uint32_t x = w[o], y = w[o + 1], ww = w[o + 2], h = w[o + 3], j; o += 4;
            for (j = 0; j < ww * h; j++) { uint32_t v = w[o + j / 2]; vram[((y + j / ww) & 511) * 1024 + ((x + j % ww) & 1023)] = j & 1 ? v >> 16 : v & 0xffff; }
            o += (ww * h + 1) / 2;
            if (gpu_frontend_hooks.vram_written) gpu_frontend_hooks.vram_written(x, y, ww, h);
        }
        else if (t == 3) { if (gpu_frontend_hooks.present) gpu_frontend_hooks.present(w[o], w[o + 1], w[o + 2], w[o + 3], 0); o += 4; }
        else { fprintf(stderr, "bad tag %u\n", t); return 1; }
    }
    if (gl) { hwr_flush(); hwr_gl_sync_all(); hwr_gl_stats(); if (argc > 3) hwr_gl_dump(argv[3]);
        if (getenv("DUMP_RECT")) { int x, y, ww, h; void hwr_gl_dump_rect(const char *, int, int, int, int);
            sscanf(getenv("DUMP_RECT"), "%d,%d,%d,%d", &x, &y, &ww, &h); hwr_gl_dump_rect("rect.ppm", x, y, ww, h); } }
    f = fopen(argv[2], "wb"); fwrite(vram, 2, 1024 * 512, f); fclose(f);
    return 0;
}
