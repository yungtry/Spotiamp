#include "tiny_spotify.h"
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
#include "playback_bridge.h"
#endif
#include <string>
#include <curl/curl.h>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <iostream>
#include <sstream>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#define popen _popen
#define pclose _pclose
typedef int ssize_t;
#else
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

// Struct definitions
struct TspItem {
  std::string name;
  std::string artist;
  std::string album_uri;
  std::string artist_uri;
  std::string image_uri;
  std::string uri;
  int duration_sec;
  TspItemType type;
  bool playable;
};

struct TspItemList {
  std::string uri;
  std::vector<TspItem*> items;
  std::vector<std::string> radio_seed_uris;
  int total_length;
  bool dynamic_loading;
  int now_playing_index;
  TspItemList() : total_length(0), dynamic_loading(false), now_playing_index(0) {}
};

struct Tsp {
  std::mutex mutex;
  TspCallback *callback;
  void *callback_context;
  TspAudioCallback *audio_callback;
  void *audio_context;
  
  std::string access_token;
  std::string username;
  
  bool connected;
  TspError connection_error;
  
  struct RootlistItem {
    std::string name;
    std::string uri;
    TspItemType type;
  };
  std::vector<RootlistItem> rootlist;
  
  TspItem now_playing_item;
  bool has_now_playing;
  int player_position_ms;
  int player_duration_ms;
  long long player_position_updated_ms;
  long long local_control_until_ms;
  long long local_audio_hold_until_ms;
  long long local_position_command_started_ms;
  int local_expected_position_ms;
  bool local_bridge_transition_seen;
  bool local_desired_playing;
  bool local_stopped;
  bool is_playing;
  int volume;
  bool shuffle;
  bool repeat;
  
  TspItemList *player_list;
  std::string queue_sync_context_uri;
  
  std::thread audio_thread;
  std::atomic<bool> audio_thread_running;
  std::thread poll_thread;
  std::atomic<bool> poll_thread_running;
  
  Tsp() : callback(NULL), callback_context(NULL), audio_callback(NULL), audio_context(NULL),
          connected(false), connection_error(kTspErrorOk), has_now_playing(false),
          player_position_ms(0), player_duration_ms(0), player_position_updated_ms(0),
          local_control_until_ms(0), local_audio_hold_until_ms(0),
          local_position_command_started_ms(0), local_expected_position_ms(-1),
          local_bridge_transition_seen(false), local_desired_playing(false),
          local_stopped(false), is_playing(false), volume(65535),
          shuffle(false), repeat(false), player_list(NULL),
          audio_thread_running(false), poll_thread_running(false) {
    player_list = new TspItemList();
  }
};

#include <fstream>

void OpenUrl(const char *url);
extern "C" void WavSetVolume(int vol);

// Global variables
static Tsp *g_global_tsp = NULL;
#if !defined(SPOTIAMP_WITH_RUST_BRIDGE) && defined(_WIN32)
static PROCESS_INFORMATION g_librespot_process = {};
static HANDLE g_librespot_stdout = INVALID_HANDLE_VALUE;
#elif !defined(SPOTIAMP_WITH_RUST_BRIDGE)
static pid_t g_librespot_pid = 0;
static int g_librespot_stdout_fd = -1;
#endif
static std::vector<std::string> g_radio_seed_uris;

static std::string GetSpotiampDeviceIdFromToken(const std::string &access_token);

static void SleepForMicroseconds(int usec) {
#if defined(_WIN32)
  Sleep((DWORD)((usec + 999) / 1000));
#else
  usleep(usec);
#endif
}

static void SleepForSeconds(int seconds) {
#if defined(_WIN32)
  Sleep((DWORD)seconds * 1000);
#else
  sleep(seconds);
#endif
}

#if defined(_WIN32)
typedef SOCKET SocketHandle;
static const SocketHandle kInvalidSocket = INVALID_SOCKET;

static bool EnsureSocketLayer() {
  static std::once_flag winsock_once;
  static bool winsock_ok = false;
  std::call_once(winsock_once, []() {
    WSADATA data;
    winsock_ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  });
  return winsock_ok;
}

static void CloseSocketHandle(SocketHandle socket_handle) {
  if (socket_handle != kInvalidSocket)
    closesocket(socket_handle);
}

static int SocketRead(SocketHandle socket_handle, char *buffer, int size) {
  return recv(socket_handle, buffer, size, 0);
}

static int SocketWrite(SocketHandle socket_handle, const char *buffer, int size) {
  return send(socket_handle, buffer, size, 0);
}
#else
typedef int SocketHandle;
static const SocketHandle kInvalidSocket = -1;

static bool EnsureSocketLayer() {
  return true;
}

static void CloseSocketHandle(SocketHandle socket_handle) {
  if (socket_handle >= 0)
    close(socket_handle);
}

static int SocketRead(SocketHandle socket_handle, char *buffer, int size) {
  return (int)read(socket_handle, buffer, size);
}

static int SocketWrite(SocketHandle socket_handle, const char *buffer, int size) {
  return (int)write(socket_handle, buffer, size);
}
#endif

static long long NowMonotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool PendingPositionSyncLocked(Tsp *tsp) {
  return tsp->local_expected_position_ms >= 0;
}

static bool CommandInFlightLocked(Tsp *tsp, long long now_ms) {
  return tsp->local_control_until_ms > now_ms;
}

static bool AudioHoldLocked(Tsp *tsp, long long now_ms) {
  return tsp->local_audio_hold_until_ms > now_ms;
}

static bool AudioTransitionLocked(Tsp *tsp, long long now_ms) {
  return AudioHoldLocked(tsp, now_ms) || PendingPositionSyncLocked(tsp);
}

static void SetAudioHoldLocked(Tsp *tsp, long long now_ms, int hold_ms) {
  tsp->local_audio_hold_until_ms = now_ms + hold_ms;
}

static void StartPositionTransitionLocked(Tsp *tsp, int expected_position_ms,
                                          long long now_ms, int timeout_ms,
                                          int hold_ms) {
  tsp->local_expected_position_ms = expected_position_ms;
  tsp->local_bridge_transition_seen = false;
  tsp->local_position_command_started_ms = now_ms;
  tsp->local_control_until_ms = now_ms + timeout_ms;
  SetAudioHoldLocked(tsp, now_ms, hold_ms);
}

static void ClearPositionTransitionLocked(Tsp *tsp) {
  tsp->local_expected_position_ms = -1;
  tsp->local_bridge_transition_seen = false;
  tsp->local_position_command_started_ms = 0;
  tsp->local_control_until_ms = 0;
  tsp->local_audio_hold_until_ms = 0;
}

static int CurrentPlayerPositionMsLocked(Tsp *tsp) {
  long long now_ms = NowMonotonicMs();
  int pos = tsp->player_position_ms;
  if (tsp->is_playing && tsp->player_position_updated_ms > 0 &&
      !PendingPositionSyncLocked(tsp)) {
    pos += (int)(now_ms - tsp->player_position_updated_ms);
  }
  if (pos < 0) pos = 0;
  if (tsp->player_duration_ms > 0 && pos > tsp->player_duration_ms)
    pos = tsp->player_duration_ms;
  return pos;
}

static int EndTransitionWindowMs(int duration_ms) {
  int window_ms = duration_ms / 20;
  if (window_ms < 3000) window_ms = 3000;
  if (window_ms > 7000) window_ms = 7000;
  return window_ms;
}

static bool NearTrackEndLocked(Tsp *tsp, int position_ms) {
  if (!tsp || tsp->player_duration_ms <= 0)
    return false;
  return position_ms >= tsp->player_duration_ms - EndTransitionWindowMs(tsp->player_duration_ms);
}

#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
static void FlushRustBridgeAudio() {
  if (sp_playback_bridge_is_running())
    sp_playback_bridge_flush();
}

static bool BridgeLoadTrackUris(const std::vector<std::string> &uris, int index, int position_ms) {
  if (!sp_playback_bridge_is_running() || uris.empty() || index < 0 ||
      index >= (int)uris.size())
    return false;
  std::vector<const char*> raw_uris;
  raw_uris.reserve(uris.size());
  for (const auto &uri : uris)
    raw_uris.push_back(uri.c_str());
  return sp_playback_bridge_load_tracks(raw_uris.data(), (uintptr_t)raw_uris.size(),
                                        (uint32_t)index, (uint32_t)position_ms, 1) == 0;
}

static bool BridgeLoadItemList(TspItemList *tl, int index, int position_ms) {
  if (!tl || index < 0 || index >= (int)tl->items.size())
    return false;
  std::vector<std::string> uris;
  int bridge_index = -1;
  for (int i = 0; i < (int)tl->items.size(); ++i) {
    TspItem *item = tl->items[i];
    if (!item || !item->playable || item->uri.empty())
      continue;
    if (i == index)
      bridge_index = (int)uris.size();
    uris.push_back(item->uri);
  }
  return BridgeLoadTrackUris(uris, bridge_index, position_ms);
}

static bool BridgeLoadContext(const std::string &context_uri, const std::string &track_uri,
                              int index, int position_ms) {
  if (!sp_playback_bridge_is_running() || context_uri.empty())
    return false;
  const char *track = track_uri.empty() ? NULL : track_uri.c_str();
  return sp_playback_bridge_load_context(context_uri.c_str(), track, (int32_t)index,
                                         (uint32_t)position_ms, 1) == 0;
}

static bool ApplyRustBridgePlaybackState(Tsp *tsp) {
  if (!tsp || !sp_playback_bridge_is_running())
    return false;

  SpPlaybackSnapshot snapshot = {};
  if (sp_playback_bridge_snapshot(&snapshot) != 0)
    return false;

  int bridge_state = snapshot.state;
  if (bridge_state <= 0)
    return false;

  int bridge_position_ms = (int)snapshot.position_ms;
  if (bridge_position_ms < 0)
    bridge_position_ms = 0;
  bool bridge_has_buffered_audio = snapshot.buffered_ms > 0;

  std::lock_guard<std::mutex> lock(tsp->mutex);
  long long now_ms = NowMonotonicMs();
  bool bridge_playing = bridge_state == 1;
  bool bridge_loading = bridge_state == 3;
  bool bridge_paused = bridge_state == 2;
  bool bridge_stopped = bridge_state == 4;
  bool command_in_flight = CommandInFlightLocked(tsp, now_ms);
  bool pending_position = PendingPositionSyncLocked(tsp);

  if (tsp->player_duration_ms > 0 && bridge_position_ms > tsp->player_duration_ms)
    bridge_position_ms = tsp->player_duration_ms;

  if (pending_position) {
    if (bridge_loading) {
      if (!tsp->local_bridge_transition_seen)
        tsp->local_bridge_transition_seen = true;
      tsp->player_position_ms = tsp->local_expected_position_ms;
      tsp->player_position_updated_ms = 0;
      if (AudioHoldLocked(tsp, now_ms) || !bridge_has_buffered_audio)
        return false;
      bridge_playing = true;
      bridge_loading = false;
    }
    if (bridge_loading || AudioHoldLocked(tsp, now_ms))
      return false;
    bool close_to_expected = tsp->local_expected_position_ms > 1000 &&
                             abs(bridge_position_ms - tsp->local_expected_position_ms) <= 1200;
    bool transition_timed_out = tsp->local_position_command_started_ms > 0 &&
                                now_ms - tsp->local_position_command_started_ms > 1200;
    bool can_start_from_buffer = tsp->local_bridge_transition_seen && bridge_has_buffered_audio &&
                                 !bridge_paused && !bridge_stopped;
    bool can_start_from_position = tsp->local_bridge_transition_seen &&
                                   close_to_expected &&
                                   !bridge_paused && !bridge_stopped;
    bool can_start_after_timeout = transition_timed_out && bridge_has_buffered_audio &&
                                   !bridge_paused && !bridge_stopped;
    bool can_accept_bridge_playing = bridge_playing &&
                                     (tsp->local_bridge_transition_seen ||
                                      close_to_expected ||
                                      transition_timed_out);
    if (!can_accept_bridge_playing && !can_start_from_buffer &&
        !can_start_from_position && !can_start_after_timeout)
      return false;
    if (!bridge_playing) {
      bridge_playing = true;
      bridge_loading = false;
    }
  }

  if (!pending_position && tsp->local_desired_playing && tsp->has_now_playing &&
      !tsp->repeat && bridge_position_ms <= 2500 &&
      (bridge_loading || bridge_playing) &&
      NearTrackEndLocked(tsp, CurrentPlayerPositionMsLocked(tsp))) {
    return false;
  }

  if (tsp->local_stopped && !bridge_stopped)
    return false;

  if (!pending_position && !tsp->local_desired_playing && bridge_playing)
    return false;

  if (bridge_loading) {
    tsp->player_position_ms = bridge_position_ms;
    tsp->player_position_updated_ms = 0;
    if (!tsp->local_desired_playing)
      tsp->is_playing = false;
    return true;
  }

  if (bridge_paused && command_in_flight && tsp->local_desired_playing)
    return false;

  if (bridge_playing || bridge_paused) {
    tsp->player_position_ms = pending_position && bridge_playing
                                  ? tsp->local_expected_position_ms
                                  : bridge_position_ms;
    tsp->player_position_updated_ms = 0;
    tsp->is_playing = bridge_playing;
    if (bridge_playing) {
      tsp->local_stopped = false;
      ClearPositionTransitionLocked(tsp);
    }
    return true;
  }

  if (bridge_stopped) {
    if (tsp->local_desired_playing && tsp->has_now_playing)
      return false;
    tsp->is_playing = false;
    tsp->player_position_updated_ms = now_ms;
    return true;
  }

  return false;
}
#endif

static std::string GetAccessToken(Tsp *tsp) {
  if (!tsp) return "";
  std::lock_guard<std::mutex> lock(tsp->mutex);
  return tsp->access_token;
}

static void DispatchPlayerCallback(Tsp *tsp, TspCallbackEvent event, void *source = NULL, void *param = NULL) {
  TspCallback *callback = NULL;
  void *callback_context = NULL;
  {
    if (!tsp) return;
    std::lock_guard<std::mutex> lock(tsp->mutex);
    callback = tsp->callback;
    callback_context = tsp->callback_context;
  }
  if (callback) {
    callback(callback_context, event, source, param);
  }
}

static void DispatchAudioControl(Tsp *tsp, int flags) {
  TspAudioCallback *callback = NULL;
  void *audio_context = NULL;
  {
    if (!tsp) return;
    std::lock_guard<std::mutex> lock(tsp->mutex);
    callback = tsp->audio_callback;
    audio_context = tsp->audio_context;
  }
  if (callback) {
    TspSampleFormat format;
    format.channels = 2;
    format.sample_rate = 44100;
    format.replaygain_track = 0.0f;
    format.replaygain_album = 0.0f;
    int samples_buffered = 0;
    callback(audio_context, flags, NULL, 0, &format, &samples_buffered);
  }
}

