#include "tiny_spotify.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <iostream>
#include <sstream>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  int now_playing_index;
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
  int local_expected_position_ms;
  bool local_desired_playing;
  bool local_stopped;
  bool is_playing;
  int volume;
  bool shuffle;
  bool repeat;
  
  TspItemList *player_list;
  
  std::thread audio_thread;
  std::atomic<bool> audio_thread_running;
  std::thread poll_thread;
  std::atomic<bool> poll_thread_running;
  
  Tsp() : callback(NULL), callback_context(NULL), audio_callback(NULL), audio_context(NULL),
          connected(false), connection_error(kTspErrorOk), has_now_playing(false),
          player_position_ms(0), player_duration_ms(0), player_position_updated_ms(0),
          local_control_until_ms(0), local_expected_position_ms(-1), local_desired_playing(false), local_stopped(false), is_playing(false), volume(65535),
          shuffle(false), repeat(false), player_list(NULL),
          audio_thread_running(false), poll_thread_running(false) {
    player_list = new TspItemList();
    player_list->now_playing_index = 0;
  }
};

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fstream>

void OpenUrl(const char *url);
extern "C" void WavSetVolume(int vol);

// Global variables
static Tsp *g_global_tsp = NULL;
static pid_t g_librespot_pid = 0;
static int g_librespot_stdout_fd = -1;

static std::string GetSpotiampDeviceIdFromToken(const std::string &access_token);

static long long NowMonotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int CurrentPlayerPositionMsLocked(Tsp *tsp) {
  int pos = tsp->player_position_ms;
  if (tsp->is_playing && tsp->player_position_updated_ms > 0) {
    pos += (int)(NowMonotonicMs() - tsp->player_position_updated_ms);
  }
  if (pos < 0) pos = 0;
  if (tsp->player_duration_ms > 0 && pos > tsp->player_duration_ms)
    pos = tsp->player_duration_ms;
  return pos;
}

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
  tsp->local_expected_position_ms = 0;
  tsp->local_stopped = false;
  tsp->local_control_until_ms = NowMonotonicMs() + 5000;
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

// Helper function to execute curl requests
static std::string HttpGet(const std::string &url, const std::string &token) {
  std::string cmd = "curl -s --max-time 10 --connect-timeout 5 -H \"Authorization: Bearer " + token + "\" \"" + url + "\"";
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

static std::string HttpPostOrPut(const std::string &method, const std::string &url, const std::string &token, const std::string &data = "") {
  std::string cmd = "curl -s --max-time 10 --connect-timeout 5 -X " + method + " -H \"Authorization: Bearer " + token + "\"";
  if (!data.empty()) {
    // Escape single quotes for shell command safety
    std::string escaped_data = "";
    for (char c : data) {
      if (c == '\'') escaped_data += "'\\''";
      else escaped_data += c;
    }
    cmd += " -H \"Content-Type: application/json\" -d '" + escaped_data + "'";
  }
  cmd += " \"" + url + "\"";
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
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) return "";
  
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(3000);
  
  if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
    close(server_fd);
    return "";
  }
  
  if (listen(server_fd, 1) < 0) {
    close(server_fd);
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
  
  int sel = select(server_fd + 1, &fds, NULL, NULL, &tv);
  if (sel <= 0) {
    std::cout << "Spotify authorization timed out." << std::endl;
    close(server_fd);
    return "";
  }
  
  int client_fd = accept(server_fd, NULL, NULL);
  if (client_fd < 0) {
    close(server_fd);
    return "";
  }
  
  char buffer[4096];
  ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
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
    write(client_fd, response.c_str(), response.length());
  }
  
  close(client_fd);
  close(server_fd);
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
  std::string devices_json = HttpGet("https://api.spotify.com/v1/me/player/devices", access_token);
  auto devices = JsonExtractArray(devices_json, "devices");
  for (const auto &dev : devices) {
    std::string name = JsonExtractString(dev, "name");
    if (name == "Spotiamp") {
      return JsonExtractString(dev, "id");
    }
  }
  return "";
}

