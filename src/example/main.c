#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include "../library/run_event_loop.h"

#define ROWS 192
#define COLUMNS 256

static float reds[COLUMNS * ROWS];
static float greens[COLUMNS * ROWS];
static float blues[COLUMNS * ROWS];

#define SAMPLES_PER_TICK 441
static float left[SAMPLES_PER_TICK];
static float right[SAMPLES_PER_TICK];

static int ticks;
static int samples;
static float previous_x, previous_y, next_x, next_y;

static float calculate_x(const int ticks)
{
  return (sin(ticks * 0.1) * 0.4 + 0.5) * COLUMNS;
}

static float calculate_y(const int ticks)
{
  return (sin(ticks * 0.12) * 0.1 + 0.5) * ROWS;
}

static void tick(const void *const context, bool (*const key_held)(const void *const context, const WPARAM virtual_key_code))
{
  previous_x = next_x;
  next_x = calculate_x(ticks);
  previous_y = next_y;
  next_y = calculate_y(ticks) + (key_held(context, 87) ? 50.0f : 0.0f) - (key_held(context, 83) ? 50.0f : 0.0f);
  ticks++;

  for (int sample = 0; sample < SAMPLES_PER_TICK; sample++)
  {
    const float x = (next_x * (sample / (float)SAMPLES_PER_TICK) + previous_x * ((SAMPLES_PER_TICK - sample) / (float)SAMPLES_PER_TICK) - COLUMNS / 2) / COLUMNS;

    const float unmixed = sin(samples * 0.0313487528344671);
    left[sample] = max(0, -x) * unmixed;
    right[sample] = max(0, x) * unmixed;
    samples++;
  }
}

static float linear_interpolate(const float from, const float to, const float progress_unit_interval, const float inverse_progress_unit_interval)
{
  return from * inverse_progress_unit_interval + to * progress_unit_interval;
}

static int video_calls = 0;

static void video(const void *const context, bool (*const key_held)(const void *const context, const WPARAM virtual_key_code), const float tick_progress_unit_interval)
{
  for (int row = 0; row < ROWS; row++)
  {
    for (int column = 0; column < COLUMNS; column++)
    {
      reds[row * COLUMNS + column] = 0.2f;
      greens[row * COLUMNS + column] = (row * 0.3f) / ROWS;
      blues[row * COLUMNS + column] = (row * 0.9f) / ROWS;
    }
  }

  const float inverse_tick_progress_unit_interval = 1.0f - tick_progress_unit_interval;

  const int x = linear_interpolate(previous_x, next_x, tick_progress_unit_interval, inverse_tick_progress_unit_interval);
  const int y = linear_interpolate(previous_y, next_y, tick_progress_unit_interval, inverse_tick_progress_unit_interval);

  for (int row = y - 2; row < y + 2; row++)
  {
    for (int column = x - 2; column < x + 2; column++)
    {
      reds[row * COLUMNS + column] = 1;
      greens[row * COLUMNS + column] = key_held(context, VK_SPACE) ? 1 : 0;
      blues[row * COLUMNS + column] = 1;
    }
  }

  greens[video_calls % 64] = 1;

  video_calls++;
}

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
  (void)(hInstance);
  (void)(hPrevInstance);
  (void)(lpCmdLine);

  previous_x = calculate_x(0);
  previous_y = calculate_y(0);
  next_x = calculate_x(1);
  next_y = calculate_y(1);
  ticks = 2;

  const char *const error_message = run_event_loop(
      "Example Application",
      100,
      tick,
      ROWS,
      COLUMNS,
      reds,
      greens,
      blues,
      video,
      SAMPLES_PER_TICK,
      left,
      right,
      nShowCmd);

  if (error_message == NULL)
  {
    printf("Successfully completed.\n");
  }
  else
  {
    fprintf(stderr, "Error: \"%s\".\n", error_message);
  }

  return 0;
}
