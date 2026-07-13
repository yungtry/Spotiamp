#include "stdafx.h"
#include "dialogs.h"
#include <iostream>
#include <string>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>

static INT_PTR CALLBACK InputDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_INITDIALOG) {
    SetWindowLongPtr(hwnd, GWLP_USERDATA, lParam);
    RECT dialog_rect;
    RECT work_area;
    if (GetWindowRect(hwnd, &dialog_rect) &&
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &work_area, 0)) {
      int width = dialog_rect.right - dialog_rect.left;
      int height = dialog_rect.bottom - dialog_rect.top;
      int x = work_area.left + ((work_area.right - work_area.left) - width) / 2;
      int y = work_area.top + ((work_area.bottom - work_area.top) - height) / 2;
      SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    SetFocus(GetDlgItem(hwnd, 100));
    return FALSE;
  }
  if (msg == WM_COMMAND) {
    if (LOWORD(wParam) == IDOK) {
      std::string *data = (std::string*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
      char buf[1024];
      GetDlgItemTextA(hwnd, 100, buf, sizeof(buf));
      *data = buf;
      EndDialog(hwnd, 1);
      return TRUE;
    } else if (LOWORD(wParam) == IDCANCEL) {
      EndDialog(hwnd, 0);
      return TRUE;
    }
  }
  return FALSE;
}

#endif

#if defined(__linux__)
// ---------------------------------------------------------------------------
// Helper: run a command and capture its stdout
// ---------------------------------------------------------------------------
static std::string RunAndCapture(const char *cmd) {
  FILE *pipe = popen(cmd, "r");
  if (!pipe) return "";
  char buf[1024];
  std::string result;
  while (fgets(buf, sizeof(buf), pipe))
    result += buf;
  pclose(pipe);
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    result.pop_back();
  return result;
}

// ---------------------------------------------------------------------------
// Helper: check if a program exists on PATH
// ---------------------------------------------------------------------------
static bool CommandExists(const char *name) {
  std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
  return system(cmd.c_str()) == 0;
}

static std::string ShellQuote(const std::string &s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

// ---------------------------------------------------------------------------
// Linux: zenity dialog
// ---------------------------------------------------------------------------
static std::string ShowSearchDialog_Zenity(const std::string &current) {
  std::string cmd =
    "zenity --entry"
    " --title='Spotiamp Search'"
    " --text='Enter search query:'"
    " --entry-text=" + ShellQuote(current) +
    " 2>/dev/null";
  return RunAndCapture(cmd.c_str());
}

static std::string ShowSearchDialog_Kdialog(const std::string &current) {
  std::string cmd =
    "kdialog --title 'Spotiamp Search'"
    " --inputbox 'Search query:' " + ShellQuote(current) + " 2>/dev/null";
  return RunAndCapture(cmd.c_str());
}

static std::string ShowTextInputDialog_Zenity(const char *title, const char *message,
                                              const char *default_value) {
  std::string cmd =
    "zenity --entry"
    " --title=" + ShellQuote(title ? title : "Spotiamp") +
    " --text=" + ShellQuote(message ? message : "") +
    " --entry-text=" + ShellQuote(default_value ? default_value : "") +
    " 2>/dev/null";
  return RunAndCapture(cmd.c_str());
}

static std::string ShowTextInputDialog_Kdialog(const char *title, const char *message,
                                               const char *default_value) {
  std::string cmd =
    "kdialog --title " + ShellQuote(title ? title : "Spotiamp") +
    " --inputbox " + ShellQuote(message ? message : "") + " " +
    ShellQuote(default_value ? default_value : "") + " 2>/dev/null";
  return RunAndCapture(cmd.c_str());
}
#endif // __linux__

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string ShowSearchDialog(PlatformWindow *parent, Tsp *tsp) {
#if defined(__linux__)
  if (CommandExists("zenity"))
    return ShowSearchDialog_Zenity("");
  if (CommandExists("kdialog"))
    return ShowSearchDialog_Kdialog("");

  std::cout << "\n=== Spotiamp Search ===" << std::endl;
  std::cout << "Enter search query: ";
  std::string q;
  std::cin >> q;
  return q;

#elif defined(_WIN32)
  std::string q;
  DialogBoxParamA(GetModuleHandle(NULL), MAKEINTRESOURCEA(1002), NULL, InputDialogProc, (LPARAM)&q);
  return q;

#else
  std::cout << "\n=== Spotiamp Search ===" << std::endl;
  std::cout << "Enter search query: ";
  std::string q;
  std::cin >> q;
  return q;
#endif
}

std::string ShowTextInputDialog(PlatformWindow *parent, const char *title,
                                const char *message, const char *default_value) {
#if defined(__linux__)
  if (CommandExists("zenity"))
    return ShowTextInputDialog_Zenity(title, message, default_value);
  if (CommandExists("kdialog"))
    return ShowTextInputDialog_Kdialog(title, message, default_value);

  std::cout << "\n=== " << (title ? title : "Spotiamp") << " ===" << std::endl;
  if (message && message[0])
    std::cout << message << std::endl;
  std::cout << "> ";
  std::string q;
  std::getline(std::cin >> std::ws, q);
  return q;

#elif defined(_WIN32)
  std::string q = default_value ? default_value : "";
  DialogBoxParamA(GetModuleHandle(NULL), MAKEINTRESOURCEA(1003), NULL, InputDialogProc, (LPARAM)&q);
  return q;

#else
  std::cout << "\n=== " << (title ? title : "Spotiamp") << " ===" << std::endl;
  if (message && message[0])
    std::cout << message << std::endl;
  std::cout << "> ";
  std::string q;
  std::getline(std::cin >> std::ws, q);
  return q;
#endif
}

void AutoCompleteCopy() {
}