static void ApplyNowPlayingFromItemLocked(Tsp *tsp, TspItem *item, int index) {
  tsp->now_playing_item.name = item->name;
  tsp->now_playing_item.artist = item->artist;
  tsp->now_playing_item.album_uri = item->album_uri;
  tsp->now_playing_item.artist_uri = item->artist_uri;
  tsp->now_playing_item.image_uri = item->image_uri;
  tsp->now_playing_item.uri = item->uri;
  tsp->now_playing_item.duration_sec = item->duration_sec;
  tsp->now_playing_item.type = item->type;
  tsp->now_playing_item.playable = item->playable;
  tsp->player_duration_ms = item->duration_sec * 1000;
  tsp->player_position_ms = 0;
  tsp->player_position_updated_ms = NowMonotonicMs();
  tsp->has_now_playing = true;
  tsp->local_desired_playing = true;
  tsp->local_stopped = false;
  long long now_ms = NowMonotonicMs();
  StartPositionTransitionLocked(tsp, 0, now_ms, 5000, 60);
  if (tsp->player_list) {
    tsp->player_list->now_playing_index = index;
  }
}

static void SetNowPlayingFromItem(Tsp *tsp, TspItem *item, int index) {
  if (!tsp || !item) return;
  {
    std::lock_guard<std::mutex> lock(tsp->mutex);
    ApplyNowPlayingFromItemLocked(tsp, item, index);
    tsp->is_playing = true;
  }
  DispatchPlayerCallback(tsp, kTspCallbackEvent_NowPlayingChanged);
  DispatchPlayerCallback(tsp, kTspCallbackEvent_Resume);
  DispatchAudioControl(tsp, kTspAudioFlag_FlushBuffer);
}

static void ClearItemList(TspItemList *tl) {
  if (!tl) return;
  for (auto *item : tl->items) {
    delete item;
  }
  tl->items.clear();
}

static void DeleteItemVector(std::vector<TspItem*> *items) {
  if (!items) return;
  for (auto *item : *items)
    delete item;
  items->clear();
}

static int FindTrackIndexByUri(TspItemList *tl, const std::string &uri) {
  if (!tl || uri.empty())
    return -1;
  for (int i = 0; i < (int)tl->items.size(); ++i) {
    TspItem *item = tl->items[i];
    if (item && item->uri == uri)
      return i;
  }
  return -1;
}

static TspItem *DuplicateItem(TspItem *src_item) {
  if (!src_item) return NULL;
  TspItem *item = new TspItem();
  item->name = src_item->name;
  item->artist = src_item->artist;
  item->album_uri = src_item->album_uri;
  item->artist_uri = src_item->artist_uri;
  item->image_uri = src_item->image_uri;
  item->uri = src_item->uri;
  item->duration_sec = src_item->duration_sec;
  item->type = src_item->type;
  item->playable = src_item->playable;
  return item;
}

static TspError CopyItemListContents(TspItemList *tl, TspItemList *src) {
  if (!tl || !src) return kTspErrorTemp;
  ClearItemList(tl);
  tl->uri = src->uri;
  tl->now_playing_index = src->now_playing_index;
  for (auto *src_item : src->items) {
    tl->items.push_back(DuplicateItem(src_item));
  }
  return kTspErrorOk;
}

static bool SetNowPlayingIndex(Tsp *tsp, int index) {
  if (!tsp || !tsp->player_list) return false;
  if (index < 0 || index >= (int)tsp->player_list->items.size()) return false;
  TspItem *item = tsp->player_list->items[index];
  if (!item || !item->playable || item->uri.empty()) return false;
  tsp->player_list->now_playing_index = index;
  SetNowPlayingFromItem(tsp, item, index);
  return true;
}

static bool SetPendingNowPlayingIndex(Tsp *tsp, int index) {
  if (!tsp || !tsp->player_list) return false;
  if (index < 0 || index >= (int)tsp->player_list->items.size()) return false;
  TspItem *item = tsp->player_list->items[index];
  if (!item || !item->playable || item->uri.empty()) return false;
  {
    std::lock_guard<std::mutex> lock(tsp->mutex);
    ApplyNowPlayingFromItemLocked(tsp, item, index);
    // Keep is_playing = true so the UI doesn't flash to paused/0:00 while
    // the Spotify API call is in flight. The poll thread will update state
    // once the API confirms playback started.
    tsp->is_playing = true;
  }
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
  FlushRustBridgeAudio();
#endif
  DispatchPlayerCallback(tsp, kTspCallbackEvent_NowPlayingChanged);
  DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
  return true;
}

static int PickNextIndex(Tsp *tsp, int direction) {
  if (!tsp || !tsp->player_list || tsp->player_list->items.empty()) return -1;
  int count = (int)tsp->player_list->items.size();
  int current = tsp->player_list->now_playing_index;

  bool shuffle = false;
  bool repeat = false;
  {
    std::lock_guard<std::mutex> lock(tsp->mutex);
    shuffle = tsp->shuffle;
    repeat = tsp->repeat;
  }

  if (shuffle && count > 1 && direction > 0) {
    static bool seeded;
    if (!seeded) {
      seeded = true;
      srand((unsigned int)time(NULL));
    }
    for (int tries = 0; tries < count * 2; ++tries) {
      int idx = rand() % count;
      TspItem *item = tsp->player_list->items[idx];
      if (idx != current && item && item->playable && !item->uri.empty())
        return idx;
    }
  }

  for (int step = 1; step <= count; ++step) {
    int idx = current + direction * step;
    if (repeat) {
      idx %= count;
      if (idx < 0) idx += count;
    }
    if (idx < 0 || idx >= count) return -1;
    TspItem *item = tsp->player_list->items[idx];
    if (item && item->playable && !item->uri.empty())
      return idx;
  }
  return -1;
}

static std::string UrlWithDeviceId(const std::string &url, const std::string &device_id) {
  if (device_id.empty()) return url;
  return url + (url.find('?') == std::string::npos ? "?device_id=" : "&device_id=") + device_id;
}

// Spotify API credentials — loaded from obfuscated build-time generated header.
// Set SPOTIFY_CLIENT_ID and SPOTIFY_CLIENT_SECRET env vars before building.
#include "credentials.h"

// Paste your Spotify Access Token here for quick local testing.
// If set, you can log in with a blank password.
static const char *DEFAULT_ACCESS_TOKEN = "";

static std::string Trim(const std::string &str) {
  size_t first = str.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  size_t last = str.find_last_not_of(" \t\r\n");
  return str.substr(first, (last - first + 1));
}

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  ((std::string*)userp)->append((char*)contents, size * nmemb);
  return size * nmemb;
}

static std::mutex g_curl_mutex;
static long g_last_http_status = 0;

static std::string PerformHttpRequest(const std::string &method, const std::string &url, const std::string &token, const std::string &data) {
  std::lock_guard<std::mutex> lock(g_curl_mutex);
  
  static CURL* curl = nullptr;
  if (!curl) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
  }
  if (!curl) return "";

  // Reset options to default values to avoid leakage from previous requests
  curl_easy_reset(curl);

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

  // Set HTTP Method
  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (!data.empty()) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)data.size());
    } else {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }
  } else if (method == "PUT") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    if (!data.empty()) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)data.size());
    }
  } else if (method == "GET") {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  }

  // Set Headers
  struct curl_slist* headers = nullptr;
  std::string auth_header = "Authorization: Bearer " + token;
  headers = curl_slist_append(headers, auth_header.c_str());
  if (!data.empty()) {
    headers = curl_slist_append(headers, "Content-Type: application/json");
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  // Perform
  g_last_http_status = 0;
  CURLcode res = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &g_last_http_status);
  
  // Cleanup headers
  curl_slist_free_all(headers);

  if (res != CURLE_OK) {
    return "";
  }
  return response;
}

static std::string HttpGet(const std::string &url, const std::string &token) {
  return PerformHttpRequest("GET", url, token, "");
}

static long LastHttpStatus() {
  return g_last_http_status;
}

static std::string HttpPostOrPut(const std::string &method, const std::string &url, const std::string &token, const std::string &data = "") {
  return PerformHttpRequest(method, url, token, data);
}

static void HttpPostOrPutAsync(const std::string &method, const std::string &url, const std::string &token, const std::string &data = "") {
  std::thread([method, url, token, data]() {
    HttpPostOrPut(method, url, token, data);
  }).detach();
}

static void HttpPostOrPutDeviceAsync(const std::string &method, const std::string &url, const std::string &token, const std::string &data = "") {
  std::thread([method, url, token, data]() {
    std::string device_id = GetSpotiampDeviceIdFromToken(token);
    HttpPostOrPut(method, UrlWithDeviceId(url, device_id), token, data);
  }).detach();
}

// Base64 helper
static std::string Base64Encode(const std::string &in) {
  static const char* b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out = "";
  int val = 0, valb = -6;
  for (unsigned char c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out.push_back(b64_chars[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(b64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

static std::string GetAccessTokenFromCode(const std::string &code, const std::string &client_id, const std::string &client_secret) {
  std::string base64_auth = Base64Encode(client_id + ":" + client_secret);
  std::string cmd = "curl -s --max-time 10 --connect-timeout 5 -X POST https://accounts.spotify.com/api/token "
                    "-H \"Authorization: Basic " + base64_auth + "\" "
                    "-H \"Content-Type: application/x-www-form-urlencoded\" "
                    "-d \"grant_type=authorization_code&code=" + code + "&redirect_uri=http://127.0.0.1:3000/callback\"";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) return "";
  char buffer[1024];
  std::string result = "";
  while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
    result += buffer;
  }
  pclose(pipe);
  return result;
}

static std::string RefreshAccessToken(const std::string &refresh_token, const std::string &client_id, const std::string &client_secret) {
  std::string base64_auth = Base64Encode(client_id + ":" + client_secret);
  std::string cmd = "curl -s --max-time 10 --connect-timeout 5 -X POST https://accounts.spotify.com/api/token "
                    "-H \"Authorization: Basic " + base64_auth + "\" "
                    "-H \"Content-Type: application/x-www-form-urlencoded\" "
                    "-d \"grant_type=refresh_token&refresh_token=" + refresh_token + "\"";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) return "";
  char buffer[1024];
  std::string result = "";
  while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
    result += buffer;
  }
  pclose(pipe);
  return result;
}

static std::string ListenForAuthCode() {
  if (!EnsureSocketLayer()) return "";
  SocketHandle server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == kInvalidSocket) return "";
  
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
  
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(3000);
  
  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    CloseSocketHandle(server_fd);
    return "";
  }
  
  if (listen(server_fd, 1) < 0) {
    CloseSocketHandle(server_fd);
    return "";
  }
  
  std::cout << "\n========================================================" << std::endl;
  std::cout << "Waiting for Spotify authorization redirect (Timeout 60s)..." << std::endl;
  std::cout << "========================================================\n" << std::endl;
  
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(server_fd, &fds);
  struct timeval tv;
  tv.tv_sec = 60;
  tv.tv_usec = 0;
  
#if defined(_WIN32)
  int sel = select(0, &fds, NULL, NULL, &tv);
#else
  int sel = select(server_fd + 1, &fds, NULL, NULL, &tv);
#endif
  if (sel <= 0) {
    std::cout << "Spotify authorization timed out." << std::endl;
    CloseSocketHandle(server_fd);
    return "";
  }
  
  SocketHandle client_fd = accept(server_fd, NULL, NULL);
  if (client_fd == kInvalidSocket) {
    CloseSocketHandle(server_fd);
    return "";
  }
  
  char buffer[4096];
  int bytes_read = SocketRead(client_fd, buffer, sizeof(buffer) - 1);
  std::string code = "";
  if (bytes_read > 0) {
    buffer[bytes_read] = '\0';
    std::string request(buffer);
    
    size_t code_pos = request.find("code=");
    if (code_pos != std::string::npos) {
      size_t space_pos = request.find(" ", code_pos);
      size_t amp_pos = request.find("&", code_pos);
      size_t end_pos = (amp_pos != std::string::npos && amp_pos < space_pos) ? amp_pos : space_pos;
      code = request.substr(code_pos + 5, end_pos - code_pos - 5);
    }
    
    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                           "<html><head><title>Spotiamp</title></head>"
                           "<body style=\"font-family: sans-serif; text-align: center; margin-top: 50px;\">"
                           "<h2>Spotiamp Authorized Successfully!</h2>"
                           "<p>You can close this tab and return to the Spotiamp application.</p>"
                           "</body></html>";
    SocketWrite(client_fd, response.c_str(), (int)response.length());
  }
  
  CloseSocketHandle(client_fd);
  CloseSocketHandle(server_fd);
  return code;
}

// Simple JSON parser helpers
static std::string JsonExtractString(const std::string &json, const std::string &key) {
  size_t start = 0;
  while (true) {
    start = json.find("\"" + key + "\"", start);
    if (start == std::string::npos) return "";
    
    size_t colon = json.find(":", start);
    if (colon == std::string::npos) return "";
    
    size_t quote_start = json.find("\"", colon);
    if (quote_start == std::string::npos) return "";
    
    size_t quote_end = json.find("\"", quote_start + 1);
    if (quote_end == std::string::npos) return "";
    
    return json.substr(quote_start + 1, quote_end - quote_start - 1);
  }
}

static int JsonExtractInt(const std::string &json, const std::string &key) {
  size_t start = json.find("\"" + key + "\"");
  if (start == std::string::npos) return 0;
  
  size_t colon = json.find(":", start);
  if (colon == std::string::npos) return 0;
  
  size_t val_start = json.find_first_not_of(" \t\r\n", colon + 1);
  if (val_start == std::string::npos) return 0;
  
  size_t val_end = json.find_first_of(",}\r\n", val_start);
  if (val_end == std::string::npos) return 0;
  
  std::string val_str = json.substr(val_start, val_end - val_start);
  if (val_str == "true") return 1;
  if (val_str == "false") return 0;
  return atoi(val_str.c_str());
}

static bool JsonHasNumericValue(const std::string &json, const std::string &key) {
  size_t start = json.find("\"" + key + "\"");
  if (start == std::string::npos) return false;

  size_t colon = json.find(":", start);
  if (colon == std::string::npos) return false;

  size_t val_start = json.find_first_not_of(" \t\r\n", colon + 1);
  if (val_start == std::string::npos) return false;

  char c = json[val_start];
  return c == '-' || (c >= '0' && c <= '9');
}

static bool JsonHasObjectValue(const std::string &json, const std::string &key, size_t *object_pos) {
  size_t start = json.find("\"" + key + "\"");
  if (start == std::string::npos) return false;

  size_t colon = json.find(":", start);
  if (colon == std::string::npos) return false;

  size_t val_start = json.find_first_not_of(" \t\r\n", colon + 1);
  if (val_start == std::string::npos || json[val_start] != '{') return false;

  if (object_pos) *object_pos = val_start;
  return true;
}

