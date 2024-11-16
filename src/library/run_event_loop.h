#ifndef RUN_EVENT_LOOP_H

#define RUN_EVENT_LOOP_H

#include <stdbool.h>
#include <windows.h>

/**
 * Runs an application event loop, blocking until the window is closed by the
 * user or an error occurs.
 * @param title The null-terminated UTF-8-encoded title of the application.
 * @param ticks_per_second The number of tick events raised each second.
 * @param tick Called each time a tick event occurs.
 * @param rows The height of the viewport in rows.  Behavior is undefined if
 *             less than 1.
 * @param columns The width of the viewport in columns.  Behavior is undefined
 *                if less than 1.
 * @param reds The intensity of the red channel of each pixel within the
 *             viewport, row-major, starting from the top left corner.  Behavior
 *             is undefined if any are NaN, less than 0 or greater than 1.
 * @param greens The intensity of the green channel of each pixel within the
 *               viewport, row-major, starting from the top left corner.
 *               Behavior is undefined if any are NaN, less than 0 or greater
 *               than 1.
 * @param blues The intensity of the blue channel of each pixel within the
 *              viewport, row-major, starting from the top left corner.
 *              Behavior is undefined if any are NaN, less than 0 or greater
 *              than 1.
 * @param video Called each time the viewport needs to be refreshed.  May be
 *              called prior to the first tick event.
 * @param samples_per_tick The number of audio samples generated each tick.
 *                         Behavior is undefined if less than 1.
 * @param left The left channel of the audio output, from sooner to later.
 *             Behavior is undefined if any are NaN, less than -1 or greater
 *             than 1.  Will not be output prior to the first tick.
 * @param right The right channel of the audio output, from sooner to later.
 *              Behavior is undefined if any are NaN, less than -1 or greater
 *              than 1.  Will not be output prior to the first tick.
 * @param nCmdShow As received by WinMain.
 * @return In the event of an error, a null-terminated UTF-8-encoded error
 *         message describing the problem, otherwise, null.
 */
const char *run_event_loop(
    const char *const title,
    const int ticks_per_second,
    void (*const tick)(const void *const context, bool (*const key_held)(const void *const context, const WPARAM virtual_key_code)),
    const int rows,
    const int columns,
    const float *const reds,
    const float *const greens,
    const float *const blues,
    void (*const video)(const void *const context, bool (*const key_held)(const void *const context, const WPARAM virtual_key_code), const float tick_progress_unit_interval),
    const int samples_per_tick,
    const float *const left,
    const float *const right,
    const int nCmdShow);

#endif
