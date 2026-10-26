/**
 * @file patch.c
 * @brief Game-specific hooks, memory patches, and custom symbol intercepts.
 */
#include <kubridge.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include <vitaGL.h>

#include "gles.h"
#include "imports.h"
#include "input.h"
#include "log.h"
#include "macho.h"
#include "patch.h"

// ---------------------------------------------------------------- dummy intrusive_ptr safety
// Boost intrusive_ptr::operator->() at 0x6278-0x63b8 asserts (px != 0).
// In BBD3, if an asset (like gameloft_logo in package 4) is missing, get_texture returns NULL,
// and the caller unconditionally dereferences the smart pointer (e.g. at 0x952e8, 0x6af68).
// We return a safe dummy Texture object with a no-op vtable and high refcount instead of asserting.
static uint32_t dummy_vtable[64];
static uint8_t dummy_texture[512] __attribute__((aligned(8)));
static uint8_t dummy_context[1024] __attribute__((aligned(8)));
static GLuint dummy_gl_tex_id = 0;

static int dummy_noop(void) {
    return 0;
}

static void init_dummy_texture(void) {
    for (int i = 0; i < 64; i++) {
        dummy_vtable[i] = (uint32_t)dummy_noop;
    }
    memset(dummy_texture, 0, sizeof(dummy_texture));
    memset(dummy_context, 0, sizeof(dummy_context));
    // +0x00: vtable pointer
    *(uint32_t **)&dummy_texture[0] = dummy_vtable;
    // +0x04: refcount (pinned high so decrefs never reach 0)
    *(uint32_t *)&dummy_texture[4] = 0x3FFFFFFF;
    // +0x0c / +0x10: dimensions (width=1, height=1)
    *(uint32_t *)&dummy_texture[0x0c] = 1;
    *(uint32_t *)&dummy_texture[0x10] = 1;
    // +0x14: 1
    *(uint32_t *)&dummy_texture[0x14] = 1;
    // +0x20: renderer context
    *(void **)&dummy_texture[0x20] = dummy_context;
}

static void *hook_intrusive_ptr_deref(void **ptr) {
    if (!ptr || !*ptr) {
        static int warn_count = 0;
        if (warn_count++ < 20) {
            l_warn("guarded null intrusive_ptr deref (ptr=%p)", ptr);
        }
        return dummy_texture;
    }
    return *ptr;
}

// ---------------------------------------------------------------- effect wrapper safety
typedef struct {
    void *vtable;
    void *mesh;
} effect_wrapper;

static void *hook_wrapper_slot2(effect_wrapper *w) {
    if (!w || !w->mesh)
        return NULL;
    void **vtable = *(void ***)w->mesh;
    void *(*fn)(void *) = (void *(*)(void *))vtable[9];
    return fn(w->mesh);
}

static void *hook_wrapper_slot3(effect_wrapper *w, void *a1, void *a2) {
    if (!w || !w->mesh)
        return NULL;
    void **vtable = *(void ***)w->mesh;
    void *(*fn)(void *, void *, void *) = (void *(*)(void *, void *, void *))vtable[3];
    return fn(w->mesh, a1, a2);
}

static void *hook_wrapper_slot4(effect_wrapper *w, int a1, int a2) {
    if (!w || !w->mesh)
        return NULL;
    void **vtable = *(void ***)w->mesh;
    void *(*fn)(void *, int, int) = (void *(*)(void *, int, int))vtable[6];
    return fn(w->mesh, a1, a2);
}

static void *hook_wrapper_get_mesh_buffer(effect_wrapper *w) {
    if (!w || !w->mesh) {
        l_warn("wrapper get_mesh_buffer: NULL mesh pointer guarded");
        return NULL;
    }
    void **vtable = *(void ***)w->mesh;
    void *(*get_buf)(void *) = (void *(*)(void *))vtable[8];
    return get_buf(w->mesh);
}

