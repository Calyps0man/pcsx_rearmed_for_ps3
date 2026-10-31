#include <unistd.h>
/* Host model of the RSX backend's full mode (ps3/src/hwr/hwr_rsx.c), on Mesa:
 * the GPU draws everything, the software renderer nothing.
 *
 *  - native VRAM: an RGBA8 framebuffer laid out like the RSX's X1R5G5B5 surface:
 *    R holds PS1 blue, G green, B red (the programs write them swapped), A is
 *    bit 15 (the programs output it as alpha; the RSX stores it with two surface
 *    formats and the alpha test). Read back as 5-bit values.
 *  - textures are sampled from a big-endian byte image of the native VRAM, the
 *    same layout the RSX samples; it is refreshed at every barrier.
 *  - CPU VRAM accesses go through the same vram_read / vram_written hooks.
 * Built natively (little-endian) with the core, so it checks the renderer's
 * logic, the core hooks and the shaders together.
 *
 * Runner integration: HWR_GL=<scale> starts it after loading; at a shot frame
 * hwr_gl_dump() writes the displayed area from the upscaled target. */
#define GL_GLEXT_PROTOTYPES
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hwr.h"
#include "gpu_hooks.h"

static int S;
static GLuint progs[2][HWR_NPROG];      /* [native pass]: vertex program hwr.vert / hwr_nat.vert */
static GLuint tex_vram, tex16, tex8, fbo[2], col[2];   /* [0] target, [1] native */
/* HWR_GL_RSX15=1: like an RSX without the blend-keep fix-up (fix_blend 0): blended
   draws store bit 15 = 0, and blended ops that set it get the backend's self copy
   (barrier, program HWR_PROG_SELF, depth EQUAL against per-triangle depth) */
static int rsx15, mask_used = -1;
static GLuint ndepth;
static unsigned char nat_be[1024 * 512 * 2];
static int px, py, pw, ph, presented;
static unsigned barriers, syncs_in, syncs_out;

static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb"); long n; char *b;
    if (!f) { fprintf(stderr, "missing %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) exit(1);
    b[n] = 0; fclose(f);
    return b;
}

static GLuint shader(GLenum t, const char *path)
{
    char *src = slurp(path), log[4096]; GLint ok; GLuint s = glCreateShader(t);
    glShaderSource(s, 1, (const char **)&src, NULL); glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "%s: %s\n", path, log); exit(1); }
    free(src);
    return s;
}

static void bind_fbo(int native)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo[native]);
    glViewport(0, 0, 1024 * (native ? 1 : S), 512 * (native ? 1 : S));
}

/* native framebuffer -> big-endian halfword image -> texture the programs sample */
static void refresh_vram_texture(void)
{
    static unsigned char px4[1024 * 512 * 4];
    int i;
    bind_fbo(1);
    glReadPixels(0, 0, 1024, 512, GL_RGBA, GL_UNSIGNED_BYTE, px4);
    /* GL rows run bottom-up: VRAM row y is GL row 511 - y */
    for (i = 0; i < 1024 * 512; i++) {
        const unsigned char *q = px4 + ((511 - i / 1024) * 1024 + i % 1024) * 4;
        unsigned b = (q[0] * 31 + 127) / 255, g = (q[1] * 31 + 127) / 255, r = (q[2] * 31 + 127) / 255;
        unsigned v = (q[3] >= 128 ? 0x8000 : 0) | b << 10 | g << 5 | r;
        nat_be[i * 2] = v >> 8; nat_be[i * 2 + 1] = v & 0xff;
    }
    glBindTexture(GL_TEXTURE_2D, tex_vram);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 512, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, nat_be);
    {   /* the fast programs' views (as on the RSX): halfwords as A1R5G5B5 (red = bits
           10-14), bytes as an 8-bit texture in memory order */
        static uint16_t h[1024 * 512];
        for (i = 0; i < 1024 * 512; i++) h[i] = (uint16_t)(nat_be[i * 2] << 8 | nat_be[i * 2 + 1]);
        glBindTexture(GL_TEXTURE_2D, tex16);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 512, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, h);
        glBindTexture(GL_TEXTURE_2D, tex8);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2048, 512, GL_LUMINANCE, GL_UNSIGNED_BYTE, nat_be);
    }
}

static int gl_snapshot(hwr_dev *d, const uint16_t *v) { (void)d; (void)v; barriers++; refresh_vram_texture(); return 1; }


