/**
 * @file gles.c
 * @brief OpenGL ES 1.1 pipeline translation over vitaGL.
 * @details The game renders into a virtual 320x480 portrait renderbuffer created on iOS by EAGLView. Here that renderbuffer is an FBO with a texture
 *          of the same size: viewport, scissor, and glCopyTexImage2D remain in iOS coordinate space untouched.
 *          During presentation (gles_present), the FBO is drawn upright and scaled onto the native 960x544 screen.
 *
 *          Float gl* entry points map directly: the loader and vitaGL (SOFTFP_ABI=1) use the iOS softfp ABI.
 *          GL_FIXED vertex arrays are converted to float immediately before draw calls.
 */
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#include "gles.h"
#include "hle.h"
#include "objc.h"
#include "patch.h"
#include "imports.h"
#include "log.h"

#define FB_W 320
#define FB_H 480
#define SCR_W 960
#define SCR_H 544

static display_mode_t g_display_mode = DISPLAY_MODE_ORIGINAL_CENTERED;

display_mode_t gles_get_display_mode(void) {
    return g_display_mode;
}

void gles_set_display_mode(display_mode_t mode) {
    g_display_mode = mode;
    const char *names[] = {
        "Landscape Fullscreen (960x544)",
        "Original Centered (362x544, black bars)",
        "Tate Mode Rotated 90 deg"
    };
    l_info("Display presentation mode: %s", names[mode % 3]);
}

void gles_cycle_display_mode(void) {
    gles_set_display_mode((display_mode_t)((g_display_mode + 1) % 3));
}

#define GL_FRAMEBUFFER_COMPLETE_OES 0x8CD5
#define GL_RENDERBUFFER_WIDTH_OES 0x8D42
#define GL_RENDERBUFFER_HEIGHT_OES 0x8D43

static GLuint fbo, fbo_tex; // window framebuffer (ours until the game creates its own) and its color texture

// ---------------------------------------------------------------- GL state cache
// gles_present() saves and restores guest GL state.  The original code used
// glIsEnabled / glGetIntegerv / glGetBooleanv / glGetFloatv every frame, which
// forces CPU–GPU synchronisation in vitaGL and tanks FPS.  Instead every
// intercepted call that changes state also updates the shadow variables below,
// so gles_present() can read them directly with zero GL calls.

typedef struct {
    GLboolean caps[10];        // mirrors caps[] in gles_present order
    GLboolean tex1_on;
    GLboolean tca, va, ca, na;
    GLint     tex;             // GL_TEXTURE_BINDING_2D on unit 0
    GLint     mode;            // GL_MATRIX_MODE
    GLint     active_tex;      // GL_ACTIVE_TEXTURE
    GLint     vp[4];           // GL_VIEWPORT
    GLint     bound_fb;        // GL_FRAMEBUFFER_BINDING
    GLboolean cmask[4];        // GL_COLOR_WRITEMASK
    GLboolean dmask;           // GL_DEPTH_WRITEMASK
    GLfloat   clear[4];        // GL_COLOR_CLEAR_VALUE
} gl_state_cache;

static gl_state_cache g_gl; // zero-initialised; updated by intercepted calls below

// Convenience: index into g_gl.caps[] matching caps[] in gles_present().
// caps[] = {GL_BLEND, GL_ALPHA_TEST, GL_SCISSOR_TEST, GL_DEPTH_TEST,
//           GL_CULL_FACE, GL_LIGHTING, GL_FOG, GL_STENCIL_TEST,
//           GL_CLIP_PLANE0, GL_TEXTURE_2D}
enum {
    CI_BLEND=0, CI_ALPHA_TEST, CI_SCISSOR_TEST, CI_DEPTH_TEST,
    CI_CULL_FACE, CI_LIGHTING, CI_FOG, CI_STENCIL_TEST,
    CI_CLIP_PLANE0, CI_TEXTURE_2D
};

static int cap_to_idx(GLenum cap) {
    switch (cap) {
    case GL_BLEND:        return CI_BLEND;
    case GL_ALPHA_TEST:   return CI_ALPHA_TEST;
    case GL_SCISSOR_TEST: return CI_SCISSOR_TEST;
    case GL_DEPTH_TEST:   return CI_DEPTH_TEST;
    case GL_CULL_FACE:    return CI_CULL_FACE;
    case GL_LIGHTING:     return CI_LIGHTING;
    case GL_FOG:          return CI_FOG;
    case GL_STENCIL_TEST: return CI_STENCIL_TEST;
    case GL_CLIP_PLANE0:  return CI_CLIP_PLANE0;
    case GL_TEXTURE_2D:   return CI_TEXTURE_2D;
    default:              return -1;
    }
}