static size_t JsonFindShallowKeyColon(const std::string &json, const std::string &key) {
  std::string needle = "\"" + key + "\"";
  bool in_string = false;
  bool escape = false;
  int depth = 0;

  for (size_t i = 0; i < json.size(); ++i) {
    char c = json[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (in_string && c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      if (!in_string && depth == 1 && json.compare(i, needle.size(), needle) == 0) {
        size_t colon = json.find(':', i + needle.size());
        return colon == std::string::npos ? std::string::npos : colon;
      }
      in_string = !in_string;
      continue;
    }
    if (in_string) continue;
    if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') depth--;
  }
  return std::string::npos;
}

static std::string JsonExtractStringShallow(const std::string &json, const std::string &key) {
  size_t colon = JsonFindShallowKeyColon(json, key);
  if (colon == std::string::npos) return "";

  size_t quote_start = json.find('"', colon + 1);
  if (quote_start == std::string::npos) return "";
  size_t quote_end = quote_start + 1;
  bool escape = false;
  while (quote_end < json.size()) {
    if (escape) {
      escape = false;
    } else if (json[quote_end] == '\\') {
      escape = true;
    } else if (json[quote_end] == '"') {
      break;
    }
    quote_end++;
  }
  if (quote_end >= json.size()) return "";
  return json.substr(quote_start + 1, quote_end - quote_start - 1);
}

static int JsonExtractIntShallow(const std::string &json, const std::string &key) {
  size_t colon = JsonFindShallowKeyColon(json, key);
  if (colon == std::string::npos) return 0;

  size_t val_start = json.find_first_not_of(" \t\r\n", colon + 1);
  if (val_start == std::string::npos) return 0;
  size_t val_end = json.find_first_of(",}\r\n", val_start);
  if (val_end == std::string::npos) return 0;
  return atoi(json.substr(val_start, val_end - val_start).c_str());
}

static bool JsonExtractBoolShallow(const std::string &json, const std::string &key, bool def) {
  size_t colon = JsonFindShallowKeyColon(json, key);
  if (colon == std::string::npos) return def;

  size_t val_start = json.find_first_not_of(" \t\r\n", colon + 1);
  if (val_start == std::string::npos) return def;
  if (json.compare(val_start, 4, "true") == 0) return true;
  if (json.compare(val_start, 5, "false") == 0) return false;
  return def;
}

static std::string JsonExtractObjectShallow(const std::string &json, const std::string &key) {
  size_t colon = JsonFindShallowKeyColon(json, key);
  if (colon == std::string::npos) return "";

  size_t start = json.find_first_not_of(" \t\r\n", colon + 1);
  if (start == std::string::npos || json[start] != '{') return "";

  int depth = 1;
  bool in_string = false;
  bool escape = false;
  size_t end = start + 1;
  while (end < json.size() && depth > 0) {
    char c = json[end];
    if (escape) {
      escape = false;
    } else if (in_string && c == '\\') {
      escape = true;
    } else if (c == '"') {
      in_string = !in_string;
    } else if (!in_string && c == '{') {
      depth++;
    } else if (!in_string && c == '}') {
      depth--;
    }
    end++;
  }
  return depth == 0 ? json.substr(start, end - start) : "";
}

static std::string JsonExtractArrayStringShallow(const std::string &json, const std::string &key) {
  size_t colon = JsonFindShallowKeyColon(json, key);
  if (colon == std::string::npos) return "";

  size_t start = json.find_first_not_of(" \t\r\n", colon + 1);
  if (start == std::string::npos || json[start] != '[') return "";

  int depth = 1;
  bool in_string = false;
  bool escape = false;
  size_t end = start + 1;
  while (end < json.size() && depth > 0) {
    char c = json[end];
    if (escape) {
      escape = false;
    } else if (in_string && c == '\\') {
      escape = true;
    } else if (c == '"') {
      in_string = !in_string;
    } else if (!in_string && c == '[') {
      depth++;
    } else if (!in_string && c == ']') {
      depth--;
    }
    end++;
  }
  return depth == 0 ? json.substr(start, end - start) : "";
}

static std::vector<std::string> JsonExtractArray(const std::string &json, const std::string &key) {
  std::vector<std::string> items;
  size_t array_start = json.find("\"" + key + "\"");
  if (array_start == std::string::npos) return items;
  
  array_start = json.find("[", array_start);
  if (array_start == std::string::npos) return items;
  
  size_t bracket_count = 1;
  size_t array_end = array_start + 1;
  while (bracket_count > 0 && array_end < json.length()) {
    if (json[array_end] == '[') bracket_count++;
    else if (json[array_end] == ']') bracket_count--;
    array_end++;
  }
  
  std::string array_content = json.substr(array_start + 1, array_end - array_start - 2);
  
  size_t pos = 0;
  while (true) {
    size_t obj_start = array_content.find("{", pos);
    if (obj_start == std::string::npos) break;
    
    size_t brace_count = 1;
    size_t obj_end = obj_start + 1;
    while (brace_count > 0 && obj_end < array_content.length()) {
      if (array_content[obj_end] == '{') brace_count++;
      else if (array_content[obj_end] == '}') brace_count--;
      obj_end++;
    }
    
    items.push_back(array_content.substr(obj_start, obj_end - obj_start));
    pos = obj_end;
  }
  
  return items;
}

static std::vector<std::string> JsonExtractObjectsFromArrayString(const std::string &array_json) {
  std::vector<std::string> items;
  size_t array_start = array_json.find("[");
  size_t array_end = array_json.rfind("]");
  if (array_start == std::string::npos || array_end == std::string::npos || array_end <= array_start)
    return items;

  std::string array_content = array_json.substr(array_start + 1, array_end - array_start - 1);
  size_t pos = 0;
  while (true) {
    size_t obj_start = array_content.find("{", pos);
    if (obj_start == std::string::npos) break;

    size_t brace_count = 1;
    size_t obj_end = obj_start + 1;
    bool in_string = false;
    bool escape = false;
    while (brace_count > 0 && obj_end < array_content.length()) {
      char c = array_content[obj_end];
      if (escape) {
        escape = false;
      } else if (in_string && c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = !in_string;
      } else if (!in_string && c == '{') {
        brace_count++;
      } else if (!in_string && c == '}') {
        brace_count--;
      }
      obj_end++;
    }

    items.push_back(array_content.substr(obj_start, obj_end - obj_start));
    pos = obj_end;
  }
  return items;
}

static std::string UrlEncode(const std::string &s) {
  std::string encoded;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else if (c == ' ') {
      encoded += "%20";
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      encoded += hex;
    }
  }
  return encoded;
}

static std::string SpotifyIdFromUri(const std::string &uri, const char *type) {
  std::string prefix = std::string("spotify:") + type + ":";
  if (uri.rfind(prefix, 0) != 0)
    return "";
  size_t start = prefix.size();
  size_t end = uri.find(':', start);
  return uri.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static TspItem *CreateTrackItemFromJson(const std::string &track_json,
                                        const std::string &album_uri_override = "",
                                        const std::string &image_uri_override = "") {
  TspItem *item = new TspItem();
  item->name = JsonExtractStringShallow(track_json, "name");
  item->uri = JsonExtractStringShallow(track_json, "uri");
  item->duration_sec = JsonExtractIntShallow(track_json, "duration_ms") / 1000;
  item->type = kTspItemType_Track;
  item->playable = JsonExtractBoolShallow(track_json, "is_playable", true) &&
                   !JsonExtractBoolShallow(track_json, "is_local", false);
  if (item->name.empty() || item->uri.empty() || item->duration_sec <= 0 || !item->playable) {
    delete item;
    return NULL;
  }

  auto artists = JsonExtractObjectsFromArrayString(JsonExtractArrayStringShallow(track_json, "artists"));
  if (artists.empty())
    artists = JsonExtractArray(track_json, "artists");
  if (!artists.empty()) {
    item->artist = JsonExtractStringShallow(artists[0], "name");
    item->artist_uri = JsonExtractStringShallow(artists[0], "uri");
  }

  std::string album_json = JsonExtractObjectShallow(track_json, "album");
  if (!album_json.empty()) {
    item->album_uri = JsonExtractStringShallow(album_json, "uri");
    auto images = JsonExtractArray(album_json, "images");
    if (!images.empty())
      item->image_uri = JsonExtractString(images[0], "url");
  }
  if (!album_uri_override.empty())
    item->album_uri = album_uri_override;
  if (!image_uri_override.empty())
    item->image_uri = image_uri_override;
  return item;
}

static void AppendTrackObjects(std::vector<TspItem*> *tracks,
                               const std::vector<std::string> &track_objects,
                               const std::string &album_uri_override = "",
                               const std::string &image_uri_override = "") {
  for (const auto &track_json : track_objects) {
    TspItem *item = CreateTrackItemFromJson(track_json, album_uri_override, image_uri_override);
    if (item)
      tracks->push_back(item);
  }
}

static void AppendPlaylistItemObjects(std::vector<TspItem*> *tracks,
                                      const std::vector<std::string> &items) {
  for (const auto &item_json : items) {
    std::string track_json = JsonExtractObjectShallow(item_json, "track");
    if (track_json.empty())
      continue;
    TspItem *item = CreateTrackItemFromJson(track_json);
    if (item)
      tracks->push_back(item);
  }
}

static void AppendPlaylistTracks(std::vector<TspItem*> *tracks, Tsp *tsp, const std::string &playlist_id) {
  int start_count = (int)tracks->size();
  std::string url = "https://api.spotify.com/v1/playlists/" + playlist_id + "/tracks?limit=50&market=from_token";
  bool got_error = false;
  while (!url.empty()) {
    std::string response = HttpGet(url, tsp->access_token);
    if (response.find("\"error\"") != std::string::npos) {
      got_error = true;
      std::cout << "[DEBUG] AppendPlaylistTracks: items endpoint status " << LastHttpStatus()
                << " for playlist " << playlist_id << ": " << response << std::endl;
      break;
    }
    AppendPlaylistItemObjects(tracks, JsonExtractArray(response, "items"));

    std::string next_url = JsonExtractString(response, "next");
    url = next_url.rfind("https://", 0) == 0 ? next_url : "";
  }

  if ((int)tracks->size() == start_count && got_error) {
    std::string response = HttpGet("https://api.spotify.com/v1/playlists/" + playlist_id +
                                   "?market=from_token", tsp->access_token);
    if (response.find("\"error\"") != std::string::npos) {
      std::cout << "[DEBUG] AppendPlaylistTracks: playlist endpoint status " << LastHttpStatus()
                << " for playlist " << playlist_id << ": " << response << std::endl;
    } else {
      std::string tracks_json = JsonExtractObjectShallow(response, "tracks");
      AppendPlaylistItemObjects(tracks, JsonExtractArray(tracks_json, "items"));
      std::string next_url = JsonExtractString(tracks_json, "next");
      while (next_url.rfind("https://", 0) == 0) {
        response = HttpGet(next_url, tsp->access_token);
        if (response.find("\"error\"") != std::string::npos) {
          std::cout << "[DEBUG] AppendPlaylistTracks: playlist next status " << LastHttpStatus()
                    << " for playlist " << playlist_id << ": " << response << std::endl;
          break;
        }
        AppendPlaylistItemObjects(tracks, JsonExtractArray(response, "items"));
        next_url = JsonExtractString(response, "next");
      }
    }
  }

  std::cout << "[DEBUG] AppendPlaylistTracks: playlist " << playlist_id << " loaded "
            << ((int)tracks->size() - start_count) << " playable tracks" << std::endl;
}

static bool RefreshPlayerListFromQueue(Tsp *tsp, const std::string &access_token,
                                       const std::string &context_uri,
                                       bool append_only = false) {
  if (!tsp || access_token.empty())
    return false;

  std::string response = HttpGet("https://api.spotify.com/v1/me/player/queue", access_token);
  if (response.find("\"error\"") != std::string::npos) {
    std::cout << "[DEBUG] RefreshPlayerListFromQueue: status " << LastHttpStatus()
              << ": " << response << std::endl;
    return false;
  }

  std::vector<TspItem*> tracks;
  std::string current_json = JsonExtractObjectShallow(response, "currently_playing");
  if (!current_json.empty()) {
    TspItem *item = CreateTrackItemFromJson(current_json);
    if (item)
      tracks.push_back(item);
  }
  AppendTrackObjects(&tracks, JsonExtractArray(response, "queue"));
  if (tracks.empty()) {
    std::cout << "[DEBUG] RefreshPlayerListFromQueue: queue returned no playable tracks" << std::endl;
    return false;
  }

  int added = 0;
  bool index_changed = false;
  if (!append_only || tsp->player_list->items.empty() ||
      tsp->player_list->uri != context_uri) {
    ClearItemList(tsp->player_list);
    tsp->player_list->uri = context_uri;
    tsp->player_list->items = tracks;
    tsp->player_list->now_playing_index = 0;
    added = (int)tracks.size();
    tracks.clear();
  } else {
    for (int i = 0; i < (int)tracks.size(); ++i) {
      TspItem *item = tracks[i];
      if (!item)
        continue;
      int existing_index = FindTrackIndexByUri(tsp->player_list, item->uri);
      if (i == 0 && existing_index >= 0 &&
          tsp->player_list->now_playing_index != existing_index) {
        tsp->player_list->now_playing_index = existing_index;
        index_changed = true;
      }
      if (existing_index >= 0) {
        delete item;
        tracks[i] = NULL;
        continue;
      }
      if (i == 0) {
        tsp->player_list->now_playing_index = (int)tsp->player_list->items.size();
        index_changed = true;
      }
      tsp->player_list->items.push_back(item);
      tracks[i] = NULL;
      added++;
    }
    DeleteItemVector(&tracks);
  }
  tsp->player_list->total_length = (int)tsp->player_list->items.size();
  tsp->player_list->dynamic_loading = false;
  if (append_only && added == 0 && !index_changed)
    return true;
  DispatchPlayerCallback(tsp, kTspCallbackEvent_ItemListChanged, tsp->player_list, NULL);
  std::cout << "[DEBUG] RefreshPlayerListFromQueue: "
            << (append_only ? "appended " : "loaded ") << added
            << " tracks from Spotify queue; player list now has "
            << tsp->player_list->items.size() << " tracks" << std::endl;
  return true;
}

static void AppendSearchTracks(std::vector<TspItem*> *tracks, Tsp *tsp, const std::string &query, int limit) {
  std::string url = "https://api.spotify.com/v1/search?q=" + UrlEncode(query) +
                    "&type=track&limit=" + std::to_string(limit);
  std::string response = HttpGet(url, tsp->access_token);
  size_t tracks_pos = response.find("\"tracks\"");
  if (tracks_pos == std::string::npos)
    return;
  auto items = JsonExtractArray(response.substr(tracks_pos), "items");
  AppendTrackObjects(tracks, items);
}

static int LoadSavedTracksPage(TspItemList *tl, Tsp *tsp, int offset, int limit) {
  if (!tl || !tsp || limit <= 0)
    return 0;
  std::string url = "https://api.spotify.com/v1/me/tracks?limit=" +
                    std::to_string(limit) + "&offset=" + std::to_string(offset);
  std::string response = HttpGet(url, tsp->access_token);
  int total = JsonExtractInt(response, "total");
  if (total > 0 && total != tl->total_length) {
    tl->total_length = total;
    tl->items.resize(total, NULL);
  }

  auto items = JsonExtractArray(response, "items");
  int loaded = 0;
  for (const auto &item_json : items) {
    std::string track_json = JsonExtractObjectShallow(item_json, "track");
    if (track_json.empty())
      continue;
    TspItem *item = CreateTrackItemFromJson(track_json);
    if (!item)
      continue;
    int slot = offset + loaded;
    if (slot >= (int)tl->items.size())
      tl->items.resize(slot + 1, NULL);
    delete tl->items[slot];
    tl->items[slot] = item;
    loaded++;
  }
  if (tl->total_length <= 0)
    tl->total_length = (int)tl->items.size();
  return loaded;
}

static const char *TopListPlaylistId(const std::string &code) {
  struct TopListMap { const char *code; const char *id; };
  static const TopListMap kTopLists[] = {
    {"everywhere", "37i9dQZEVXbMDoHDwVN2tF"},
    {"BE", "37i9dQZEVXbJNSeeHswcKB"},
    {"DK", "37i9dQZEVXbL3J0k32lWnN"},
    {"FI", "37i9dQZEVXbMxcczTSoGwZ"},
    {"FR", "37i9dQZEVXbIPWwFssbupI"},
    {"DE", "37i9dQZEVXbJiZcmkrIHGU"},
    {"IE", "37i9dQZEVXbKM896FDX8L1"},
    {"IT", "37i9dQZEVXbIQnj7RRhdSX"},
    {"NL", "37i9dQZEVXbKCF6dqVpDkS"},
    {"NO", "37i9dQZEVXbJvfa0Yxg7E7"},
    {"ES", "37i9dQZEVXbNFJfN1Vw8d9"},
    {"SE", "37i9dQZEVXbLoATJ81JYXz"},
    {"GB", "37i9dQZEVXbLnolsZ8PSNw"},
    {"US", "37i9dQZEVXbLRQDuF5jeBp"},
  };
  for (const auto &it : kTopLists) {
    if (code == it.code)
      return it.id;
  }
  return NULL;
}

static const char *TopListSearchQuery(const std::string &code) {
  struct TopListQuery { const char *code; const char *query; };
  static const TopListQuery kQueries[] = {
    {"everywhere", "Top 50 - Global"},
    {"BE", "Top 50 - Belgium"},
    {"DK", "Top 50 - Denmark"},
    {"FI", "Top 50 - Finland"},
    {"FR", "Top 50 - France"},
    {"DE", "Top 50 - Germany"},
    {"IE", "Top 50 - Ireland"},
    {"IT", "Top 50 - Italy"},
    {"NL", "Top 50 - Netherlands"},
    {"NO", "Top 50 - Norway"},
    {"ES", "Top 50 - Spain"},
    {"SE", "Top 50 - Sweden"},
    {"GB", "Top 50 - United Kingdom"},
    {"US", "Top 50 - USA"},
  };
  for (const auto &it : kQueries) {
    if (code == it.code)
      return it.query;
  }
  return NULL;
}

static bool EqualFold(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
      return false;
  }
  return true;
}

