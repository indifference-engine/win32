#include "../library/run_event_loop.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <winuser.h>

#define ROWS 192
#define COLUMNS 256

static float opacities[COLUMNS * ROWS];
static float reds[COLUMNS * ROWS];
static float greens[COLUMNS * ROWS];
static float blues[COLUMNS * ROWS];

#define SAMPLES_PER_TICK 441
static float left[SAMPLES_PER_TICK];
static float right[SAMPLES_PER_TICK];

static int ticks;
static int samples;
static float previous_x, previous_y, next_x, next_y;

static int tick_pointer_state;
static float tick_pointer_row, tick_pointer_column;

static float calculate_x(const int ticks) {
  return (sin(ticks * 0.1) * 0.4 + 0.5) * COLUMNS;
}

static float calculate_y(const int ticks) {
  return (sin(ticks * 0.12) * 0.1 + 0.5) * ROWS;
}

static void tick(const void *const context, const int pointer_state,
                 const float pointer_row, const float pointer_column,
                 bool (*const key_held)(const void *const context,
                                        const WPARAM virtual_key_code)) {
  previous_x = next_x;
  next_x = calculate_x(ticks);
  previous_y = next_y;
  next_y = calculate_y(ticks) + (key_held(context, 87) ? 50.0f : 0.0f) -
           (key_held(context, 83) ? 50.0f : 0.0f);
  tick_pointer_state = pointer_state;
  tick_pointer_row = pointer_row;
  tick_pointer_column = pointer_column;
  ticks++;

  for (int sample = 0; sample < SAMPLES_PER_TICK; sample++) {
    const float x =
        (next_x * (sample / (float)SAMPLES_PER_TICK) +
         previous_x * ((SAMPLES_PER_TICK - sample) / (float)SAMPLES_PER_TICK) -
         COLUMNS / 2) /
        COLUMNS;

    const float unmixed = sin(samples * 0.0313487528344671);
    left[sample] = max(0, -x) * unmixed;
    right[sample] = max(0, x) * unmixed;
    samples++;
  }
}

static float linear_interpolate(const float from, const float to,
                                const float progress_unit_interval,
                                const float inverse_progress_unit_interval) {
  return from * inverse_progress_unit_interval + to * progress_unit_interval;
}

static int video_calls = 0;

static void video_pointer(const int state, const float row, const float column,
                          const float none_red, const float none_green,
                          const float none_blue, const float hover_red,
                          const float hover_green, const float hover_blue,
                          const float select_red, const float select_green,
                          const float select_blue) {
  const int rounded_row = row;

  if (row < 0) {
    return;
  }

  if (row >= ROWS) {
    return;
  }

  const int rounded_column = column;

  if (column < 0) {
    return;
  }

  if (column >= COLUMNS) {
    return;
  }

  float red, green, blue;

  switch (state) {
  case POINTER_STATE_NONE:
    red = none_red;
    green = none_green;
    blue = none_blue;
    break;

  case POINTER_STATE_HOVER:
    red = hover_red;
    green = hover_green;
    blue = hover_blue;
    break;

  default:
    red = select_red;
    green = select_green;
    blue = select_blue;
    break;
  }

  const int index = rounded_row * COLUMNS + rounded_column;
  opacities[index] = 1.0f;
  reds[index] = red;
  greens[index] = green;
  blues[index] = blue;
}

static void video(const void *const context, const int pointer_state,
                  const float pointer_row, const float pointer_column,
                  bool (*const key_held)(const void *const context,
                                         const WPARAM virtual_key_code),
                  const float tick_progress_unit_interval) {
  for (int row = 0; row < ROWS; row++) {
    for (int column = 0; column < COLUMNS; column++) {
      opacities[row * COLUMNS + column] = 0.25f;
      reds[row * COLUMNS + column] = (row + column) % 2 ? 0.2f : 0.7f;
      greens[row * COLUMNS + column] = (row * 0.3f) / ROWS;
      blues[row * COLUMNS + column] = (row * 0.9f) / ROWS;
    }
  }

  const float inverse_tick_progress_unit_interval =
      1.0f - tick_progress_unit_interval;

  const int x =
      linear_interpolate(previous_x, next_x, tick_progress_unit_interval,
                         inverse_tick_progress_unit_interval);
  const int y =
      linear_interpolate(previous_y, next_y, tick_progress_unit_interval,
                         inverse_tick_progress_unit_interval);

  for (int row = y - 2; row < y + 2; row++) {
    for (int column = x - 2; column < x + 2; column++) {
      opacities[row * COLUMNS + column] = 1.0f;
      reds[row * COLUMNS + column] = 1;
      greens[row * COLUMNS + column] = key_held(context, VK_SPACE) ? 1 : 0;
      blues[row * COLUMNS + column] = 1;
    }
  }

  greens[video_calls % 64] = 1;

  video_pointer(tick_pointer_state, tick_pointer_row, tick_pointer_column, 1, 0,
                0, 0, 1, 0, 0, 0, 1);
  video_pointer(pointer_state, pointer_row, pointer_column, 0, 1, 1, 1, 0, 1, 1,
                1, 0);

  video_calls++;
}

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine,
            int nShowCmd) {
  (void)(hInstance);
  (void)(hPrevInstance);
  (void)(lpCmdLine);

  previous_x = calculate_x(0);
  previous_y = calculate_y(0);
  next_x = calculate_x(1);
  next_y = calculate_y(1);
  ticks = 2;

  const char *const error_message = run_event_loop(
      "Example Application", 100, tick, ROWS, COLUMNS,
      MessageBox(HWND_DESKTOP,
                 "Would you like to display a transparent window?", "Example",
                 MB_YESNO | MB_ICONQUESTION) == IDYES
          ? opacities
          : NULL,
      reds, greens, blues, video, SAMPLES_PER_TICK, left, right, nShowCmd);

  if (error_message == NULL) {
    printf("Successfully completed.\n");
  } else {
    fprintf(stderr, "Error: \"%s\".\n", error_message);
  }

  return 0;
}