// Wrappers that update the cache and forward to vitaGL.
static void gc_glEnable(GLenum cap) {
    if (cap == GL_TEXTURE_2D) {
        // Track per-unit state: unit 0 goes into caps[], unit 1 into tex1_on.
        if (g_gl.active_tex == GL_TEXTURE1) g_gl.tex1_on = GL_TRUE;
        else                                g_gl.caps[CI_TEXTURE_2D] = GL_TRUE;
    } else {
        int i = cap_to_idx(cap);
        if (i >= 0) g_gl.caps[i] = GL_TRUE;
    }
    glEnable(cap);
}
static void gc_glDisable(GLenum cap) {
    if (cap == GL_TEXTURE_2D) {
        if (g_gl.active_tex == GL_TEXTURE1) g_gl.tex1_on = GL_FALSE;
        else                                g_gl.caps[CI_TEXTURE_2D] = GL_FALSE;
    } else {
        int i = cap_to_idx(cap);
        if (i >= 0) g_gl.caps[i] = GL_FALSE;
    }
    glDisable(cap);
}
static void gc_glBindTexture(GLenum target, GLuint tex) {
    if (target == GL_TEXTURE_2D && g_gl.active_tex == GL_TEXTURE0)
        g_gl.tex = tex;
    glBindTexture(target, tex);
}
static void gc_glActiveTexture(GLenum unit) {
    g_gl.active_tex = unit;
    glActiveTexture(unit);
}
static void gc_glMatrixMode(GLenum m) {
    g_gl.mode = m;
    glMatrixMode(m);
}
static void gc_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    g_gl.vp[0]=x; g_gl.vp[1]=y; g_gl.vp[2]=w; g_gl.vp[3]=h;
    glViewport(x, y, w, h);
}
static void gc_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    g_gl.cmask[0]=r; g_gl.cmask[1]=g; g_gl.cmask[2]=b; g_gl.cmask[3]=a;
    glColorMask(r, g, b, a);
}
static void gc_glDepthMask(GLboolean m) {
    g_gl.dmask = m;
    glDepthMask(m);
}
static void gc_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    g_gl.clear[0]=r; g_gl.clear[1]=g; g_gl.clear[2]=b; g_gl.clear[3]=a;
    glClearColor(r, g, b, a);
}
static void gc_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    glScissor(x, y, w, h);
}
static void gc_glBindFramebuffer(GLenum target, GLuint fb) {
    if (target == GL_FRAMEBUFFER) g_gl.bound_fb = fb;
    glBindFramebuffer(target, fb);
}

// Client array state, tracked because (1) vitaGL rejects some GLES1 array types (GL_BYTE
// vertices/texcoords, GL_FIXED colors; we also convert GL_FIXED vertex/texcoord/normal as before):
// on GL_INVALID_ENUM it keeps the PREVIOUS pointer and the draw uses another mesh's data;
// (2) gles_present must re-apply every pointer with the buffer object it was declared with.
typedef struct {
    GLint size;
    GLenum type;
    GLsizei stride;
    const void *ptr;
    GLuint vbo; // GL_ARRAY_BUFFER bound when the pointer was set (ptr is then an offset)
    int convert;
} array_ptr;
enum { A_VTX, A_TC0, A_TC1, A_COL, A_NRM, A_COUNT };
static array_ptr arr[A_COUNT];
static GLuint array_vbo;
static uint32_t enabled; // bit per A_* kind, from glEnable/DisableClientState
static int client_unit; // glClientActiveTexture - GL_TEXTURE0
static GLint env_mode = GL_MODULATE;
static GLfixed color[4] = {0x10000, 0x10000, 0x10000, 0x10000};

static int needs_convert(int kind, GLenum type) {
    switch (kind) {
    case A_COL:
        return type == GL_FIXED;
    case A_NRM:
        return type == GL_FIXED;
    default: // vertex / texcoord: vitaGL takes GL_FLOAT and GL_SHORT
        return type == GL_FIXED || type == GL_BYTE;
    }
}

// vitaGL buffer names are pointers to its vbo struct, whose first field is the CPU-visible data
// pointer (read directly: glUnmapNamedBuffer would mark it unused by the GPU).
static const uint8_t *array_source(const array_ptr *a) {
    if (!a->vbo)
        return a->ptr;
    const uint8_t *base = *(const uint8_t **)(uintptr_t)a->vbo;
    return base ? base + (uintptr_t)a->ptr : NULL;
}

static float *conv_buf[A_COUNT];
static int conv_cap[A_COUNT];