static std::string GetSpotiampDeviceId(Tsp *tsp) {
  return GetSpotiampDeviceIdFromToken(GetAccessToken(tsp));
}

// Subprocess control
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
    bool is_playing = false;
    {
      std::lock_guard<std::mutex> lock(tsp->mutex);
      is_playing = tsp->is_playing;
    }
    if (!is_playing) {
      if (!output_paused) {
        DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
        output_paused = true;
      }
      usleep(10000);
      continue;
    } else if (output_paused) {
      DispatchAudioControl(tsp, kTspAudioFlag_FlushBuffer);
      output_paused = false;
    }

    if (g_librespot_stdout_fd < 0) {
      usleep(10000);
      continue;
    }

    ssize_t bytes_read = read(g_librespot_stdout_fd, buffer + residual_count, chunk_size - residual_count);
    if (bytes_read <= 0) {
      if (bytes_read == 0) {
        break;
      }
      usleep(10000);
      continue;
    }

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
          usleep(5000);
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
    std::string access_token;
    {
      std::lock_guard<std::mutex> lock(tsp->mutex);
      if (tsp->connected) {
        access_token = tsp->access_token;
      }
    }
    if (access_token.empty()) {
      sleep(1);
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
      int shuffle = JsonExtractInt(response, "shuffle_state");
      std::string repeat_state = JsonExtractString(response, "repeat_state");
      bool repeat = (repeat_state != "off" && !repeat_state.empty());
      
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
        std::string album_json = JsonExtractObjectShallow(item_json, "album");
        if (!album_json.empty()) {
          auto images = JsonExtractArray(album_json, "images");
          if (!images.empty()) {
            image_url = JsonExtractString(images[0], "url");
          }
        }
        
        std::lock_guard<std::mutex> lock(tsp->mutex);
        controls_changed = (tsp->shuffle != (shuffle != 0)) || (tsp->repeat != repeat);
        tsp->shuffle = shuffle != 0;
        tsp->repeat = repeat;
        long long now_ms = NowMonotonicMs();
        bool command_in_flight = tsp->local_control_until_ms > now_ms;
        bool suppress_play_state = command_in_flight && tsp->local_desired_playing != is_p;
        bool suppress_stopped_metadata = tsp->local_stopped && !is_p;
        bool suppress_progress = command_in_flight &&
                                 tsp->local_expected_position_ms >= 0 &&
                                 abs(pos - tsp->local_expected_position_ms) > 2500;
        bool suppress_metadata = suppress_stopped_metadata ||
                                 (command_in_flight &&
                                  ((tsp->has_now_playing && tsp->now_playing_item.uri != track_uri) ||
                                   tsp->local_desired_playing != is_p ||
                                   suppress_progress));

        if (has_valid_item && has_numeric_progress && !stale_idle_update &&
            !suppress_play_state && !suppress_metadata) {
          tsp->is_playing = is_p;
          tsp->player_position_ms = pos;
          tsp->player_position_updated_ms = now_ms;
          tsp->local_control_until_ms = 0;
          tsp->local_expected_position_ms = -1;
          if (is_p)
            tsp->local_stopped = false;
        } else if (stale_idle_update) {
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
          tsp->now_playing_item.duration_sec = duration_ms / 1000;
          tsp->now_playing_item.image_uri = image_url;
          tsp->player_duration_ms = duration_ms;
          tsp->has_now_playing = true;
          tsp->local_stopped = false;
          now_playing_changed = true;
        }
      } else {
        std::lock_guard<std::mutex> lock(tsp->mutex);
        controls_changed = (tsp->shuffle != (shuffle != 0)) || (tsp->repeat != repeat);
        tsp->shuffle = shuffle != 0;
        tsp->repeat = repeat;
      }

      if (now_playing_changed) {
        DispatchPlayerCallback(tsp, kTspCallbackEvent_NowPlayingChanged);
      } else if (!play_state_suppressed && has_numeric_progress && !(old_is_playing && !is_p && pos == 0 && old_pos > 0) &&
                 (old_pos / 1000 != pos / 1000 || old_is_playing != is_p)) {
        DispatchPlayerCallback(tsp, is_p ? kTspCallbackEvent_Resume : kTspCallbackEvent_Pause);
      }
      if (controls_changed) {
        DispatchPlayerCallback(tsp, kTspCallbackEvent_PlayControlsChanged);
      }
    } else {
    }
    
    for (int i = 0; i < 20 && tsp->poll_thread_running; ++i) {
      usleep(100000);
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
  return "";
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
  return "";
}