static std::string FindUserPlaylistByName(Tsp *tsp, const char *name) {
  if (!tsp || !name)
    return "";
  for (const auto &item : tsp->rootlist) {
    if (item.type == kTspItemType_Playlist && EqualFold(item.name, name)) {
      return SpotifyIdFromUri(item.uri, "playlist");
    }
  }
  std::string url = "https://api.spotify.com/v1/me/playlists?limit=50";
  while (!url.empty()) {
    std::string response = HttpGet(url, tsp->access_token);
    auto items = JsonExtractArray(response, "items");
    for (const auto &item_json : items) {
      if (!EqualFold(JsonExtractStringShallow(item_json, "name"), name))
        continue;
      std::string id = SpotifyIdFromUri(JsonExtractStringShallow(item_json, "uri"), "playlist");
      if (!id.empty())
        return id;
    }
    std::string next_url = JsonExtractString(response, "next");
    url = next_url.rfind("https://", 0) == 0 ? next_url : "";
  }
  return "";
}

static std::string FindPlaylistBySearch(Tsp *tsp, const char *query, const char *preferred_name) {
  if (!tsp || !query)
    return "";
  std::string url = "https://api.spotify.com/v1/search?q=" + UrlEncode(query) +
                    "&type=playlist&limit=10";
  std::string response = HttpGet(url, tsp->access_token);
  size_t playlists_pos = response.find("\"playlists\"");
  if (playlists_pos == std::string::npos)
    return "";
  auto items = JsonExtractArray(response.substr(playlists_pos), "items");
  std::string first_id;
  for (const auto &playlist_json : items) {
    std::string uri = JsonExtractStringShallow(playlist_json, "uri");
    std::string id = SpotifyIdFromUri(uri, "playlist");
    if (id.empty())
      continue;
    if (first_id.empty())
      first_id = id;
    std::string name = JsonExtractStringShallow(playlist_json, "name");
    if (preferred_name && EqualFold(name, preferred_name))
      return id;
  }
  return first_id;
}

static void AppendPlaylistSearchTracks(std::vector<TspItem*> *tracks, Tsp *tsp,
                                       const char *query, const char *preferred_name) {
  std::string playlist_id = FindPlaylistBySearch(tsp, query, preferred_name);
  if (!playlist_id.empty())
    AppendPlaylistTracks(tracks, tsp, playlist_id);
}


static void AppendAlbumTracks(std::vector<TspItem*> *tracks, Tsp *tsp, const std::string &album_id) {
  if (album_id.empty())
    return;
  int start_count = (int)tracks->size();
  std::string response = HttpGet("https://api.spotify.com/v1/albums/" + album_id +
                                 "?market=from_token", tsp->access_token);
  if (response.find("\"error\"") != std::string::npos) {
    std::cout << "[DEBUG] AppendAlbumTracks: status " << LastHttpStatus()
              << " for album " << album_id << ": " << response << std::endl;
    return;
  }
  std::string album_uri = JsonExtractStringShallow(response, "uri");
  std::string image_uri;
  auto images = JsonExtractArray(response, "images");
  if (!images.empty())
    image_uri = JsonExtractString(images[0], "url");

  std::string url = "https://api.spotify.com/v1/albums/" + album_id +
                    "/tracks?limit=50&market=from_token";
  while (!url.empty()) {
    response = HttpGet(url, tsp->access_token);
    if (response.find("\"error\"") != std::string::npos) {
      std::cout << "[DEBUG] AppendAlbumTracks: tracks status " << LastHttpStatus()
                << " for album " << album_id << ": " << response << std::endl;
      break;
    }
    AppendTrackObjects(tracks, JsonExtractArray(response, "items"),
                       album_uri.empty() ? "spotify:album:" + album_id : album_uri,
                       image_uri);
    std::string next_url = JsonExtractString(response, "next");
    url = next_url.rfind("https://", 0) == 0 ? next_url : "";
  }
  std::cout << "[DEBUG] AppendAlbumTracks: album " << album_id << " loaded "
            << ((int)tracks->size() - start_count) << " playable tracks" << std::endl;
}

static bool TrackListHasUri(const std::vector<TspItem*> &tracks, const std::string &uri) {
  if (uri.empty())
    return true;
  for (auto *item : tracks) {
    if (item && item->uri == uri)
      return true;
  }
  return false;
}

static void MoveUniqueTracks(std::vector<TspItem*> *tracks, std::vector<TspItem*> *source, int max_tracks) {
  for (auto *item : *source) {
    if (!item)
      continue;
    if ((max_tracks > 0 && (int)tracks->size() >= max_tracks) || TrackListHasUri(*tracks, item->uri)) {
      delete item;
      continue;
    }
    tracks->push_back(item);
  }
  source->clear();
}

static bool StringVectorContains(const std::vector<std::string> &values, const std::string &needle) {
  for (const auto &value : values) {
    if (value == needle)
      return true;
  }
  return false;
}

static void AppendArtistCatalogTracks(std::vector<TspItem*> *tracks, Tsp *tsp,
                                      const std::string &artist_id, int max_tracks) {
  if (artist_id.empty() || (max_tracks > 0 && (int)tracks->size() >= max_tracks))
    return;

  std::string url = "https://api.spotify.com/v1/artists/" + artist_id +
                    "/albums?include_groups=album,single&limit=10&market=from_token";
  std::string response = HttpGet(url, tsp->access_token);
  auto albums = JsonExtractArray(response, "items");
  std::vector<std::string> album_ids;
  for (const auto &album_json : albums) {
    std::string album_id = SpotifyIdFromUri(JsonExtractStringShallow(album_json, "uri"), "album");
    if (album_id.empty() || StringVectorContains(album_ids, album_id))
      continue;
    album_ids.push_back(album_id);

    std::vector<TspItem*> album_tracks;
    AppendAlbumTracks(&album_tracks, tsp, album_id);
    MoveUniqueTracks(tracks, &album_tracks, max_tracks);
    if (max_tracks > 0 && (int)tracks->size() >= max_tracks)
      break;
  }
}

static void AppendTrackSeedCatalogTracks(std::vector<TspItem*> *tracks, Tsp *tsp,
                                         const std::string &track_id, int max_tracks) {
  if (track_id.empty() || (max_tracks > 0 && (int)tracks->size() >= max_tracks))
    return;

  std::string response = HttpGet("https://api.spotify.com/v1/tracks/" + track_id, tsp->access_token);
  auto artists = JsonExtractObjectsFromArrayString(JsonExtractArrayStringShallow(response, "artists"));
  if (artists.empty())
    artists = JsonExtractArray(response, "artists");

  int artist_count = 0;
  for (const auto &artist_json : artists) {
    std::string artist_id = SpotifyIdFromUri(JsonExtractStringShallow(artist_json, "uri"), "artist");
    if (artist_id.empty())
      continue;
    AppendArtistCatalogTracks(tracks, tsp, artist_id, max_tracks);
    if (++artist_count >= 3 || (max_tracks > 0 && (int)tracks->size() >= max_tracks))
      break;
  }

  if (tracks->empty()) {
    std::string album_id = SpotifyIdFromUri(JsonExtractStringShallow(JsonExtractObjectShallow(response, "album"), "uri"),
                                           "album");
    if (!album_id.empty()) {
      std::vector<TspItem*> album_tracks;
      AppendAlbumTracks(&album_tracks, tsp, album_id);
      MoveUniqueTracks(tracks, &album_tracks, max_tracks);
    }
  }

  if (tracks->empty()) {
    TspItem *seed_item = CreateTrackItemFromJson(response);
    if (seed_item)
      tracks->push_back(seed_item);
  }
}

static void AppendPlaylistSeedCatalogTracks(std::vector<TspItem*> *tracks, Tsp *tsp,
                                            const std::vector<std::string> &radio_seed_uris,
                                            int max_tracks) {
  for (const auto &uri : radio_seed_uris) {
    AppendTrackSeedCatalogTracks(tracks, tsp, SpotifyIdFromUri(uri, "track"), max_tracks);
    if (max_tracks > 0 && (int)tracks->size() >= max_tracks)
      break;
  }
}

static std::string GenreSearchQueryFromUriCode(std::string genre) {
  for (char &c : genre) {
    if (c == '_')
      c = '-';
  }
  if (genre == "hip-hop")
    return "genre:hip-hop";
  if (genre == "r-n-b")
    return "genre:r-n-b";
  if (genre == "60s" || genre == "70s" || genre == "80s" || genre == "90s" || genre == "00s")
    return genre;
  return "genre:" + genre;
}

static void BuildTracksForUri(std::vector<TspItem*> *tracks, Tsp *tsp, const std::string &uri_str,
                              const std::string &playlist_id,
                              const std::vector<std::string> &radio_seed_uris) {
  if (!playlist_id.empty()) {
    AppendPlaylistTracks(tracks, tsp, playlist_id);
  } else if (uri_str.rfind("search:", 0) == 0) {
    AppendSearchTracks(tracks, tsp, uri_str.substr(7), 50);
  } else if (uri_str.rfind("spotify:album:", 0) == 0) {
    AppendAlbumTracks(tracks, tsp, SpotifyIdFromUri(uri_str, "album"));
  } else if (uri_str.rfind("spotify:toplist:track:", 0) == 0) {
    std::string code = uri_str.substr(strlen("spotify:toplist:track:"));
    if (code == "discoverweekly" || code.rfind("discoverweekly:", 0) == 0) {
      std::string discover_playlist_id;
      bool has_saved_playlist_id = code.rfind("discoverweekly:", 0) == 0;
      if (code.rfind("discoverweekly:", 0) == 0)
        discover_playlist_id = code.substr(strlen("discoverweekly:"));
      if (!discover_playlist_id.empty())
        AppendPlaylistTracks(tracks, tsp, discover_playlist_id);
      if (tracks->empty()) {
        discover_playlist_id = FindUserPlaylistByName(tsp, "Discover Weekly");
        if (!discover_playlist_id.empty())
          AppendPlaylistTracks(tracks, tsp, discover_playlist_id);
      }
      if (tracks->empty()) {
        std::cout << "[DEBUG] Discover Weekly: no API-visible playlist tracks"
                  << (has_saved_playlist_id ? " for saved playlist" : "") << std::endl;
      }
    } else if (const char *top_playlist_id = TopListPlaylistId(code)) {
      AppendPlaylistTracks(tracks, tsp, top_playlist_id);
      if (tracks->empty()) {
        const char *query = TopListSearchQuery(code);
        AppendPlaylistSearchTracks(tracks, tsp, query ? query : ("Top 50 " + code).c_str(), query);
      }
    } else {
      AppendPlaylistSearchTracks(tracks, tsp, ("Top 50 " + code).c_str(), NULL);
    }
  } else if (uri_str.rfind("spotify:genre:", 0) == 0) {
    std::string genre = uri_str.substr(strlen("spotify:genre:"));
    AppendSearchTracks(tracks, tsp, GenreSearchQueryFromUriCode(genre), 50);
  } else if (uri_str.rfind("spotify:radio:track:", 0) == 0) {
    AppendTrackSeedCatalogTracks(tracks, tsp, SpotifyIdFromUri(uri_str, "radio:track"), 50);
  } else if (uri_str.rfind("spotify:radio:artist:", 0) == 0) {
    std::string artist_id = SpotifyIdFromUri(uri_str, "radio:artist");
    AppendArtistCatalogTracks(tracks, tsp, artist_id, 50);
  } else if (uri_str.rfind("spotify:radio:album:", 0) == 0) {
    std::string album_id = SpotifyIdFromUri(uri_str, "radio:album");
    std::vector<TspItem*> album_tracks;
    AppendAlbumTracks(&album_tracks, tsp, album_id);
    for (size_t i = 0; i < album_tracks.size() && i < 5 && (int)tracks->size() < 50; ++i) {
      AppendTrackSeedCatalogTracks(tracks, tsp, SpotifyIdFromUri(album_tracks[i]->uri, "track"), 50);
    }
    if (tracks->empty())
      tracks->swap(album_tracks);
    for (auto *item : album_tracks)
      delete item;
  } else if (uri_str == kTspUri_TrackRadio) {
    AppendPlaylistSeedCatalogTracks(tracks, tsp, radio_seed_uris, 50);
  }
}