/* drawn-block map, same logic as hwr_rsx.c */
static unsigned char drawn[16][16];
static void mark_drawn(int x0, int y0, int x1, int y1, int v)
{
    int bx, by;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 1024) x1 = 1024;
    if (y1 > 512) y1 = 512;
    if (x1 <= x0 || y1 <= y0) return;
    if (!v) {
        x0 = (x0 + 63) & ~63; y0 = (y0 + 31) & ~31; x1 &= ~63; y1 &= ~31;
        if (x1 <= x0 || y1 <= y0) return;
    }
    for (by = y0 >> 5; by <= (y1 - 1) >> 5; by++)
        for (bx = x0 >> 6; bx <= (x1 - 1) >> 6; bx++) drawn[by][bx] = (unsigned char)v;
}
static void split_rect(int x, int y, int w, int h, void (*f)(int, int, int, int))
{
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    x &= 1023; y &= 511;
    if (x + w > 1024) { split_rect(x, y, 1024 - x, h, f); split_rect(0, y, x + w - 1024, h, f); return; }
    if (y + h > 512) { f(x, y, w, 512 - y); f(x, 0, w, y + h - 512); return; }
    f(x, y, w, h);
}
static void undraw(int x, int y, int w, int h) { mark_drawn(x, y, x + w, y + h, 0); }

/* as hwr_rsx.c stp_flags (HWR_GL_NOSTPSKIP=1: all ops keep their STP draws) */
static unsigned char op_stp[4096];
static void stp_flags(const hwr_op *ops, int nops, const hwr_vtx *v)
{
    const uint16_t *core = (const uint16_t *)gpulib_get_vram();
    int i, off = getenv("HWR_GL_NOSTPSKIP") != NULL;
    for (i = 0; i < nops && i < 4096; i++) {
        const hwr_op *o = &ops[i];
        int b = HWR_PROG_BASE(o->prog), s = 1;
        if (!off && !o->mset && o->pass == 0 && (b == 1 || b == 2 || b == 5 || b == 6)) {
            unsigned k;
            s = 0;
            for (k = 0; k + 2 < o->count && !s; k += 3) {
                const hwr_vtx *a = &v[o->first + k];
                int bx, x0, x1, by;
                if (a->mode != 1 && a->mode != 2) { s = 1; break; }
                by = (a->cy & 511) >> 5; x0 = a->cx & 1023; x1 = x0 + (a->mode == 1 ? 16 : 256) - 1;
                for (bx = x0 >> 6; bx <= x1 >> 6 && !s; bx++) if (drawn[by][bx & 15]) s = 1;
                if (!s) s = hwr_clut_stp(core, a->mode, a->cx, a->cy);
            }
        }
        op_stp[i] = (unsigned char)s;
        if (o->prog != 4 && o->bx1 > o->bx0 && o->by1 > o->by0)
            mark_drawn(o->bx0, o->by0, o->bx1, o->by1, 1);
    }
}
static int drawn_bbox(int *x, int *y, int *w, int *h)
{
    int bx, by, X0 = 99, Y0 = 99, X1 = -1, Y1 = -1;
    for (by = *y >> 5; by <= (*y + *h - 1) >> 5; by++)
        for (bx = *x >> 6; bx <= (*x + *w - 1) >> 6; bx++)
            if (drawn[by][bx]) {
                if (bx < X0) X0 = bx;
                if (bx > X1) X1 = bx;
                if (by < Y0) Y0 = by;
                if (by > Y1) Y1 = by;
            }
    if (X1 < 0) return 0;
    {
        int x0 = X0 * 64 > *x ? X0 * 64 : *x, y0 = Y0 * 32 > *y ? Y0 * 32 : *y;
        int x1 = (X1 + 1) * 64 < *x + *w ? (X1 + 1) * 64 : *x + *w, y1 = (Y1 + 1) * 32 < *y + *h ? (Y1 + 1) * 32 : *y + *h;
        *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    }
    return 1;
}
static void sync_rect(int x, int y, int w, int h, int to_native);
static void read_part(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || !drawn_bbox(&x, &y, &w, &h)) return;
    sync_rect(x, y, w, h, 0);
    mark_drawn(x, y, x + w, y + h, 0);
}

static hwr_dev dev;
static int op_self(const hwr_op *o)      /* hwr_rsx.c op_bit15() == 2 with fix_blend 0 */
{
    int b = HWR_PROG_BASE(o->prog);
    if (o->prog >= HWR_PROG_SELF || b == 4 || o->blend == HWR_BLEND_NONE) return 0;
    if (!o->mset && (b == 0 || o->pass == 1)) return 0;
    if (getenv("HWR_GL_NOSELF")) return 0;
    return o->mset || mask_used;
}

