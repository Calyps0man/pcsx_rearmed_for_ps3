/* Replays a GP0T trace (PS3 VRAM check mode) through hwr_core with a counting
 * backend: how many draw ops, vertices, snapshots the frame produces, and the
 * time hwr_core itself takes.  usage: gp0prof trace.bin [repeat] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include "hwr.h"

static uint16_t vram[1024 * 512];
static unsigned n_submit, n_ops, n_vtx, n_snap, n_present;
static unsigned blend_hist[8], prog_hist[16], ops_per_submit_max;
static unsigned state_changes;   /* consecutive ops that differ in blend/prog/scissor/pass/mset */
static double area_pass[3], area_blend[5], area_prog[8], area_total;   /* native pixels drawn */
static double opa;
static int show_ops;   /* OPS=1 */
static void tri_area(const hwr_op *o, const hwr_vtx *v)
{
    unsigned k;
    for (k = 0; k + 2 < o->count; k += 3) {
        const hwr_vtx *a = &v[o->first + k], *b = a + 1, *c = a + 2;
        double ar = ((b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y)) * 0.5;
        if (ar < 0) ar = -ar;
        area_pass[o->pass] += ar; area_blend[o->blend < 5 ? o->blend : 0] += ar; if (show_ops) opa += ar; area_prog[o->prog & 7] += ar; area_total += ar;
    }
}

static int snap(hwr_dev *d, const uint16_t *v) { (void)d; (void)v; n_snap++; return 1; }
static void sub(hwr_dev *d, const hwr_op *ops, int nops, const hwr_vtx *v, int nv)
{
    static hwr_op last; static int have;
    int i; (void)d;
    n_submit++; n_ops += nops; n_vtx += nv;
    if ((unsigned)nops > ops_per_submit_max) ops_per_submit_max = nops;
    for (i = 0; i < nops; i++) {
        blend_hist[ops[i].blend & 7]++; prog_hist[ops[i].prog & 15]++;
        if (!have || ops[i].blend != last.blend || ops[i].prog != last.prog || ops[i].sx0 != last.sx0 ||
            ops[i].sy0 != last.sy0 || ops[i].sx1 != last.sx1 || ops[i].sy1 != last.sy1 ||
            ops[i].pass != last.pass || ops[i].mset != last.mset) state_changes++;
        last = ops[i]; have = 1;
        opa = 0; tri_area(&ops[i], v);
        if (show_ops) printf("op prog %d blend %d pass %d mset %d tris %u area %.0f scissor %d,%d-%d,%d\n", ops[i].prog, ops[i].blend, ops[i].pass, ops[i].mset, ops[i].count / 3, opa, ops[i].sx0, ops[i].sy0, ops[i].sx1, ops[i].sy1);
    }
}
static int pres(hwr_dev *d, int x, int y, int w, int h) { (void)d; (void)x; (void)y; (void)w; (void)h; n_present++; return 1; }
static void sync_cb(void) {}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    long n; uint32_t *w; size_t i, o; int rep = argc > 2 ? atoi(argv[2]) : 1, r;
    hwr_dev dev = { snap, sub, pres, 1 };
    struct timespec t0, t1;
    unsigned cmds = 0, cmd_words = 0, uploads = 0;
    if (!f) return 1;
    show_ops = getenv("OPS") != NULL;
    fseek(f, 0, SEEK_END); n = ftell(f) / 4; fseek(f, 0, SEEK_SET);
    w = malloc(n * 4); if (fread(w, 4, n, f) != (size_t)n) return 1; fclose(f);
    if (w[0] != 0x54305047) { fprintf(stderr, "not GP0T\n"); return 1; }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (r = 0; r < rep; r++) {
        memcpy(vram, w + 2, sizeof vram);
        n_submit = n_ops = n_vtx = n_snap = n_present = 0; cmds = cmd_words = uploads = 0;
        memset(blend_hist, 0, sizeof blend_hist); memset(prog_hist, 0, sizeof prog_hist); state_changes = 0;
        memset(area_pass, 0, sizeof area_pass); memset(area_blend, 0, sizeof area_blend); memset(area_prog, 0, sizeof area_prog); area_total = 0;
        hwr_attach(&dev, vram, sync_cb);
        hwr_sync_ecmds(w + 2 + 256 * 1024);
        o = 2 + 256 * 1024 + 8;
        while (o < (size_t)n) {
            uint32_t t = w[o++];
            if (t == 1) { uint32_t k = w[o++]; hwr_cmd(w + o, w + o + k); cmds++; cmd_words += k; o += k; }
            else if (t == 2) { uint32_t x = w[o], y = w[o+1], ww = w[o+2], h = w[o+3]; o += 4;
                for (i = 0; i < (size_t)ww * h; i++) { uint32_t v = w[o + i / 2]; vram[((y + i / ww) & 511) * 1024 + ((x + i % ww) & 1023)] = i & 1 ? v >> 16 : v & 0xffff; }
                o += ((size_t)ww * h + 1) / 2; hwr_vram_written(x, y, ww, h); uploads++; }
            else if (t == 3) { hwr_present(w[o], w[o+1], w[o+2], w[o+3], 0); o += 4; }
            else { fprintf(stderr, "bad tag %u at %zu\n", t, o - 1); break; }
        }
        hwr_flush();
        hwr_detach();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("hwr_cmd calls %u (%u words), uploads %u\n", cmds, cmd_words, uploads);
    printf("submits %u, ops %u (max %u per submit), vertices %u, snapshots %u, presents %u, state changes %u\n",
           n_submit, n_ops, ops_per_submit_max, n_vtx, n_snap, n_present, state_changes);
    printf("blend:"); for (i = 0; i < 5; i++) printf(" %u", blend_hist[i]);
    printf("  prog:"); for (i = 0; i < 8; i++) printf(" %u", prog_hist[i]); printf("\n");
    printf("native pixels drawn (last repeat): total %.0f; pass0 %.0f pass1 %.0f pass2 %.0f; blend none %.0f avg %.0f add %.0f sub %.0f addq %.0f\n",
           area_total, area_pass[0], area_pass[1], area_pass[2], area_blend[0], area_blend[1], area_blend[2], area_blend[3], area_blend[4]);
    printf("by program: flat %.0f 4bpp %.0f 8bpp %.0f 15bpp %.0f copy %.0f 4w %.0f 8w %.0f 15w %.0f\n",
           area_prog[0], area_prog[1], area_prog[2], area_prog[3], area_prog[4], area_prog[5], area_prog[6], area_prog[7]);
    printf("hwr_core time per frame: %.3f ms (host)\n", ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / rep);
    return 0;
}