static const float *convert_array(int kind, int n) {
    const array_ptr *a = &arr[kind];
    const uint8_t *src = array_source(a);
    if (!src)
        return NULL;
    int need = n * a->size;
    if (need > conv_cap[kind]) {
        conv_buf[kind] = realloc(conv_buf[kind], need * sizeof(float));
        conv_cap[kind] = need;
    }
    int esz = a->type == GL_BYTE ? 1 : 4;
    int stride = a->stride ? a->stride : a->size * esz;
    float *dst = conv_buf[kind];
    for (int i = 0; i < n; i++, src += stride)
        for (int c = 0; c < a->size; c++)
            dst[i * a->size + c] = a->type == GL_BYTE ? (float)((const int8_t *)src)[c]
                                                      : ((const int32_t *)src)[c] * (1.0f / 65536.0f);
    return dst;
}

// Passes one array to vitaGL as the game declared it (native types) or as converted floats.
static void set_native(int kind, GLint size, GLenum type, GLsizei stride, const void *ptr) {
    if (kind == A_TC0 || kind == A_TC1)
        glClientActiveTexture(GL_TEXTURE0 + kind - A_TC0);
    switch (kind) {
    case A_VTX:
        glVertexPointer(size, type, stride, ptr);
        break;
    case A_COL:
        glColorPointer(size, type, stride, ptr);
        break;
    case A_NRM:
        glNormalPointer(type, stride, ptr);
        break;
    default:
        glTexCoordPointer(size, type, stride, ptr);
        break;
    }
    if (kind == A_TC0 || kind == A_TC1)
        glClientActiveTexture(GL_TEXTURE0 + client_unit);
}

static void declare(int kind, GLint size, GLenum type, GLsizei stride, const void *ptr) {
    static uint32_t seen[64];
    static int nseen;
    uint32_t key = kind << 24 | (type & 0xFFFF) << 8 | size << 4 | (array_vbo != 0);
    int known = 0;
    for (int i = 0; i < nseen; i++)
        known |= seen[i] == key;
    if (!known && nseen < 64) {
        seen[nseen++] = key;
        l_info("GL array %d: size %d type 0x%04X stride %d %s%s", kind, size, type, stride,
               array_vbo ? "vbo" : "client", needs_convert(kind, type) ? " (converted)" : "");
    }
    arr[kind] = (array_ptr){size, type, stride, ptr, array_vbo, needs_convert(kind, type)};
    if (!arr[kind].convert)
        set_native(kind, size, type, stride, ptr);
}

// Before a draw: converted arrays are uploaded as client floats for vertices [0, n).
static void prepare_draw(int n) {
    int any = 0;
    for (int k = 0; k < A_COUNT; k++)
        if (arr[k].size && arr[k].convert && enabled & 1u << k) {
            const float *f = convert_array(k, n);
            if (!f)
                continue;
            if (!any++)
                glBindBuffer(GL_ARRAY_BUFFER, 0);
            set_native(k, arr[k].size, GL_FLOAT, 0, f);
        }
    if (any && array_vbo)
        glBindBuffer(GL_ARRAY_BUFFER, array_vbo);
}

static int any_convert(void) {
    for (int k = 0; k < A_COUNT; k++)
        if (arr[k].size && arr[k].convert && enabled & 1u << k)
            return 1;
    return 0;
}

// Re-applies every native pointer with its own buffer binding (after gles_present's blit).
static void reapply_arrays(void) {
    for (int k = 0; k < A_COUNT; k++)
        if (arr[k].size && !arr[k].convert) {
            glBindBuffer(GL_ARRAY_BUFFER, arr[k].vbo);
            set_native(k, arr[k].size, arr[k].type, arr[k].stride, arr[k].ptr);
        }
    glBindBuffer(GL_ARRAY_BUFFER, array_vbo);
}

static int state_kind(GLenum cap) {
    return cap == GL_VERTEX_ARRAY ? A_VTX : cap == GL_COLOR_ARRAY ? A_COL : cap == GL_NORMAL_ARRAY ? A_NRM
         : cap == GL_TEXTURE_COORD_ARRAY ? A_TC0 + client_unit : -1;
}
static void g_glEnableClientState(GLenum cap) {
    int k = state_kind(cap);
    if (k >= 0)
        enabled |= 1u << k;
    // Update the present-cache for vertex/texcoord/color/normal arrays.
    if (cap == GL_VERTEX_ARRAY)       g_gl.va  = GL_TRUE;
    else if (cap == GL_TEXTURE_COORD_ARRAY && client_unit == 0) g_gl.tca = GL_TRUE;
    else if (cap == GL_COLOR_ARRAY)   g_gl.ca  = GL_TRUE;
    else if (cap == GL_NORMAL_ARRAY)  g_gl.na  = GL_TRUE;
    glEnableClientState(cap);
}
static void g_glDisableClientState(GLenum cap) {
    int k = state_kind(cap);
    if (k >= 0)
        enabled &= ~(1u << k);
    if (cap == GL_VERTEX_ARRAY)       g_gl.va  = GL_FALSE;
    else if (cap == GL_TEXTURE_COORD_ARRAY && client_unit == 0) g_gl.tca = GL_FALSE;
    else if (cap == GL_COLOR_ARRAY)   g_gl.ca  = GL_FALSE;
    else if (cap == GL_NORMAL_ARRAY)  g_gl.na  = GL_FALSE;
    glDisableClientState(cap);
}