/* the collected stencil-only draws (STP texels of opaque textured ops), as hwr_rsx.c */
static int pend[4096], npend;
static void flush_stp(const hwr_op *ops, int native)
{
    int k;
    glColorMask(0, 0, 0, 0); glDepthFunc(GL_EQUAL); glDepthMask(GL_FALSE);
    glStencilFunc(GL_ALWAYS, 1, 1);
    glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GEQUAL, 0.5f);   /* the texel's bit 15 */
    for (k = 0; k < npend; k++) {
        const hwr_op *o = &ops[pend[k]];
        int p = o->prog >= HWR_PROG_MCHK && o->prog < HWR_PROG_SELF ? HWR_PROG_BASE(o->prog) : o->prog, s = native ? 1 : S;
        glUseProgram(progs[native][p]);
        glScissor(o->sx0 * s, (512 - o->sy1) * s, (o->sx1 - o->sx0) * s, (o->sy1 - o->sy0) * s);
        glDrawArrays(GL_TRIANGLES, o->first, o->count);
    }
    npend = 0;
    glDisable(GL_ALPHA_TEST);
    glColorMask(1, 1, 1, 1); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE);
}

static void draw_ops(const hwr_op *ops, int nops, int native)
{
    int i, s = native ? 1 : S, self = 0, um = mask_used > 0;
    npend = 0;
    bind_fbo(native);
    glEnable(GL_SCISSOR_TEST);
    if (um || (native && rsx15)) {     /* per-triangle depth (cleared per submit here) */
        glDisable(GL_SCISSOR_TEST); glClearDepth(0.0); glClear(GL_DEPTH_BUFFER_BIT); glEnable(GL_SCISSOR_TEST);
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE);
    }
    if (um) {   /* each surface's mask plane, as hwr_rsx.c keeps it in the stencil */
        glEnable(GL_STENCIL_TEST); glStencilMask(0xff); glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        dev.mask_stencil = 1;
    }
    for (i = 0; i < nops; i++) {
        const hwr_op *o = &ops[i];
        int p = o->prog, chk = p >= HWR_PROG_MCHK && p < HWR_PROG_SELF, v = 0;
        if (native && o->prog == 4 && !um) continue;
        if (um && chk) p = HWR_PROG_BASE(p);
        if (um && chk && npend) flush_stp(ops, native);   /* before this op's program and scissor */
        glUseProgram(progs[native][p]);
        glScissor(o->sx0 * s, (512 - o->sy1) * s, (o->sx1 - o->sx0) * s, (o->sy1 - o->sy0) * s);
        /* alpha: source one, destination zero (bit 15 from the program) */
        switch (o->blend) {
        case HWR_BLEND_NONE: glDisable(GL_BLEND); break;
        case HWR_BLEND_AVG: glEnable(GL_BLEND); glBlendColor(.5f, .5f, .5f, .5f); glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR, GL_ONE, GL_ZERO); break;
        case HWR_BLEND_ADD: glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO); break;
        case HWR_BLEND_SUB: glEnable(GL_BLEND); glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO); break;
        case HWR_BLEND_ADDQ: glEnable(GL_BLEND); glBlendColor(.25f, .25f, .25f, .25f); glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE, GL_ONE, GL_ZERO); break;
        }
        if (native && rsx15 && o->blend != HWR_BLEND_NONE) {   /* the RSX writes X = 0 while blending */
            GLint e; glGetIntegerv(GL_BLEND_EQUATION_RGB, &e); glBlendEquationSeparate(e, GL_FUNC_ADD);
            { GLint sr, dr; glGetIntegerv(GL_BLEND_SRC_RGB, &sr); glGetIntegerv(GL_BLEND_DST_RGB, &dr); glBlendFuncSeparate(sr, dr, GL_ZERO, GL_ZERO); }
        }
        if (um) {
            v = o->mset || o->pass == 2;
            glStencilFunc(!chk ? GL_ALWAYS : v ? GL_NOTEQUAL : GL_EQUAL, v, 1);
        }
        if (native && p == 4) glColorMask(0, 0, 0, 0);   /* uploads: only the stencil */
        glDrawArrays(GL_TRIANGLES, o->first, o->count);
        if (native && p == 4) glColorMask(1, 1, 1, 1);
        if (um && !v && o->pass == 0 && HWR_PROG_BASE(p) != 0 && op_stp[i]) pend[npend++] = i;   /* as hwr_rsx.c */
        if (native && rsx15) self |= op_self(o);
    }
    if (npend) flush_stp(ops, native);
    glDisable(GL_BLEND);
    if (um) { glDisable(GL_STENCIL_TEST); if (!native || !rsx15) glDisable(GL_DEPTH_TEST); }
    if (self) {                        /* the self copy, as hwr_rsx.c draws it */
        refresh_vram_texture();        /* the barrier */
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tex16);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, tex8);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex_vram);
        bind_fbo(1);
        glUseProgram(progs[1][HWR_PROG_SELF]);
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_EQUAL); glDepthMask(GL_FALSE);
        for (i = 0; i < nops; i++) {
            const hwr_op *o = &ops[i];
            if (!op_self(o)) continue;
            glScissor(o->sx0, 512 - o->sy1, o->sx1 - o->sx0, o->sy1 - o->sy0);
            glDrawArrays(GL_TRIANGLES, o->first, o->count);
        }
    }
    if (native && rsx15) { glDisable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); }
}