TSP_PUBLIC const char * TspGetVersionString() {
  return "Spotiamp Custom Connect Backend v1.0";
}

TSP_PUBLIC TspBool TspIsConnected(Tsp *tsp) {
  return tsp ? (TspBool)tsp->connected : 0;
}

TSP_PUBLIC const char * TspItemGetAlbumUri(TspItem *item, Tsp *tsp) {
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
  return tl ? (int)tl->items.size() - 1 : 0;
}

TSP_PUBLIC int TspItemListGetNowPlayingIndex(TspItemList *tl) {
  return tl ? tl->now_playing_index : 0;
}

TSP_PUBLIC int TspItemListGetTotalLength(TspItemList *tl) {
  return tl ? (int)tl->items.size() : 0;
}

TSP_PUBLIC const char * TspItemListGetUri(TspItemList *tl) {
  return tl ? tl->uri.c_str() : "";
}

TSP_PUBLIC TspError TspItemListLoad(TspItemList *tl, const char *uri, int load_offset) {
  if (!tl || !uri) return kTspErrorTemp;
  
  tl->uri = uri;
  ClearItemList(tl);
  
  Tsp *tsp = g_global_tsp;
  if (!tsp || tsp->access_token.empty()) return kTspErrorOk;
  
  std::string uri_str = uri;
  size_t playlist_id_pos = uri_str.find("spotify:playlist:");
  std::string playlist_id = "";
  if (playlist_id_pos != std::string::npos) {
    playlist_id = uri_str.substr(playlist_id_pos + 17);
  } else {
    size_t playlist_url_pos = uri_str.find("/playlist/");
    if (playlist_url_pos != std::string::npos) {
      size_t question_pos = uri_str.find("?", playlist_url_pos);
      if (question_pos != std::string::npos) {
        playlist_id = uri_str.substr(playlist_url_pos + 10, question_pos - playlist_url_pos - 10);
      } else {
        playlist_id = uri_str.substr(playlist_url_pos + 10);
      }
    }
  }
  
  if (!playlist_id.empty()) {
    std::thread([tl, tsp, playlist_id]() {
      std::vector<TspItem*> all_tracks;
      std::string url = "https://api.spotify.com/v1/playlists/" + playlist_id + "/tracks?limit=100";
      while (!url.empty()) {
        std::string response = HttpGet(url, tsp->access_token);
        auto items = JsonExtractArray(response, "items");
        
        for (const auto &item_json : items) {
          std::string track_json = JsonExtractObjectShallow(item_json, "track");
          if (track_json.empty()) continue;
          
          TspItem *item = new TspItem();
          item->name = JsonExtractStringShallow(track_json, "name");
          item->uri = JsonExtractStringShallow(track_json, "uri");
          item->duration_sec = JsonExtractIntShallow(track_json, "duration_ms") / 1000;
          item->type = kTspItemType_Track;
          item->playable = JsonExtractBoolShallow(track_json, "is_playable", true) &&
                           !JsonExtractBoolShallow(track_json, "is_local", false);
          if (item->name.empty() || item->uri.empty() || item->duration_sec <= 0 || !item->playable) {
            delete item;
            continue;
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
            if (!images.empty()) {
              item->image_uri = JsonExtractString(images[0], "url");
            }
          }
          
          all_tracks.push_back(item);
        }
        
        std::string next_url = JsonExtractString(response, "next");
        if (next_url.rfind("https://", 0) == 0) {
          url = next_url;
        } else {
          url = "";
        }
      }
      
      tl->items = all_tracks;
      
      if (tsp->callback) {
        tsp->callback(tsp->callback_context, kTspCallbackEvent_ItemListChanged, tl, NULL);
      }
    }).detach();
  } else if (uri_str.rfind("search:", 0) == 0) {
    std::string query = uri_str.substr(7);
    std::string encoded_query = "";
    for (char c : query) {
      if (isalnum(c)) encoded_query += c;
      else if (c == ' ') encoded_query += "%20";
      else {
        char hex[4];
        sprintf(hex, "%%%02X", (unsigned char)c);
        encoded_query += hex;
      }
    }
    
    std::thread([tl, tsp, encoded_query]() {
      std::string url = "https://api.spotify.com/v1/search?q=" + encoded_query + "&type=track&limit=50";
      std::string response = HttpGet(url, tsp->access_token);
      
      size_t tracks_pos = response.find("\"tracks\"");
      if (tracks_pos != std::string::npos) {
        std::string tracks_json = response.substr(tracks_pos);
        auto items = JsonExtractArray(tracks_json, "items");
        
        for (const auto &track_json : items) {
          TspItem *item = new TspItem();
          item->name = JsonExtractStringShallow(track_json, "name");
          item->uri = JsonExtractStringShallow(track_json, "uri");
          item->duration_sec = JsonExtractIntShallow(track_json, "duration_ms") / 1000;
          item->type = kTspItemType_Track;
          item->playable = JsonExtractBoolShallow(track_json, "is_playable", true) &&
                           !JsonExtractBoolShallow(track_json, "is_local", false);
          if (item->name.empty() || item->uri.empty() || item->duration_sec <= 0 || !item->playable) {
            delete item;
            continue;
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
            if (!images.empty()) {
              item->image_uri = JsonExtractString(images[0], "url");
            }
          }
          
          tl->items.push_back(item);
        }
      }
      
      if (tsp->callback) {
        tsp->callback(tsp->callback_context, kTspCallbackEvent_ItemListChanged, tl, NULL);
      }
    }).detach();
  }
  
  return kTspErrorOk;
}

