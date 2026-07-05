#include "stdafx.h"
#include "types.h"
#include "window.h"
#include "spotifyamp.h"
#include <math.h>
#include <mutex>
#include <string.h>

extern unsigned int viscolors[24];

#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct Filter {
  float a0, a1, a2, a3, a4;
  float x1, x2, y1, y2;
  float tx1, tx2, ty1, ty2;
  float val;
};

enum {
  LPF,
  HPF,
  BPF,
  NOTCH,
  PEQ,
  LSH,
  HSH
};

static std::mutex g_vis_mutex;
static Filter g_vis_filters[19];
static float g_vis_values[19];
static float g_vis_peak_rows[19];
static float g_vis_peak_vel[19];
static signed char g_vis_wave[76];
static TspSampleType g_vis_sample_buffer[576 * 2];
static int g_vis_total;
static bool g_vis_filters_ready;

static void BiQuad_new(Filter *b, int type, float dbGain, float freq, float srate, float bandwidth) {
  float A = powf(10.0f, dbGain / 40.0f);
  float omega = 2.0f * (float)M_PI * freq / srate;
  float sn = sinf(omega);
  float cs = cosf(omega);
  float alpha = sn * sinhf((float)M_LN2 / 2.0f * bandwidth * omega / sn);
  float beta = sqrtf(A + A);
  float a0, a1, a2, b0, b1, b2;

  switch (type) {
  case LPF:
    b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2;
    a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
    break;
  case HPF:
    b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2;
    a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
    break;
  case BPF:
    b0 = alpha; b1 = 0; b2 = -alpha;
    a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
    break;
  case NOTCH:
    b0 = 1; b1 = -2 * cs; b2 = 1;
    a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
    break;
  case PEQ:
    b0 = 1 + (alpha * A); b1 = -2 * cs; b2 = 1 - (alpha * A);
    a0 = 1 + (alpha / A); a1 = -2 * cs; a2 = 1 - (alpha / A);
    break;
  case LSH:
    b0 = A * ((A + 1) - (A - 1) * cs + beta * sn);
    b1 = 2 * A * ((A - 1) - (A + 1) * cs);
    b2 = A * ((A + 1) - (A - 1) * cs - beta * sn);
    a0 = (A + 1) + (A - 1) * cs + beta * sn;
    a1 = -2 * ((A - 1) + (A + 1) * cs);
    a2 = (A + 1) + (A - 1) * cs - beta * sn;
    break;
  default:
    b0 = A * ((A + 1) + (A - 1) * cs + beta * sn);
    b1 = -2 * A * ((A - 1) + (A + 1) * cs);
    b2 = A * ((A + 1) + (A - 1) * cs - beta * sn);
    a0 = (A + 1) - (A - 1) * cs + beta * sn;
    a1 = 2 * ((A - 1) - (A + 1) * cs);
    a2 = (A + 1) - (A - 1) * cs - beta * sn;
    break;
  }

  memset(b, 0, sizeof(*b));
  b->a0 = b0 / a0;
  b->a1 = b1 / a0;
  b->a2 = b2 / a0;
  b->a3 = a1 / a0;
  b->a4 = a2 / a0;
}

static void UpdateFilter(int sample, Filter *b) {
  float input = (float)sample;
  float output = b->a0 * input + b->a1 * b->x1 + b->a2 * b->x2 - b->a3 * b->y1 - b->a4 * b->y2;
  b->x2 = b->x1;
  b->x1 = input;
  b->y2 = b->y1;
  b->y1 = output;

  input = output;
  output = b->a0 * input + b->a1 * b->tx1 + b->a2 * b->tx2 - b->a3 * b->ty1 - b->a4 * b->ty2;
  b->tx2 = b->tx1;
  b->tx1 = input;
  b->ty2 = b->ty1;
  b->ty1 = output;

  b->val = (b->val * 0.99f) + fabsf(output) * 0.01f;
}

static void EnsureFiltersReady() {
  if (g_vis_filters_ready) return;
  g_vis_filters_ready = true;
  float freq = 1.0f;
  for (int j = 0; j < 19; ++j) {
    BiQuad_new(&g_vis_filters[j], BPF, 0.0f, (float)(50.0f * freq + 0.5f), 44100.0f, 0.1f);
    freq *= 1.3868094922572831f;
  }
}

void InitVisualizer() {
  std::lock_guard<std::mutex> lock(g_vis_mutex);
  g_vis_filters_ready = false;
  g_vis_total = 0;
  memset(g_vis_values, 0, sizeof(g_vis_values));
  for (int i = 0; i < 19; ++i)
    g_vis_peak_rows[i] = 16.0f;
  memset(g_vis_peak_vel, 0, sizeof(g_vis_peak_vel));
  memset(g_vis_wave, 0, sizeof(g_vis_wave));
}