static void gl_submit(hwr_dev *d, const hwr_op *ops, int nops, const hwr_vtx *v, int nv)
{
    int i;
    (void)d;
    if (!nops || !nv) return;
    if (rsx15 || mask_used > 0) {   /* per-triangle depth, as the RSX backend writes it (vertices copied) */
        static hwr_vtx *cp; static int cap; int k;
        if (nv > cap) { cap = nv; cp = realloc(cp, (size_t)cap * sizeof *cp); }
        memcpy(cp, v, (size_t)nv * sizeof *v);
        for (k = 0; k + 2 < nv; k += 3) cp[k].z = cp[k + 1].z = cp[k + 2].z = (float)(k / 3 + 1) / 4194304.0f;
        v = cp;
        for (i = 0; i < nops; i++) if (ops[i].prog >= HWR_PROG_MCHK && ops[i].prog < HWR_PROG_SELF) mask_used = 1;
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, sizeof *v, &v->x);
    for (i = 0; i < 5; i++) {   /* see hwr.h: TEXCOORD0 floats, 1..4 16-bit integers */
        glClientActiveTexture(GL_TEXTURE0 + i);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        if (i == 0) glTexCoordPointer(4, GL_FLOAT, sizeof *v, &v->r);
        else glTexCoordPointer(4, GL_SHORT, sizeof *v, &v->tpx + 4 * (i - 1));
    }
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, tex16);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, tex8);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_vram);
    stp_flags(ops, nops, v);
    draw_ops(ops, nops, 0);
    draw_ops(ops, nops, 1);
    for (i = 0; i < nops; i++) {
        const hwr_op *o = &ops[i];
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        int k;
        if (o->prog == 4) continue;
        for (k = 0; k < (int)o->count; k++) {
            const hwr_vtx *q = &v[o->first + k];
            if (q->x < x0) x0 = q->x;
            if (q->x > x1) x1 = q->x;
            if (q->y < y0) y0 = q->y;
            if (q->y > y1) y1 = q->y;
        }
        if (x1 < x0) continue;
        mark_drawn((int)x0 > o->sx0 ? (int)x0 : o->sx0, (int)y0 > o->sy0 ? (int)y0 : o->sy0,
                   (int)x1 + 1 < o->sx1 ? (int)x1 + 1 : o->sx1, (int)y1 + 1 < o->sy1 ? (int)y1 + 1 : o->sy1, 1);
    }
    glFinish();          /* the arrays live in the core's buffers */
}

static int gl_present(hwr_dev *d, int x, int y, int w, int h)
{
    (void)d;
    px = x; py = y; pw = w; ph = h; presented++;
    return 1;
}

static hwr_dev dev = { gl_snapshot, gl_submit, gl_present, 1, 0 };