// Archive (PK/GBMP) open-entry-by-name, 0x2cef88. Entries: vector of 48-byte records at +0x10/+0x14,
// name std::string (COW, char* to data) at record +4.
static void *hook_zip_open(void *zip, const char *name) {
    void *r = ((void *(*)(void *, const char *))(MACHO_SLIDE + 0x2cef88))(zip, name);
    if (!r) {
        uint8_t *b = *(uint8_t **)((uintptr_t)zip + 0x10), *e = *(uint8_t **)((uintptr_t)zip + 0x14);
        l_warn("zip open '%s' failed: %d entries, flags 0x%X", name, (int)((e - b) / 48),
               (unsigned)*(uint32_t *)((uintptr_t)zip + 0xc));
        for (uint8_t *p = b; p < e && p < b + 48 * 8; p += 48)
            l_warn("  entry '%s'", *(const char **)(p + 4));
        // Source = memory stream (vtable 0x4dbed0: +0xc data, +0x18 size, +0x1c pos)? Show where the
        // 2nd local header should be: record +0xc data offset, record +16 = 30-byte header (csize at +18).
        uint8_t *src = *(uint8_t **)((uintptr_t)zip + 8);
        if (b < e && src && *(uint32_t *)src == MACHO_SLIDE + 0x4dbed0) {
            uint8_t *data = *(uint8_t **)(src + 0xc);
            uint32_t size = *(uint32_t *)(src + 0x18), next = *(uint32_t *)(b + 0xc);
            uint32_t csize;
            memcpy(&csize, b + 16 + 18, 4);
            next += csize;
            l_warn("  memstream size %u pos %u, entry0 data@%u csize %u -> next@%u", (unsigned)size,
                   (unsigned)*(uint32_t *)(src + 0x1c), (unsigned)*(uint32_t *)(b + 0xc), (unsigned)csize,
                   (unsigned)next);
            if (next + 8 <= size)
                l_warn("  bytes@next %02X %02X %02X %02X %02X %02X %02X %02X", data[next], data[next + 1],
                       data[next + 2], data[next + 3], data[next + 4], data[next + 5], data[next + 6],
                       data[next + 7]);
            if (size >= 22) // a fully inflated zip ends with the EOCD record "PK\5\6"
                l_warn("  bytes@size-22 %02X %02X %02X %02X", data[size - 22], data[size - 21], data[size - 20],
                       data[size - 19]);
        }
    }
    return r;
}

void hook_decref_mesh(void *mesh) {
    if (!mesh)
        return;
    uint32_t *ref = (uint32_t *)((uintptr_t)mesh + 4);
    if (--(*ref) == 0) {
        void **vtable = *(void ***)mesh;
        void (*dtor)(void *) = (void (*)(void *))vtable[2];
        void (*del)(void *) = (void (*)(void *))vtable[1];
        dtor(mesh);
        del(mesh);
    }
}

// Replaces `bl 0x35c43c` + the decref at 0x233e00: a NULL stream (archive entry not found) used to
// reach the bdae parser, which then sized a never-read header -> new(garbage) -> bad_alloc (log_016).
__attribute__((naked, target("arm"))) static void hook_bdae_load_trampoline(void) {
    __asm__ volatile(
        ".arm\n"
        "cmp r4, #0\n"
        "beq 1f\n"
        "movw r12, #0xc43c\n" // MACHO_SLIDE + 0x35c43c (bdae parser), r0/r1 already set by the caller
        "movt r12, #0x9835\n"
        "blx r12\n"
        "mov r0, r4\n"
        "bl hook_decref_mesh\n"
        "1:\n"
        "sub sp, r7, #0x10\n"
        "ldm sp!, {r8}\n"
        "pop {r4, r5, r6, r7, pc}\n"
    );
}

static void patch_jump(uintptr_t addr, void *dst) {
    uint32_t patch[2] = {
        0xE51FF004, // ldr pc, [pc, #-4]
        (uint32_t)dst
    };
    kuKernelCpuUnrestrictedMemcpy((void *)addr, patch, sizeof(patch));
    kuKernelFlushCaches((void *)(addr & ~3), 16);
}

