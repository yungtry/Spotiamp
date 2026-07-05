#include "stdafx.h"
#if defined(WITH_SDL)
#include "spotifyamp.h"
#include "window.h"
#include <stdio.h>
#include "resource.h"
#include "SDL.h"
#include "SDL_surface.h"
#include "SDL_clipboard.h"
#include "SDL_video.h"
#include <algorithm>
#include <sys/stat.h>
#include "zipfile.h"

static ZipFile *current_skin_zip = NULL;
static std::string current_skin_basedir;

class SimpleFileIO : public FileIO {
  FILE *f_;
public:
  SimpleFileIO(const char *filename) {
    f_ = fopen(filename, "rb");
  }
  virtual ~SimpleFileIO() {
    if (f_) fclose(f_);
  }
  virtual size_t Read(int64 read_offs, void *buf, size_t buf_size) override {
    if (!f_) return 0;
    fseek(f_, (long)read_offs, SEEK_SET);
    return fread(buf, 1, buf_size, f_);
  }
};

#pragma comment (lib, "ws2_32.lib")
#pragma comment (lib, "sdl2.lib")

static bool g_quit;
static Point g_drag_start;
static Point g_drag_start_global;
static PlatformWindow *g_captured_window = NULL;
static bool HandleActiveMenuEvent(SDL_Event *event);

void PlatformWindow::UpdateActiveWindowDrag() {
  if (g_num_drag_windows <= 0)
    return;

  PlatformWindow *w = g_drag_windows[0];
  if (!w)
    return;

  int gx = 0;
  int gy = 0;
  SDL_GetGlobalMouseState(&gx, &gy);
  int dx = gx - g_drag_start_global.x;
  int dy = gy - g_drag_start_global.y;
  if (dx || dy) {
    w->HandleMouseDragOrSize(dx, dy, true);
    g_drag_start_global.x = gx;
    g_drag_start_global.y = gy;
  }
}

PlatformWindow::PlatformWindow() {
  active_window_ = true;
  need_repaint_ = true;
}

PlatformWindow::~PlatformWindow() {
}