static void g_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    declare(A_VTX, size, type, stride, ptr);
}
static void g_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    declare(A_TC0 + client_unit, size, type, stride, ptr);
}
static void g_glColorPointer(GLint size, GLenum type, GLsizei stride, const void *ptr) {
    declare(A_COL, size, type, stride, ptr);
}
static void g_glNormalPointer(GLenum type, GLsizei stride, const void *ptr) {
    declare(A_NRM, 3, type, stride, ptr);
}

static void g_glClientActiveTexture(GLenum unit) {
    client_unit = (unit - GL_TEXTURE0) & 1;
    glClientActiveTexture(unit);
}

static uint32_t gles_draw_calls;

uint32_t gles_get_draw_calls(void) {
    return gles_draw_calls;
}

static void g_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    gles_draw_calls++;
    if (any_convert())
        prepare_draw(first + count);
    glDrawArrays(mode, first, count);
}

static void g_glGetPointerv(GLenum pname, void **out) {
    if (pname == GL_VERTEX_ARRAY_POINTER)
        *out = (void *)arr[A_VTX].ptr;
    else if (pname == GL_TEXTURE_COORD_ARRAY_POINTER)
        *out = (void *)arr[A_TC0 + client_unit].ptr;
    else if (pname == GL_COLOR_ARRAY_POINTER)
        *out = (void *)arr[A_COL].ptr;
    else if (pname == GL_NORMAL_ARRAY_POINTER)
        *out = (void *)arr[A_NRM].ptr;
    else
        glGetPointerv(pname, out);
}

// ---------------------------------------------------------------- *x state

static void g_glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a) {
    color[0] = r, color[1] = g, color[2] = b, color[3] = a;
    glColor4x(r, g, b, a);
}

static void g_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    color[0] = (GLfixed)(r * 65536.0f);
    color[1] = (GLfixed)(g * 65536.0f);
    color[2] = (GLfixed)(b * 65536.0f);
    color[3] = (GLfixed)(a * 65536.0f);
    glColor4f(r, g, b, a);
}

static void g_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) {
    color[0] = (GLfixed)((r / 255.0f) * 65536.0f);
    color[1] = (GLfixed)((g / 255.0f) * 65536.0f);
    color[2] = (GLfixed)((b / 255.0f) * 65536.0f);
    color[3] = (GLfixed)((a / 255.0f) * 65536.0f);
    glColor4ub(r, g, b, a);
}

static void g_glTexEnvi(GLenum target, GLenum pname, GLint v) {
    if (pname == GL_TEXTURE_ENV_MODE)
        env_mode = v;
    glTexEnvi(target, pname, v);
}

static void g_glTexEnvxv(GLenum target, GLenum pname, const GLfixed *v) {
    if (pname == GL_TEXTURE_ENV_MODE) // enum value, not fixed point
        return g_glTexEnvi(target, pname, v[0]);
    glTexEnvxv(target, pname, (GLfixed *)v);
}

// Texture parameters used by the engine are enums (filters, wrap modes): pass unscaled.
static void g_glTexParameterx(GLenum target, GLenum pname, GLfixed v) { glTexParameteri(target, pname, v); }

static void g_glGetFixedv(GLenum pname, GLfixed *out) {
    float f[16];
    int n = 1;
    if (pname == GL_MODELVIEW_MATRIX || pname == GL_PROJECTION_MATRIX || pname == GL_TEXTURE_MATRIX)
        n = 16;
    else if (pname == GL_CURRENT_COLOR) {
        memcpy(out, color, sizeof(color));
        return;
    }
    glGetFloatv(pname, f);
    for (int i = 0; i < n; i++)
        out[i] = (GLfixed)(f[i] * 65536.0f);
}

static void g_glGetLightxv(GLenum light, GLenum pname, GLfixed *out) { memset(out, 0, 4 * sizeof(GLfixed)); }

