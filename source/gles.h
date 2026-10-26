/**
 * @file gles.h
 * @brief OpenGL ES 1.1 / 2.0 graphics pipeline over vitaGL with SoftFP calling convention.
 */
#pragma once
#include <stdint.h>

typedef enum {
    DISPLAY_MODE_LANDSCAPE_FULL = 0,    /**< Stretched to 960x544 (Landscape) [Default]. */
    DISPLAY_MODE_ORIGINAL_CENTERED = 1, /**< 362x544 centered with black side bars (Aspect Ratio preserved). */
    DISPLAY_MODE_TATE_ROTATED = 2       /**< Rotated 90 degrees for holding PS Vita vertically (Tate mode). */
} display_mode_t;

/**
 * @brief Initializes the OpenGL ES pipeline and off-screen virtual FBO.
 */
void gles_init(void);

/**
 * @brief Presents current frame: blits and scales the portrait iOS FBO to the PS Vita 960x544 screen.
 */
void gles_present(void);

/**
 * @brief Returns the total number of draw calls dispatched this session.
 */
uint32_t gles_get_draw_calls(void);

/**
 * @brief Marks the currently bound renderbuffer as the CAEAGLLayer drawable
 *        ([EAGLContext renderbufferStorage:fromDrawable:]).
 */
void gles_bind_drawable(void);

/**
 * @brief Gets current display presentation mode.
 */
display_mode_t gles_get_display_mode(void);

/**
 * @brief Sets current display presentation mode.
 */
void gles_set_display_mode(display_mode_t mode);

/**
 * @brief Cycles through available display presentation modes.
 */
void gles_cycle_display_mode(void);