static bool IsGeneratedPlayableUri(const std::string &uri) {
  return uri.rfind("spotify:radio:", 0) == 0 ||
         uri.rfind("spotify:genre:", 0) == 0 ||
         uri == kTspUri_TrackRadio;
}

static std::string ExtractSpotifyUri(const std::string &json, const std::string &type) {
  std::string prefix = "spotify:" + type + ":";
  size_t pos = json.find(prefix);
  if (pos == std::string::npos) return "";
  size_t quote_end = json.find("\"", pos);
  if (quote_end == std::string::npos) return "";
  return json.substr(pos, quote_end - pos);
}

static std::string GetSpotiampDeviceIdFromToken(const std::string &access_token) {
  if (access_token.empty()) return "";
  static std::mutex device_cache_mutex;
  static std::string cached_token;
  static std::string cached_device_id;
  {
    std::lock_guard<std::mutex> lock(device_cache_mutex);
    if (cached_token == access_token && !cached_device_id.empty()) {
      return cached_device_id;
    }
  }

  std::string devices_json = HttpGet("https://api.spotify.com/v1/me/player/devices", access_token);
  auto devices = JsonExtractArray(devices_json, "devices");
  std::string found_id = "";
  for (const auto &dev : devices) {
    std::string name = JsonExtractString(dev, "name");
    if (name == "Spotiamp") {
      found_id = JsonExtractString(dev, "id");
      break;
    }
  }

  if (!found_id.empty()) {
    std::lock_guard<std::mutex> lock(device_cache_mutex);
    cached_token = access_token;
    cached_device_id = found_id;
  }
  return found_id;
}

static std::string GetSpotiampDeviceId(Tsp *tsp) {
  return GetSpotiampDeviceIdFromToken(GetAccessToken(tsp));
}

// Playback bridge / subprocess control
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
static void SpawnLibrespot(const char *user, const char *pass, const char *token) {
  (void)user;
  (void)pass;
  int rc = sp_playback_bridge_start(token, NULL);
  if (rc != 0) {
    std::cout << "[DEBUG] Rust playback bridge start failed: " << rc << std::endl;
  }
}

static void KillLibrespot() {
  sp_playback_bridge_stop();
}

static bool LibrespotOutputAvailable() {
  return sp_playback_bridge_is_running() != 0;
}

static int ReadLibrespotOutput(char *buffer, int size) {
  intptr_t bytes_read = sp_playback_bridge_read(buffer, (uintptr_t)size);
  if (bytes_read <= 0)
    return -1;
  return (int)bytes_read;
}