static void patch_write32(uintptr_t addr, uint32_t val) {
    kuKernelCpuUnrestrictedMemcpy((void *)addr, &val, 4);
    kuKernelFlushCaches((void *)(addr & ~3), 16);
}

// The guest's statically linked COW std::string code has its own _S_empty_rep_storage copy in __data
// (0x4b1098, referenced both by literals and PC-relative), while imported libstdc++ (cxx.c) uses the
// host one. Each side only recognizes its own as "empty" and refcounts/frees the other's: heap corruption
// (archive index with 1 entry, log_018) or free(host empty rep) (log_019).
// A huge refcount (_Rep_base: length, capacity, refcount) makes both immune: foreign decrefs never reach
// 0, foreign increfs never go negative, and "shared" makes mutations clone instead of writing in place.
extern char host_empty_rep[] __asm__("_ZNSs4_Rep20_S_empty_rep_storageE");
extern char host_empty_wrep[] __asm__("_ZNSbIwSt11char_traitsIwESaIwEE4_Rep20_S_empty_rep_storageE");

static void pin_empty_string_reps(void) {
    const int32_t pinned = 0x3FFFFFFF;
    *(volatile int32_t *)(host_empty_rep + 8) = pinned;
    *(volatile int32_t *)(host_empty_wrep + 8) = pinned;
    patch_write32(MACHO_SLIDE + 0x4b1098 + 8, pinned);
}

// Archive entry inflate (0x2ce5f0: inflateInit2_ + inflate(Z_FINISH) + inflateEnd into a full buffer).
// On console the guest zlib stopped early (log_020: DefaultEffects.bdae buffer left unwritten past the
// first entry) while the same guest code is byte-exact under emulation, so do it with host zlib.
// The guest z_stream is laid out like the host one (32-bit, 56 bytes). state = NULL afterwards turns the
// guest's following inflate/inflateEnd calls into harmless Z_STREAM_ERROR returns (ignored by the caller).
_Static_assert(sizeof(z_stream) == 56, "guest z_stream layout");
int hook_entry_inflate(z_stream *g, int window_bits) {
    z_stream z;
    memset(&z, 0, sizeof(z));
    z.next_in = g->next_in;
    z.avail_in = g->avail_in;
    z.next_out = g->next_out;
    z.avail_out = g->avail_out;
    int ret = inflateInit2(&z, window_bits);
    if (ret != Z_OK)
        return ret;
    ret = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (ret != Z_STREAM_END)
        l_warn("entry inflate: ret %d, in %lu left %u, out %lu left %u", ret, z.total_in, z.avail_in, z.total_out,
               z.avail_out);
    g->next_in = z.next_in;
    g->avail_in = z.avail_in;
    g->total_in = z.total_in;
    g->next_out = z.next_out;
    g->avail_out = z.avail_out;
    g->total_out = z.total_out;
    g->state = NULL;
    return Z_OK;
}

// Replaces `bl inflateInit2_` + `cmp r0, r5` (r5 == 0) at 0x2ce5f0, then resumes at 0x2ce5f8 (bne error).
__attribute__((naked, target("arm"))) static void hook_entry_inflate_trampoline(void) {
    __asm__ volatile(
        ".arm\n"
        "push {r4, lr}\n"
        "bl hook_entry_inflate\n"
        "pop {r4, lr}\n"
        "cmp r0, #0\n"
        "movw r12, #0xe5f8\n" // MACHO_SLIDE + 0x2ce5f8
        "movt r12, #0x982c\n"
        "bx r12\n"
    );
}

// ARMv6 SWP/SWPB is disabled on the Vita's Cortex-A9 (undefined instruction, log_021 at 0x2c1398).
// Each one becomes `b` to an arena thunk doing the same exchange with ldrex/strex, keeping flags and
// using three stacked scratch registers.
static uint32_t arm_b(uintptr_t from, uintptr_t to) {
    return 0xEA000000 | (((to - from - 8) >> 2) & 0xFFFFFF);
}