void UpdateEqualizer(const TspSampleType *ptr, int count, int buff) {
  if (!ptr || count <= 0) return;

  std::lock_guard<std::mutex> lock(g_vis_mutex);
  EnsureFiltersReady();

#define COPY_INTERVAL (44100 / 60)
  int p = 0;
  while (p < count) {
    int remaining_frames = (count - p) >> 1;
    int frames_to_copy = COPY_INTERVAL - g_vis_total;
    if (frames_to_copy > remaining_frames) frames_to_copy = remaining_frames;
    if (frames_to_copy <= 0) break;

    int samples_to_copy = frames_to_copy * 2;
    int buffer_offset = g_vis_total * 2;
    if (buffer_offset + samples_to_copy <= (int)(sizeof(g_vis_sample_buffer) / sizeof(g_vis_sample_buffer[0]))) {
      memcpy(g_vis_sample_buffer + buffer_offset, ptr + p, samples_to_copy * sizeof(TspSampleType));
    }

    for (int i = 0; i < samples_to_copy; i += 2) {
      int v = ((int)ptr[p + i] + (int)ptr[p + i + 1]) >> 1;
      for (int j = 0; j < 19; ++j)
        UpdateFilter(v, &g_vis_filters[j]);
    }

    p += samples_to_copy;
    g_vis_total += frames_to_copy;

    if (g_vis_total == COPY_INTERVAL) {
      g_vis_total = 0;
      for (int j = 0; j < 19; ++j)
        g_vis_values[j] = g_vis_filters[j].val;
      for (int j = 0; j < 76; ++j) {
        int idx = j * 2 * (576 / 76);
        int v = (g_vis_sample_buffer[idx] + g_vis_sample_buffer[idx + 1]) >> 9;
        if (v < -8) v = -8;
        if (v > 7) v = 7;
        g_vis_wave[j] = (signed char)v;
      }
    }
  }
}

void PaintSdlVisualizer(PlatformWindow *w) {
  if (!w) return;

  float values[19];
  float peak_rows[19];
  float peak_vel[19];
  {
    std::lock_guard<std::mutex> lock(g_vis_mutex);
    for (int i = 0; i < 19; ++i) {
      values[i] = g_vis_values[i];
      peak_rows[i] = g_vis_peak_rows[i];
      peak_vel[i] = g_vis_peak_vel[i];
    }
  }

  const int left = 24;
  const int top = 43;
  w->Fill(left, top, 76, 16, viscolors[0]);
  for (int i = 0; i < 19; ++i) {
    float u = 20.0f * log10f(values[i] / 32768.0f + 1e-16f);
    u = u * 16.0f / 32.0f + 26.0f;
    int v = 15 - Clamp((int)u, -1, 15);
    int x = left + i * 4;
    for (int y = 15; y >= v; --y) {
      // viscolors[2] = top of bar (loudest, low y), viscolors[17] = bottom (quietest, y=15)
      unsigned int color = viscolors[2 + y];
      w->Fill(x, top + y, 3, 1, color);
    }
    float bar_top = v <= 15 ? (float)v : 16.0f;
    if (bar_top < peak_rows[i]) {
      peak_rows[i] = bar_top;
      peak_vel[i] = 0.0f;
    } else if (peak_rows[i] < 16.0f) {
      peak_vel[i] += 0.08f;
      peak_rows[i] += peak_vel[i];
      if (peak_rows[i] > 16.0f) {
        peak_rows[i] = 16.0f;
        peak_vel[i] = 0.0f;
      }
    }
    if (peak_rows[i] >= 0.0f && peak_rows[i] <= 15.0f) {
      w->Fill(x, top + Clamp((int)(peak_rows[i] + 0.5f), 0, 15), 3, 1, viscolors[23]);
    }
  }
  {
    std::lock_guard<std::mutex> lock(g_vis_mutex);
    for (int i = 0; i < 19; ++i) {
      g_vis_peak_rows[i] = peak_rows[i];
      g_vis_peak_vel[i] = peak_vel[i];
    }
  }
}

// Stubs for winamp visualization plugins
const char *VisPlugin_Iterate(int i) { return NULL; }
void VisPlugin_StartStop(int i) {}
void VisPlugin_Config(int i) {}
void VisPlugin_Load(int i) {}

// Stub for SetVisualizer in PlatformWindow
WindowHandle PlatformWindow::SetVisualizer(int x, int y, int w, int h) {
    return (WindowHandle)1; // dummy window handle
}

#include "shoutcast.h"

extern "C" {
bool Mp3CompressorHadError() { return false; }
Shoutcast *ShoutcastCreate(Tsp *tsp) { return NULL; }
void ShoutcastDestroy(Shoutcast *shoutcast) {}
bool ShoutcastListen(Shoutcast *shoutcast, int port) { return false; }
int ShoutcastWavPush(void *context, int flags, const TspSampleType *datai, int sizei, const TspSampleFormat *sample_format, int *samples_buffered) { return 0; }
void ShoutcastSetNowPlaying(Shoutcast *shoutcast, TspItem *item) {}
int ShoutcastNumClients(Shoutcast *shoutcast) { return 0; }
}

void DoAutoUpdateStuff(PlatformWindow *w) {}
void CheckUpdate() {}