/* core VRAM (little-endian halfwords) <-> native framebuffer, rect may wrap */
static void sync_rect(int x, int y, int w, int h, int to_native)
{
    uint16_t *core = (uint16_t *)gpulib_get_vram();
    static unsigned char row[1024 * 4];
    int r, i, ux, uy, uw, uh, up = !to_native && gpulib_pending_write(&ux, &uy, &uw, &uh);
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    x &= 1023; y &= 511;
    if (x + w > 1024) { x = 0; w = 1024; }
    bind_fbo(1);
    glDisable(GL_SCISSOR_TEST);
    for (r = 0; r < h; r++) {
        int yy = (y + r) & 511;
        if (to_native) {
            for (i = 0; i < w; i++) {
                unsigned v = core[yy * 1024 + x + i], R5 = v & 31, G5 = (v >> 5) & 31, B5 = (v >> 10) & 31;
                row[i * 4] = B5 << 3 | B5 >> 2; row[i * 4 + 1] = G5 << 3 | G5 >> 2; row[i * 4 + 2] = R5 << 3 | R5 >> 2;
                row[i * 4 + 3] = (v & 0x8000) ? 255 : 0;
            }
            glBindTexture(GL_TEXTURE_2D, col[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, 511 - yy, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
        } else {
            glReadPixels(x, 511 - yy, w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
            for (i = 0; i < w; i++) {
                const unsigned char *q = row + i * 4;
                unsigned b = (q[0] * 31 + 127) / 255, g = (q[1] * 31 + 127) / 255, rr = (q[2] * 31 + 127) / 255;
                if (up && ((yy - uy) & 511) < uh && ((x + i - ux) & 1023) < uw) continue;  /* upload in progress */
                core[yy * 1024 + x + i] = (q[3] >= 128 ? 0x8000 : 0) | b << 10 | g << 5 | rr;
            }
        }
    }
}

static void gl_vram_written(int x, int y, int w, int h)
{
    if (getenv("HWR_GL_LOG")) { uint16_t *c = (uint16_t *)gpulib_get_vram(); fprintf(stderr, "written %d,%d %dx%d: row0 %04x %04x row100 %04x row300 %04x\n", x, y, w, h, c[100], c[101], c[100 * 1024 + 100], c[300 * 1024 + 100]); }
    hwr_flush();
    sync_rect(x, y, w, h, 1);
    split_rect(x, y, w, h, undraw);
    syncs_in++;
    hwr_vram_written(x, y, w, h);
}

static void gl_vram_read(int x, int y, int w, int h)
{
    if (getenv("HWR_GL_LOG")) fprintf(stderr, "read %d,%d %dx%d\n", x, y, w, h);
    hwr_flush();
    split_rect(x, y, w, h, read_part);
    syncs_out++;
    if (getenv("HWR_GL_LOG")) {
        uint16_t *c = (uint16_t *)gpulib_get_vram();
        fprintf(stderr, "  after read: row0 %04x %04x row100 %04x row300 %04x\n", c[100], c[101], c[100 * 1024 + 100], c[300 * 1024 + 100]);
    }
}

void hwr_gl_sync_all(void) { gl_vram_read(0, 0, 1024, 512); }

static int present_hook(int x, int y, int w, int h, int bgr24)
{
    int r = hwr_present(x, y, w, h, bgr24);
    if (!r) gl_vram_read(x, y, bgr24 ? (w * 3 + 1) / 2 : w, h);
    return r;
}

void hwr_gl_start(int scale)
{
    static const char *names[HWR_NPROG] = { "f0", "f1", "f2", "f3", "f4", "f1w", "f2w", "f3w",
                                            "mf0", "mf1", "mf2", "mf3", "mf4", "mf1w", "mf2w", "mf3w", "f5" };
    static unsigned char dummy[4 * 16 * 16];
    OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 0, 0, 0, NULL);
    const char *dir = getenv("GLSL_DIR") ? getenv("GLSL_DIR") : "glsl";
    GLuint vs; int i; char p[256];
    S = scale;
    if (!ctx || !OSMesaMakeCurrent(ctx, dummy, GL_UNSIGNED_BYTE, 16, 16)) { fprintf(stderr, "osmesa failed\n"); exit(1); }
    for (int native = 0; native < 2; native++) {
    snprintf(p, sizeof p, "%s/hwr.vert", dir);
    vs = shader(GL_VERTEX_SHADER, p);
    for (i = 0; i < HWR_NPROG; i++) {
        GLint ok;
        snprintf(p, sizeof p, "%s/%s.frag", dir, names[i]);
        progs[native][i] = glCreateProgram();
        glAttachShader(progs[native][i], vs); glAttachShader(progs[native][i], shader(GL_FRAGMENT_SHADER, p));
        glLinkProgram(progs[native][i]);
        glGetProgramiv(progs[native][i], GL_LINK_STATUS, &ok);
        if (!ok) { fprintf(stderr, "link %s failed\n", p); exit(1); }
        {   /* fast programs: samplers named _vram16* / _vram8* by cgc -> units 1, 2 */
            GLint n, k;
            glUseProgram(progs[native][i]);
            glGetProgramiv(progs[native][i], GL_ACTIVE_UNIFORMS, &n);
            for (k = 0; k < n; k++) {
                char nm[64]; GLint sz; GLenum ty;
                glGetActiveUniform(progs[native][i], (GLuint)k, sizeof nm, NULL, &sz, &ty, nm);
                if (strstr(nm, "native")) glUniform1f(glGetUniformLocation(progs[native][i], nm), (float)native);
                if (!strncmp(nm, "_vram16", 7)) glUniform1i(glGetUniformLocation(progs[native][i], nm), 1);
                else if (!strncmp(nm, "_vram8", 6)) glUniform1i(glGetUniformLocation(progs[native][i], nm), 2);
            }
            glUseProgram(0);
        }
    }
    }
    glGenTextures(1, &tex_vram);
    glBindTexture(GL_TEXTURE_2D, tex_vram);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glGenTextures(1, &tex16);
    glBindTexture(GL_TEXTURE_2D, tex16);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, 1024, 512, 0, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, NULL);
    glGenTextures(1, &tex8);
    glBindTexture(GL_TEXTURE_2D, tex8);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE8, 2048, 512, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    for (i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, i ? tex8 : tex16);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    glGenFramebuffers(2, fbo);
    glGenTextures(2, col);
    for (i = 0; i < 2; i++) {
        int s = i ? 1 : S;
        glBindTexture(GL_TEXTURE_2D, col[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024 * s, 512 * s, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, col[i], 0);
        if (mask_used < 0) mask_used = getenv("HWR_GL_MASK_USED") != NULL;
        if (i == 0) {   /* the target: depth + stencil (the mask plane) */
            GLuint ds; glGenRenderbuffers(1, &ds); glBindRenderbuffer(GL_RENDERBUFFER, ds);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1024 * S, 512 * S);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds);
            glClearStencil(0); glClear(GL_STENCIL_BUFFER_BIT);
        }
        if (i == 1) {   /* native: depth (per triangle) + stencil (the mask plane), as on the RSX */
            rsx15 = getenv("HWR_GL_RSX15") != NULL;
            glGenRenderbuffers(1, &ndepth);
            glBindRenderbuffer(GL_RENDERBUFFER, ndepth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1024, 512);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ndepth);
            glClearStencil(0); glClear(GL_STENCIL_BUFFER_BIT);
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo %d incomplete\n", i); exit(1); }
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    }
    /* GL rows run bottom-up (the vertex program puts VRAM row 0 at the top) */
    sync_rect(0, 0, 1024, 512, 1);
    hwr_attach(&dev, gpulib_get_vram(), NULL);
    hwr_sync_ecmds(gpulib_get_ex_regs());
    gpu_frontend_hooks.cmd = hwr_cmd;
    gpu_frontend_hooks.vram_written = gl_vram_written;
    gpu_frontend_hooks.vram_read = gl_vram_read;
    gpu_frontend_hooks.sync_ecmds = hwr_sync_ecmds;
    gpu_frontend_hooks.present = present_hook;
}

