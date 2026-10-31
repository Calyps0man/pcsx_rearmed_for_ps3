#include <unistd.h>
/* Replays a hardware-renderer trace (from hwr_trace.c) with OpenGL (Mesa,
 * offscreen) using the same Cg shaders the PS3 uses, compiled to GLSL by cgc.
 * At scale 1 every 'F' frame's displayed area is compared with the software
 * renderer's VRAM; at higher scales the frames are written out as images.
 *
 * usage: hwr_replay trace.bin scale outprefix   (prints one line per compared frame) */
#define GL_GLEXT_PROTOTYPES
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "hwr.h"

static int S;
static GLuint tex, prog[HWR_NPROG];
static unsigned char *fb;
static FILE *tr;

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

static void gl_init(const char *glsl_dir)
{
    static const char *names[HWR_NPROG] = { "f0", "f1", "f2", "f3", "f4", "f1w", "f2w", "f3w" };
    OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 0, 0, 0, NULL);
    GLuint vs; int i; char p[256];
    fb = calloc((size_t)1024 * S * 512 * S, 4);
    if (!ctx || !OSMesaMakeCurrent(ctx, fb, GL_UNSIGNED_BYTE, 1024 * S, 512 * S)) { fprintf(stderr, "osmesa failed\n"); exit(1); }
    OSMesaPixelStore(OSMESA_Y_UP, 0);
    glViewport(0, 0, 1024 * S, 512 * S);
    snprintf(p, sizeof p, "%s/hwr.vert", glsl_dir);
    vs = shader(GL_VERTEX_SHADER, p);
    for (i = 0; i < HWR_PROG_MCHK; i++) {   /* hybrid mode: no mask-check variants */
        GLint ok;
        snprintf(p, sizeof p, "%s/%s.frag", glsl_dir, names[i]);
        prog[i] = glCreateProgram();
        glAttachShader(prog[i], vs); glAttachShader(prog[i], shader(GL_FRAGMENT_SHADER, p));
        glLinkProgram(prog[i]);
        glGetProgramiv(prog[i], GL_LINK_STATUS, &ok);
        if (!ok) { fprintf(stderr, "link %s failed\n", p); exit(1); }
    }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* same byte -> channel mapping as the RSX A8R8G8B8 snapshot: byte0 A, 1 R, 2 G, 3 B */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 512, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
}

static uint32_t r32(void)
{
    unsigned char b[4];
    if (fread(b, 1, 4, tr) != 4) { fprintf(stderr, "truncated trace\n"); exit(1); }
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static void batch(void)
{
    int nops = (int)r32(), nv = (int)r32(), i;
    hwr_op *ops = malloc(sizeof *ops * (nops ? nops : 1));
    hwr_vtx *v = malloc(sizeof *v * (nv ? nv : 1));
    for (i = 0; i < nops; i++) {
        ops[i].blend = (uint8_t)r32(); ops[i].prog = (uint8_t)r32();
        ops[i].sx0 = (int16_t)r32(); ops[i].sy0 = (int16_t)r32();
        ops[i].sx1 = (int16_t)r32(); ops[i].sy1 = (int16_t)r32();
        ops[i].first = r32(); ops[i].count = r32();
    }
    for (i = 0; i < nv; i++) {   /* 8 floats (bit patterns), then 16 integers (see hwr_trace.c) */
        uint32_t *f = (uint32_t *)&v[i]; int16_t *s = &v[i].tpx; int k;
        for (k = 0; k < 8; k++) f[k] = r32();
        for (k = 0; k < 16; k++) s[k] = (int16_t)r32();
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, sizeof *v, &v->x);
    for (i = 0; i < 5; i++) {   /* see hwr.h: TEXCOORD0 floats, 1..4 16-bit integers */
        glClientActiveTexture(GL_TEXTURE0 + i);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        if (i == 0) glTexCoordPointer(4, GL_FLOAT, sizeof *v, &v->r);
        else glTexCoordPointer(4, GL_SHORT, sizeof *v, &v->tpx + 4 * (i - 1));
    }
    glEnable(GL_SCISSOR_TEST);
    for (i = 0; i < nops; i++) {
        const hwr_op *o = &ops[i];
        glUseProgram(prog[o->prog]);
        glScissor(o->sx0 * S, (512 - o->sy1) * S, (o->sx1 - o->sx0) * S, (o->sy1 - o->sy0) * S);
        switch (o->blend) {
        case HWR_BLEND_NONE: glDisable(GL_BLEND); break;
        case HWR_BLEND_AVG: glEnable(GL_BLEND); glBlendColor(.5f, .5f, .5f, .5f); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR); break;
        case HWR_BLEND_ADD: glEnable(GL_BLEND); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_ONE, GL_ONE); break;
        case HWR_BLEND_SUB: glEnable(GL_BLEND); glBlendEquation(GL_FUNC_REVERSE_SUBTRACT); glBlendFunc(GL_ONE, GL_ONE); break;
        case HWR_BLEND_ADDQ: glEnable(GL_BLEND); glBlendColor(.25f, .25f, .25f, .25f); glBlendEquation(GL_FUNC_ADD); glBlendFunc(GL_CONSTANT_COLOR, GL_ONE); break;
        }
        glDrawArrays(GL_TRIANGLES, o->first, o->count);
    }
    glDisable(GL_BLEND);
    glFinish();
    free(ops); free(v);
}