static void patch_swp(uintptr_t addr) {
    uint32_t insn = *(uint32_t *)addr;
    uint32_t rn = (insn >> 16) & 15, rt = (insn >> 12) & 15, rt2 = insn & 15, byte = insn & (1 << 22);
    uint32_t s[3], mask = 0;
    for (uint32_t r = 0, n = 0; n < 3; r++)
        if (r != rn && r != rt && r != rt2) {
            s[n++] = r;
            mask |= 1u << r;
        }
    uint32_t code[9] = {
        0xE92D0000 | mask,                                                   // push {s0-s2}
        0xE10F0000 | s[2] << 12,                                             // mrs s2, apsr
        (byte ? 0xE1D00F9F : 0xE1900F9F) | rn << 16 | s[0] << 12,            // 1: ldrex{b} s0, [rn]
        (byte ? 0xE1C00F90 : 0xE1800F90) | rn << 16 | s[1] << 12 | rt2,      // strex{b} s1, rt2, [rn]
        0xE3500000 | s[1] << 16,                                             // cmp s1, #0
        0x1AFFFFFB,                                                          // bne 1b
        0xE128F000 | s[2],                                                   // msr apsr_nzcvq, s2
        0xE1A00000 | rt << 12 | s[0],                                        // mov rt, s0
        0xE8BD0000 | mask,                                                   // pop {s0-s2}
    };
    uintptr_t thunk = (uintptr_t)macho_alloc_code(code, 9);
    uint32_t back = arm_b(thunk + 9 * 4, addr + 4);
    macho_alloc_code(&back, 1);
    patch_write32(addr, arm_b(addr, thunk));
}

static void patch_all_swp(void) {
    int n = 0;
    // ponytail: only the engine's atomics block (0x2c0e00-0x2c1800, 26 swp per objdump); a blind scan of
    // all __text could hit literal-pool words. Widen if another SWP crash shows up elsewhere.
    for (uintptr_t a = MACHO_SLIDE + 0x2c0e00; a < MACHO_SLIDE + 0x2c1800; a += 4)
        if ((*(uint32_t *)a & 0xFFB00FF0) == 0xE1000090) {
            patch_swp(a);
            n++;
        }
    l_info("swp -> ldrex/strex: %d sites", n); // expected 26
}

// Game-specific import tables for the port being developed.
// Register any guest binary symbols you wish to intercept or replace here.
const import_entry patch_imports[] = {
    // Example:
    // F(my_custom_function, my_implementation),
};
const int patch_imports_count = sizeof(patch_imports) / sizeof(patch_imports[0]);

void patch_init(void) {
    l_info("Initializing game-specific patches...");

    pin_empty_string_reps();
    patch_all_swp();

    init_dummy_texture();

    // Guard boost::intrusive_ptr<T>::operator->() against NULL dereferences (crash in log_027 at 0x63b8)
    patch_jump(MACHO_SLIDE + 0x6278, hook_intrusive_ptr_deref);
    patch_jump(MACHO_SLIDE + 0x62b8, hook_intrusive_ptr_deref);
    patch_jump(MACHO_SLIDE + 0x62f8, hook_intrusive_ptr_deref);
    patch_jump(MACHO_SLIDE + 0x6338, hook_intrusive_ptr_deref);
    patch_jump(MACHO_SLIDE + 0x6378, hook_intrusive_ptr_deref);
    patch_jump(MACHO_SLIDE + 0x63b8, hook_intrusive_ptr_deref);

    // Protect wrapper methods against NULL mesh dereference (crash 0x233934 in log_015)
    patch_jump(MACHO_SLIDE + 0x2338f0, hook_wrapper_slot2);
    patch_jump(MACHO_SLIDE + 0x233900, hook_wrapper_slot3);
    patch_jump(MACHO_SLIDE + 0x233910, hook_wrapper_slot4);
    patch_jump(MACHO_SLIDE + 0x233930, hook_wrapper_get_mesh_buffer);

    // Update wrapper vtable in __DATA.__const
    uintptr_t vtable = MACHO_SLIDE + 0x4d0724;
    patch_write32(vtable + 0x8, (uint32_t)hook_wrapper_slot2);
    patch_write32(vtable + 0xc, (uint32_t)hook_wrapper_slot3);
    patch_write32(vtable + 0x10, (uint32_t)hook_wrapper_slot4);
    patch_write32(vtable + 0x14, (uint32_t)hook_wrapper_get_mesh_buffer);

    // Skip the bdae parse when the stream is NULL (overwrites 0x233e00 and 0x233e04)
    patch_jump(MACHO_SLIDE + 0x233e00, hook_bdae_load_trampoline);

    // Decompress archive entries with host zlib (overwrites 0x2ce5f0 and 0x2ce5f4)
    patch_jump(MACHO_SLIDE + 0x2ce5f0, hook_entry_inflate_trampoline);

    // Log why archive entry lookups fail (zip/GBMP reader vtable slot 3)
    patch_write32(MACHO_SLIDE + 0x4dc20c + 0xc, (uint32_t)hook_zip_open);
}