static void DrainLibrespotPipeAligned(char *buffer, int *residual_count, int buffer_size) {
  (void)buffer;
  (void)buffer_size;
  *residual_count = 0;
  // The Rust bridge position is advanced by reads because reads represent PCM
  // handed to the audio device. During a local transition hold we intentionally
  // do not drain here, otherwise discarded audio would move the visible clock.
}
#elif defined(_WIN32)
static std::string QuoteWindowsArg(const std::string &arg) {
  std::string out = "\"";
  for (char c : arg) {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  out += "\"";
  return out;
}

static void SpawnLibrespot(const char *user, const char *pass, const char *token) {
  if (g_librespot_process.hProcess || g_librespot_stdout != INVALID_HANDLE_VALUE)
    return;

  SECURITY_ATTRIBUTES sa;
  memset(&sa, 0, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_pipe = INVALID_HANDLE_VALUE;
  HANDLE write_pipe = INVALID_HANDLE_VALUE;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
    return;
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  HANDLE log_file = CreateFileA("librespot_err.log", GENERIC_WRITE,
                                FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
  if (log_file == INVALID_HANDLE_VALUE)
    log_file = GetStdHandle(STD_ERROR_HANDLE);

  std::vector<std::string> args_vec;
  args_vec.push_back("librespot");
  args_vec.push_back("-B");
  args_vec.push_back("pipe");
  args_vec.push_back("-f");
  args_vec.push_back("S16");
  args_vec.push_back("-R");
  args_vec.push_back("100");
  args_vec.push_back("-E");
  args_vec.push_back("fixed");
  args_vec.push_back("-n");
  args_vec.push_back("Spotiamp");

  if (token && *token) {
    args_vec.push_back("-k");
    args_vec.push_back(token);
  } else if (user && *user && pass && *pass) {
    args_vec.push_back("-u");
    args_vec.push_back(user);
    args_vec.push_back("-p");
    args_vec.push_back(pass);
  }

  std::string command_line;
  for (size_t i = 0; i < args_vec.size(); ++i) {
    if (i > 0)
      command_line += " ";
    command_line += QuoteWindowsArg(args_vec[i]);
  }

  STARTUPINFOA si;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = write_pipe;
  si.hStdError = log_file;

  PROCESS_INFORMATION pi;
  memset(&pi, 0, sizeof(pi));
  std::vector<char> mutable_command(command_line.begin(), command_line.end());
  mutable_command.push_back('\0');
  BOOL ok = CreateProcessA(NULL, mutable_command.data(), NULL, NULL, TRUE,
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

  CloseHandle(write_pipe);
  if (log_file != INVALID_HANDLE_VALUE && log_file != GetStdHandle(STD_ERROR_HANDLE))
    CloseHandle(log_file);

  if (!ok) {
    CloseHandle(read_pipe);
    return;
  }

  g_librespot_process = pi;
  g_librespot_stdout = read_pipe;
}

static void KillLibrespot() {
  if (g_librespot_process.hProcess) {
    TerminateProcess(g_librespot_process.hProcess, 0);
    WaitForSingleObject(g_librespot_process.hProcess, 3000);
    CloseHandle(g_librespot_process.hThread);
    CloseHandle(g_librespot_process.hProcess);
    memset(&g_librespot_process, 0, sizeof(g_librespot_process));
  }
  if (g_librespot_stdout != INVALID_HANDLE_VALUE) {
    CloseHandle(g_librespot_stdout);
    g_librespot_stdout = INVALID_HANDLE_VALUE;
  }
}

static bool LibrespotOutputAvailable() {
  return g_librespot_stdout != INVALID_HANDLE_VALUE;
}

static int ReadLibrespotOutput(char *buffer, int size) {
  if (g_librespot_stdout == INVALID_HANDLE_VALUE)
    return -1;
  DWORD bytes_read = 0;
  if (!ReadFile(g_librespot_stdout, buffer, (DWORD)size, &bytes_read, NULL))
    return -1;
  return (int)bytes_read;
}

static void DrainLibrespotPipeAligned(char *buffer, int *residual_count, int buffer_size) {
  if (g_librespot_stdout == INVALID_HANDLE_VALUE)
    return;

  while (true) {
    DWORD available = 0;
    if (!PeekNamedPipe(g_librespot_stdout, NULL, 0, NULL, &available, NULL) || available == 0)
      break;

    DWORD to_read = available;
    if (to_read > (DWORD)(buffer_size - *residual_count))
      to_read = (DWORD)(buffer_size - *residual_count);

    DWORD bytes_read = 0;
    if (!ReadFile(g_librespot_stdout, buffer + *residual_count, to_read, &bytes_read, NULL) || bytes_read == 0)
      break;

    int total_bytes = *residual_count + (int)bytes_read;
    int process_bytes = total_bytes & ~3;
    *residual_count = total_bytes - process_bytes;
    if (*residual_count > 0)
      memmove(buffer, buffer + process_bytes, *residual_count);
  }
}
#else
static void SpawnLibrespot(const char *user, const char *pass, const char *token) {
  int pipefd[2];
  if (pipe(pipefd) < 0) return;

  g_librespot_pid = fork();
  if (g_librespot_pid == 0) {
    // Child
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    int logfd = open("librespot_err.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (logfd >= 0) {
      dup2(logfd, STDERR_FILENO);
      close(logfd);
    }

    std::vector<std::string> args_vec;
    args_vec.push_back("librespot");
    args_vec.push_back("-B");
    args_vec.push_back("pipe");
    args_vec.push_back("-f");
    args_vec.push_back("S16");
    args_vec.push_back("-R");
    args_vec.push_back("100");
    args_vec.push_back("-E");
    args_vec.push_back("fixed");
    args_vec.push_back("-n");
    args_vec.push_back("Spotiamp");

    if (token && *token) {
      args_vec.push_back("-k");
      args_vec.push_back(token);
    } else if (user && *user && pass && *pass) {
      args_vec.push_back("-u");
      args_vec.push_back(user);
      args_vec.push_back("-p");
      args_vec.push_back(pass);
    }

    std::vector<char*> args;
    for (size_t i = 0; i < args_vec.size(); ++i) {
      args.push_back((char*)args_vec[i].c_str());
    }
    args.push_back(NULL);

    execvp("librespot", args.data());
    exit(1);
  } else {
    // Parent
    close(pipefd[1]);
    g_librespot_stdout_fd = pipefd[0];
  }
}

static void KillLibrespot() {
  if (g_librespot_pid > 0) {
    kill(g_librespot_pid, SIGTERM);
    int status;
    waitpid(g_librespot_pid, &status, 0);
    g_librespot_pid = 0;
  }
  if (g_librespot_stdout_fd >= 0) {
    close(g_librespot_stdout_fd);
    g_librespot_stdout_fd = -1;
  }
}

static bool LibrespotOutputAvailable() {
  return g_librespot_stdout_fd >= 0;
}

static int ReadLibrespotOutput(char *buffer, int size) {
  if (g_librespot_stdout_fd < 0)
    return -1;
  return (int)read(g_librespot_stdout_fd, buffer, size);
}

static void DrainLibrespotPipeAligned(char *buffer, int *residual_count, int buffer_size) {
  if (g_librespot_stdout_fd < 0)
    return;

  while (true) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(g_librespot_stdout_fd, &read_fds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    int ready = select(g_librespot_stdout_fd + 1, &read_fds, NULL, NULL, &tv);
    if (ready <= 0 || !FD_ISSET(g_librespot_stdout_fd, &read_fds))
      break;

    ssize_t bytes_read = read(g_librespot_stdout_fd, buffer + *residual_count,
                              buffer_size - *residual_count);
    if (bytes_read <= 0)
      break;

    int total_bytes = *residual_count + (int)bytes_read;
    int process_bytes = total_bytes & ~3;
    *residual_count = total_bytes - process_bytes;
    if (*residual_count > 0)
      memmove(buffer, buffer + process_bytes, *residual_count);
  }
}
#endif

// Background threads
static void AudioThreadProc(Tsp *tsp) {
  const int chunk_size = 4096;
  char buffer[chunk_size + 4];
  int residual_count = 0;
  bool output_paused = false;
  
  TspSampleFormat format;
  format.channels = 2;
  format.sample_rate = 44100;
  format.replaygain_track = 0.0f;
  format.replaygain_album = 0.0f;

  while (tsp->audio_thread_running) {
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
    ApplyRustBridgePlaybackState(tsp);
#endif
    bool is_playing = false;
    bool transition_audio = false;
    bool desired_playing = false;
    bool locally_stopped = false;
    {
      std::lock_guard<std::mutex> lock(tsp->mutex);
      is_playing = tsp->is_playing;
      desired_playing = tsp->local_desired_playing;
      locally_stopped = tsp->local_stopped;
      long long now_ms = NowMonotonicMs();
      transition_audio = AudioTransitionLocked(tsp, now_ms);
    }
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
    if (desired_playing && !locally_stopped && !transition_audio) {
      SpPlaybackSnapshot snapshot = {};
      if (sp_playback_bridge_snapshot(&snapshot) == 0 &&
          (snapshot.state == 1 || snapshot.buffered_ms > 0)) {
        is_playing = true;
      }
    }
#endif
    if (!is_playing || transition_audio) {
      if (!output_paused) {
        DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
        output_paused = true;
      }
      SleepForMicroseconds(10000);
      continue;
    } else if (output_paused) {
      DispatchAudioControl(tsp, kTspAudioFlag_FlushBuffer);
      output_paused = false;
    }

    if (!LibrespotOutputAvailable()) {
      SleepForMicroseconds(10000);
      continue;
    }

    int bytes_read = ReadLibrespotOutput(buffer + residual_count, chunk_size - residual_count);
    if (bytes_read <= 0) {
      if (bytes_read == 0) {
        break;
      }
      SleepForMicroseconds(10000);
      continue;
    }
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
    ApplyRustBridgePlaybackState(tsp);
#endif

    int total_bytes = residual_count + bytes_read;
    int process_bytes = total_bytes & ~3; // Frame-align to 4 bytes
    
    if (process_bytes > 0 && tsp->audio_callback) {
      int samples_size = process_bytes / sizeof(TspSampleType);
      int samples_buffered = 0;
      int offset = 0;
      
      while (offset < samples_size && tsp->audio_thread_running) {
        int written = tsp->audio_callback(tsp->audio_context, 0, 
                                          (const TspSampleType*)(buffer + offset * sizeof(TspSampleType)), 
                                          samples_size - offset, 
                                          &format, &samples_buffered);
        if (written > 0) {
          offset += written;
        } else {
          SleepForMicroseconds(5000);
        }
      }
    }
    
    residual_count = total_bytes - process_bytes;
    if (residual_count > 0) {
      memmove(buffer, buffer + process_bytes, residual_count);
    }
  }
}

static void PollThreadProc(Tsp *tsp) {
  while (tsp->poll_thread_running) {
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
    ApplyRustBridgePlaybackState(tsp);
#endif
    std::string access_token;
    {
      std::lock_guard<std::mutex> lock(tsp->mutex);
      if (tsp->connected) {
        access_token = tsp->access_token;
      }
    }
    if (access_token.empty()) {
      SleepForSeconds(1);
      continue;
    }
    
    std::string response = HttpGet("https://api.spotify.com/v1/me/player", access_token);
    if (!response.empty()) {
      bool is_p = (JsonExtractString(response, "is_playing") == "true" || JsonExtractInt(response, "is_playing") == 1);
      bool has_numeric_progress = JsonHasNumericValue(response, "progress_ms");
      int pos = has_numeric_progress ? JsonExtractInt(response, "progress_ms") : 0;
      int old_pos = 0;
      bool old_is_playing = false;
      bool now_playing_changed = false;
      bool controls_changed = false;
      bool play_state_suppressed = false;
      {
        std::lock_guard<std::mutex> lock(tsp->mutex);
        old_pos = CurrentPlayerPositionMsLocked(tsp);
        old_is_playing = tsp->is_playing;
      }
      size_t item_pos = 0;
      if (JsonHasObjectValue(response, "item", &item_pos)) {
        std::string item_json = response.substr(item_pos);
        std::string track_name = JsonExtractStringShallow(item_json, "name");
        std::string track_uri = JsonExtractStringShallow(item_json, "uri");
        int duration_ms = JsonExtractIntShallow(item_json, "duration_ms");
        bool has_valid_item = !track_uri.empty() && duration_ms > 0;
        bool stale_idle_update = has_valid_item && has_numeric_progress &&
                                 old_is_playing && !is_p && pos == 0 && old_pos > 0;
        
        std::string artist_name = "";
        auto artists = JsonExtractObjectsFromArrayString(JsonExtractArrayStringShallow(item_json, "artists"));
        if (artists.empty())
          artists = JsonExtractArray(item_json, "artists");
        if (!artists.empty()) {
          artist_name = JsonExtractStringShallow(artists[0], "name");
        }
        
        // Parse cover art url
        std::string image_url = "";
        std::string album_uri = "";
        std::string album_json = JsonExtractObjectShallow(item_json, "album");
        if (!album_json.empty()) {
          album_uri = JsonExtractStringShallow(album_json, "uri");
          auto images = JsonExtractArray(album_json, "images");
          if (!images.empty()) {
            image_url = JsonExtractString(images[0], "url");
          }
        }
        
        std::lock_guard<std::mutex> lock(tsp->mutex);
        long long now_ms = NowMonotonicMs();
        int local_predicted_pos = CurrentPlayerPositionMsLocked(tsp);
        bool command_in_flight = tsp->local_control_until_ms > now_ms;
        bool suppress_play_state = command_in_flight && tsp->local_desired_playing != is_p;
        bool suppress_stopped_metadata = tsp->local_stopped && !is_p;
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
        bool bridge_owns_position = sp_playback_bridge_is_running() != 0;
#else
        bool bridge_owns_position = false;
#endif
        bool suppress_progress = PendingPositionSyncLocked(tsp) &&
                                 abs(pos - local_predicted_pos) > 1500;
        bool stale_end_restart_update = has_valid_item && has_numeric_progress &&
                                        old_is_playing && is_p && pos <= 2500 &&
                                        tsp->has_now_playing &&
                                        tsp->now_playing_item.uri == track_uri &&
                                        !tsp->repeat &&
                                        NearTrackEndLocked(tsp, old_pos);
        bool suppress_metadata = suppress_stopped_metadata ||
                                 stale_end_restart_update ||
                                 (command_in_flight &&
                                  ((tsp->has_now_playing && tsp->now_playing_item.uri != track_uri) ||
                                   tsp->local_desired_playing != is_p));
        bool suppress_position = bridge_owns_position || suppress_progress ||
                                 suppress_play_state || suppress_metadata;

        if (has_valid_item && has_numeric_progress && !stale_idle_update &&
            !stale_end_restart_update && !suppress_position) {
          tsp->is_playing = is_p;
          tsp->player_position_ms = pos;
          tsp->player_position_updated_ms = now_ms;
          ClearPositionTransitionLocked(tsp);
          if (is_p)
            tsp->local_stopped = false;
        } else if (stale_idle_update) {
        } else if (stale_end_restart_update) {
          play_state_suppressed = true;
        } else if (suppress_position) {
          play_state_suppressed = true;
        } else if (suppress_stopped_metadata) {
          play_state_suppressed = true;
        } else if (suppress_metadata) {
          play_state_suppressed = true;
        } else if (suppress_play_state) {
          play_state_suppressed = true;
        }

        if (has_valid_item && !suppress_metadata && tsp->now_playing_item.uri != track_uri) {
          tsp->now_playing_item.name = track_name;
          tsp->now_playing_item.artist = artist_name;
          tsp->now_playing_item.uri = track_uri;
          tsp->now_playing_item.album_uri = album_uri;
          tsp->now_playing_item.duration_sec = duration_ms / 1000;
          tsp->now_playing_item.image_uri = image_url;
          tsp->player_duration_ms = duration_ms;
          tsp->has_now_playing = true;
          tsp->local_stopped = false;
          now_playing_changed = true;
        }
      } else {
      }

      if (now_playing_changed) {
        DispatchPlayerCallback(tsp, kTspCallbackEvent_NowPlayingChanged);
        std::string queue_sync_context;
        {
          std::lock_guard<std::mutex> lock(tsp->mutex);
          if (tsp->player_list && !tsp->queue_sync_context_uri.empty() &&
              tsp->player_list->uri == tsp->queue_sync_context_uri)
            queue_sync_context = tsp->queue_sync_context_uri;
        }
        if (!queue_sync_context.empty())
          RefreshPlayerListFromQueue(tsp, access_token, queue_sync_context, true);
      } else if (!play_state_suppressed && has_numeric_progress && !(old_is_playing && !is_p && pos == 0 && old_pos > 0) &&
                 (old_pos / 1000 != pos / 1000 || old_is_playing != is_p)) {
        DispatchPlayerCallback(tsp, is_p ? kTspCallbackEvent_Resume : kTspCallbackEvent_Pause);
      }
      if (controls_changed) {
        DispatchPlayerCallback(tsp, kTspCallbackEvent_PlayControlsChanged);
      }
    } else {
    }

#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
    ApplyRustBridgePlaybackState(tsp);
#endif
    
    int sleep_steps = 20;
    {
      std::lock_guard<std::mutex> lock(tsp->mutex);
      if (CommandInFlightLocked(tsp, NowMonotonicMs()))
        sleep_steps = 3;
    }
    for (int i = 0; i < sleep_steps && tsp->poll_thread_running; ++i) {
      SleepForMicroseconds(100000);
      if (sleep_steps == 20) {
        std::lock_guard<std::mutex> lock(tsp->mutex);
        if (CommandInFlightLocked(tsp, NowMonotonicMs()))
          break;
      }
    }
  }
}

extern "C" {

TSP_PUBLIC TspBool TspAudioThreadingDoWork(Tsp *tsp) {
  return 0;
}

TSP_PUBLIC void TspAutoComplete(Tsp *tsp, const char *prefix) {
}

TSP_PUBLIC const char * TspAutoCompleteGetResult(Tsp *tsp, int i) {
  return NULL;
}

TSP_PUBLIC Tsp * TspCreate5(const char *ident, TspCallback *callback, void *context, const void *appkey, size_t appkey_size, const char *user_agent, int flags, TspMemoryLimits *limits, TspOs *os) {
  Tsp *tsp = new Tsp();
  tsp->callback = callback;
  tsp->callback_context = context;
  g_global_tsp = tsp;
  return tsp;
}

TSP_PUBLIC void TspDestroy(Tsp *tsp) {
  if (tsp) {
    tsp->audio_thread_running = false;
    tsp->poll_thread_running = false;
    KillLibrespot();
    if (tsp->audio_thread.joinable()) tsp->audio_thread.join();
    if (tsp->poll_thread.joinable()) tsp->poll_thread.join();
    TspItemListDestroy(tsp->player_list);
    delete tsp;
    if (g_global_tsp == tsp) g_global_tsp = NULL;
  }
}

TSP_PUBLIC int TspDoWork(Tsp *tsp) {
  return 100;
}

TSP_PUBLIC TspBool TspEnableAudioThreading(Tsp *tsp, TspMutex *mutex) {
  return 0;
}

TSP_PUBLIC int TspGetActiveRemoteDevice(Tsp *tsp) {
  return -1;
}

TSP_PUBLIC TspError TspGetConnectionError(Tsp *tsp) {
  return tsp ? tsp->connection_error : kTspErrorOk;
}

TSP_PUBLIC TspError TspGetCredentialToken(Tsp *tsp, char *token, int token_size) {
  if (tsp && token && token_size > 0) {
    strncpy(token, tsp->access_token.c_str(), token_size - 1);
    token[token_size - 1] = '\0';
    return kTspErrorOk;
  }
  return kTspErrorTemp;
}

TSP_PUBLIC TspOs * TspGetOs(Tsp *tsp) {
  return NULL;
}

TSP_PUBLIC const char * TspGetRadioUri(Tsp *tsp, const char *uri) {
  static std::string radio_uri;
  std::string seed = uri ? uri : "";
  if (seed.rfind("spotify:track:", 0) == 0) {
    radio_uri = "spotify:radio:track:" + SpotifyIdFromUri(seed, "track");
  } else if (seed.rfind("spotify:album:", 0) == 0) {
    radio_uri = "spotify:radio:album:" + SpotifyIdFromUri(seed, "album");
  } else if (seed.rfind("spotify:artist:", 0) == 0) {
    radio_uri = "spotify:radio:artist:" + SpotifyIdFromUri(seed, "artist");
  } else {
    radio_uri = "";
  }
  return radio_uri.c_str();
}

TSP_PUBLIC const char * TspGetRemoteDeviceId(Tsp *tsp, int index) {
  return NULL;
}

TSP_PUBLIC const char * TspGetRemoteDeviceName(Tsp *tsp, int index) {
  return NULL;
}

TSP_PUBLIC int TspGetRootlistCount(Tsp *tsp) {
  return tsp ? (int)tsp->rootlist.size() : 0;
}

TSP_PUBLIC int TspGetRootlistIndent(Tsp *tsp, int idx) {
  return 0;
}

TSP_PUBLIC const char * TspGetRootlistName(Tsp *tsp, int idx) {
  if (!tsp || idx < 0 || idx >= (int)tsp->rootlist.size()) return "";
  return tsp->rootlist[idx].name.c_str();
}

TSP_PUBLIC TspItemType TspGetRootlistType(Tsp *tsp, int idx) {
  if (!tsp || idx < 0 || idx >= (int)tsp->rootlist.size()) return kTspItemType_Track;
  return tsp->rootlist[idx].type;
}

TSP_PUBLIC const char * TspGetRootlistUri(Tsp *tsp, int idx) {
  if (!tsp || idx < 0 || idx >= (int)tsp->rootlist.size()) return "";
  return tsp->rootlist[idx].uri.c_str();
}

TSP_PUBLIC int TspGetRootlistUriInfo(Tsp *tsp, int idx) {
  return 0;
}

TSP_PUBLIC const char * TspGetSearchUri(Tsp *tsp, const char *query) {
  if (!query) return "";
  static std::string search_uri;
  search_uri = "search:" + std::string(query);
  return search_uri.c_str();
}

TSP_PUBLIC const char * TspGetStarredUri(Tsp *tsp, const char *username) {
  return "spotify:collection:tracks";
}

TSP_PUBLIC const char * TspGetVersionString() {
  return "Spotiamp Custom Connect Backend v1.0";
}

TSP_PUBLIC TspBool TspIsConnected(Tsp *tsp) {
  return tsp ? (TspBool)tsp->connected : 0;
}

TSP_PUBLIC const char * TspItemGetAlbumUri(TspItem *item, Tsp *tsp) {
  if (item && item->album_uri.empty() && tsp) {
    std::string track_id = SpotifyIdFromUri(item->uri, "track");
    std::string access_token = GetAccessToken(tsp);
    if (!track_id.empty() && !access_token.empty()) {
      std::string response = HttpGet("https://api.spotify.com/v1/tracks/" + track_id +
                                     "?market=from_token", access_token);
      if (response.find("\"error\"") == std::string::npos) {
        std::string album_json = JsonExtractObjectShallow(response, "album");
        item->album_uri = JsonExtractStringShallow(album_json, "uri");
        if (item->image_uri.empty()) {
          auto images = JsonExtractArray(album_json, "images");
          if (!images.empty())
            item->image_uri = JsonExtractString(images[0], "url");
        }
      } else {
        std::cout << "[DEBUG] TspItemGetAlbumUri: status " << LastHttpStatus()
                  << " for track " << track_id << ": " << response << std::endl;
      }
    }
  }
  return item ? item->album_uri.c_str() : "";
}

TSP_PUBLIC const char * TspItemGetArtistName(TspItem *item) {
  return item ? item->artist.c_str() : "";
}

TSP_PUBLIC const char * TspItemGetArtistUri(TspItem *item, Tsp *tsp) {
  return item ? item->artist_uri.c_str() : "";
}

TSP_PUBLIC const char * TspItemGetImageUri(TspItem *item, Tsp *tsp) {
  if (item && !item->image_uri.empty()) {
    if (item->image_uri.rfind("spotify:image:", 0) == 0) {
      return item->image_uri.c_str();
    }
    size_t last_slash = item->image_uri.find_last_of("/");
    if (last_slash != std::string::npos) {
      std::string id = item->image_uri.substr(last_slash + 1);
      item->image_uri = "spotify:image:" + id;
      return item->image_uri.c_str();
    }
  }
  return "";
}

TSP_PUBLIC int TspItemGetLength(TspItem *item) {
  return item ? item->duration_sec : 0;
}

TSP_PUBLIC const char * TspItemGetName(TspItem *item) {
  return item ? item->name.c_str() : "";
}

TSP_PUBLIC TspItemType TspItemGetType(TspItem *item) {
  return item ? item->type : kTspItemType_Track;
}

TSP_PUBLIC const char * TspItemGetUri(TspItem *item, Tsp *tsp) {
  return item ? item->uri.c_str() : "";
}

TSP_PUBLIC TspBool TspItemIsPlayable(TspItem *item) {
  return item ? item->playable : 0;
}

TSP_PUBLIC TspError TspItemListCopyFrom(TspItemList *tl, TspItemList *src) {
  return CopyItemListContents(tl, src);
}

TSP_PUBLIC TspItemList * TspItemListCreate(Tsp *tsp, int item_size) {
  TspItemList *tl = new TspItemList();
  tl->now_playing_index = 0;
  return tl;
}

TSP_PUBLIC void TspItemListDestroy(TspItemList *tl) {
  if (tl) {
    ClearItemList(tl);
    delete tl;
  }
}

TSP_PUBLIC int TspItemListGetIndent(TspItemList *tl, int i) {
  return 0;
}

TSP_PUBLIC TspItem * TspItemListGetItem(TspItemList *tl, int n) {
  if (!tl || n < 0 || n >= (int)tl->items.size()) return NULL;
  return tl->items[n];
}

TSP_PUBLIC int TspItemListGetLastVisibleItem(TspItemList *tl) {
  if (!tl) return 0;
  if (tl->dynamic_loading && tl->total_length > 0)
    return tl->total_length - 1;
  return (int)tl->items.size() - 1;
}

TSP_PUBLIC int TspItemListGetNowPlayingIndex(TspItemList *tl) {
  return tl ? tl->now_playing_index : 0;
}

TSP_PUBLIC int TspItemListGetTotalLength(TspItemList *tl) {
  if (!tl) return 0;
  if (tl->dynamic_loading && tl->total_length > 0)
    return tl->total_length;
  return (int)tl->items.size();
}

TSP_PUBLIC const char * TspItemListGetUri(TspItemList *tl) {
  return tl ? tl->uri.c_str() : "";
}

TSP_PUBLIC TspError TspItemListLoad(TspItemList *tl, const char *uri, int load_offset) {
  if (!tl || !uri) return kTspErrorTemp;
  
  std::string uri_str = uri;
  std::cout << "[DEBUG] TspItemListLoad: uri = " << uri_str
            << ", offset = " << load_offset << std::endl;
  tl->uri = uri;
  ClearItemList(tl);
  tl->total_length = 0;
  tl->dynamic_loading = false;
  
  Tsp *tsp = g_global_tsp;
  if (!tsp || GetAccessToken(tsp).empty()) {
    std::cout << "[DEBUG] TspItemListLoad: no access token for " << uri_str << std::endl;
    return kTspErrorOk;
  }
  
  if (uri_str == "spotify:collection:tracks") {
    tl->dynamic_loading = true;
    tl->total_length = -1;
    tl->items.clear();
    std::thread([tl, tsp]() {
      LoadSavedTracksPage(tl, tsp, 0, 50);
      if (tsp->callback)
        tsp->callback(tsp->callback_context, kTspCallbackEvent_ItemListChanged, tl, NULL);
    }).detach();
    return kTspErrorOk;
  }

  std::string playlist_id = SpotifyIdFromUri(uri_str, "playlist");
  if (playlist_id.empty()) {
    size_t playlist_url_pos = uri_str.find("/playlist/");
    if (playlist_url_pos != std::string::npos) {
      size_t question_pos = uri_str.find("?", playlist_url_pos);
      playlist_id = uri_str.substr(playlist_url_pos + 10,
                                   question_pos == std::string::npos ? std::string::npos
                                                                     : question_pos - playlist_url_pos - 10);
    }
  }

  std::vector<std::string> radio_seed_uris = tl->radio_seed_uris;
  std::thread([tl, tsp, uri_str, playlist_id, radio_seed_uris]() {
    std::vector<TspItem*> tracks;
    BuildTracksForUri(&tracks, tsp, uri_str, playlist_id, radio_seed_uris);
    std::cout << "[DEBUG] TspItemListLoad: " << uri_str << " produced "
              << tracks.size() << " tracks" << std::endl;

    tl->items = tracks;
    tl->total_length = (int)tracks.size();
    tl->dynamic_loading = false;
    if (tsp->callback)
      tsp->callback(tsp->callback_context, kTspCallbackEvent_ItemListChanged, tl, NULL);
  }).detach();
  
  return kTspErrorOk;
}

TSP_PUBLIC TspError TspItemListLoadRange(TspItemList *tl, int first_item, int num_items) {
  if (!tl || !tl->dynamic_loading || tl->uri != "spotify:collection:tracks")
    return kTspErrorOk;
  Tsp *tsp = g_global_tsp;
  if (!tsp || tsp->access_token.empty())
    return kTspErrorOk;

  int start = first_item - 10;
  if (start < 0)
    start = 0;
  int end = first_item + num_items + 20;
  if (tl->total_length > 0 && end > tl->total_length)
    end = tl->total_length;
  int page_start = (start / 50) * 50;
  int page_end = ((end + 49) / 50) * 50;
  for (int offset = page_start; offset < page_end; offset += 50) {
    bool needs_page = offset >= (int)tl->items.size();
    for (int i = offset; !needs_page && i < offset + 50 && i < (int)tl->items.size(); ++i) {
      if (!tl->items[i])
        needs_page = true;
    }
    if (!needs_page)
      continue;
    std::thread([tl, tsp, offset]() {
      LoadSavedTracksPage(tl, tsp, offset, 50);
      if (tsp->callback)
        tsp->callback(tsp->callback_context, kTspCallbackEvent_ItemListChanged, tl, NULL);
    }).detach();
  }
  return kTspErrorOk;
}

TSP_PUBLIC void TspItemListSetFolderOpen(TspItemList *tl, int i, int open) {
}

TSP_PUBLIC void TspItemListSetRadioTracks(TspItemList *tl) {
  if (!tl) return;
  tl->radio_seed_uris.clear();
  g_radio_seed_uris.clear();
  for (auto *item : tl->items) {
    if (!item || item->type != kTspItemType_Track || item->uri.empty())
      continue;
    tl->radio_seed_uris.push_back(item->uri);
    g_radio_seed_uris.push_back(item->uri);
    if (tl->radio_seed_uris.size() >= 5)
      break;
  }
}

TSP_PUBLIC TspError TspLogin(Tsp *tsp, const char *username, const char *password, int flags) {
  if (!tsp) return kTspErrorTemp;
  
  KillLibrespot();
  
  tsp->username = username ? username : "";
  std::string access_token = "";
  std::string refresh_token = "";
  
  // Try to load refresh token from file
  std::ifstream infile("refresh_token.txt");
  if (infile.is_open()) {
    std::getline(infile, refresh_token);
    infile.close();
    refresh_token = Trim(refresh_token);
  }
  
  if (!refresh_token.empty()) {
    std::cout << "Refreshing Spotify access token..." << std::endl;
    std::string response = RefreshAccessToken(refresh_token, GetSpotifyClientId(), GetSpotifyClientSecret());
    access_token = JsonExtractString(response, "access_token");
    std::string new_refresh = JsonExtractString(response, "refresh_token");
    if (!new_refresh.empty()) {
      refresh_token = new_refresh;
      std::ofstream outfile("refresh_token.txt");
      if (outfile.is_open()) {
        outfile << refresh_token;
        outfile.close();
      }
    }
  }
  
  // If refresh failed or no refresh token, perform browser authorization flow
  if (access_token.empty()) {
    std::string auth_url = "https://accounts.spotify.com/authorize?client_id=" + GetSpotifyClientId() +
                           "&response_type=code&redirect_uri=http://127.0.0.1:3000/callback" +
                           "&scope=user-modify-playback-state%20user-read-playback-state%20user-read-currently-playing%20user-library-read%20user-top-read%20playlist-read-private%20playlist-read-collaborative";
    
    std::cout << "Opening browser for Spotify authentication..." << std::endl;
    OpenUrl(auth_url.c_str());
    
    std::string code = ListenForAuthCode();
    if (code.empty()) {
      tsp->connection_error = kTspErrorTemp;
      return kTspErrorTemp;
    }
    
    std::cout << "Exchanging code for access tokens..." << std::endl;
    std::string response = GetAccessTokenFromCode(code, GetSpotifyClientId(), GetSpotifyClientSecret());
    access_token = JsonExtractString(response, "access_token");
    refresh_token = JsonExtractString(response, "refresh_token");
    
    if (access_token.empty() || refresh_token.empty()) {
      tsp->connection_error = kTspErrorTemp;
      return kTspErrorTemp;
    }
    
    // Save refresh token to file
    std::ofstream outfile("refresh_token.txt");
    if (outfile.is_open()) {
      outfile << refresh_token;
      outfile.close();
    }
  }
  
  tsp->access_token = access_token;
  
  // Spawn librespot Connect player daemon using access token
  SpawnLibrespot(NULL, NULL, access_token.c_str());
  
  tsp->connected = true;
  tsp->connection_error = kTspErrorOk;
  
  if (!tsp->audio_thread_running) {
    tsp->audio_thread_running = true;
    tsp->audio_thread = std::thread(AudioThreadProc, tsp);
  }
  
  if (!tsp->poll_thread_running) {
    tsp->poll_thread_running = true;
    tsp->poll_thread = std::thread(PollThreadProc, tsp);
  }
  
  // Load user playlists asynchronously and trigger connection complete
  std::thread([tsp]() {
    std::vector<Tsp::RootlistItem> all_playlists;
    std::string url = "https://api.spotify.com/v1/me/playlists?limit=50";
    while (!url.empty()) {
      std::string response = HttpGet(url, tsp->access_token);
      auto items = JsonExtractArray(response, "items");
      for (const auto &item_json : items) {
        Tsp::RootlistItem rl;
        rl.name = JsonExtractString(item_json, "name");
        rl.uri = ExtractSpotifyUri(item_json, "playlist");
        rl.type = kTspItemType_Playlist;
        all_playlists.push_back(rl);
      }
      
      std::string next_url = JsonExtractString(response, "next");
      if (next_url.rfind("https://", 0) == 0) {
        url = next_url;
      } else {
        url = "";
      }
    }
    
    tsp->rootlist = all_playlists;
    
    SleepForMicroseconds(500000); // 0.5s pause for visual responsiveness
    
    if (tsp->callback) {
      tsp->callback(tsp->callback_context, kTspCallbackEvent_Connected, NULL, NULL);
    }
    
    if (tsp->callback) {
      tsp->callback(tsp->callback_context, kTspCallbackEvent_RootlistChanged, NULL, NULL);
    }
  }).detach();
  
  return kTspErrorOk;
}

TSP_PUBLIC int TspPlayerGetBitrate(Tsp *tsp) {
  return 160;
}

TSP_PUBLIC int TspPlayerGetDuration(Tsp *tsp) {
  if (!tsp) return 0;
  std::lock_guard<std::mutex> lock(tsp->mutex);
  return tsp->player_duration_ms / 1000;
}

TSP_PUBLIC int TspPlayerGetDurationMs(Tsp *tsp) {
  if (!tsp) return 0;
  std::lock_guard<std::mutex> lock(tsp->mutex);
  return tsp->player_duration_ms;
}

TSP_PUBLIC TspItemList * TspPlayerGetItemList(Tsp *tsp) {
  return tsp ? tsp->player_list : NULL;
}

TSP_PUBLIC TspItem * TspPlayerGetNowPlaying(Tsp *tsp) {
  if (!tsp) return NULL;
  std::lock_guard<std::mutex> lock(tsp->mutex);
  if (tsp->has_now_playing) {
    return &tsp->now_playing_item;
  }
  return NULL;
}

TSP_PUBLIC int TspPlayerGetPosition(Tsp *tsp) {
  if (!tsp) return 0;
  std::lock_guard<std::mutex> lock(tsp->mutex);
  return CurrentPlayerPositionMsLocked(tsp) / 1000;
}

TSP_PUBLIC int TspPlayerGetPositionMs(Tsp *tsp) {
  if (!tsp) return 0;
  std::lock_guard<std::mutex> lock(tsp->mutex);
  return CurrentPlayerPositionMsLocked(tsp);
}

TSP_PUBLIC int TspPlayerGetRepeat(Tsp *tsp) {
  if (!tsp) return 0;
  std::lock_guard<std::mutex> lock(tsp->mutex);
  return (int)tsp->repeat;
}

TSP_PUBLIC int TspPlayerGetShuffle(Tsp *tsp) {
  if (!tsp) return 0;
  std::lock_guard<std::mutex> lock(tsp->mutex);
  return (int)tsp->shuffle;
}

TSP_PUBLIC TspPlayState TspPlayerGetState(Tsp *tsp) {
  if (!tsp) return kTspPlayState_Stopped;
  std::lock_guard<std::mutex> lock(tsp->mutex);
  if (!tsp->has_now_playing && !tsp->is_playing) return kTspPlayState_Stopped;
  return tsp->is_playing ? kTspPlayState_Playing : kTspPlayState_Paused;
}

TSP_PUBLIC int TspPlayerGetVolume(Tsp *tsp) {
  if (!tsp) return 50;
  std::lock_guard<std::mutex> lock(tsp->mutex);
  return tsp->volume;
}

TSP_PUBLIC TspBool TspPlayerIsLoading(Tsp *tsp) {
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
  (void)tsp;
  SpPlaybackSnapshot snapshot = {};
  return sp_playback_bridge_snapshot(&snapshot) == 0 && snapshot.loading;
#else
  (void)tsp;
  return 0;
#endif
}

TSP_PUBLIC void TspPlayerNextTrack(Tsp *tsp) {
  int next_index = PickNextIndex(tsp, 1);
  std::string next_uri;
  if (tsp && tsp->player_list && next_index >= 0 && next_index < (int)tsp->player_list->items.size()) {
    TspItem *item = tsp->player_list->items[next_index];
    if (item) next_uri = item->uri;
    if (!next_uri.empty())
      SetPendingNowPlayingIndex(tsp, next_index);
  }

  std::string access_token = GetAccessToken(tsp);
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
  if (sp_playback_bridge_is_running()) {
    if (tsp && tsp->player_list && next_index >= 0 &&
        BridgeLoadItemList(tsp->player_list, next_index, 0))
      return;
    if (sp_playback_bridge_next() == 0)
      return;
  }
#endif
  if (!access_token.empty()) {
    if (!next_uri.empty()) {
      std::string data = "{ \"uris\": [ \"" + next_uri + "\" ], \"position_ms\": 0 }";
      HttpPostOrPutDeviceAsync("PUT", "https://api.spotify.com/v1/me/player/play", access_token, data);
    } else {
      HttpPostOrPutDeviceAsync("POST", "https://api.spotify.com/v1/me/player/next", access_token);
    }
  }
}

TSP_PUBLIC void TspPlayerPause(Tsp *tsp) {
  std::string access_token = GetAccessToken(tsp);
  if (tsp) {
    std::lock_guard<std::mutex> lock(tsp->mutex);
    tsp->player_position_ms = CurrentPlayerPositionMsLocked(tsp);
    tsp->player_position_updated_ms = NowMonotonicMs();
    tsp->is_playing = false;
    tsp->local_desired_playing = false;
    ClearPositionTransitionLocked(tsp);
    tsp->local_stopped = false;
    tsp->local_control_until_ms = NowMonotonicMs() + 5000;
  }
  DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
  if (sp_playback_bridge_is_running()) {
    sp_playback_bridge_pause();
  } else
#endif
  if (!access_token.empty())
    HttpPostOrPutDeviceAsync("PUT", "https://api.spotify.com/v1/me/player/pause", access_token);
  DispatchPlayerCallback(tsp, kTspCallbackEvent_Pause);
}

TSP_PUBLIC void TspPlayerPlay(Tsp *tsp) {
  std::string access_token = GetAccessToken(tsp);
  if (tsp) {
    std::lock_guard<std::mutex> lock(tsp->mutex);
    tsp->player_position_updated_ms = NowMonotonicMs();
    tsp->local_desired_playing = true;
    ClearPositionTransitionLocked(tsp);
    tsp->local_stopped = false;
    tsp->local_control_until_ms = NowMonotonicMs() + 5000;
  }
  DispatchAudioControl(tsp, kTspAudioFlag_FlushBuffer);
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
  if (sp_playback_bridge_is_running()) {
    sp_playback_bridge_play();
  } else
#endif
  if (!access_token.empty()) {
    std::thread([access_token]() {
      std::string device_id = GetSpotiampDeviceIdFromToken(access_token);
      HttpPostOrPut("PUT", UrlWithDeviceId("https://api.spotify.com/v1/me/player/play", device_id), access_token);
    }).detach();
  }
}

TSP_PUBLIC TspError TspPlayerPlayContext(Tsp *tsp, const char *context_uri, const char *track_uri, int index) {
  std::string access_token = GetAccessToken(tsp);
  std::string context = context_uri ? context_uri : "";
  if (tsp && !access_token.empty() && IsGeneratedPlayableUri(context)) {
    std::vector<std::string> radio_seed_uris = g_radio_seed_uris;
    if (tsp) {
      std::lock_guard<std::mutex> lock(tsp->mutex);
      tsp->is_playing = false;
      tsp->player_position_ms = 0;
      tsp->player_position_updated_ms = NowMonotonicMs();
      tsp->local_desired_playing = true;
      tsp->local_stopped = false;
      tsp->queue_sync_context_uri.clear();
      long long now_ms = NowMonotonicMs();
      StartPositionTransitionLocked(tsp, 0, now_ms, 5000, 60);
    }
    DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
    DispatchPlayerCallback(tsp, kTspCallbackEvent_Pause);
    std::thread([tsp, access_token, context, radio_seed_uris]() {
      std::vector<TspItem*> tracks;
      BuildTracksForUri(&tracks, tsp, context, "", radio_seed_uris);
      if (tracks.empty())
        return;

      ClearItemList(tsp->player_list);
      tsp->player_list->uri = context;
      tsp->player_list->items = tracks;
      tsp->player_list->now_playing_index = 0;
      SetPendingNowPlayingIndex(tsp, 0);

      std::string data = "{ \"uris\": [";
      int added = 0;
      for (auto *item : tsp->player_list->items) {
        if (!item || item->uri.empty())
          continue;
        if (added)
          data += ",";
        data += " \"" + item->uri + "\"";
        if (++added >= 50)
          break;
      }
      data += " ], \"position_ms\": 0 }";
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
      if (sp_playback_bridge_is_running()) {
        BridgeLoadItemList(tsp->player_list, 0, 0);
      } else
#endif
      if (added > 0)
        HttpPostOrPutDeviceAsync("PUT", "https://api.spotify.com/v1/me/player/play", access_token, data);
    }).detach();
    return kTspErrorOk;
  }

  bool refresh_queue = false;
  bool handled_by_bridge = false;
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
  if (sp_playback_bridge_is_running()) {
    std::string track = track_uri ? track_uri : "";
    handled_by_bridge = BridgeLoadContext(context, track, index, 0);
  }
#endif
  if (!handled_by_bridge && !access_token.empty()) {
    std::string track = track_uri ? track_uri : "";
    std::string data = "{ \"context_uri\": \"" + context + "\"";
    if (!track.empty()) {
      data += ", \"offset\": { \"uri\": \"" + track + "\" }";
    } else if (index >= 0) {
      data += ", \"offset\": { \"position\": " + std::to_string(index) + " }";
    }
    if (!track.empty() || index >= 0)
      data += ", \"position_ms\": 0";
    data += " }";
    refresh_queue = track.empty() && index < 0 && context.rfind("spotify:playlist:", 0) == 0;
    if (refresh_queue) {
      std::thread([tsp, access_token, context, data]() {
        std::string device_id = GetSpotiampDeviceIdFromToken(access_token);
        HttpPostOrPut("PUT", UrlWithDeviceId("https://api.spotify.com/v1/me/player/play", device_id),
                      access_token, data);
        for (int attempt = 0; attempt < 5; ++attempt) {
          SleepForMicroseconds(500000);
          if (RefreshPlayerListFromQueue(tsp, access_token, context))
            break;
        }
      }).detach();
    } else {
      HttpPostOrPutDeviceAsync("PUT", "https://api.spotify.com/v1/me/player/play", access_token, data);
    }
  }
  if (tsp) {
    std::lock_guard<std::mutex> lock(tsp->mutex);
    tsp->is_playing = true;
    tsp->player_position_ms = 0;
    tsp->player_position_updated_ms = NowMonotonicMs();
    tsp->local_desired_playing = true;
    tsp->local_stopped = false;
    tsp->queue_sync_context_uri = refresh_queue ? context : "";
    {
      long long now_ms = NowMonotonicMs();
      StartPositionTransitionLocked(tsp, 0, now_ms, 5000, 60);
    }
  }
  DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
  DispatchPlayerCallback(tsp, kTspCallbackEvent_Resume);
  return kTspErrorOk;
}

TSP_PUBLIC void TspPlayerPlayItemList(Tsp *tsp, TspItemList *tl, int index) {
  if (!tsp || !tl || index < 0 || index >= (int)tl->items.size()) return;
  
  std::string list_uri = tl->uri;
  bool queue_synced_context = list_uri.rfind("spotify:playlist:", 0) == 0 &&
                              !tsp->queue_sync_context_uri.empty() &&
                              tsp->queue_sync_context_uri == list_uri;
  CopyItemListContents(tsp->player_list, tl);
  TspItem *item = tl->items[index];
  tl->now_playing_index = index;
  tsp->player_list->now_playing_index = index;
  tsp->queue_sync_context_uri = queue_synced_context ? list_uri : "";
  std::string item_uri = item->uri;
  SetPendingNowPlayingIndex(tsp, index);
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
  if (sp_playback_bridge_is_running() && BridgeLoadItemList(tsp->player_list, index, 0))
    return;
#endif
  
  std::string access_token = GetAccessToken(tsp);
  if (!access_token.empty()) {
    if (queue_synced_context) {
      std::string data = "{ \"context_uri\": \"" + list_uri +
                         "\", \"offset\": { \"uri\": \"" + item_uri +
                         "\" }, \"position_ms\": 0 }";
      std::thread([tsp, access_token, list_uri, data]() {
        std::string device_id = GetSpotiampDeviceIdFromToken(access_token);
        HttpPostOrPut("PUT", UrlWithDeviceId("https://api.spotify.com/v1/me/player/play", device_id),
                      access_token, data);
        for (int attempt = 0; attempt < 5; ++attempt) {
          SleepForMicroseconds(500000);
          if (RefreshPlayerListFromQueue(tsp, access_token, list_uri, true))
            break;
        }
      }).detach();
    } else {
      std::string data = "{ \"uris\": [ \"" + item_uri + "\" ], \"position_ms\": 0 }";
      HttpPostOrPutDeviceAsync("PUT", "https://api.spotify.com/v1/me/player/play", access_token, data);
    }
  }
}

TSP_PUBLIC void TspPlayerPrevTrack(Tsp *tsp) {
  int prev_index = PickNextIndex(tsp, -1);
  std::string prev_uri;
  if (tsp && tsp->player_list && prev_index >= 0 && prev_index < (int)tsp->player_list->items.size()) {
    TspItem *item = tsp->player_list->items[prev_index];
    if (item) prev_uri = item->uri;
    if (!prev_uri.empty())
      SetPendingNowPlayingIndex(tsp, prev_index);
  }

  std::string access_token = GetAccessToken(tsp);
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
  if (sp_playback_bridge_is_running()) {
    if (tsp && tsp->player_list && prev_index >= 0 &&
        BridgeLoadItemList(tsp->player_list, prev_index, 0))
      return;
    if (sp_playback_bridge_prev() == 0)
      return;
  }
#endif
  if (!access_token.empty()) {
    if (!prev_uri.empty()) {
      std::string data = "{ \"uris\": [ \"" + prev_uri + "\" ], \"position_ms\": 0 }";
      HttpPostOrPutDeviceAsync("PUT", "https://api.spotify.com/v1/me/player/play", access_token, data);
    } else {
      HttpPostOrPutDeviceAsync("POST", "https://api.spotify.com/v1/me/player/previous", access_token);
    }
  }
}

TSP_PUBLIC TspError TspPlayerSeek(Tsp *tsp, int position_ms) {
  if (tsp) {
    if (position_ms < 0) position_ms = 0;
    bool resume_after_seek = false;
    {
      std::lock_guard<std::mutex> lock(tsp->mutex);
      if (tsp->player_duration_ms > 0 && position_ms > tsp->player_duration_ms)
        position_ms = tsp->player_duration_ms;
      resume_after_seek = tsp->is_playing || tsp->local_desired_playing;
      tsp->player_position_ms = position_ms;
      tsp->player_position_updated_ms = NowMonotonicMs();
      tsp->is_playing = resume_after_seek;
      tsp->local_desired_playing = resume_after_seek;
      tsp->local_stopped = false;
      {
        long long now_ms = NowMonotonicMs();
        StartPositionTransitionLocked(tsp, position_ms, now_ms, 7000,
                                      resume_after_seek ? 40 : 0);
      }
    }
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
    FlushRustBridgeAudio();
#endif
    if (resume_after_seek) {
      DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
    } else {
      DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
      DispatchPlayerCallback(tsp, kTspCallbackEvent_Pause);
    }
    std::string access_token = GetAccessToken(tsp);
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
    if (sp_playback_bridge_is_running()) {
      sp_playback_bridge_seek((uint32_t)position_ms);
      return kTspErrorOk;
    }
#endif
    if (!access_token.empty()) {
      std::string url = "https://api.spotify.com/v1/me/player/seek?position_ms=" + std::to_string(position_ms);
      HttpPostOrPutDeviceAsync("PUT", url, access_token);
    }
  }
  return kTspErrorOk;
}

TSP_PUBLIC void TspPlayerSetDevice(Tsp *tsp, const char *device_id, int flags) {
}

TSP_PUBLIC void TspPlayerSetEqualizer(Tsp *tsp, int enable, float pregain, const float bands[10]) {
}

TSP_PUBLIC void TspPlayerSetNowPlayingIndex(Tsp *tsp, int track) {
  if (SetNowPlayingIndex(tsp, track)) {
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
    if (sp_playback_bridge_is_running() && tsp && tsp->player_list)
      BridgeLoadItemList(tsp->player_list, track, 0);
#endif
  } else if (tsp) {
    std::lock_guard<std::mutex> lock(tsp->mutex);
    tsp->player_position_ms = 0;
    tsp->player_position_updated_ms = NowMonotonicMs();
  }
}

TSP_PUBLIC TspError TspPlayerSetRepeat(Tsp *tsp, int repeat) {
  if (tsp) {
    {
      std::lock_guard<std::mutex> lock(tsp->mutex);
      tsp->repeat = repeat != 0;
    }
    std::string access_token = GetAccessToken(tsp);
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
    if (sp_playback_bridge_is_running()) {
      sp_playback_bridge_set_repeat(repeat ? 1 : 0);
      DispatchPlayerCallback(tsp, kTspCallbackEvent_PlayControlsChanged);
      return kTspErrorOk;
    }
#endif
    if (!access_token.empty()) {
      std::string url = "https://api.spotify.com/v1/me/player/repeat?state=" + std::string(repeat ? "track" : "off");
      HttpPostOrPutDeviceAsync("PUT", url, access_token);
    }
    DispatchPlayerCallback(tsp, kTspCallbackEvent_PlayControlsChanged);
  }
  return kTspErrorOk;
}

TSP_PUBLIC TspError TspPlayerSetShuffle(Tsp *tsp, int shuffle) {
  if (tsp) {
    {
      std::lock_guard<std::mutex> lock(tsp->mutex);
      tsp->shuffle = shuffle != 0;
    }
    std::string access_token = GetAccessToken(tsp);
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
    if (sp_playback_bridge_is_running()) {
      sp_playback_bridge_set_shuffle(shuffle);
      DispatchPlayerCallback(tsp, kTspCallbackEvent_PlayControlsChanged);
      return kTspErrorOk;
    }
#endif
    if (!access_token.empty()) {
      std::string url = "https://api.spotify.com/v1/me/player/shuffle?state=" + std::string(shuffle ? "true" : "false");
      HttpPostOrPutDeviceAsync("PUT", url, access_token);
    }
    DispatchPlayerCallback(tsp, kTspCallbackEvent_PlayControlsChanged);
  }
  return kTspErrorOk;
}

TSP_PUBLIC TspError TspPlayerSetVolume(Tsp *tsp, int volume) {
  if (tsp) {
    if (volume < 0) volume = 0;
    if (volume > 65535) volume = 65535;
    {
      std::lock_guard<std::mutex> lock(tsp->mutex);
      tsp->volume = volume;
    }
    WavSetVolume(volume);
    DispatchPlayerCallback(tsp, kTspCallbackEvent_VolumeChanged);
  }
  return kTspErrorOk;
}

TSP_PUBLIC void TspPlayerStop(Tsp *tsp) {
  std::string access_token = GetAccessToken(tsp);
  if (tsp) {
    std::lock_guard<std::mutex> lock(tsp->mutex);
    tsp->player_position_ms = 0;
    tsp->player_position_updated_ms = NowMonotonicMs();
    tsp->is_playing = false;
    tsp->has_now_playing = false;
    tsp->local_desired_playing = false;
    ClearPositionTransitionLocked(tsp);
    tsp->local_stopped = true;
    tsp->local_control_until_ms = NowMonotonicMs() + 5000;
  }
  DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
#if defined(SPOTIAMP_WITH_RUST_BRIDGE)
  if (sp_playback_bridge_is_running()) {
    sp_playback_bridge_stop_playback();
  } else
#endif
  if (!access_token.empty())
    HttpPostOrPutDeviceAsync("PUT", "https://api.spotify.com/v1/me/player/pause", access_token);
  DispatchPlayerCallback(tsp, kTspCallbackEvent_Pause);
}

TSP_PUBLIC TspError TspPlaylistSimpleModify(Tsp *tsp, const char *playlist_uri, const char * const * uris, int num_uris, int operation) {
  return kTspErrorOk;
}

TSP_PUBLIC TspError TspSetAudioCallback(Tsp *tsp, TspAudioCallback *callback, void *context) {
  if (tsp) {
    tsp->audio_callback = callback;
    tsp->audio_context = context;
  }
  return kTspErrorOk;
}

TSP_PUBLIC void TspSetDebugLogger(TspDebugLogger *logger) {
}

TSP_PUBLIC TspError TspSetDisplayName(Tsp *tsp, const char *display_name) {
  return kTspErrorOk;
}

TSP_PUBLIC TspError TspSetTargetBitrate(Tsp *tsp, TspBitrate bitrate) {
  return kTspErrorOk;
}

TSP_PUBLIC TspError TspFormatTimeMonotonous(Tsp *tsp, int i) {
  return kTspErrorOk;
}

TSP_PUBLIC TspError TspWaitForNetworkEvents(Tsp *tsp, int max_wait) {
  SleepForMicroseconds(max_wait * 1000);
  return kTspErrorOk;
}

} // extern "C"
