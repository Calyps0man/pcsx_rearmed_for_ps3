/* FXAA program check on the host: cgc -profile glslv/glslf quad.vcg/fxaa.fcg to quad.vert/fxaa.frag,
   gcc -o fx fx.c -lOSMesa -lGL, then fx in.ppm out.ppm W H (a picture through the RSX FXAA pass). */
/* runs quad.vcg + fxaa.fcg (cgc GLSL output) on a PPM: fx in.ppm out.ppm outW outH */
#define GL_GLEXT_PROTOTYPES
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdlib.h>
static char *load(const char *p) { FILE *f = fopen(p, "rb"); long n; char *b; fseek(f, 0, 2); n = ftell(f); rewind(f); b = calloc(n + 1, 1); fread(b, 1, n, f); fclose(f); return b; }
static GLuint sh(GLenum t, const char *p) { GLuint s = glCreateShader(t); const char *src = load(p); GLint ok; char log[4096];
    glShaderSource(s, 1, &src, NULL); glCompileShader(s); glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "%s: %s\n", p, log); exit(1); } return s; }
int main(int argc, char **argv)
{
    int W = atoi(argv[3]), H = atoi(argv[4]), tw, th, mx, i; FILE *f = fopen(argv[1], "rb");
    unsigned char *src, *fb = malloc(W * H * 4);
    fscanf(f, "P6 %d %d %d", &tw, &th, &mx); fgetc(f);
    src = malloc(tw * th * 3); fread(src, 1, tw * th * 3, f); fclose(f);
    OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 0, 0, 0, NULL);
    OSMesaMakeCurrent(ctx, fb, GL_UNSIGNED_BYTE, W, H);
    OSMesaPixelStore(OSMESA_Y_UP, 0);
    glViewport(0, 0, W, H);
    GLuint p = glCreateProgram(); glAttachShader(p, sh(GL_VERTEX_SHADER, "quad.vert")); glAttachShader(p, sh(GL_FRAGMENT_SHADER, argc > 5 ? argv[5] : "fxaa.frag"));
    glLinkProgram(p); glUseProgram(p);
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, tw, th, 0, GL_RGB, GL_UNSIGNED_BYTE, src);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glUniform1i(glGetUniformLocation(p, "_tex1"), 0);
    /* texel: one output pixel in texture units (the direct pass); the source-res pass has W = tw */
    float tx = 1.f / W, ty = 1.f / H, NW = getenv("NW") ? atof(getenv("NW")) : 0, NH = getenv("NH") ? atof(getenv("NH")) : 0;
    glBegin(GL_TRIANGLE_STRIP);
    glMultiTexCoord2f(GL_TEXTURE1, tx, ty); glMultiTexCoord2f(GL_TEXTURE2, 0 * NW, 0 * NH); glMultiTexCoord2f(GL_TEXTURE0, 0, 0); glVertex2f(-1, 1);
    glMultiTexCoord2f(GL_TEXTURE1, tx, ty); glMultiTexCoord2f(GL_TEXTURE2, 1 * NW, 0 * NH); glMultiTexCoord2f(GL_TEXTURE0, 1, 0); glVertex2f(1, 1);
    glMultiTexCoord2f(GL_TEXTURE1, tx, ty); glMultiTexCoord2f(GL_TEXTURE2, 0 * NW, 1 * NH); glMultiTexCoord2f(GL_TEXTURE0, 0, 1); glVertex2f(-1, -1);
    glMultiTexCoord2f(GL_TEXTURE1, tx, ty); glMultiTexCoord2f(GL_TEXTURE2, 1 * NW, 1 * NH); glMultiTexCoord2f(GL_TEXTURE0, 1, 1); glVertex2f(1, -1);
    glEnd(); glFinish();
    f = fopen(argv[2], "wb"); fprintf(f, "P6 %d %d 255\n", W, H);
    for (i = 0; i < W * H; i++) fwrite(fb + i * 4, 1, 3, f);
    fclose(f); return 0;
}