// ---------------------------------------------------------------- EAGLView framebuffer emulation

// OES framebuffers/renderbuffers are real vitaGL objects (render-to-texture keeps working). The
// one exception is the renderbuffer backed by the CAEAGLLayer ([EAGLContext renderbufferStorage:
// fromDrawable:]): attaching it as a color buffer attaches fbo_tex instead, which gles_present blits.
static GLuint bound_rb, drawable_rb;
static GLuint depth_fbs[64]; // framebuffers whose depth attachment was already forwarded
static int ndepth_fbs;

void gles_bind_drawable(void) {
    drawable_rb = bound_rb;
    l_info("EAGL drawable renderbuffer = %u (%ux%u)", drawable_rb, FB_W, FB_H);
}

static void g_glBindRenderbufferOES(GLenum target, GLuint rb) {
    bound_rb = rb;
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
}
static void g_glFramebufferRenderbufferOES(GLenum target, GLenum attachment, GLenum rbtarget, GLuint rb) {
    if (rb && rb == drawable_rb && attachment == GL_COLOR_ATTACHMENT0) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint *)&fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
        l_info("EAGL window framebuffer = %u", fbo);
        return;
    }
    // vitaGL renderbuffers hold no depth memory: every depth attach DISCARDS the framebuffer's own
    // depth buffer and a fresh, uninitialized one is created later. LibEffects (cutscene effects)
    // re-attaches depth every frame, which corrupted the scene's depth mid-frame. Since the attached
    // renderbuffer cannot change which memory is used anyway, only the first depth attach of each
    // framebuffer is forwarded.
    if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_STENCIL_ATTACHMENT) {
        GLint cur;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &cur);
        for (int i = 0; i < ndepth_fbs; i++)
            if (depth_fbs[i] == (GLuint)cur)
                return;
        if (rb && ndepth_fbs < 64)
            depth_fbs[ndepth_fbs++] = cur;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, attachment, GL_RENDERBUFFER, rb);
}
static void g_glDeleteFramebuffersOES(GLsizei n, const GLuint *ids) {
    for (int k = 0; k < n; k++)
        for (int i = 0; i < ndepth_fbs; i++)
            if (depth_fbs[i] == ids[k])
                depth_fbs[i--] = depth_fbs[--ndepth_fbs];
    glDeleteFramebuffers(n, ids);
}

static void g_glGetRenderbufferParameterivOES(GLenum target, GLenum pname, GLint *v) {
    if (bound_rb && bound_rb == drawable_rb)
        *v = pname == GL_RENDERBUFFER_WIDTH_OES ? FB_W : pname == GL_RENDERBUFFER_HEIGHT_OES ? FB_H : 0;
    else
        *v = 0; // ponytail: vitaGL has no glGetRenderbufferParameteriv; only the drawable is queried by EAGLView
}
static GLuint element_vbo;
static void g_glBindBuffer(GLenum target, GLuint b) {
    if (target == GL_ARRAY_BUFFER)
        array_vbo = b;
    else if (target == GL_ELEMENT_ARRAY_BUFFER)
        element_vbo = b;
    glBindBuffer(target, b);
}

// Converted arrays need the highest referenced index (indices may live in a buffer object).
static void g_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *idx) {
    gles_draw_calls++;
    if (any_convert()) {
        const uint8_t *ix = idx;
        if (element_vbo) {
            const uint8_t *base = *(const uint8_t **)(uintptr_t)element_vbo;
            ix = base ? base + (uintptr_t)idx : NULL;
        }
        int n = 0;
        for (int i = 0; ix && i < count; i++) {
            int v = type == GL_UNSIGNED_BYTE ? ix[i] : ((const uint16_t *)ix)[i];
            if (v >= n)
                n = v + 1;
        }
        prepare_draw(n);
    }
    glDrawElements(mode, count, type, idx);
}

static void g_glLightf(GLenum light, GLenum pname, GLfloat v) { glLightfv(light, pname, &v); }
static void g_glLightModelf(GLenum pname, GLfloat v) { glLightModelfv(pname, &v); }
static GLboolean g_glIsBuffer(GLuint b) { return b != 0; } // ponytail: vitaGL has no glIsBuffer
static void g_glNoop(void) {}

// ---------------------------------------------------------------- init / present

