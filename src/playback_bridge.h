#ifndef SPOTIAMP_PLAYBACK_BRIDGE_H_
#define SPOTIAMP_PLAYBACK_BRIDGE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t sp_playback_bridge_abi_version(void);
const char *sp_playback_bridge_version_string(void);
int sp_playback_bridge_start(const char *access_token, const char *cache_dir, char *error_buf, int error_buf_size);
void sp_playback_bridge_stop(void);
int sp_playback_bridge_is_running(void);
intptr_t sp_playback_bridge_read(void *buffer, uintptr_t buffer_size);
uintptr_t sp_playback_bridge_buffered_bytes(void);
int sp_playback_bridge_play_state(void);
uint32_t sp_playback_bridge_position_ms(void);
void sp_playback_bridge_flush(void);

typedef struct SpPlaybackSnapshot {
  uint64_t command_id;
  uint64_t event_generation;
  uint64_t audio_generation;
  uint64_t play_request_id;
  int32_t state;
  uint32_t position_ms;
  uint32_t nominal_position_ms;
  uint32_t buffered_ms;
  int32_t started_audio;
  int32_t loading;
} SpPlaybackSnapshot;

int sp_playback_bridge_snapshot(SpPlaybackSnapshot *snapshot);
int sp_playback_bridge_load_tracks(const char * const *uris, uintptr_t count,
                                   uint32_t index, uint32_t position_ms,
                                   int start_playing);
int sp_playback_bridge_load_context(const char *context_uri,
                                    const char *track_uri,
                                    int32_t index,
                                    uint32_t position_ms,
                                    int start_playing);
int sp_playback_bridge_play(void);
int sp_playback_bridge_pause(void);
int sp_playback_bridge_stop_playback(void);
int sp_playback_bridge_seek(uint32_t position_ms);
int sp_playback_bridge_next(void);
int sp_playback_bridge_prev(void);
int sp_playback_bridge_set_shuffle(int enabled);
int sp_playback_bridge_set_repeat(int mode);

#ifdef __cplusplus
}
#endif

#endif  // SPOTIAMP_PLAYBACK_BRIDGE_H_