void PlatformWindow::Create(PlatformWindow *owner_window) {
  PlatformWindowBase::Create(owner_window);
  window_ = SDL_CreateWindow("SpotifyAmp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             width_, height_, SDL_WINDOW_BORDERLESS | (visible_ ? 0 : SDL_WINDOW_HIDDEN));
  window_id_ = SDL_GetWindowID(window_);
  surface_ = SDL_GetWindowSurface(window_);
}

void PlatformWindow::Blit(int dx, int dy, int w, int h, Bitmap *src, int sx, int sy) {
  if (!surface_ || !src) return;
  SDL_Surface *srcb = (SDL_Surface*)src;
  SDL_Rect srcrect = {sx, sy, w, h};
  
  int d = double_size() ? 2 : 1;
  SDL_Rect dstrect = {dx * d, dy * d, w * d, h * d};
  
  if (d == 2) {
    SDL_BlitScaled(srcb, &srcrect, surface_, &dstrect);
  } else {
    SDL_BlitSurface(srcb, &srcrect, surface_, &dstrect);
  }
}

void PlatformWindow::StretchBlit(int dx, int dy, int w, int h, Bitmap *src, int sx, int sy, int sw, int sh) {
  if (!surface_ || !src) return;
  SDL_Surface *srcb = (SDL_Surface*)src;
  SDL_Rect srcrect = {sx, sy, sw, sh};
  
  int d = double_size() ? 2 : 1;
  SDL_Rect dstrect = {dx * d, dy * d, w * d, h * d};
  
  SDL_BlitScaled(srcb, &srcrect, surface_, &dstrect);
}

void PlatformWindow::Fill(int x, int y, int w, int h, unsigned int color) {
  if (!surface_) return;
  int d = double_size() ? 2 : 1;
  SDL_Rect rect = {x * d, y * d, w * d, h * d};
  
  uint8_t r = (color >> 16) & 0xFF;
  uint8_t g = (color >> 8) & 0xFF;
  uint8_t b = color & 0xFF;
  uint32_t mapped = SDL_MapRGB(surface_->format, r, g, b);
  SDL_FillRect(surface_, &rect, mapped);
}

void PlatformWindow::DrawText(int x, int y, int w, int h, const char *text, int flags, unsigned int color) {
  if (!text || !res.text) return;
  
  static const unsigned char charmap_local[128] = {
    30,30,30,30, 30,30,30,30, 30,30,30,30, 30,30,30,30,
    30,30,30,30, 30,30,30,30, 30,30,30,30, 30,30,30,30,
    30,48,26,61, 60,57,56,47, 44,45,30,50, 58,46,42,52,
    31,32,33,34, 35,36,37,38, 39,40,43,30, 30,59,30,30,
    30, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,53, 51,54,55,49,
    47, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,30, 30,30,66,30,
  };
  
  int text_len = 0;
  while (text[text_len]) text_len++;
  
  int text_width = text_len * 6;
  int start_x = x;
  if (flags == 1) { // right align
    start_x = x + w - text_width;
  }
  
  SDL_Surface *srcb = (SDL_Surface*)res.text;

  // Make the bitmap background (#181818) transparent so glyphs render
  // cleanly over any background (selection highlight, normal bg, etc.).
  uint32_t color_key = SDL_MapRGB(srcb->format, 0x18, 0x18, 0x18);
  SDL_SetColorKey(srcb, SDL_TRUE, color_key);

  // Tint the white glyph pixels to the requested text color.
  SDL_SetSurfaceColorMod(srcb,
      (color >> 16) & 0xFF,
      (color >>  8) & 0xFF,
       color        & 0xFF);

  int top_y = y + (h - 6) / 2;
  int cur_x = start_x;
  for (int i = 0; i < text_len; i++) {
    unsigned char b = text[i];
    int glyph = (b <= 127) ? charmap_local[b] : 30;
    
    if (cur_x + 5 > x + w) break;
    
    if (cur_x >= x) {
      Blit(cur_x, top_y, 5, 6, res.text, (glyph % 31) * 5, (glyph / 31) * 6);
    }
    cur_x += 6;
  }

  // Reset color mod and disable color key so other bitmap blits are unaffected.
  SDL_SetSurfaceColorMod(srcb, 0xFF, 0xFF, 0xFF);
  SDL_SetColorKey(srcb, SDL_FALSE, 0);
}

void PlatformWindow::Repaint() {
  need_repaint_ = true;
}

void PlatformWindow::Minimize() {
  SDL_MinimizeWindow(window_);
}

void PlatformWindow::InitDraggingOrSizing() {
  SDL_GetMouseState(&g_drag_start.x, &g_drag_start.y);
}

void PlatformWindow::PlatformPaint() {
  if (need_repaint_ && visible_) {
    need_repaint_ = false;
    surface_ = SDL_GetWindowSurface(window_);
    Paint();
    SDL_UpdateWindowSurface(window_);
  }
}

void PlatformWindow::Quit() {
  g_quit = true;
}

PlatformWindow *PlatformWindow::FromID(Uint32 window_id) {
  for(PlatformWindow *w = g_platform_windows; w; w = w->next())
    if (w->window_id_ == window_id)
      return w;
  return NULL;
}


void PlatformWindow::HandleEvent(SDL_Event *event) {
  if (HandleActiveMenuEvent(event))
    return;

  switch(event->type) {
  case SDL_WINDOWEVENT: {
    if (event->window.type == SDL_WINDOWEVENT_EXPOSED) {
      PlatformWindow *w = FromID(event->window.windowID);
      if (w) w->Repaint();
    }
    break;
  }
  case SDL_KEYDOWN:
    break;
  case SDL_MOUSEMOTION: {
    PlatformWindow *w = g_captured_window;
    int mx, my;
    if (g_captured_window) {
      int wx, wy;
      SDL_GetWindowPosition(g_captured_window->window_, &wx, &wy);
      int gx, gy;
      SDL_GetGlobalMouseState(&gx, &gy);
      mx = gx - wx;
      my = gy - wy;
    } else {
      PlatformWindow *hovered = FromID(event->motion.windowID);
      w = hovered;
      mx = event->motion.x;
      my = event->motion.y;
    }
    if (w) {
      int d = w->double_size() ? 1 : 0;
      w->MouseMove(mx >> d, my >> d);
    }
    PlatformWindow::UpdateActiveWindowDrag();
    break;
  }
  case SDL_MOUSEBUTTONDOWN: {
    PlatformWindow *w = FromID(event->button.windowID);
    if (w) {
      int d = w->double_size() ? 1 : 0;
      int x = event->button.x >> d;
      int y = event->button.y >> d;
      if (event->button.button == SDL_BUTTON_LEFT) {
        if (event->button.clicks == 2) {
          w->LeftButtonDouble(x, y);
        } else {
          g_captured_window = w;
          SDL_CaptureMouse(SDL_TRUE);
          w->LeftButtonDown(x, y);
          if (w->dragable_) {
            w->InitWindowDragging();
            SDL_GetGlobalMouseState(&g_drag_start_global.x, &g_drag_start_global.y);
          }
        }
      } else if (event->button.button == SDL_BUTTON_RIGHT) {
        w->RightButtonDown(x, y);
      }
    }
    break;
  }
  case SDL_MOUSEBUTTONUP: {
    PlatformWindow *w = g_captured_window ? g_captured_window : FromID(event->button.windowID);
    if (w) {
      int mx, my;
      if (g_captured_window) {
        int wx, wy;
        SDL_GetWindowPosition(g_captured_window->window_, &wx, &wy);
        int gx, gy;
        SDL_GetGlobalMouseState(&gx, &gy);
        mx = gx - wx;
        my = gy - wy;
      } else {
        mx = event->button.x;
        my = event->button.y;
      }
      int d = w->double_size() ? 1 : 0;
      int x = mx >> d;
      int y = my >> d;
      if (event->button.button == SDL_BUTTON_LEFT) {
        g_captured_window = NULL;
        SDL_CaptureMouse(SDL_FALSE);
        g_num_drag_windows = 0;
        w->LeftButtonUp(x, y);
      } else if (event->button.button == SDL_BUTTON_RIGHT) {
        w->RightButtonUp(x, y);
      }
    }
    break;
  }
  case SDL_MOUSEWHEEL: {
    PlatformWindow *w = FromID(event->wheel.windowID);
    if (w) {
      int mx, my;
      SDL_GetMouseState(&mx, &my);
      int d = w->double_size() ? 1 : 0;
      w->MouseWheel(mx >> d, my >> d, 0, event->wheel.y * 120);
    }
    break;
  }
  case SDL_QUIT:
    g_quit = true;
    break;
  }
}

void PlatformWindow::MoveAllWindows() {
  for(PlatformWindow *w = g_platform_windows; w; w = w->next()) {
    if (w->need_move_) {
      const Rect *r = w->screen_rect();
      SDL_SetWindowPosition(w->window_, r->left, r->top);
      SDL_SetWindowSize(w->window_, r->right - r->left, r->bottom - r->top);
      w->surface_ = SDL_GetWindowSurface(w->window_);
      w->need_repaint_ = true;
      w->need_move_ = false;
    }
  }
}

void PlatformWindow::FindMonitors() {

}

void PlatformWindow::VisibleChanged() {
  if (!window_) return;
  if (visible_) {
    SDL_ShowWindow(window_);
    surface_ = SDL_GetWindowSurface(window_);
    need_repaint_ = true;
  } else {
    SDL_HideWindow(window_);
  }
}

void PlatformWindow::AlwaysOnTopChanged() {

}

void PlatformWindow::Line(int x, int y, int x2, int y2, unsigned int color) {

}

void PlatformWindow::SetPixel(int x, int y, unsigned int color) {
  if (!surface_) return;
  int d = double_size() ? 2 : 1;
  uint8_t r = (color >> 16) & 0xFF;
  uint8_t g = (color >> 8) & 0xFF;
  uint8_t b = color & 0xFF;
  uint32_t mapped = SDL_MapRGB(surface_->format, r, g, b);
  SDL_Rect rect = {x * d, y * d, d, d};
  SDL_FillRect(surface_, &rect, mapped);
}

unsigned int PlatformWindow::GetBitmapPixel(Bitmap *src, int x, int y) {
  return 0;
}

void PlatformWindow::Move(int l, int t) {
  if (window_)
    SDL_SetWindowPosition(window_, l, t);
}

void PlatformWindow::BringWindowToTop() {
  if (window_)
    SDL_RaiseWindow(window_);
}

#if defined(_MSC_VER) && defined(OS_WIN)
extern "C" int _stdcall WinMain(int a, int b, int c, int d) {
  SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_AUDIO);
  InitSpotamp(__argc, __argv);
#else
#include <mutex>
#include <vector>

static std::mutex g_main_thread_mutex;
static std::vector<std::function<void()>> g_main_thread_tasks;

void RunOnMainThread(std::function<void()> task) {
  std::lock_guard<std::mutex> lock(g_main_thread_mutex);
  g_main_thread_tasks.push_back(task);
}

void ProcessMainThreadTasks() {
  std::vector<std::function<void()>> tasks;
  {
    std::lock_guard<std::mutex> lock(g_main_thread_mutex);
    tasks = std::move(g_main_thread_tasks);
    g_main_thread_tasks.clear();
  }
  for (const auto &task : tasks) {
    task();
  }
}

int main(int argc, char *argv[]) {
  // Disable HiDPI on macOS so the window surface is at logical pixel size
  // and macOS scales it up using nearest-neighbor. Without this, macOS uses
  // bilinear interpolation on the 1x surface, making the pixel art blurry.
  SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");
  SDL_Init(SDL_INIT_TIMER | SDL_INIT_VIDEO | SDL_INIT_AUDIO);
  InitSpotamp(argc, argv);
#endif
  
  SDL_Event event;
  while (!g_quit) {
    if (SDL_WaitEventTimeout(&event, 5)) {
      PlatformWindow::HandleEvent(&event);
      while (SDL_PollEvent(&event))
        PlatformWindow::HandleEvent(&event);
    }
    PlatformWindow::UpdateActiveWindowDrag();
    ProcessMainThreadTasks();
    g_main_window->MainLoop();
    for(PlatformWindow *w = g_platform_windows; w; w = w->next())
      w->PlatformPaint();
  }

  return 0;
}

void PlatformDeleteBitmap(Bitmap *bitmap) {
  SDL_FreeSurface((SDL_Surface*)bitmap);
}

Size PlatformGetBitmapSize(Bitmap *bitmap) {
  Size rv = {0,0};
  if (bitmap) {
    SDL_Surface *surf = (SDL_Surface*)bitmap;
    rv.w = surf->w;
    rv.h = surf->h;
  }
  return rv;
}

Bitmap *PlatformLoadBitmapFromBuf(const void *data, size_t data_size) {
  SDL_RWops *rw = SDL_RWFromConstMem(data, data_size);
  if (!rw) return NULL;
  SDL_Surface *surf = SDL_LoadBMP_RW(rw, 1);
  return (Bitmap*)surf;
}

bool PlatformLoadBitmap(Bitmap **bitmap, const char *name) {
  if (*bitmap) {
    SDL_FreeSurface((SDL_Surface*)*bitmap);
    *bitmap = NULL;
  }
  std::string zipdata;
  if (current_skin_zip && ZipFileReader::ReadFileToString(current_skin_zip, name, &zipdata)) {
    *bitmap = PlatformLoadBitmapFromBuf(zipdata.data(), zipdata.size());
    if (*bitmap) return true;
  }
  if (current_skin_basedir.size()) {
    std::string path = current_skin_basedir + name;
    #if !defined(_WIN32)
    std::replace(path.begin(), path.end(), '\\', '/');
    #endif
    *bitmap = (Bitmap*)SDL_LoadBMP(path.c_str());
  }
  if (!*bitmap) {
    std::string path = std::string("skin/") + name;
    *bitmap = (Bitmap*)SDL_LoadBMP(path.c_str());
  }
  return *bitmap != NULL;
}

bool PlatformLoadString(const char *name, std::string *result) {
  if (current_skin_zip && ZipFileReader::ReadFileToString(current_skin_zip, name, result))
    return true;
  if (current_skin_basedir.size()) {
    std::string path = current_skin_basedir + name;
    #if !defined(_WIN32)
    std::replace(path.begin(), path.end(), '\\', '/');
    #endif
    FILE *f = fopen(path.c_str(), "rb");
    if (f) {
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      result->resize(sz);
      fread(&((*result)[0]), 1, sz, f);
      fclose(f);
      return true;
    }
  }
  return false;
}

void PlatformSetSkin(const char *filename) {
  if (current_skin_zip) {
    ZipFileReader::Free(current_skin_zip);
    current_skin_zip = NULL;
  }
  current_skin_basedir.clear();
  
  if (filename && *filename) {
    struct stat st;
    if (stat(filename, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        current_skin_basedir = filename;
        if (current_skin_basedir.back() != '/' && current_skin_basedir.back() != '\\') {
          current_skin_basedir += '/';
        }
      } else {
        FileIO *file_io = new SimpleFileIO(filename);
        current_skin_zip = ZipFileReader::Open(file_io, true);
      }
    }
  }
}

char *for_each_line(char **ptr) {
  char *s = *ptr;
  if (s == NULL) return NULL;
  char *x = strchr(s, '\n');
  if (x) {
    x[x > s && x[-1] == '\r' ? -1 : 0] = '\0';
    *ptr = x + 1;
  } else {
    *ptr = NULL;
  }
  return s;
}

char *PlatformReadClipboard() {
  static char *data;
  if (data) { SDL_free(data); data = NULL; }
  data = SDL_GetClipboardText();
  return data;
}

// Global variables
char machine_id[128] = "spotiamp_mac_machine";
char exepath[MAX_PATH] = "./";
WindowHandle g_visualizer_wnd = 0;
int vis_pause = 0;
unsigned int viscolors[24] = {0};
extern const unsigned int default_viscolors[24] = {
  // color 0 = background (black)
  0x181818,
  // color 1 = unused / grey for dots (Winamp default)
  0x181818,
  // colors 2..17 = bar rows top to bottom (Spotify green gradient)
  0x1dd954, // 2  top
  0x1dd954, // 3
  0x1dd954, // 4
  0x1dd954, // 5
  0x1dd954, // 6
  0x1dd954, // 7
  0x1dd954, // 8
  0x1dd954, // 9
  0x1dd954, // 10
  0x1dd954, // 11
  0x148840, // 12
  0x148840, // 13
  0x148840, // 14
  0x148840, // 15
  0x148840, // 16
  0x148840, // 17 bottom
  // colors 18..22 = oscilloscope (unused in bar mode)
  0xbababa,
  0xbababa,
  0xbababa,
  0xbababa,
  0xbababa,
  // color 23 = analyzer peak dots
  0xc8c8c8,
};

bool PlatformWindow::g_easy_move = false;

void PlatformWindow::MakeActive() {
  if (window_) SDL_RaiseWindow(window_);
}

void PlatformWindow::RepaintRange(int x, int y, int w, int h) {
  Repaint();
}

void PlatformWindow::SetFont(const char *fontname, int size) {
}

int PlatformWindow::GetFontHeight() {
  return 13;
}

void PlatformWindow::GetWindowText(char *s, int size) {
  if (size > 0) s[0] = 0;
}

void PlatformWindow::SetWindowText(const char *s) {
  if (window_) {
    SDL_SetWindowTitle(window_, s);
  }
}

int MsgBox(const char *message, const char *title, int flags) {
  printf("[%s] %s\n", title, message);
  return IDOK;
}

void OpenUrl(const char *url) {
#if defined(__APPLE__)
  std::string cmd = std::string("open \"") + url + "\"";
  system(cmd.c_str());
#endif
}

const char *PlatformEnumAudioDevices(int i) {
  return NULL;
}

unsigned int PlatformGetTicks() {
  return SDL_GetTicks();
}

bool PlatformWriteClipboard(const char *text) {
  return SDL_SetClipboardText(text) == 0;
}

FileEnumerator::FileEnumerator(const char *directory) {
  strncpy(directory_, directory ? directory : "", sizeof(directory_) - 1);
  directory_[sizeof(directory_) - 1] = 0;
#if !defined(_WIN32)
  std::replace(directory_, directory_ + strlen(directory_), '\\', '/');
#endif
  dir_ = opendir(directory_);
  filename_[0] = 0;
  current_is_dir_ = false;
}

FileEnumerator::~FileEnumerator() {
  if (dir_)
    closedir(dir_);
}

bool FileEnumerator::Next() {
  if (!dir_)
    return false;

  struct dirent *entry = NULL;
  while ((entry = readdir(dir_)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;

    strncpy(filename_, entry->d_name, sizeof(filename_) - 1);
    filename_[sizeof(filename_) - 1] = 0;

    std::string path = std::string(directory_) + "/";
    path += filename_;
    struct stat st;
    current_is_dir_ = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    return true;
  }

  return false;
}

bool FileEnumerator::is_directory() const {
  return current_is_dir_;
}

char *FileEnumerator::filename() const {
  return (char*)filename_;
}

#include <vector>
#include <string>

struct MenuItem {
  int id;
  std::string title;
  int flags;
  int depth;
};
static std::vector<MenuItem> g_menu_items;
static std::vector<int> g_menu_stack;
static int g_menu_depth = 0;
static const int kMenuSeparator = 8;

MenuBuilder::MenuBuilder() {
  g_menu_items.clear();
  g_menu_stack.clear();
  g_menu_depth = 0;
}

MenuBuilder::~MenuBuilder() {}

void MenuBuilder::BeginSubMenu() {
  g_menu_stack.push_back((int)g_menu_items.size());
  g_menu_depth++;
}

void MenuBuilder::EndSubMenu(const char *title, int flags) {
  int start = g_menu_stack.empty() ? 0 : g_menu_stack.back();
  if (!g_menu_stack.empty())
    g_menu_stack.pop_back();
  if (g_menu_depth > 0)
    g_menu_depth--;
  g_menu_items.insert(g_menu_items.begin() + start,
                      {0, title ? title : "", flags | kGrayed, g_menu_depth});
}

void MenuBuilder::AddSeparator() {
  g_menu_items.push_back({0, "", kMenuSeparator, g_menu_depth});
}

void MenuBuilder::AddItem(int id, const char *title, int flags) {
  g_menu_items.push_back({id, title ? title : "", flags, g_menu_depth});
}

int MenuBuilder::Popup(PlatformWindow *window) {
  int x = 0;
  int y = 0;
  SDL_GetGlobalMouseState(&x, &y);
  return PopupAt(NULL, x, y);
}

static std::string MenuDisplayTitle(const MenuItem &item) {
  std::string title;
  if (item.flags & MenuBuilder::kRadio)
    title += "* ";
  else if (item.flags & MenuBuilder::kChecked)
    title += "[x] ";
  else
    title += "  ";

  for (char c : item.title) {
    if (c == '\t') {
      title += "  ";
    } else {
      title += c;
    }
  }
  return title;
}

static int MenuTextWidth(const std::string &text) {
  return (int)text.size() * 6;
}

static Uint32 GetSurfacePixel(SDL_Surface *surface, int x, int y) {
  int bpp = surface->format->BytesPerPixel;
  Uint8 *p = (Uint8*)surface->pixels + y * surface->pitch + x * bpp;
  switch (bpp) {
  case 1:
    return *p;
  case 2:
    return *(Uint16*)p;
  case 3:
    if (SDL_BYTEORDER == SDL_BIG_ENDIAN)
      return p[0] << 16 | p[1] << 8 | p[2];
    return p[0] | p[1] << 8 | p[2] << 16;
  case 4:
    return *(Uint32*)p;
  default:
    return 0;
  }
}

static void DrawMenuText(SDL_Surface *surface, int x, int y, const std::string &text, Uint8 r, Uint8 g, Uint8 b) {
  if (!surface || !res.text)
    return;

  static const unsigned char charmap_local[128] = {
    30,30,30,30, 30,30,30,30, 30,30,30,30, 30,30,30,30,
    30,30,30,30, 30,30,30,30, 30,30,30,30, 30,30,30,30,
    30,48,26,61, 60,57,56,47, 44,45,30,50, 58,46,42,52,
    31,32,33,34, 35,36,37,38, 39,40,43,30, 30,59,30,30,
    30, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,53, 51,54,55,49,
    47, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,30, 30,30,66,30,
  };

  SDL_Surface *font = (SDL_Surface*)res.text;
  if (SDL_MUSTLOCK(font))
    SDL_LockSurface(font);
  Uint32 dst_color = SDL_MapRGB(surface->format, r, g, b);
  int bg_glyph = charmap_local[' '];
  Uint32 bg = GetSurfacePixel(font, (bg_glyph % 31) * 5, (bg_glyph / 31) * 6);
  Uint8 bg_r = 0;
  Uint8 bg_g = 0;
  Uint8 bg_b = 0;
  SDL_GetRGB(bg, font->format, &bg_r, &bg_g, &bg_b);

  int cur_x = x;
  for (char c : text) {
    unsigned char ch = (unsigned char)c;
    int glyph = (ch <= 127) ? charmap_local[ch] : 30;
    int src_x = (glyph % 31) * 5;
    int src_y = (glyph / 31) * 6;

    for (int py = 0; py < 6; ++py) {
      for (int px = 0; px < 5; ++px) {
        Uint32 src_pixel = GetSurfacePixel(font, src_x + px, src_y + py);
        Uint8 src_r = 0;
        Uint8 src_g = 0;
        Uint8 src_b = 0;
        SDL_GetRGB(src_pixel, font->format, &src_r, &src_g, &src_b);
        int diff = abs((int)src_r - (int)bg_r) +
                   abs((int)src_g - (int)bg_g) +
                   abs((int)src_b - (int)bg_b);
        if (diff > 24) {
          SDL_Rect dot = {cur_x + px, y + py, 1, 1};
          SDL_FillRect(surface, &dot, dst_color);
        }
      }
    }
    cur_x += 6;
  }

  if (SDL_MUSTLOCK(font))
    SDL_UnlockSurface(font);
}

static bool MenuItemSelectable(const MenuItem &item) {
  return item.id != 0 && !(item.flags & kMenuSeparator) && !(item.flags & MenuBuilder::kGrayed);
}

static SDL_Window *g_active_menu_popup = NULL;
static Uint32 g_active_menu_popup_id = 0;
static int g_active_menu_width = 0;
static int g_active_menu_row_h = 17;
static int g_active_menu_visible_rows = 0;
static int g_active_menu_scroll = 0;
static int g_active_menu_hover = -1;
static std::function<void(int)> g_active_menu_callback;

static void PaintPopupMenu(SDL_Window *popup, int width, int row_h, int visible_rows,
                           int scroll, int hover) {
  SDL_Surface *surface = SDL_GetWindowSurface(popup);
  if (!surface)
    return;

  Uint32 bg = SDL_MapRGB(surface->format, 0xee, 0xee, 0xe8);
  Uint32 border = SDL_MapRGB(surface->format, 0x21, 0x21, 0x21);
  Uint32 sep = SDL_MapRGB(surface->format, 0x9c, 0x9c, 0x94);
  Uint32 header = SDL_MapRGB(surface->format, 0xd7, 0xd7, 0xcf);
  Uint32 hover_bg = SDL_MapRGB(surface->format, 0x24, 0x63, 0xa8);

  SDL_FillRect(surface, NULL, bg);
  SDL_Rect top = {0, 0, width, 1};
  SDL_Rect left = {0, 0, 1, visible_rows * row_h + 2};
  SDL_Rect right = {width - 1, 0, 1, visible_rows * row_h + 2};
  SDL_Rect bottom = {0, visible_rows * row_h + 1, width, 1};
  SDL_FillRect(surface, &top, border);
  SDL_FillRect(surface, &left, border);
  SDL_FillRect(surface, &right, border);
  SDL_FillRect(surface, &bottom, border);

  for (int row = 0; row < visible_rows; ++row) {
    int item_index = scroll + row;
    if (item_index < 0 || item_index >= (int)g_menu_items.size())
      continue;

    const MenuItem &item = g_menu_items[item_index];
    int y = 1 + row * row_h;
    if (item.flags & kMenuSeparator) {
      SDL_Rect line = {8, y + row_h / 2, width - 16, 1};
      SDL_FillRect(surface, &line, sep);
      continue;
    }

    bool selectable = MenuItemSelectable(item);
    bool selected = item_index == hover && selectable;
    if (selected) {
      SDL_Rect rect = {2, y + 1, width - 4, row_h - 2};
      SDL_FillRect(surface, &rect, hover_bg);
    } else if (item.flags & MenuBuilder::kGrayed) {
      SDL_Rect rect = {2, y + 1, width - 4, row_h - 2};
      SDL_FillRect(surface, &rect, header);
    }

    std::string title = MenuDisplayTitle(item);
    int text_x = 8 + item.depth * 14;
    int text_y = y + (row_h - 6) / 2;
    Uint8 c = selectable ? 0x00 : 0x66;
    if (selected)
      DrawMenuText(surface, text_x, text_y, title, 0xff, 0xff, 0xff);
    else
      DrawMenuText(surface, text_x, text_y, title, c, c, c);
  }

  SDL_UpdateWindowSurface(popup);
}

int MenuBuilder::PopupAt(PlatformWindow *window, int x, int y) {
  if (g_menu_items.empty())
    return 0;

  int screen_x = x;
  int screen_y = y;
  if (window) {
    const Rect *rect = window->screen_rect();
    int d = window->double_size() ? 2 : 1;
    screen_x = rect->left + x * d;
    screen_y = rect->top + y * d;
  }

  const int row_h = 17;
  int max_width = 150;
  for (const auto &item : g_menu_items) {
    if (item.flags & kMenuSeparator)
      continue;
    int width = 18 + item.depth * 14 + MenuTextWidth(MenuDisplayTitle(item));
    if (width > max_width)
      max_width = width;
  }

  SDL_Rect bounds = {0, 0, 1440, 900};
  SDL_GetDisplayUsableBounds(0, &bounds);
  int visible_rows = (int)g_menu_items.size();
  int max_rows = IntMax(1, (bounds.h - 24) / row_h);
  if (visible_rows > max_rows)
    visible_rows = max_rows;

  int popup_w = IntMin(IntMax(max_width + 12, 150), IntMax(150, bounds.w - 24));
  int popup_h = visible_rows * row_h + 2;
  if (screen_x + popup_w > bounds.x + bounds.w)
    screen_x = bounds.x + bounds.w - popup_w;
  if (screen_y + popup_h > bounds.y + bounds.h)
    screen_y = bounds.y + bounds.h - popup_h;
  screen_x = IntMax(bounds.x, screen_x);
  screen_y = IntMax(bounds.y, screen_y);

  SDL_Window *popup = SDL_CreateWindow("Spotiamp Menu", screen_x, screen_y,
                                       popup_w, popup_h,
                                       SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP);
  if (!popup)
    return 0;

  Uint32 popup_id = SDL_GetWindowID(popup);
  SDL_RaiseWindow(popup);

  int scroll = 0;
  int hover = -1;
  int result = 0;
  bool done = false;
  PaintPopupMenu(popup, popup_w, row_h, visible_rows, scroll, hover);

  while (!done) {
    SDL_Event event;
    bool has_event = SDL_WaitEventTimeout(&event, 10) != 0;

    ProcessMainThreadTasks();
    if (g_main_window)
      g_main_window->MainLoop();
    for (PlatformWindow *w = g_platform_windows; w; w = w->next())
      w->PlatformPaint();

    if (!has_event)
      continue;

    switch (event.type) {
    case SDL_QUIT:
      g_quit = true;
      done = true;
      break;
    case SDL_KEYDOWN:
      if (event.key.keysym.sym == SDLK_ESCAPE) {
        done = true;
      } else if (event.key.keysym.sym == SDLK_RETURN && hover >= 0 &&
                 hover < (int)g_menu_items.size() && MenuItemSelectable(g_menu_items[hover])) {
        result = g_menu_items[hover].id;
        done = true;
      }
      break;
    case SDL_MOUSEMOTION:
      if (event.motion.windowID == popup_id) {
        int row = event.motion.y / row_h;
        int new_hover = scroll + row;
        if (row < 0 || row >= visible_rows || new_hover >= (int)g_menu_items.size())
          new_hover = -1;
        if (new_hover != hover) {
          hover = new_hover;
          PaintPopupMenu(popup, popup_w, row_h, visible_rows, scroll, hover);
        }
      }
      break;
    case SDL_MOUSEWHEEL:
      if ((int)g_menu_items.size() > visible_rows) {
        int max_scroll = (int)g_menu_items.size() - visible_rows;
        scroll = Clamp(scroll - event.wheel.y * 3, 0, max_scroll);
        hover = -1;
        PaintPopupMenu(popup, popup_w, row_h, visible_rows, scroll, hover);
      }
      break;
    case SDL_MOUSEBUTTONDOWN:
      if (event.button.windowID != popup_id) {
        done = true;
      } else if (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT) {
        int row = event.button.y / row_h;
        int clicked = scroll + row;
        if (row >= 0 && row < visible_rows && clicked >= 0 &&
            clicked < (int)g_menu_items.size() && MenuItemSelectable(g_menu_items[clicked])) {
          result = g_menu_items[clicked].id;
        }
        done = true;
      }
      break;
    case SDL_WINDOWEVENT:
      if (event.window.windowID == popup_id && event.window.event == SDL_WINDOWEVENT_EXPOSED)
        PaintPopupMenu(popup, popup_w, row_h, visible_rows, scroll, hover);
      break;
    default:
      break;
    }
  }

  SDL_DestroyWindow(popup);
  return result;
}

static void CloseActiveMenu(int result, bool invoke_callback) {
  if (!g_active_menu_popup)
    return;

  SDL_Window *popup = g_active_menu_popup;
  std::function<void(int)> callback = g_active_menu_callback;
  g_active_menu_popup = NULL;
  g_active_menu_popup_id = 0;
  g_active_menu_callback = NULL;
  SDL_DestroyWindow(popup);

  if (invoke_callback && callback)
    callback(result);
}

static bool OpenActiveMenu(PlatformWindow *window, int x, int y, std::function<void(int)> callback) {
  CloseActiveMenu(0, false);
  if (g_menu_items.empty())
    return false;

  int screen_x = x;
  int screen_y = y;
  if (window) {
    const Rect *rect = window->screen_rect();
    int d = window->double_size() ? 2 : 1;
    screen_x = rect->left + x * d;
    screen_y = rect->top + y * d;
  }

  int max_width = 150;
  for (const auto &item : g_menu_items) {
    if (item.flags & kMenuSeparator)
      continue;
    int width = 18 + item.depth * 14 + MenuTextWidth(MenuDisplayTitle(item));
    if (width > max_width)
      max_width = width;
  }

  SDL_Rect bounds = {0, 0, 1440, 900};
  SDL_GetDisplayUsableBounds(0, &bounds);
  g_active_menu_row_h = 17;
  g_active_menu_visible_rows = (int)g_menu_items.size();
  int max_rows = IntMax(1, (bounds.h - 24) / g_active_menu_row_h);
  if (g_active_menu_visible_rows > max_rows)
    g_active_menu_visible_rows = max_rows;

  g_active_menu_width = IntMin(IntMax(max_width + 12, 150), IntMax(150, bounds.w - 24));
  int popup_h = g_active_menu_visible_rows * g_active_menu_row_h + 2;
  if (screen_x + g_active_menu_width > bounds.x + bounds.w)
    screen_x = bounds.x + bounds.w - g_active_menu_width;
  if (screen_y + popup_h > bounds.y + bounds.h)
    screen_y = bounds.y + bounds.h - popup_h;
  screen_x = IntMax(bounds.x, screen_x);
  screen_y = IntMax(bounds.y, screen_y);

  g_active_menu_popup = SDL_CreateWindow("Spotiamp Menu", screen_x, screen_y,
                                         g_active_menu_width, popup_h,
                                         SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP);
  if (!g_active_menu_popup)
    return false;

  g_active_menu_popup_id = SDL_GetWindowID(g_active_menu_popup);
  g_active_menu_scroll = 0;
  g_active_menu_hover = -1;
  g_active_menu_callback = callback;
  SDL_RaiseWindow(g_active_menu_popup);
  PaintPopupMenu(g_active_menu_popup, g_active_menu_width, g_active_menu_row_h,
                 g_active_menu_visible_rows, g_active_menu_scroll, g_active_menu_hover);
  return true;
}

void MenuBuilder::PopupAsync(PlatformWindow *window, std::function<void(int)> callback) {
  int x = 0;
  int y = 0;
  SDL_GetGlobalMouseState(&x, &y);
  PopupAtAsync(NULL, x, y, callback);
}

void MenuBuilder::PopupAtAsync(PlatformWindow *window, int x, int y, std::function<void(int)> callback) {
  if (!OpenActiveMenu(window, x, y, callback) && callback)
    callback(0);
}

static bool HandleActiveMenuEvent(SDL_Event *event) {
  if (!g_active_menu_popup)
    return false;

  switch (event->type) {
  case SDL_QUIT:
    CloseActiveMenu(0, false);
    return false;
  case SDL_KEYDOWN:
    if (event->key.keysym.sym == SDLK_ESCAPE) {
      CloseActiveMenu(0, false);
    } else if (event->key.keysym.sym == SDLK_RETURN &&
               g_active_menu_hover >= 0 &&
               g_active_menu_hover < (int)g_menu_items.size() &&
               MenuItemSelectable(g_menu_items[g_active_menu_hover])) {
      CloseActiveMenu(g_menu_items[g_active_menu_hover].id, true);
    }
    return true;
  case SDL_MOUSEMOTION:
    if (event->motion.windowID == g_active_menu_popup_id) {
      int row = event->motion.y / g_active_menu_row_h;
      int new_hover = g_active_menu_scroll + row;
      if (row < 0 || row >= g_active_menu_visible_rows || new_hover >= (int)g_menu_items.size())
        new_hover = -1;
      if (new_hover != g_active_menu_hover) {
        g_active_menu_hover = new_hover;
        PaintPopupMenu(g_active_menu_popup, g_active_menu_width, g_active_menu_row_h,
                       g_active_menu_visible_rows, g_active_menu_scroll, g_active_menu_hover);
      }
      return true;
    }
    return false;
  case SDL_MOUSEWHEEL:
    if ((int)g_menu_items.size() > g_active_menu_visible_rows) {
      int max_scroll = (int)g_menu_items.size() - g_active_menu_visible_rows;
      g_active_menu_scroll = Clamp(g_active_menu_scroll - event->wheel.y * 3, 0, max_scroll);
      g_active_menu_hover = -1;
      PaintPopupMenu(g_active_menu_popup, g_active_menu_width, g_active_menu_row_h,
                     g_active_menu_visible_rows, g_active_menu_scroll, g_active_menu_hover);
    }
    return true;
  case SDL_MOUSEBUTTONDOWN:
    if (event->button.windowID != g_active_menu_popup_id) {
      CloseActiveMenu(0, false);
      return true;
    }
    if (event->button.button == SDL_BUTTON_LEFT || event->button.button == SDL_BUTTON_RIGHT) {
      int row = event->button.y / g_active_menu_row_h;
      int clicked = g_active_menu_scroll + row;
      int result = 0;
      if (row >= 0 && row < g_active_menu_visible_rows &&
          clicked >= 0 && clicked < (int)g_menu_items.size() &&
          MenuItemSelectable(g_menu_items[clicked])) {
        result = g_menu_items[clicked].id;
      }
      CloseActiveMenu(result, result != 0);
    }
    return true;
  case SDL_WINDOWEVENT:
    if (event->window.windowID == g_active_menu_popup_id) {
      if (event->window.event == SDL_WINDOWEVENT_EXPOSED) {
        PaintPopupMenu(g_active_menu_popup, g_active_menu_width, g_active_menu_row_h,
                       g_active_menu_visible_rows, g_active_menu_scroll, g_active_menu_hover);
      } else if (event->window.event == SDL_WINDOWEVENT_CLOSE) {
        CloseActiveMenu(0, false);
      }
      return true;
    }
    return false;
  default:
    return false;
  }
}

#endif  // defined(WITH_SDL)