/* displayed area from the upscaled target, red/blue swapped back */
void hwr_gl_dump(const char *name)
{
    int x, y;
    unsigned char *buf;
    FILE *f;
    if (!presented) return;
    hwr_flush();
    buf = malloc((size_t)pw * S * 4);
    f = fopen(name, "wb");
    fprintf(f, "P6 %d %d 255\n", pw * S, ph * S);
    bind_fbo(0);
    for (y = 0; y < ph * S; y++) {
        glReadPixels(px * S, (511 - py) * S + S - 1 - y, pw * S, 1, GL_RGBA, GL_UNSIGNED_BYTE, buf);
        for (x = 0; x < pw * S; x++) { unsigned char c[3] = { buf[x * 4 + 2], buf[x * 4 + 1], buf[x * 4] }; fwrite(c, 1, 3, f); }
    }
    fclose(f);
    free(buf);
}

/* any VRAM rect of the upscaled target (host tools) */
void hwr_gl_dump_rect(const char *name, int x, int y, int w, int h)
{
    int spx = px, spy = py, spw = pw, sph = ph, sp = presented;
    px = x; py = y; pw = w; ph = h; presented = 1;
    hwr_gl_dump(name);
    px = spx; py = spy; pw = spw; ph = sph; presented = sp;
}

void hwr_gl_stats(void)
{
    fprintf(stderr, "hwr-gl: frames %u prims %u barriers %u syncs in %u out %u presented %d\n",
            hwr_stats.frames, hwr_stats.prims, barriers, syncs_in, syncs_out, presented);
}