static void rgb5(const unsigned char *vram, int x, int y, int *r, int *g, int *b)
{
    const unsigned char *p = vram + ((size_t)y * 1024 + x) * 2;
    unsigned c = p[0] | p[1] << 8;           /* VRAM halfwords are little-endian */
    *r = c & 31; *g = (c >> 5) & 31; *b = (c >> 10) & 31;
}

static void write_ppm(const char *name, int x0, int y0, int w, int h, const unsigned char *soft)
{
    FILE *f = fopen(name, "wb");
    int x, y, sc = soft ? 1 : S;
    fprintf(f, "P6 %d %d 255\n", w * sc, h * sc);
    for (y = y0 * sc; y < (y0 + h) * sc; y++)
        for (x = x0 * sc; x < (x0 + w) * sc; x++) {
            unsigned char px[3];
            if (soft) {
                int r, g, b; rgb5(soft, x, y, &r, &g, &b);
                px[0] = (unsigned char)(r << 3 | r >> 2); px[1] = (unsigned char)(g << 3 | g >> 2); px[2] = (unsigned char)(b << 3 | b >> 2);
            } else { const unsigned char *q = fb + ((size_t)y * 1024 * S + x) * 4; px[0] = q[2]; px[1] = q[1]; px[2] = q[0]; }
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

int main(int argc, char **argv)
{
    static unsigned char soft[1024 * 512 * 2], snap[1024 * 512 * 2];
    const char *pre = argc > 3 ? argv[3] : "out/f";
    int c, nf = 0;
    double worst = 0;
    tr = fopen(argv[1], "rb");
    S = argc > 2 ? atoi(argv[2]) : 1;
    if (!tr) { fprintf(stderr, "no trace\n"); return 1; }
    gl_init(getenv("GLSL_DIR") ? getenv("GLSL_DIR") : "glsl");
    while ((c = fgetc(tr)) != EOF) {
        if (c == 'S') {
            if (fread(snap, 1, sizeof snap, tr) != sizeof snap) return 1;
            glBindTexture(GL_TEXTURE_2D, tex);
            {   /* the shaders read big-endian halfwords (the RSX surface layout) */
                size_t k;
                for (k = 0; k < sizeof snap; k += 2) { unsigned char t = snap[k]; snap[k] = snap[k + 1]; snap[k + 1] = t; }
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 512, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8, snap);
        } else if (c == 'B') batch();
        else if (c == 'P') { r32(); r32(); r32(); r32(); }
        else if (c == 'F') {
            int no = (int)r32(), x = (int)r32(), y = (int)r32(), w = (int)r32(), h = (int)r32(), bad = 0, xx, yy;
            char p[300];
            if (fread(soft, 1, sizeof soft, tr) != sizeof soft) return 1;
            if (S == 1) {
                for (yy = y; yy < y + h; yy++)
                    for (xx = x; xx < x + w; xx++) {
                        int r, g, b, R, G, B;
                        const unsigned char *q = fb + ((size_t)yy * 1024 + xx) * 4;
                        rgb5(soft, xx, yy, &r, &g, &b);
                        /* the programs write PS1 red to blue and blue to red */
                        B = (q[0] * 31 + 127) / 255; G = (q[1] * 31 + 127) / 255; R = (q[2] * 31 + 127) / 255;
                        if (abs(R - r) > 1 || abs(G - g) > 1 || abs(B - b) > 1) bad++;
                    }
                {
                    double pct = 100.0 * bad / (w * h > 0 ? w * h : 1);
                    if (pct > worst) worst = pct;
                    printf("frame %d (%dx%d at %d,%d): %.2f%% px differ\n", no, w, h, x, y, pct);
                }
                snprintf(p, sizeof p, "%s_%04d_soft.ppm", pre, no); write_ppm(p, x, y, w, h, soft);
            }
            snprintf(p, sizeof p, "%s_%04d_%dx.ppm", pre, no, S); write_ppm(p, x, y, w, h, NULL);
            nf++;
        } else { fprintf(stderr, "bad trace tag %d\n", c); return 1; }
    }
    if (S == 1) printf("worst %.2f%% over %d frames\n", worst, nf);
    return 0;
}