TSP_PUBLIC TspError TspItemListLoadRange(TspItemList *tl, int first_item, int num_items) {
  return kTspErrorOk;
}

TSP_PUBLIC void TspItemListSetFolderOpen(TspItemList *tl, int i, int open) {
}

TSP_PUBLIC void TspItemListSetRadioTracks(TspItemList *tl) {
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
                           "&scope=user-modify-playback-state%20user-read-playback-state%20playlist-read-private%20playlist-read-collaborative";
    
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
    
    usleep(500000); // 0.5s pause for visual responsiveness
    
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
  return 0;
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
  if (!access_token.empty()) {
    if (!next_uri.empty()) {
      std::string data = "{ \"uris\": [ \"" + next_uri + "\" ] }";
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
    tsp->local_expected_position_ms = -1;
    tsp->local_stopped = false;
    tsp->local_control_until_ms = NowMonotonicMs() + 5000;
  }
  DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
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
    tsp->local_expected_position_ms = -1;
    tsp->local_stopped = false;
    tsp->local_control_until_ms = NowMonotonicMs() + 5000;
  }
  DispatchAudioControl(tsp, kTspAudioFlag_FlushBuffer);
  if (!access_token.empty()) {
    std::thread([access_token]() {
      std::string device_id = GetSpotiampDeviceIdFromToken(access_token);
      HttpPostOrPut("PUT", UrlWithDeviceId("https://api.spotify.com/v1/me/player/play", device_id), access_token);
    }).detach();
  }
}