void patch_frame(void) {
    if (!dummy_gl_tex_id) {
        uint32_t white_pixel = 0xFFFFFFFF;
        glGenTextures(1, &dummy_gl_tex_id);
        glBindTexture(GL_TEXTURE_2D, dummy_gl_tex_id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white_pixel);
        *(uint32_t *)&dummy_texture[0x48] = dummy_gl_tex_id;
    }
}

void patch_overlay(void) {}

static void add_touch(int id, int x, int y) {
    if (touch_count >= SCE_TOUCH_MAX_REPORT)
        return;
    touch_point *t = &touches[touch_count++];
    memset(t, 0, sizeof(*t));
    t->id = id;
    t->gx = x;
    t->gy = y;
    t->down = 1;
}

// Physical controls as synthetic touches in the 320x480 view:
// - SELECT button -> cycles display mode (Landscape Fullscreen -> Original Centered -> Tate Rotated)
// - Left stick / D-Pad Left/Right -> drag along the paddle line, stick X = paddle X (id 100)
// - Cross/Circle/Square/Triangle -> one tap in the middle of the play field (id 101)
// ponytail: paddle line y=440 and the stick range are guesses; calibrate on console (Gate 3).
void patch_input(void) {
    static const uint32_t btn_mask = SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE | SCE_CTRL_TRIANGLE;
    static int prev_btn;
    static uint32_t prev_buttons;

    // Toggle display mode with SELECT button
    if ((pad.buttons & SCE_CTRL_SELECT) && !(prev_buttons & SCE_CTRL_SELECT)) {
        gles_cycle_display_mode();
    }
    prev_buttons = pad.buttons;

    display_mode_t mode = gles_get_display_mode();
    int stick_val = pad.lx;
    if (mode == DISPLAY_MODE_TATE_ROTATED) {
        if (pad.ly < 108 || pad.ly > 148)
            stick_val = 255 - pad.ly;
    }

    static int dpad_x = 160;
    if (pad.buttons & SCE_CTRL_LEFT) {
        dpad_x -= 6;
        if (dpad_x < 0) dpad_x = 0;
        add_touch(100, dpad_x, 440);
    } else if (pad.buttons & SCE_CTRL_RIGHT) {
        dpad_x += 6;
        if (dpad_x > 319) dpad_x = 319;
        add_touch(100, dpad_x, 440);
    } else if (stick_val < 108 || stick_val > 148) { // dead zone around the center (128)
        dpad_x = stick_val * 319 / 255;
        add_touch(100, dpad_x, 440);
    }

    int btn = (pad.buttons & btn_mask) != 0;
    if (btn && !prev_btn)
        add_touch(101, 160, 240);
    prev_btn = btn;
}
