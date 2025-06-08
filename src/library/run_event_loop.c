#include "run_event_loop.h"
#include <dwmapi.h>
#include <math.h>
#include <mmreg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
#include <windowsx.h>
#include <winuser.h>

#define VSYNC_CONTEXT_STATE_STARTING 0
#define VSYNC_CONTEXT_STATE_RUNNING 1
#define VSYNC_CONTEXT_STATE_STOPPING 2
#define VSYNC_CONTEXT_STATE_STOPPED 3

#define OPAQUE_WS WS_OVERLAPPEDWINDOW
#define TRANSPARENT_WS (WS_POPUP | WS_THICKFRAME)

typedef struct {
  const HWND hwnd;
  int state;
  const char *error;
  CRITICAL_SECTION critical_section;
} vsync_context;

typedef struct {
  const int ticks_per_second;
  void (*const tick)(const void *const context, const int pointer_state,
                     const float pointer_row, const float pointer_column,
                     bool (*const key_held)(const void *const context,
                                            const WPARAM virtual_key_code));
  const int rows;
  const int columns;
  const int skipped_bytes_per_row;
  const float *const opacities;
  const float *const reds;
  const float *const greens;
  const float *const blues;
  void (*const video)(const void *const context, const int pointer_state,
                      const float pointer_row, const float pointer_column,
                      bool (*const key_held)(const void *const context,
                                             const WPARAM virtual_key_code),
                      const float tick_progress_unit_interval);
  const int samples_per_tick;
  const float *const left;
  const float *const right;
  const char *error;
  void *const scratch;
  HWAVEOUT hwaveout;
  int next_buffer;
  const int buffers;
  DWORD minimum_position;
  WPARAM *held_virtual_key_codes;
  int number_of_held_virtual_key_codes;
  int position_x;
  int position_y;
  int scaled_width;
  int scaled_height;
  int x_offset;
  int y_offset;
  int inverse_x_offset;
  int inverse_y_offset;
  int pointer_state;
  float pointer_row;
  float pointer_column;
  bool audio_paused;
} context;

static bool key_held(const void *const _context,
                     const WPARAM virtual_key_code) {
  const context *const our_context = (context *)_context;
  const int number_of_held_virtual_key_codes =
      our_context->number_of_held_virtual_key_codes;
  const WPARAM *const held_virtual_key_codes =
      our_context->held_virtual_key_codes;

  for (int index = 0; index < number_of_held_virtual_key_codes; index++) {
    if (virtual_key_code == held_virtual_key_codes[index]) {
      return true;
    }
  }

  return false;
}

static const char *video(const context *const context) {
  const HWAVEOUT hwaveout = context->hwaveout;

  if (hwaveout == NULL) {
    context->video(context, context->pointer_state, context->pointer_row,
                   context->pointer_column, key_held, 0.0f);
  } else {

    MMTIME mmtime = {.wType = TIME_SAMPLES};

    if (waveOutGetPosition(hwaveout, &mmtime, sizeof(mmtime)) !=
        MMSYSERR_NOERROR) {
      return "Failed to get wave out position.";
    }

    if (mmtime.wType != TIME_SAMPLES) {
      return "Wave out position does not support sample time.";
    }

    const int samples_per_tick = context->samples_per_tick;

    const DWORD minimum_position = context->minimum_position;

    const DWORD position = mmtime.u.sample;
    const DWORD elapsed = position < minimum_position
                              ? position + ((4294967295 - minimum_position) + 1)
                              : position - minimum_position;

    context->video(context, context->pointer_state, context->pointer_row,
                   context->pointer_column, key_held,
                   max(0.0f, min(1.0f, elapsed / (float)samples_per_tick)));
  }

  return NULL;
}