void gles_init(void) {
    // Enable triple buffering for stable 60 FPS frame pacing without 30 FPS drops
    vglUseTripleBuffering(GL_TRUE);
    // 16 MB vertex/VBO pool — BBD3 loads many particle textures; 8 MB caused stalls.
    vglInitExtended(0, SCR_W, SCR_H, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);

    glGenTextures(1, &fbo_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FB_W, FB_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Seed the state cache with the initial values we set here.
    g_gl.active_tex = GL_TEXTURE0;
    g_gl.mode       = GL_MODELVIEW;
    g_gl.bound_fb   = fbo;
    g_gl.cmask[0] = g_gl.cmask[1] = g_gl.cmask[2] = g_gl.cmask[3] = GL_TRUE;
    g_gl.dmask    = GL_TRUE;
    g_gl.clear[3] = 1.0f; // clear alpha = 1

    // Set initial viewport (game uses FB_W x FB_H until the first frame).
    g_gl.vp[0]=0; g_gl.vp[1]=0; g_gl.vp[2]=FB_W; g_gl.vp[3]=FB_H;
    glViewport(0, 0, FB_W, FB_H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    l_info("vitaGL: FBO %ux%u -> screen %ux%u (mode: %d)", FB_W, FB_H, SCR_W, SCR_H, g_display_mode);
}

// Blits the 320x480 framebuffer to the screen according to g_display_mode.
// Every fixed-function state that could alter the blit (lighting, fog, culling, a second texture
// unit, the texture matrix, write masks...) is saved, neutralized and restored: the game leaves
// them enabled across frames (in-engine cutscenes use fog/lighting).
// PERF: All state is read from g_gl shadow variables — zero GL readback calls per frame.
void gles_present(void) {
    // Take a snapshot of the cached GL state (zero GL calls).
    GLboolean on[10];
    for (int i = 0; i < 10; i++) on[i] = g_gl.caps[i];
    GLboolean tex1_on = g_gl.tex1_on;
    GLboolean tca = g_gl.tca, va = g_gl.va, ca = g_gl.ca, na = g_gl.na;
    GLint tex = g_gl.tex;
    GLint mode = g_gl.mode;
    GLint active = g_gl.active_tex;
    GLint vp[4]; vp[0]=g_gl.vp[0]; vp[1]=g_gl.vp[1]; vp[2]=g_gl.vp[2]; vp[3]=g_gl.vp[3];
    GLint bound_fb = g_gl.bound_fb;
    GLboolean cmask[4]; cmask[0]=g_gl.cmask[0]; cmask[1]=g_gl.cmask[1]; cmask[2]=g_gl.cmask[2]; cmask[3]=g_gl.cmask[3];
    GLboolean dmask = g_gl.dmask;
    GLfloat clear[4]; clear[0]=g_gl.clear[0]; clear[1]=g_gl.clear[1]; clear[2]=g_gl.clear[2]; clear[3]=g_gl.clear[3];

    // Disable all tracked caps, enable TEXTURE_2D for blit.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glViewport(0, 0, SCR_W, SCR_H);
    if (on[CI_BLEND])        glDisable(GL_BLEND);
    if (on[CI_ALPHA_TEST])   glDisable(GL_ALPHA_TEST);
    if (on[CI_SCISSOR_TEST]) glDisable(GL_SCISSOR_TEST);
    if (on[CI_DEPTH_TEST])   glDisable(GL_DEPTH_TEST);
    if (on[CI_CULL_FACE])    glDisable(GL_CULL_FACE);
    if (on[CI_LIGHTING])     glDisable(GL_LIGHTING);
    if (on[CI_FOG])          glDisable(GL_FOG);
    if (on[CI_STENCIL_TEST]) glDisable(GL_STENCIL_TEST);
    if (on[CI_CLIP_PLANE0])  glDisable(GL_CLIP_PLANE0);
    if (tex1_on) {
        glActiveTexture(GL_TEXTURE1);
        glDisable(GL_TEXTURE_2D);
    }
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    if (!on[CI_TEXTURE_2D])  glEnable(GL_TEXTURE_2D);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    if (ca)  glDisableClientState(GL_COLOR_ARRAY);
    if (na)  glDisableClientState(GL_NORMAL_ARRAY);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glMatrixMode(GL_TEXTURE);
    glPushMatrix();
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrthof(0, SCR_W, SCR_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    // Centered and Tate leave uncovered screen: plain black bars.
    if (g_display_mode != DISPLAY_MODE_LANDSCAPE_FULL) {
        glClearColor(0, 0, 0, 1); // side bars: black, not the game's clear color
        glClear(GL_COLOR_BUFFER_BIT);
    }

    static const float uv_normal[] = {0, 1, 1, 1, 0, 0, 1, 0};
    static const float uv_tate[]   = {1, 1, 1, 0, 0, 1, 0, 0}; // 90 deg rotated

    const float *uv = uv_normal;
    float xy[8];

    if (g_display_mode == DISPLAY_MODE_LANDSCAPE_FULL) {
        // Landscape full screen 960x544 stretch
        xy[0] = 0;     xy[1] = 0;
        xy[2] = SCR_W; xy[3] = 0;
        xy[4] = 0;     xy[5] = SCR_H;
        xy[6] = SCR_W; xy[7] = SCR_H;
    } else if (g_display_mode == DISPLAY_MODE_ORIGINAL_CENTERED) {
        // Original aspect ratio centered (362x544)
        int dst_w = SCR_H * FB_W / FB_H;
        int dst_x = (SCR_W - dst_w) / 2;
        xy[0] = dst_x;         xy[1] = 0;
        xy[2] = dst_x + dst_w; xy[3] = 0;
        xy[4] = dst_x;         xy[5] = SCR_H;
        xy[6] = dst_x + dst_w; xy[7] = SCR_H;
    } else {
        // Tate Mode: rotated 90 degrees, scaled to 544 height (816x544)
        int tate_w = 816;
        int tate_x = (SCR_W - tate_w) / 2;
        xy[0] = tate_x;          xy[1] = 0;
        xy[2] = tate_x + tate_w; xy[3] = 0;
        xy[4] = tate_x;          xy[5] = SCR_H;
        xy[6] = tate_x + tate_w; xy[7] = SCR_H;
        uv = uv_tate;
    }

    glVertexPointer(2, GL_FLOAT, 0, xy);
    glTexCoordPointer(2, GL_FLOAT, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    patch_overlay(); // loader OSD on the 960x544 screen (scissor + clear only)
    vglSwapBuffers(ime_active()); // GL_TRUE while system dialog is active

    // Restore guest OpenGL state using cached values — no GL readbacks.
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_TEXTURE);
    glPopMatrix();
    glMatrixMode(mode);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound_fb);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glDepthMask(dmask);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env_mode);

    // Restore caps that we changed (only the ones that differ).
    if (on[CI_BLEND])        glEnable(GL_BLEND);
    if (on[CI_ALPHA_TEST])   glEnable(GL_ALPHA_TEST);
    if (on[CI_SCISSOR_TEST]) glEnable(GL_SCISSOR_TEST);
    if (on[CI_DEPTH_TEST])   glEnable(GL_DEPTH_TEST);
    if (on[CI_CULL_FACE])    glEnable(GL_CULL_FACE);
    if (on[CI_LIGHTING])     glEnable(GL_LIGHTING);
    if (on[CI_FOG])          glEnable(GL_FOG);
    if (on[CI_STENCIL_TEST]) glEnable(GL_STENCIL_TEST);
    if (on[CI_CLIP_PLANE0])  glEnable(GL_CLIP_PLANE0);
    if (!on[CI_TEXTURE_2D])  glDisable(GL_TEXTURE_2D);
    if (tex1_on) {
        glActiveTexture(GL_TEXTURE1);
        glEnable(GL_TEXTURE_2D);
    }
    if (!va)  glDisableClientState(GL_VERTEX_ARRAY);
    if (!tca) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (ca)   glEnableClientState(GL_COLOR_ARRAY);
    if (na)   glEnableClientState(GL_NORMAL_ARRAY);
    glColor4x(color[0], color[1], color[2], color[3]);
    reapply_arrays();
    glClientActiveTexture(GL_TEXTURE0 + client_unit);
    glActiveTexture(active);
}

static const GLubyte *g_glGetString(GLenum name) {
    if (name == GL_VERSION) {
        // Irrlicht's GLES 1.1 driver parses "%u.%u" and asserts (version > 100 && version <= 199).
        // vitaGL natively returns "OpenGL ES 2.0 VitaGL", which causes Irrlicht to reject the driver
        // as unknown/unsupported and return NULL, leading to crash on NULL driver dereference.
        return (const GLubyte *)"OpenGL ES-CM 1.1 VitaGL";
    }
    return glGetString(name);
}

static void g_glSampleCoverage(GLclampf value, GLboolean invert) {
    (void)value;
    (void)invert;
}

// ---------------------------------------------------------------- import table

#undef F
#undef D
#define F(name, fn) {"_" #name, (uintptr_t)(fn)}
#define D(name) {"_" #name, (uintptr_t)(name)}

const import_entry gles_imports[] = {
    F(glActiveTexture, gc_glActiveTexture), F(glBindTexture, gc_glBindTexture),
    D(glBlendFunc), D(glClear), D(glCopyTexImage2D),
    D(glCopyTexSubImage2D), D(glDeleteTextures), F(glDepthMask, gc_glDepthMask),
    F(glDisable, gc_glDisable), F(glDisableClientState, g_glDisableClientState),
    F(glEnable, gc_glEnable), F(glEnableClientState, g_glEnableClientState),
    D(glGenTextures), D(glGetIntegerv), D(glIsEnabled),
    D(glLoadIdentity), D(glLoadMatrixx), F(glMatrixMode, gc_glMatrixMode),
    D(glOrthox), D(glPopMatrix), D(glPushMatrix),
    D(glRotatex), D(glScalex), F(glScissor, gc_glScissor), D(glShadeModel),
    D(glTexImage2D), D(glTexParameteri),
    D(glTexSubImage2D), D(glTranslatex), F(glViewport, gc_glViewport),
    F(glColor4x, g_glColor4x), F(glTexEnvi, g_glTexEnvi), F(glTexEnvxv, g_glTexEnvxv),
    F(glTexParameterx, g_glTexParameterx), F(glGetFixedv, g_glGetFixedv), F(glGetLightxv, g_glGetLightxv),
    F(glVertexPointer, g_glVertexPointer), F(glTexCoordPointer, g_glTexCoordPointer),
    F(glClientActiveTexture, g_glClientActiveTexture), F(glDrawArrays, g_glDrawArrays),
    F(glGetPointerv, g_glGetPointerv),
    F(glGenFramebuffersOES, glGenFramebuffers), F(glGenRenderbuffersOES, glGenRenderbuffers),
    F(glBindFramebufferOES, gc_glBindFramebuffer), F(glBindRenderbufferOES, g_glBindRenderbufferOES),
    F(glDeleteFramebuffersOES, g_glDeleteFramebuffersOES), F(glDeleteRenderbuffersOES, glDeleteRenderbuffers),
    F(glRenderbufferStorageOES, glRenderbufferStorage),
    F(glFramebufferRenderbufferOES, g_glFramebufferRenderbufferOES),
    F(glFramebufferTexture2DOES, glFramebufferTexture2D),
    F(glCheckFramebufferStatusOES, glCheckFramebufferStatus),
    F(glGetRenderbufferParameterivOES, g_glGetRenderbufferParameterivOES),
    F(glGetFramebufferAttachmentParameterivOES, glGetFramebufferAttachmentParameteriv),
    // Float entry points: loader and vitaGL (SOFTFP_ABI=1) share the iOS softfp convention.
    D(glAlphaFunc), F(glClearColor, gc_glClearColor), D(glClearDepthf), D(glClipPlanef),
    F(glColor4f, g_glColor4f), F(glColor4ub, g_glColor4ub),
    F(glColorMask, gc_glColorMask), F(glColorPointer, g_glColorPointer), D(glCompressedTexImage2D), D(glCullFace), D(glDepthFunc),
    D(glDepthRangef), D(glFogf), D(glFogfv), D(glFrontFace), D(glGetBooleanv), D(glGetError),
    D(glGetFloatv), F(glGetString, g_glGetString), D(glHint), D(glLightModelfv), D(glLightfv), D(glLineWidth),
    D(glLoadMatrixf), D(glMaterialf), D(glMaterialfv), D(glMultMatrixf), D(glNormal3f),
    F(glNormalPointer, g_glNormalPointer), D(glPixelStorei), D(glPointSize), D(glPolygonOffset), D(glReadPixels),
    D(glRotatef), D(glScalef), D(glStencilFunc), D(glStencilMask), D(glStencilOp), D(glTexEnvf),
    D(glTexEnvfv), D(glTexEnvx), D(glTexParameterf), D(glTranslatef), D(glGenBuffers),
    D(glDeleteBuffers), D(glBufferData), D(glBufferSubData), F(glBindBuffer, g_glBindBuffer),
    F(glDrawElements, g_glDrawElements), F(glLightf, g_glLightf), F(glPointParameterf, g_glNoop),
    // BBD3 extras (not used by Zombie Infection)
    D(glOrthof), F(glFlush, g_glNoop), D(glClearStencil), D(glIsTexture), F(glIsBuffer, g_glIsBuffer),
    F(glSampleCoverage, g_glSampleCoverage),
    F(glLightModelf, g_glLightModelf), F(glPointParameterfv, g_glNoop), F(glDiscardFramebufferEXT, g_glNoop),
    F(glGenerateMipmapOES, glGenerateMipmap), F(glBlendEquationOES, glBlendEquation),
    F(glMapBufferOES, glMapBuffer), F(glUnmapBufferOES, glUnmapBuffer),
};
const int gles_imports_count = sizeof(gles_imports) / sizeof(gles_imports[0]);
