#include "stdafx.h"
#include "types.h"
#include "tiny_spotify/tiny_spotify.h"
#include "SDL.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Defined in vis_sdl.cpp — called from the audio callback so vis is synced to output
void UpdateEqualizer(const TspSampleType *ptr, int count, int buff);

static std::atomic<int> g_volume(65535);
static int g_devid = -1;
static SDL_AudioDeviceID g_audio_device = 0;
static SDL_AudioSpec g_audio_spec;
static std::mutex g_audio_mutex;
static std::vector<unsigned char> g_audio_buffer;
static int g_audio_channels = 2;
static int g_audio_sample_rate = 44100;

static void SdlAudioCallback(void *userdata, Uint8 *stream, int len) {
    memset(stream, 0, len);

    std::lock_guard<std::mutex> lock(g_audio_mutex);
    int bytes_to_copy = (int)g_audio_buffer.size();
    if (bytes_to_copy > len) bytes_to_copy = len;
    int frame_bytes = (int)sizeof(int16_t) * (g_audio_channels ? g_audio_channels : 1);
    bytes_to_copy -= bytes_to_copy % frame_bytes;
    if (bytes_to_copy <= 0) return;

    int volume = g_volume.load();
    const int16_t *src = (const int16_t*)g_audio_buffer.data();
    int16_t *dst = (int16_t*)stream;
    int samples = bytes_to_copy / sizeof(int16_t);
    for (int i = 0; i < samples; ++i) {
        int v = (int)src[i] * volume / 65535;
        if (v < -32768) v = -32768;
        if (v > 32767) v = 32767;
        dst[i] = (int16_t)v;
    }

    // Feed the visualizer from here so it's synced to actual audio output,
    // not to the push side (which runs ahead of playback by the buffer size).
    UpdateEqualizer((const TspSampleType*)stream, samples, 0);

    if (bytes_to_copy < (int)g_audio_buffer.size()) {
        memmove(g_audio_buffer.data(), g_audio_buffer.data() + bytes_to_copy,
                g_audio_buffer.size() - bytes_to_copy);
    }
    g_audio_buffer.resize(g_audio_buffer.size() - bytes_to_copy);
}

extern "C" {

TSP_PUBLIC void WavSetDeviceId(int id) {
    g_devid = id;
}

TSP_PUBLIC int WavGetDeviceId() {
    return g_devid;
}

TSP_PUBLIC void WavSetVolume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 65535) vol = 65535;
    g_volume.store(vol);
}

TSP_PUBLIC int WavGetVolume() {
    return g_volume.load();
}

TSP_PUBLIC bool WavInit(Tsp *tsp) {
    // SDL audio initialization is handled dynamically in WavPush when format is known
    return true;
}

TSP_PUBLIC void WavFree() {
    if (g_audio_device) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
    std::lock_guard<std::mutex> lock(g_audio_mutex);
    g_audio_buffer.clear();
}

TSP_PUBLIC int WavPush(void *context, int flags, const TspSampleType *data, int size, 
                       const TspSampleFormat *format, int *samples_buffered) {
    if (!format) {
        if (samples_buffered) {
            std::lock_guard<std::mutex> lock(g_audio_mutex);
            int channels = g_audio_channels ? g_audio_channels : 1;
            *samples_buffered = (int)(g_audio_buffer.size() / sizeof(TspSampleType) / channels);
        }
        return 0;
    }

    if (flags & kTspAudioFlag_FlushBuffer) {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        g_audio_buffer.clear();
    }

    bool paused = (flags & kTspAudioFlag_Pause) != 0;

    if (paused) {
        if (g_audio_device) {
            SDL_PauseAudioDevice(g_audio_device, 1);
        }
    } else {
        if (g_audio_device) {
            SDL_PauseAudioDevice(g_audio_device, 0);
        }
    }

    int byte_size = size * sizeof(TspSampleType);
    if (byte_size <= 0 || !data) {
        if (samples_buffered) {
            std::lock_guard<std::mutex> lock(g_audio_mutex);
            int channels = g_audio_channels ? g_audio_channels : 1;
            *samples_buffered = (int)(g_audio_buffer.size() / sizeof(TspSampleType) / channels);
        }
        return 0;
    }

    // Reopen device if format (sample rate or channels) changes
    if (g_audio_device == 0 || g_audio_spec.channels != format->channels || g_audio_spec.freq != format->sample_rate) {
        if (g_audio_device) {
            SDL_CloseAudioDevice(g_audio_device);
            g_audio_device = 0;
        }
        SDL_AudioSpec wanted;
        SDL_zero(wanted);
        wanted.freq = format->sample_rate;
        wanted.format = AUDIO_S16SYS; // TspSampleType is int16_t
        wanted.channels = format->channels;
        wanted.samples = 2048;
        wanted.callback = SdlAudioCallback;
        wanted.userdata = NULL;
        
        g_audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted, &g_audio_spec, 0);
        if (g_audio_device == 0) {
            fprintf(stderr, "Failed to open SDL audio device: %s\n", SDL_GetError());
            return 0;
        }
        g_audio_channels = g_audio_spec.channels;
        g_audio_sample_rate = g_audio_spec.freq;
        {
            std::lock_guard<std::mutex> lock(g_audio_mutex);
            g_audio_buffer.clear();
        }
        SDL_PauseAudioDevice(g_audio_device, paused ? 1 : 0);
    }
    
    if (paused) {
        if (samples_buffered) {
            std::lock_guard<std::mutex> lock(g_audio_mutex);
            int channels = g_audio_channels ? g_audio_channels : 1;
            *samples_buffered = (int)(g_audio_buffer.size() / sizeof(TspSampleType) / channels);
        }
        return 0;
    }

    if (byte_size > 0 && data) {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        size_t max_buffer = (size_t)g_audio_sample_rate * (size_t)g_audio_channels *
                            sizeof(TspSampleType) * 3 / 20; // ~150ms max ahead
        if (g_audio_buffer.size() > max_buffer) {
            if (samples_buffered) {
                int channels = g_audio_channels ? g_audio_channels : 1;
                *samples_buffered = (int)(g_audio_buffer.size() / sizeof(TspSampleType) / channels);
            }
            return 0;
        }
        size_t old_size = g_audio_buffer.size();
        g_audio_buffer.resize(old_size + byte_size);
        memcpy(g_audio_buffer.data() + old_size, data, byte_size);
    }
    
    if (samples_buffered) {
        std::lock_guard<std::mutex> lock(g_audio_mutex);
        int channels = format->channels ? format->channels : 1;
        *samples_buffered = (int)(g_audio_buffer.size() / sizeof(TspSampleType) / channels);
    }
    
    return size;
}

} // extern "C"