static LRESULT handle_mouse_event(const HWND hwnd, const UINT uMsg,
                                  const WPARAM wParam, const LPARAM lParam,
                                  context *const context) {
  int x = GET_X_LPARAM(lParam);
  int y = GET_Y_LPARAM(lParam);

  if (context->opacities != NULL) {
    RECT insets = {0, 0, 0, 0};

    if (AdjustWindowRect(&insets, TRANSPARENT_WS, FALSE)) {
      x -= insets.left;
      y -= insets.top;
    } else {
      context->error = "Failed to calculate the dimensions of the window.";
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
  }

  context->pointer_state =
      wParam & MK_LBUTTON ? POINTER_STATE_SELECT : POINTER_STATE_HOVER;
  context->pointer_row = ((float)((y - context->y_offset) * context->rows)) /
                         ((float)context->scaled_height);
  context->pointer_column =
      ((float)((x - context->x_offset) * context->columns)) /
      ((float)context->scaled_width);

  TRACKMOUSEEVENT event_track = {
      .cbSize = sizeof(TRACKMOUSEEVENT),
      .dwFlags = TME_LEAVE,
      .hwndTrack = hwnd,
      .dwHoverTime = HOVER_DEFAULT,
  };

  if (TrackMouseEvent(&event_track)) {
    return 0;
  } else {
    context->error = "Failed to track the mouse.";
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

static const char *refresh_layered(const HWND hwnd,
                                   const context *const context) {
  const char *const error = video(context);

  if (error != NULL) {
    return error;
  }

  const int rows = context->rows;
  const int y_offset = context->y_offset;
  const int scaled_height = context->scaled_height;
  const int position_y = context->position_y;
  const int columns = context->columns;
  const int x_offset = context->x_offset;
  const int scaled_width = context->scaled_width;
  const int position_x = context->position_x;
  const float *const blues = context->blues;
  const float *const greens = context->greens;
  const float *const reds = context->reds;
  const float *const opacities = context->opacities;
  uint8_t *const scratch = context->scratch;
  const int pixels = rows * columns;
  uint8_t *const scratch_blues = scratch;
  uint8_t *const scratch_greens = scratch_blues + pixels;
  uint8_t *const scratch_reds = scratch_greens + pixels;
  uint8_t *const scratch_opacities = scratch_reds + pixels;

  BITMAPINFO bitmapinfo = {
      .bmiHeader =
          {
              .biSize = sizeof(BITMAPINFOHEADER),
              .biWidth = scaled_width,
              .biHeight = -scaled_height,
              .biPlanes = 1,
              .biBitCount = 32,
              .biCompression = BI_RGB,
              .biSizeImage = 0,
              .biXPelsPerMeter = 0,
              .biYPelsPerMeter = 0,
              .biClrUsed = 0,
              .biClrImportant = 0,
          },
      .bmiColors =
          {{.rgbRed = 0, .rgbGreen = 0, .rgbBlue = 0, .rgbReserved = 0}},
  };

  HDC screen_hdc = GetDC(NULL);

  if (screen_hdc == NULL) {
    return "Failed to get a DC for the screen.";
  }

  uint8_t *pixel_bytes = NULL;
  HBITMAP hBitmap = CreateDIBSection(screen_hdc, &bitmapinfo, DIB_RGB_COLORS,
                                     (void **)&pixel_bytes, NULL, 0);

  if (hBitmap == NULL) {
    if (ReleaseDC(NULL, screen_hdc)) {
      return "Failed to create a DIB section.";
    } else {
      return "Failed to create a DIB section.  Additionally failed to release "
             "a DC for the screen.";
    }
  }

  HDC hdcMem = CreateCompatibleDC(screen_hdc);

  if (hdcMem == NULL) {
    if (DeleteObject(hBitmap)) {
      if (ReleaseDC(NULL, screen_hdc)) {
        return "Failed to create a compatible DC.";
      } else {
        return "Failed to create a compatible DC.  Additionally failed to "
               "release a DC for the screen.";
      }
    } else {
      if (ReleaseDC(NULL, screen_hdc)) {
        return "Failed to create a compatible DC.  Additionally failed to "
               "delete a DIB section.";
      } else {
        return "Failed to create a compatible DC.  Additionally failed to "
               "delete a DIB section and release a DC for the screen.";
      }
    }
  }

  HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBitmap);

  if (hOld == NULL) {
    if (DeleteObject(hdcMem)) {
      if (DeleteObject(hBitmap)) {
        if (ReleaseDC(NULL, screen_hdc)) {
          return "Failed to select a compatible DC.";
        } else {
          return "Failed to select a compatible DC.  Additionally failed to "
                 "release a DC for the screen.";
        }
      } else {
        if (ReleaseDC(NULL, screen_hdc)) {
          return "Failed to select a compatible DC.  Additionally failed to "
                 "delete a DIB section.";
        } else {
          return "Failed to select a compatible DC.  Additionally failed to "
                 "delete a DIB section and release a DC for the screen.";
        }
      }
    } else {
      if (DeleteObject(hBitmap)) {
        if (ReleaseDC(NULL, screen_hdc)) {
          return "Failed to select a compatible DC.  Additionally failed to "
                 "delete a compatible DC.";
        } else {
          return "Failed to select a compatible DC.  Additionally failed to "
                 "delete a compatible DC and release a DC for the screen.";
        }
      } else {
        if (ReleaseDC(NULL, screen_hdc)) {
          return "Failed to select a compatible DC.  Additionally failed to "
                 "delete a compatible DC and delete a DIB section.";
        } else {
          return "Failed to select a compatible DC.  Additionally failed to "
                 "delete a compatible DC and delete a DIB section and release "
                 "a DC for the screen.";
        }
      }
    }
  }

  for (int source_index = 0; source_index < pixels; source_index++) {
    const float opacity = opacities[source_index] * 255.0f;
    scratch_blues[source_index] = blues[source_index] * opacity;
    scratch_greens[source_index] = greens[source_index] * opacity;
    scratch_reds[source_index] = reds[source_index] * opacity;
    scratch_opacities[source_index] = opacity;
  }

  const float y_per_row = ((float)rows) / ((float)scaled_height);
  const int rows_minus_one = rows - 1;
  const float x_per_column = ((float)columns) / ((float)scaled_width);
  const int columns_minus_one = columns - 1;
  int destination_index = 0;

  for (int row = 0; row < scaled_height; row++) {
    int y = row * y_per_row;

    if (y < 0) {
      y = 0;
    }

    if (y > rows_minus_one) {
      y = rows_minus_one;
    }

    const int y_index = y * columns;

    for (int column = 0; column < scaled_width; column++) {
      int x = column * x_per_column;

      if (x < 0) {
        x = 0;
      }

      if (x > columns_minus_one) {
        x = columns_minus_one;
      }

      const int source_index = y_index + x;

      pixel_bytes[destination_index++] = scratch_blues[source_index];
      pixel_bytes[destination_index++] = scratch_greens[source_index];
      pixel_bytes[destination_index++] = scratch_reds[source_index];
      pixel_bytes[destination_index++] = scratch_opacities[source_index];
    }
  }

  (void)(x_offset);
  (void)(y_offset);
  POINT ptPos = {position_x, position_y};
  SIZE sizeWnd = {scaled_width, scaled_height};
  POINT ptSrc = {0, 0};

  BLENDFUNCTION blend = {0};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;

  if (UpdateLayeredWindow(hwnd, screen_hdc, &ptPos, &sizeWnd, hdcMem, &ptSrc, 0,
                          &blend, ULW_ALPHA) == 0) {
    if (SelectObject(hdcMem, hOld) == NULL) {
      if (DeleteObject(hdcMem)) {
        if (DeleteObject(hBitmap)) {
          if (ReleaseDC(NULL, screen_hdc)) {
            return "Failed to update a layered window.  Additionally failed to "
                   "revert the selection of a compatible DC.";
          } else {
            return "Failed to update a layered window.  Additionally failed to "
                   "revert the selection of a compatible DC and release a DC "
                   "for the screen.";
          }
        } else {
          if (ReleaseDC(NULL, screen_hdc)) {
            return "Failed to update a layered window.  Additionally failed to "
                   "revert the selection of a compatible DC and delete a DIB "
                   "section.";
          } else {
            return "Failed to update a layered window.  Additionally failed to "
                   "revert the selection of a compatible DC, delete a DIB "
                   "section and release a DC for the screen.";
          }
        }
      } else {
        if (DeleteObject(hBitmap)) {
          if (ReleaseDC(NULL, screen_hdc)) {
            return "Failed to update a layered window.  Additionally failed to "
                   "revert the selection of a compatible DC and delete a "
                   "compatible DC.";
          } else {
            return "Failed to update a layered window.  Additionally failed to "
                   "revert the selection of a compatible DC , delete a "
                   "compatible DC and release a DC for the screen.";
          }
        } else {
          if (ReleaseDC(NULL, screen_hdc)) {
            return "Failed to update a layered window.  Additionally failed to "
                   "revert the selection of a compatible DC, delete a "
                   "compatible DC and delete a DIB section.";
          } else {
            return "Failed to update a layered window.  Additionally failed to "
                   "revert the selection of a compatible DC, delete a "
                   "compatible DC, delete a DIB section and release a DC for "
                   "the screen.";
          }
        }
      }
    } else {
      if (DeleteObject(hdcMem)) {
        if (DeleteObject(hBitmap)) {
          if (ReleaseDC(NULL, screen_hdc)) {
            return "Failed to update a layered window.";
          } else {
            return "Failed to update a layered window.  Additionally failed to "
                   "release a DC for the screen.";
          }
        } else {
          if (ReleaseDC(NULL, screen_hdc)) {
            return "Failed to update a layered window.  Additionally failed to "
                   "delete a DIB section.";
          } else {
            return "Failed to update a layered window.  Additionally failed to "
                   "delete a DIB section and release a DC for the screen.";
          }
        }
      } else {
        if (DeleteObject(hBitmap)) {
          if (ReleaseDC(NULL, screen_hdc)) {
            return "Failed to update a layered window.  Additionally failed to "
                   "delete a compatible DC.";
          } else {
            return "Failed to update a layered window.  Additionally failed to "
                   "delete a compatible DC and release a DC for the screen.";
          }
        } else {
          if (ReleaseDC(NULL, screen_hdc)) {
            return "Failed to update a layered window.  Additionally failed to "
                   "delete a compatible DC and delete a DIB section.";
          } else {
            return "Failed to update a layered window.  Additionally failed to "
                   "delete a compatible DC, delete a DIB section and release "
                   "a DC for the screen.";
          }
        }
      }
    }
  }

  if (SelectObject(hdcMem, hOld) == NULL) {
    if (DeleteObject(hdcMem)) {
      if (DeleteObject(hBitmap)) {
        if (ReleaseDC(NULL, screen_hdc)) {
          return "Failed to revert the selection of a compatible DC.";
        } else {
          return "Failed to revert the selection of a compatible DC.  "
                 "Additionally failed to release a DC for the screen.";
        }
      } else {
        if (ReleaseDC(NULL, screen_hdc)) {
          return "Failed to revert the selection of a compatible DC.  "
                 "Additionally failed to delete a DIB section.";
        } else {
          return "Failed to revert the selection of a compatible DC.  "
                 "Additionally failed to delete a DIB section and release a DC "
                 "for the screen.";
        }
      }
    } else {
      if (DeleteObject(hBitmap)) {
        if (ReleaseDC(NULL, screen_hdc)) {
          return "Failed to revert the selection of a compatible DC.  "
                 "Additionally failed to delete a compatible DC.";
        } else {
          return "Failed to revert the selection of a compatible DC.  "
                 "Additionally failed to delete a compatible DC and release a "
                 "DC for the screen.";
        }
      } else {
        if (ReleaseDC(NULL, screen_hdc)) {
          return "Failed to revert the selection of a compatible DC.  "
                 "Additionally failed to delete a compatible DC and delete a "
                 "DIB section.";
        } else {
          return "Failed to revert the selection of a compatible DC.  "
                 "Additionally failed to delete a compatible DC and delete a "
                 "DIB section and release a DC for the screen.";
        }
      }
    }
  }

  if (!DeleteObject(hdcMem)) {
    if (DeleteObject(hBitmap)) {
      if (ReleaseDC(NULL, screen_hdc)) {
        return "Failed to delete a compatible DC.";
      } else {
        return "Failed to delete a compatible DC.  Additionally failed to "
               "release a DC for the screen.";
      }
    } else {
      if (ReleaseDC(NULL, screen_hdc)) {
        return "Failed to delete a compatible DC.  Additionally failed to "
               "delete a DIB section.";
      } else {
        return "Failed to delete a compatible DC.  Additionally failed to "
               "delete a DIB section and release a DC for the screen.";
      }
    }
  }

  if (!DeleteObject(hBitmap)) {
    if (ReleaseDC(NULL, screen_hdc)) {
      return "Failed to delete a DIB section.";
    } else {
      return "Failed to delete a DIB section.  Additionally failed to release "
             "a DC for the screen.";
    }
  }

  if (!ReleaseDC(NULL, screen_hdc)) {
    return "Failed to release a DC for the screen.";
  }

  return NULL;
}

static LRESULT repaint(const HWND hwnd, const UINT uMsg, const WPARAM wParam,
                       const LPARAM lParam, context *const context) {
  if (context->audio_paused) {
    if (waveOutRestart(context->hwaveout) != MMSYSERR_NOERROR) {
      context->error = "Failed to restart wave out.";
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    } else {
      context->audio_paused = false;
    }
  }

  if (context->opacities == NULL) {
    if (InvalidateRect(hwnd, NULL, FALSE) == 0) {
      context->error = "Failed to invalidate the window.";
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    } else {
      return 0;
    }
  } else {
    context->error = refresh_layered(hwnd, context);

    return context->error == NULL ? 0
                                  : DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

static LRESULT CALLBACK window_procedure(const HWND hwnd, const UINT uMsg,
                                         const WPARAM wParam,
                                         const LPARAM lParam) {
  if (uMsg == WM_CREATE) {
    context *const our_context = ((CREATESTRUCT *)lParam)->lpCreateParams;
    if (SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)our_context) == 0) {
      ((context *)lParam)->error = "Failed to record the window context.";
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
  }

  context *const our_context = (context *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

  if (our_context == NULL || our_context->error != NULL) {
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }

  switch (uMsg) {
  case MM_WOM_DONE: {
    const int samples_per_tick = our_context->samples_per_tick;
    const int next_buffer = our_context->next_buffer;
    const int buffers = our_context->buffers;
    HWAVEOUT hwaveout = our_context->hwaveout;

    float *const start_of_buffers =
        (float *)(((uint8_t *)our_context->scratch) +
                  our_context->rows *
                      (our_context->opacities == NULL
                           ? (our_context->columns * 3 +
                              our_context->skipped_bytes_per_row)
                           : (our_context->columns * 4)));
    float *buffer = start_of_buffers + samples_per_tick * 2 * next_buffer;
    const float *left = our_context->left;
    const float *right = our_context->right;

    WAVEHDR *wavehdr =
        ((WAVEHDR *)(start_of_buffers + samples_per_tick * 2 * buffers)) +
        next_buffer;

    if (waveOutUnprepareHeader(hwaveout, wavehdr, sizeof(WAVEHDR)) !=
        MMSYSERR_NOERROR) {
      our_context->error = "Failed to unprepare wave out.";
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    our_context->tick(our_context, our_context->pointer_state,
                      our_context->pointer_row, our_context->pointer_column,
                      key_held);

    for (int index = 0; index < samples_per_tick; index++) {
      *buffer = *left;
      buffer++;
      left++;

      *buffer = *right;
      buffer++;
      right++;
    }

    if (waveOutPrepareHeader(hwaveout, wavehdr, sizeof(WAVEHDR)) !=
        MMSYSERR_NOERROR) {
      our_context->error = "Failed to prepare wave out.";
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    if (waveOutWrite(hwaveout, wavehdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
      our_context->error = "Failed to write wave out.";
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    our_context->next_buffer = (next_buffer + 1) % buffers;

    const DWORD previous_minimum_position = our_context->minimum_position;
    const DWORD distance_to_end = (4294967295 - previous_minimum_position) + 1;

    if (distance_to_end <= (DWORD)samples_per_tick) {
      our_context->minimum_position = samples_per_tick - distance_to_end;
    } else {
      our_context->minimum_position =
          previous_minimum_position + samples_per_tick;
    }

    return 0;
  }

  case WM_PAINT: {
    if (our_context->audio_paused) {
      if (waveOutRestart(our_context->hwaveout) != MMSYSERR_NOERROR) {
        our_context->error = "Failed to restart wave out.";
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
      } else {
        our_context->audio_paused = false;
      }
    }

    if (our_context->opacities == NULL) {
      PAINTSTRUCT paint;
      HDC hdc = BeginPaint(hwnd, &paint);

      if (hdc == NULL) {
        our_context->error = "Failed to begin painting.";
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
      }

      our_context->error = video(our_context);

      if (our_context->error != NULL) {
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
      }

      const int rows = our_context->rows;
      const int columns = our_context->columns;
      const int skipped_bytes_per_row = our_context->skipped_bytes_per_row;
      const float *const reds = our_context->reds;
      const float *const greens = our_context->greens;
      const float *const blues = our_context->blues;
      uint8_t *const pixels = our_context->scratch;

      int input = 0;
      int output = 0;

      for (int row = 0; row < rows; row++) {
        for (int column = 0; column < columns; column++) {
          pixels[output++] = blues[input] * 255.0f;
          pixels[output++] = greens[input] * 255.0f;
          pixels[output++] = reds[input] * 255.0f;
          input++;
        }

        output += skipped_bytes_per_row;
      }

      BITMAPINFO bitmapinfo = {.bmiHeader = {
                                   sizeof(BITMAPINFO),
                                   columns,
                                   -rows,
                                   1,
                                   24,
                                   BI_RGB,
                                   0,
                                   0,
                                   0,
                                   0,
                                   0,
                               }};

      if (SelectObject(hdc, GetStockObject(NULL_PEN)) == NULL) {
        EndPaint(hwnd, &paint);
        our_context->error = "Failed to set the pen.";
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
      }

      if (SelectObject(hdc, GetStockObject(BLACK_BRUSH)) == NULL) {
        EndPaint(hwnd, &paint);
        our_context->error = "Failed to set the brush.";
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
      }

      const int x_offset = our_context->x_offset;
      const int scaled_width = our_context->scaled_width;
      const int inverse_x_offset = our_context->inverse_x_offset;
      const int destination_width = x_offset + scaled_width + inverse_x_offset;
      const int y_offset = our_context->y_offset;
      const int scaled_height = our_context->scaled_height;
      const int inverse_y_offset = our_context->inverse_y_offset;
      const int destination_height =
          y_offset + scaled_height + inverse_y_offset;

      if (x_offset > 0) {
        if (!Rectangle(hdc, 0, 0, x_offset + 1, destination_height)) {
          EndPaint(hwnd, &paint);
          our_context->error = "Failed draw the left border.";
          return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
      }

      if (inverse_x_offset > 0) {
        if (!Rectangle(hdc, destination_width - inverse_x_offset, 0,
                       destination_width, destination_height)) {
          EndPaint(hwnd, &paint);
          our_context->error = "Failed draw the right border.";
          return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
      }

      if (y_offset > 0) {
        if (!Rectangle(hdc, x_offset, 0, destination_width - inverse_x_offset,
                       y_offset + 1)) {
          EndPaint(hwnd, &paint);
          our_context->error = "Failed draw the top border.";
          return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
      }

      if (inverse_y_offset > 0) {
        if (!Rectangle(hdc, x_offset, destination_height - inverse_y_offset,
                       destination_width - inverse_x_offset,
                       destination_height)) {
          EndPaint(hwnd, &paint);
          our_context->error = "Failed draw the bottom border.";
          return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }
      }

      if (StretchDIBits(hdc, x_offset, y_offset, scaled_width, scaled_height, 0,
                        0, columns, rows, pixels, &bitmapinfo, DIB_RGB_COLORS,
                        SRCCOPY) == 0) {
        EndPaint(hwnd, &paint);
        our_context->error = "Failed to paint the framebuffer.";
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
      }

      EndPaint(hwnd, &paint);
      return 0;
    } else {
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
  }

  case WM_WINDOWPOSCHANGED: {
    const WINDOWPOS *const windowpos = (const WINDOWPOS *const)lParam;

    int width = windowpos->cx;
    int height = windowpos->cy;

    if (our_context->opacities == NULL) {
      RECT insets = {0, 0, 0, 0};

      if (AdjustWindowRect(&insets, OPAQUE_WS, FALSE)) {
        width += insets.left;
        height += insets.top;
      } else {
        our_context->error =
            "Failed to calculate the dimensions of the window.";
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
      }
    }

    const int columns = our_context->columns;
    const int rows = our_context->rows;
    const double x_scale = (double)width / columns;
    const double y_scale = (double)height / rows;
    const double scale = x_scale < y_scale ? x_scale : y_scale;
    const int scaled_width = columns * scale;
    our_context->scaled_width = scaled_width;
    const int scaled_height = rows * scale;
    our_context->scaled_height = scaled_height;
    const int x_offset = (width - scaled_width) / 2;
    our_context->x_offset = x_offset;
    const int y_offset = (height - scaled_height) / 2;
    our_context->y_offset = y_offset;
    our_context->inverse_x_offset = width - scaled_width - x_offset;
    our_context->inverse_y_offset = height - scaled_height - y_offset;

    our_context->position_x = windowpos->x;
    our_context->position_y = windowpos->y;

    return repaint(hwnd, uMsg, wParam, lParam, our_context);
  }

  case WM_APP:
    return repaint(hwnd, uMsg, wParam, lParam, our_context);

  case WM_GETMINMAXINFO: {
    LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;

    RECT insets = {0, 0, 0, 0};

    if (AdjustWindowRect(&insets,
                         our_context->opacities == NULL ? OPAQUE_WS
                                                        : TRANSPARENT_WS,
                         FALSE)) {
      lpMMI->ptMinTrackSize.x =
          our_context->columns + insets.right - insets.left;
      lpMMI->ptMinTrackSize.y = our_context->rows + insets.bottom - insets.top;

      const int cxmaxtrack = GetSystemMetrics(SM_CXMAXTRACK);

      if (cxmaxtrack == 0) {
        our_context->error =
            "Failed to retrieve the maximum width of a window.";
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
      }

      const int cymaxtrack = GetSystemMetrics(SM_CYMAXTRACK);

      if (cymaxtrack == 0) {
        our_context->error =
            "Failed to retrieve the maximum height of a window.";
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
      }

      float maximum_x_scale = (float)(cxmaxtrack + insets.left - insets.right) /
                              our_context->columns;
      float maximum_y_scale =
          (float)(cymaxtrack + insets.top - insets.bottom) / our_context->rows;
      float maximum_scale = min(maximum_x_scale, maximum_y_scale);

      lpMMI->ptMaxTrackSize.x =
          maximum_scale * our_context->columns + insets.right - insets.left;
      lpMMI->ptMaxTrackSize.y =
          maximum_scale * our_context->rows + insets.bottom - insets.top;

      return 0;
    } else {
      our_context->error = "Failed to calculate the dimensions of the window.";
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
  }

  case WM_SIZING: {
    PRECT outer = (PRECT)lParam;

    RECT insets = {0, 0, 0, 0};

    if (AdjustWindowRect(&insets,
                         our_context->opacities == NULL ? OPAQUE_WS
                                                        : TRANSPARENT_WS,
                         FALSE)) {
      RECT inner = {outer->left - insets.left, outer->top - insets.top,
                    outer->right - insets.right, outer->bottom - insets.bottom};

      int inner_width = inner.right - inner.left;
      int inner_height = inner.bottom - inner.top;

      switch (wParam) {
      case WMSZ_TOP:
      case WMSZ_BOTTOM: {
        int scaled_inner_width =
            inner_height * our_context->columns / our_context->rows;

        int width_change = scaled_inner_width - inner_width;

        outer->left -= width_change / 2;
        outer->right += width_change / 2;
        break;
      }

      case WMSZ_LEFT:
      case WMSZ_RIGHT: {
        int scaled_inner_height =
            inner_width * our_context->rows / our_context->columns;

        int height_change = scaled_inner_height - inner_height;

        outer->bottom += height_change;
        break;
      }

      case WMSZ_BOTTOMLEFT:
      case WMSZ_BOTTOMRIGHT:
      case WMSZ_TOPLEFT:
      case WMSZ_TOPRIGHT: {
        float x_scale_factor = (float)inner_width / our_context->columns;
        float y_scale_factor = (float)inner_height / our_context->rows;

        float scale_factor = max(x_scale_factor, y_scale_factor);

        int scaled_inner_width = scale_factor * our_context->columns;
        int scaled_inner_height = scale_factor * our_context->rows;

        int width_change = scaled_inner_width - inner_width;
        int height_change = scaled_inner_height - inner_height;

        switch (wParam) {
        case WMSZ_BOTTOMLEFT:
          outer->bottom += height_change;
          outer->left -= width_change;
          break;

        case WMSZ_BOTTOMRIGHT:
          outer->bottom += height_change;
          outer->right += width_change;
          break;

        case WMSZ_TOPLEFT:
          outer->top -= height_change;
          outer->left -= width_change;
          break;

        case WMSZ_TOPRIGHT:
          outer->top -= height_change;
          outer->right += width_change;
          break;
        }

        break;
      }
      }

      return 0;
    } else {
      our_context->error = "Failed to calculate the dimensions of the window.";
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
  }

  case WM_KEYDOWN: {
    const int number_of_held_virtual_key_codes =
        our_context->number_of_held_virtual_key_codes;
    WPARAM *held_virtual_key_codes = our_context->held_virtual_key_codes;

    for (int index = 0; index < number_of_held_virtual_key_codes; index++) {
      if (held_virtual_key_codes[index] == wParam) {
        return 0;
      }
    }

    if (number_of_held_virtual_key_codes) {
      held_virtual_key_codes =
          realloc(held_virtual_key_codes,
                  sizeof(WPARAM) * (number_of_held_virtual_key_codes + 1));
      held_virtual_key_codes[number_of_held_virtual_key_codes] = wParam;
      our_context->held_virtual_key_codes = held_virtual_key_codes;
      our_context->number_of_held_virtual_key_codes =
          number_of_held_virtual_key_codes + 1;
    } else {
      held_virtual_key_codes = malloc(sizeof(WPARAM));
      held_virtual_key_codes[0] = wParam;
      our_context->held_virtual_key_codes = held_virtual_key_codes;
      our_context->number_of_held_virtual_key_codes = 1;
    }

    return 0;
  }

  case WM_KEYUP: {
    const int number_of_held_virtual_key_codes =
        our_context->number_of_held_virtual_key_codes;
    WPARAM *held_virtual_key_codes = our_context->held_virtual_key_codes;

    for (int index = 0; index < number_of_held_virtual_key_codes; index++) {
      if (held_virtual_key_codes[index] == wParam) {
        if (number_of_held_virtual_key_codes == 1) {
          free(held_virtual_key_codes);
          our_context->held_virtual_key_codes = NULL;
          our_context->number_of_held_virtual_key_codes = 0;
        } else {
          memmove(held_virtual_key_codes + index,
                  held_virtual_key_codes + index + 1,
                  sizeof(WPARAM) *
                      (number_of_held_virtual_key_codes - index - 1));
          held_virtual_key_codes =
              realloc(held_virtual_key_codes,
                      sizeof(WPARAM) * (number_of_held_virtual_key_codes - 1));
          our_context->held_virtual_key_codes = held_virtual_key_codes;
          our_context->number_of_held_virtual_key_codes =
              number_of_held_virtual_key_codes - 1;
        }

        break;
      }
    }

    return 0;
  }

  case WM_LBUTTONDOWN:
    SetCapture(hwnd);
    return handle_mouse_event(hwnd, uMsg, wParam, lParam, our_context);

  case WM_MOUSEMOVE:
    return handle_mouse_event(hwnd, uMsg, wParam, lParam, our_context);

  case WM_LBUTTONUP:
    if (ReleaseCapture()) {
      return handle_mouse_event(hwnd, uMsg, wParam, lParam, our_context);
    } else {
      our_context->error = "Failed to release the capture of the mouse.";
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

  case WM_MOUSELEAVE: {
    our_context->pointer_state = POINTER_STATE_NONE;
    return 0;
  }

  case WM_NCLBUTTONDOWN:
  case WM_NCRBUTTONDOWN: {
    if ((wParam == HTCAPTION || wParam == HTMAXBUTTON ||
         wParam == HTMINBUTTON || wParam == HTCLOSE) &&
        !our_context->audio_paused) {
      if (waveOutPause(our_context->hwaveout) == MMSYSERR_NOERROR) {
        our_context->audio_paused = true;
      } else {
        our_context->error = "Failed to pause wave out.";
      }
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }

  case WM_DESTROY:
    exit(0);

  default:
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }
}

DWORD WINAPI vsync_thread(LPVOID lpParam) {
  vsync_context *const context = (vsync_context *)lpParam;
  const HWND hwnd = context->hwnd;

  EnterCriticalSection(&context->critical_section);

  if (context->state == VSYNC_CONTEXT_STATE_STARTING) {
    context->state = VSYNC_CONTEXT_STATE_RUNNING;

    while (context->state == VSYNC_CONTEXT_STATE_RUNNING) {
      LeaveCriticalSection(&context->critical_section);

      if (DwmFlush() == S_OK) {
        if (SendMessage(hwnd, WM_APP, 0, 0)) {
          // NOTE: As far as is known, this can only happen if the window
          //       unexpectedly closes, in which case, the main thread will
          //       already be awaiting our exit.
          //       In any other scenario which hits this branch, the application
          //       will freeze until the next window message (e.g. mouse input).
          EnterCriticalSection(&context->critical_section);
          context->error =
              "Failed to notify the window that it needs to re-draw.";
          break;
        } else {
          EnterCriticalSection(&context->critical_section);
        }
      } else {
        EnterCriticalSection(&context->critical_section);
        context->error = "Failed to wait for vertical sync.";
        break;
      }
    }
  }

  context->state = VSYNC_CONTEXT_STATE_STOPPED;
  LeaveCriticalSection(&context->critical_section);
  return 0;
}

const char *run_event_loop(
    const char *const title, const int ticks_per_second,
    void (*const tick)(const void *const context, const int pointer_state,
                       const float pointer_row, const float pointer_column,
                       bool (*const key_held)(const void *const context,
                                              const WPARAM virtual_key_code)),
    const int rows, const int columns, const float *const opacities,
    const float *const reds, const float *const greens,
    const float *const blues,
    void (*const video)(const void *const context, const int pointer_state,
                        const float pointer_row, const float pointer_column,
                        bool (*const key_held)(const void *const context,
                                               const WPARAM virtual_key_code),
                        const float tick_progress_unit_interval),
    const int samples_per_tick, const float *const left,
    const float *const right, const int nCmdShow) {
  // We need a minimum of two buffers.
  // We also need a minimum of enough buffers for 100msec in my experience.
  int buffers = ((int)ceil(max(1, 1.0 / 10 / (1.0 / ticks_per_second)))) + 1;

  const int bytes_per_row =
      opacities == NULL ? (int)GDI_WIDTHBYTES(columns * 24) : columns * 4;

  context context = {
      .ticks_per_second = ticks_per_second,
      .tick = tick,
      .rows = rows,
      .columns = columns,
      .skipped_bytes_per_row =
          opacities == NULL ? bytes_per_row - (columns * 3) : 0,
      .opacities = opacities,
      .reds = reds,
      .greens = greens,
      .blues = blues,
      .video = video,
      .samples_per_tick = samples_per_tick,
      .left = left,
      .right = right,
      .error = NULL,
      .scratch = malloc(sizeof(uint8_t) * rows * bytes_per_row +
                        sizeof(float) * 2 * buffers * samples_per_tick +
                        sizeof(WAVEHDR) * buffers),
      .hwaveout = NULL,
      .next_buffer = 0,
      .buffers = buffers,
      .minimum_position = 0,
      .held_virtual_key_codes = NULL,
      .number_of_held_virtual_key_codes = 0,
      .position_x = 0,
      .position_y = 0,
      .scaled_width = columns,
      .scaled_height = rows,
      .x_offset = 0,
      .y_offset = 0,
      .inverse_x_offset = 0,
      .inverse_y_offset = 0,
      .pointer_state = POINTER_STATE_NONE,
      .pointer_row = 0.0f,
      .pointer_column = 0.0f,
  };

  if (context.scratch == NULL) {
    return "Failed to allocate scratch memory.";
  }

  RECT insets = {0, 0, 0, 0};

  if (!AdjustWindowRect(&insets, opacities == NULL ? OPAQUE_WS : TRANSPARENT_WS,
                        FALSE)) {
    free(context.scratch);

    return "Failed to calculate the dimensions of the window.";
  }

  HINSTANCE instance = GetModuleHandle(NULL);

  if (instance == NULL) {
    free(context.scratch);
    return "Failed to retrieve the module handle.";
  }

  const HCURSOR cursor = LoadCursor(NULL, IDC_ARROW);

  if (cursor == NULL) {
    free(context.scratch);
    return "Failed to retrieve the default cursor.";
  }

  WNDCLASSEX wc = {
      .cbSize = sizeof(WNDCLASSEX),
      .style = 0,
      .lpfnWndProc = window_procedure,
      .cbClsExtra = 0,
      .cbWndExtra = 0,
      .hInstance = instance,
      .hIcon = (HICON)LoadImage(instance, MAKEINTRESOURCE(1), IMAGE_ICON,
                                GetSystemMetrics(SM_CXICON),
                                GetSystemMetrics(SM_CYICON), 0),
      .hCursor = cursor,
      .hbrBackground = NULL,
      .lpszMenuName = NULL,
      .lpszClassName = title,
      .hIconSm = (HICON)LoadImage(instance, MAKEINTRESOURCE(1), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0)};

  if (RegisterClassEx(&wc) == 0) {
    free(context.scratch);
    return "Failed to register the window class.";
  }

  HWND hwnd = CreateWindowEx(
      opacities == NULL ? 0 : WS_EX_LAYERED, title, title,
      opacities == NULL ? OPAQUE_WS : TRANSPARENT_WS,
      opacities == NULL ? CW_USEDEFAULT : 100,
      opacities == NULL ? CW_USEDEFAULT : 100,
      columns + insets.right - insets.left, rows + insets.bottom - insets.top,
      HWND_DESKTOP, NULL, wc.hInstance, &context);

  if (hwnd == NULL) {
    free(context.scratch);

    if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
      return "Failed to create the window.";
    } else {
      return "Failed to create the window.  Additionally failed to unregister "
             "the window class.";
    }
  }

  RECT window_rect;
  if (GetWindowRect(hwnd, &window_rect) == 0) {
    if (DestroyWindow(hwnd) || GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
      free(context.scratch);

      if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
        return "Failed to measure the window.";
      } else {
        return "Failed to measure the window.  Additionally failed to "
               "unregister the window class.";
      }
    } else {
      free(context.scratch);

      if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
        return "Failed to measure the window.  Additionally failed to destroy "
               "the window.";
      } else {
        return "Failed to measure the window.  Additionally failed to destroy "
               "the window and unregister the window class.";
      }
    }
  }

  context.position_x = window_rect.left;
  context.position_y = window_rect.top;

  if (opacities != NULL) {
    refresh_layered(hwnd, &context);
  }

  const WAVEFORMATEX wave_format = {
      WAVE_FORMAT_IEEE_FLOAT,
      2,
      samples_per_tick * ticks_per_second,
      2 * sizeof(float) * samples_per_tick * ticks_per_second,
      2 * sizeof(float),
      sizeof(float) * 8,
      0,
  };

  if (waveOutOpen(&context.hwaveout, WAVE_MAPPER, &wave_format, (DWORD_PTR)hwnd,
                  (DWORD_PTR)&context, CALLBACK_WINDOW) != MMSYSERR_NOERROR) {
    if (DestroyWindow(hwnd) || GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
      free(context.scratch);

      if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
        return "Failed to open wave out.";
      } else {
        return "Failed to open wave out.  Additionally failed to unregister "
               "the window class.";
      }
    } else {
      free(context.scratch);

      if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
        return "Failed to open wave out.  Additionally failed to destroy the "
               "window.";
      } else {
        return "Failed to open wave out.  Additionally failed to destroy the "
               "window and unregister the window class.";
      }
    }
  }

  if (waveOutPause(context.hwaveout) != MMSYSERR_NOERROR) {
    if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
      if (DestroyWindow(hwnd) ||
          GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
        free(context.scratch);

        if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
          return "Failed to pause wave out.";
        } else {
          return "Failed to pause wave out.  Additionally failed to unregister "
                 "the window class.";
        }
      } else {
        free(context.scratch);

        if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
          return "Failed to pause wave out.  Additionally failed to destroy "
                 "the window.";
        } else {
          return "Failed to pause wave out.  Additionally failed to destroy "
                 "the window and unregister the window class.";
        }
      }
    } else {
      if (DestroyWindow(hwnd) ||
          GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
        free(context.scratch);

        if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
          return "Failed to pause wave out.  Additionally failed to close wave "
                 "out.";
        } else {
          return "Failed to pause wave out.  Additionally failed to close wave "
                 "out and unregister the window class.";
        }
      } else {
        free(context.scratch);

        if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
          return "Failed to pause wave out.  Additionally failed to close wave "
                 "out and destroy the window.";
        } else {
          return "Failed to pause wave out.  Additionally failed to close wave "
                 "out, destroy the window and unregister the window class.";
        }
      }
    }
  }

  float *buffer =
      (float *)(((uint8_t *)context.scratch) + rows * bytes_per_row);
  WAVEHDR *const first_wavehdr =
      (WAVEHDR *)(buffer + buffers * samples_per_tick * 2);
  WAVEHDR *wavehdr = first_wavehdr;

  for (int buffer_index = 0; buffer_index < buffers; buffer_index++) {
    tick(&context, POINTER_STATE_NONE, 0, 0, key_held);

    wavehdr->lpData = (LPSTR)buffer;
    wavehdr->dwBufferLength = samples_per_tick * 2 * sizeof(float);
    wavehdr->dwBytesRecorded = 0;
    wavehdr->dwUser = 0;
    wavehdr->dwFlags = WHDR_DONE;
    wavehdr->dwLoops = 0;
    wavehdr->lpNext = NULL;
    wavehdr->reserved = 0;

    for (int index = 0; index < samples_per_tick; index++) {
      *buffer = left[index];
      buffer++;
      *buffer = right[index];
      buffer++;
    }

    if (waveOutPrepareHeader(context.hwaveout, wavehdr, sizeof(WAVEHDR)) !=
        MMSYSERR_NOERROR) {
      if (waveOutReset(context.hwaveout) == MMSYSERR_NOERROR) {
        if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to prepare wave out.";
            } else {
              return "Failed to prepare wave out.  Additionally failed to "
                     "unregister the window class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to prepare wave out.  Additionally failed to "
                     "destroy the window.";
            } else {
              return "Failed to prepare wave out.  Additionally failed to "
                     "destroy the window and unregister the window class.";
            }
          }
        } else {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to prepare wave out.  Additionally failed to "
                     "close wave out.";
            } else {
              return "Failed to prepare wave out.  Additionally failed to "
                     "close wave out and unregister the window class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to prepare wave out.  Additionally failed to "
                     "close wave out and destroy the window.";
            } else {
              return "Failed to prepare wave out.  Additionally failed to "
                     "close wave out, destroy the window and unregister the "
                     "window class.";
            }
          }
        }
      } else {
        // In the event this fails, we can't unprepare wave outs safely.
        // The process is probably about to close in any case.

        if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to prepare wave out.  Additionally failed to "
                     "reset wave out.";
            } else {
              return "Failed to prepare wave out.  Additionally failed to "
                     "reset wave out and unregister the window class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to prepare wave out.  Additionally failed to "
                     "reset wave out and destroy the window.";
            } else {
              return "Failed to prepare wave out.  Additionally failed to "
                     "reset wave out, destroy the window and unregister the "
                     "window class.";
            }
          }
        } else {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to prepare wave out.  Additionally failed to "
                     "reset wave out and close wave out.";
            } else {
              return "Failed to prepare wave out.  Additionally failed to "
                     "reset wave out, close wave out and unregister the window "
                     "class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to prepare wave out.  Additionally failed to "
                     "reset wave out, close wave out and destroy the window.";
            } else {
              return "Failed to prepare wave out.  Additionally failed to "
                     "reset wave out, close wave out, destroy the window and "
                     "unregister the window class.";
            }
          }
        }
      }
    }

    if (waveOutWrite(context.hwaveout, wavehdr, sizeof(WAVEHDR)) !=
        MMSYSERR_NOERROR) {
      if (waveOutReset(context.hwaveout) == MMSYSERR_NOERROR) {
        if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to write wave out.";
            } else {
              return "Failed to write wave out.  Additionally failed to "
                     "unregister the window class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to write wave out.  Additionally failed to "
                     "destroy the window.";
            } else {
              return "Failed to write wave out.  Additionally failed to "
                     "destroy the window and unregister the window class.";
            }
          }
        } else {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to write wave out.  Additionally failed to close "
                     "wave out.";
            } else {
              return "Failed to write wave out.  Additionally failed to close "
                     "wave out and unregister the window class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to write wave out.  Additionally failed to close "
                     "wave out and destroy the window.";
            } else {
              return "Failed to write wave out.  Additionally failed to close "
                     "wave out, destroy the window and unregister the window "
                     "class.";
            }
          }
        }
      } else {
        // In the event this fails, we can't unprepare wave outs safely.
        // The process is probably about to close in any case.

        if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to write wave out.  Additionally failed to reset "
                     "wave out.";
            } else {
              return "Failed to write wave out.  Additionally failed to reset "
                     "wave out and unregister the window class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to write wave out.  Additionally failed to reset "
                     "wave out and destroy the window.";
            } else {
              return "Failed to write wave out.  Additionally failed to reset "
                     "wave out, destroy the window and unregister the window "
                     "class.";
            }
          }
        } else {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to write wave out.  Additionally failed to reset "
                     "wave out and close wave out.";
            } else {
              return "Failed to write wave out.  Additionally failed to reset "
                     "wave out, close wave out and unregister the window "
                     "class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to write wave out.  Additionally failed to reset "
                     "wave out, close wave out and destroy the window.";
            } else {
              return "Failed to write wave out.  Additionally failed to reset "
                     "wave out, close wave out, destroy the window and "
                     "unregister the window class.";
            }
          }
        }
      }
    }

    wavehdr++;
  }

  vsync_context vc = {
      .hwnd = hwnd, .state = VSYNC_CONTEXT_STATE_STARTING, .error = NULL};

  InitializeCriticalSection(&vc.critical_section);

  CreateThread(NULL, 0, vsync_thread, &vc, 0, NULL);

  ShowWindow(hwnd, nCmdShow);

  if (waveOutRestart(context.hwaveout) != MMSYSERR_NOERROR) {
    EnterCriticalSection(&vc.critical_section);

    if (vc.state != VSYNC_CONTEXT_STATE_STOPPED) {
      vc.state = VSYNC_CONTEXT_STATE_STOPPING;

      while (vc.state != VSYNC_CONTEXT_STATE_STOPPED) {
        LeaveCriticalSection(&vc.critical_section);

        Sleep(10);

        EnterCriticalSection(&vc.critical_section);
      }
    }

    LeaveCriticalSection(&vc.critical_section);
    DeleteCriticalSection(&vc.critical_section);

    if (vc.error == NULL) {
      if (waveOutReset(context.hwaveout) == MMSYSERR_NOERROR) {
        if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to restart wave out.";
            } else {
              return "Failed to restart wave out.  Additionally failed to "
                     "unregister the window class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to restart wave out.  Additionally failed to "
                     "destroy the window.";
            } else {
              return "Failed to restart wave out.  Additionally failed to "
                     "destroy the window and unregister the window class.";
            }
          }
        } else {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to restart wave out.  Additionally failed to "
                     "close wave out.";
            } else {
              return "Failed to restart wave out.  Additionally failed to "
                     "close wave out and unregister the window class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to restart wave out.  Additionally failed to "
                     "close wave out and destroy the window.";
            } else {
              return "Failed to restart wave out.  Additionally failed to "
                     "close wave out, destroy the window and unregister the "
                     "window class.";
            }
          }
        }
      } else {
        // In the event this fails, we can't unprepare wave outs safely.
        // The process is probably about to close in any case.

        if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to restart wave out.  Additionally failed to "
                     "reset wave out.";
            } else {
              return "Failed to restart wave out.  Additionally failed to "
                     "reset wave out and unregister the window class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to restart wave out.  Additionally failed to "
                     "reset wave out and destroy the window.";
            } else {
              return "Failed to restart wave out.  Additionally failed to "
                     "reset wave out, destroy the window and unregister the "
                     "window class.";
            }
          }
        } else {
          if (DestroyWindow(hwnd) ||
              GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to restart wave out.  Additionally failed to "
                     "reset wave out and close wave out.";
            } else {
              return "Failed to restart wave out.  Additionally failed to "
                     "reset wave out, close wave out and unregister the window "
                     "class.";
            }
          } else {
            free(context.scratch);

            if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
              return "Failed to restart wave out.  Additionally failed to "
                     "reset wave out, close wave out and destroy the window.";
            } else {
              return "Failed to restart wave out.  Additionally failed to "
                     "reset wave out, close wave out, destroy the window and "
                     "unregister the window class.";
            }
          }
        }
      }
    } else if (waveOutReset(context.hwaveout) == MMSYSERR_NOERROR) {
      if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
        if (DestroyWindow(hwnd) ||
            GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to restart wave out.  An error additionally "
                   "occurred in the vsync thread.";
          } else {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread and while unregistering the window "
                   "class.";
          }
        } else {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread and while destroying the window.";
          } else {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while destroying the window and "
                   "unregistering the window class.";
          }
        }
      } else {
        if (DestroyWindow(hwnd) ||
            GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread and while closing wave out.";
          } else {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while closing wave out and "
                   "unregistering the window class.";
          }
        } else {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while closing wave out and destroying "
                   "the window.";
          } else {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while closing wave out, destroying "
                   "the window and unregistering the window class.";
          }
        }
      }
    } else {
      // In the event this fails, we can't unprepare wave outs safely.
      // The process is probably about to close in any case.

      if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
        if (DestroyWindow(hwnd) ||
            GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread and while resetting wave out.";
          } else {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while restting wave out and "
                   "unregistering the window class.";
          }
        } else {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while resetting wave out and "
                   "destroying the window.";
          } else {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while resetting wave out, destroying "
                   "the window and unregistering the window class.";
          }
        }
      } else {
        if (DestroyWindow(hwnd) ||
            GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while resetting wave out and closing "
                   "wave out.";
          } else {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while restting wave out, closing wave "
                   "out and unregistering the window class.";
          }
        } else {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while retting wave out, closing wave "
                   "out and destroying the window.";
          } else {
            return "Failed to restart wave out.  Errors additionally occurred "
                   "in the vsync thread, while resetting wave out, closing "
                   "wave out, destroing the window and unregistering the "
                   "window class.";
          }
        }
      }
    }
  }

  while (context.error == NULL) {
    MSG msg;

    while (GetMessage(&msg, hwnd, 0, 0)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);

      if (context.error != NULL) {
        break;
      }
    }
  }

  if (waveOutReset(context.hwaveout) != MMSYSERR_NOERROR) {
    // In the event this fails, we can't unprepare wave outs safely.
    // The process is probably about to close in any case.

    EnterCriticalSection(&vc.critical_section);

    if (vc.state != VSYNC_CONTEXT_STATE_STOPPED) {
      vc.state = VSYNC_CONTEXT_STATE_STOPPING;

      while (vc.state != VSYNC_CONTEXT_STATE_STOPPED) {
        LeaveCriticalSection(&vc.critical_section);

        Sleep(10);

        EnterCriticalSection(&vc.critical_section);
      }
    }

    LeaveCriticalSection(&vc.critical_section);
    DeleteCriticalSection(&vc.critical_section);

    if (vc.error == NULL) {
      if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
        if (DestroyWindow(hwnd) ||
            GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to reset wave out.";
          } else {
            return "Failed to reset wave out.  Additionally failed to "
                   "unregister the window class.";
          }
        } else {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to reset wave out.  Additionally failed to destroy "
                   "the window.";
          } else {
            return "Failed to reset wave out.  Additionally failed to destroy "
                   "the window and unregister the window class.";
          }
        }
      } else {
        if (DestroyWindow(hwnd) ||
            GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to reset wave out.  Additionally failed to close "
                   "wave out.";
          } else {
            return "Failed to reset wave out.  Additionally failed to close "
                   "wave out and unregister the window class.";
          }
        } else {
          free(context.scratch);

          if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
            return "Failed to reset wave out.  Additionally failed to close "
                   "wave out and destroy the window.";
          } else {
            return "Failed to reset wave out.  Additionally failed to close "
                   "wave out, destroy the window and unregister the window "
                   "class.";
          }
        }
      }
    } else if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
      if (DestroyWindow(hwnd) ||
          GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
        free(context.scratch);

        if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
          return "Failed to reset wave out.  An error additionally occurred in "
                 "the vsync thread.";
        } else {
          return "Failed to reset wave out.  Errors additionally occurred in "
                 "the vsync thread and while unregistering the window class.";
        }
      } else {
        free(context.scratch);

        if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
          return "Failed to reset wave out.  Errors additionally occurred in "
                 "the vsync thread and while destroying the window.";
        } else {
          return "Failed to reset wave out.  Errors additionally occurred in "
                 "the vsync thread, while destroying the window and "
                 "unregistering the window class.";
        }
      }
    } else {
      if (DestroyWindow(hwnd) ||
          GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
        free(context.scratch);

        if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
          return "Failed to reset wave out.  Errors additionally occurred in "
                 "the vsync thread and while closing wave out.";
        } else {
          return "Failed to reset wave out.  Errors additionally occurred in "
                 "the vsync thread, while cling wave out and unregistering the "
                 "window class.";
        }
      } else {
        free(context.scratch);

        if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
          return "Failed to reset wave out.  Errors additionally occurred in "
                 "the vsync thread, while closing wave out and destroying the "
                 "window.";
        } else {
          return "Failed to reset wave out.  Errors additionally occurred in "
                 "the vsync thread, while closing wave out, destroying the "
                 "window and unregistering the window class.";
        }
      }
    }
  }

  EnterCriticalSection(&vc.critical_section);

  if (vc.state != VSYNC_CONTEXT_STATE_STOPPED) {
    vc.state = VSYNC_CONTEXT_STATE_STOPPING;

    while (vc.state != VSYNC_CONTEXT_STATE_STOPPED) {
      LeaveCriticalSection(&vc.critical_section);

      MSG msg;
      if (PeekMessage(&msg, hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      } else {
        Sleep(10);
      }

      EnterCriticalSection(&vc.critical_section);
    }
  }

  LeaveCriticalSection(&vc.critical_section);
  DeleteCriticalSection(&vc.critical_section);

  if (vc.error != NULL) {
    if (waveOutClose(context.hwaveout) == MMSYSERR_NOERROR) {
      return vc.error;
    } else {
      if (DestroyWindow(hwnd) ||
          GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
        free(context.scratch);

        if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
          return "An error occurred in the vsync thread.  Additionally failed "
                 "to close wave out.";
        } else {
          return "An error occurred in the vsync thread.  Additionally failed "
                 "to close wave out and unregister the window class.";
        }
      } else {
        free(context.scratch);

        if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
          return "An error occurred in the vsync thread.  Additionally failed "
                 "to close wave out and destroy the window.";
        } else {
          return "An error occurred in the vsync thread.  Additionally failed "
                 "to close wave out, destroy the window and unregister the "
                 "window class.";
        }
      }
    }
  }

  if (waveOutClose(context.hwaveout) != MMSYSERR_NOERROR) {
    if (DestroyWindow(hwnd) || GetLastError() == ERROR_INVALID_WINDOW_HANDLE) {
      free(context.scratch);

      if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
        return "Failed to close wave out.";
      } else {
        return "Failed to close wave out.  Additionally failed to unregister "
               "the window class.";
      }
    } else {
      free(context.scratch);

      if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
        return "Failed to close wave out.  Additionally failed to destroy the "
               "window.";
      } else {
        return "Failed to close wave out.  Additionally failed to destroy the "
               "window and unregister the window class.";
      }
    }
  }

  if (!DestroyWindow(hwnd) && GetLastError() != ERROR_INVALID_WINDOW_HANDLE) {
    free(context.scratch);

    if (UnregisterClass(wc.lpszClassName, wc.hInstance)) {
      return "Failed to destroy the window.";
    } else {
      return "Failed to destroy the window.  Additionally failed to unregister "
             "the window class.";
    }
  }

  free(context.scratch);

  if (!UnregisterClass(wc.lpszClassName, wc.hInstance)) {
    return "Failed to unregister the window class.";
  }

  return context.error;
}