TSP_PUBLIC TspError TspPlayerPlayContext(Tsp *tsp, const char *context_uri, const char *track_uri, int index) {
  std::string access_token = GetAccessToken(tsp);
  if (!access_token.empty()) {
    std::string context = context_uri ? context_uri : "";
    std::string track = track_uri ? track_uri : "";
    std::string data = "{ \"context_uri\": \"" + context + "\"";
    if (!track.empty()) {
      data += ", \"offset\": { \"uri\": \"" + track + "\" }";
    } else if (index >= 0) {
      data += ", \"offset\": { \"position\": " + std::to_string(index) + " }";
    }
    data += " }";
    HttpPostOrPutDeviceAsync("PUT", "https://api.spotify.com/v1/me/player/play", access_token, data);
  }
  if (tsp) {
    std::lock_guard<std::mutex> lock(tsp->mutex);
    tsp->is_playing = false;
    tsp->player_position_ms = 0;
    tsp->player_position_updated_ms = NowMonotonicMs();
    tsp->local_desired_playing = true;
    tsp->local_expected_position_ms = 0;
    tsp->local_stopped = false;
    tsp->local_control_until_ms = NowMonotonicMs() + 5000;
  }
  DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
  DispatchPlayerCallback(tsp, kTspCallbackEvent_Pause);
  return kTspErrorOk;
}

TSP_PUBLIC void TspPlayerPlayItemList(Tsp *tsp, TspItemList *tl, int index) {
  if (!tsp || !tl || index < 0 || index >= (int)tl->items.size()) return;
  
  CopyItemListContents(tsp->player_list, tl);
  TspItem *item = tl->items[index];
  tl->now_playing_index = index;
  tsp->player_list->now_playing_index = index;
  std::string item_uri = item->uri;
  SetPendingNowPlayingIndex(tsp, index);
  
  std::string access_token = GetAccessToken(tsp);
  if (!access_token.empty()) {
    std::string data = "{ \"uris\": [ \"" + item_uri + "\" ], \"position_ms\": 0 }";
    HttpPostOrPutDeviceAsync("PUT", "https://api.spotify.com/v1/me/player/play", access_token, data);
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
  if (!access_token.empty()) {
    if (!prev_uri.empty()) {
      std::string data = "{ \"uris\": [ \"" + prev_uri + "\" ] }";
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
      resume_after_seek = tsp->is_playing;
      tsp->player_position_ms = position_ms;
      tsp->player_position_updated_ms = NowMonotonicMs();
      tsp->is_playing = false;
      tsp->local_desired_playing = resume_after_seek;
      tsp->local_expected_position_ms = position_ms;
      tsp->local_stopped = false;
      tsp->local_control_until_ms = NowMonotonicMs() + 7000;
    }
    if (resume_after_seek)
      DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
    std::string access_token = GetAccessToken(tsp);
    if (!access_token.empty()) {
      std::string url = "https://api.spotify.com/v1/me/player/seek?position_ms=" + std::to_string(position_ms);
      HttpPostOrPutDeviceAsync("PUT", url, access_token);
    }
    DispatchPlayerCallback(tsp, kTspCallbackEvent_Pause);
  }
  return kTspErrorOk;
}

TSP_PUBLIC void TspPlayerSetDevice(Tsp *tsp, const char *device_id, int flags) {
}

TSP_PUBLIC void TspPlayerSetEqualizer(Tsp *tsp, int enable, float pregain, const float bands[10]) {
}

TSP_PUBLIC void TspPlayerSetNowPlayingIndex(Tsp *tsp, int track) {
  if (!SetNowPlayingIndex(tsp, track) && tsp) {
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
    tsp->local_expected_position_ms = -1;
    tsp->local_stopped = true;
    tsp->local_control_until_ms = NowMonotonicMs() + 5000;
  }
  DispatchAudioControl(tsp, kTspAudioFlag_Pause | kTspAudioFlag_FlushBuffer);
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
  usleep(max_wait * 1000);
  return kTspErrorOk;
}

} // extern "C"
