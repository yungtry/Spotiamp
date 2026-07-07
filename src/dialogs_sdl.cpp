#include "stdafx.h"
#include "dialogs.h"
#include <iostream>
#include <string>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

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
// macOS helper (kept for the non-bundle macOS build that uses this file)
// ---------------------------------------------------------------------------
#if defined(__APPLE__)
static std::string ShowMacPrompt(const std::string &prompt,
                                  const std::string &default_val = "",
                                  bool hidden = false) {
  std::string cmd = "osascript -e 'display dialog \"" + prompt +
                    "\" default answer \"" + default_val + "\"";
  if (hidden)
    cmd += " with hidden answer";
  cmd += " with title \"Spotiamp\"' -e 'text returned of result' 2>/dev/null";
  return RunAndCapture(cmd.c_str());
}
#endif

// ---------------------------------------------------------------------------
// Linux: zenity dialog
// ---------------------------------------------------------------------------
#if defined(__linux__)
static bool ShowLoginDialog_Zenity(std::string *username, std::string *password) {
  // Build a zenity --forms dialog with two fields
  std::string cmd =
    "zenity --forms"
    " --title='Sign in to Spotify'"
    " --text='Enter your Spotify credentials'"
    " --add-entry='Username'"
    " --add-password='Password'"
    " --ok-label='Sign In'"
    " --separator='\\n'"
    " 2>/dev/null";

  // Pre-fill username by piping isn't possible with zenity forms, but we
  // can fall back to two separate zenity calls if a username is already set.
  if (!username->empty()) {
    // Two-step: username already known, just ask for password
    std::string pcmd =
      "zenity --password"
      " --title='Sign in to Spotify'"
      " --username"
      " 2>/dev/null";
    // zenity --password --username prints "user\npass\n" when --username is given
    std::string out = RunAndCapture(pcmd.c_str());
    if (out.empty()) return false;
    size_t nl = out.find('\n');
    if (nl != std::string::npos) {
      *username = out.substr(0, nl);
      *password = out.substr(nl + 1);
    } else {
      *password = out;
    }
    return !password->empty();
  }

  std::string out = RunAndCapture(cmd.c_str());
  if (out.empty()) return false;

  size_t nl = out.find('\n');
  if (nl != std::string::npos) {
    *username = out.substr(0, nl);
    *password = out.substr(nl + 1);
  } else {
    *username = out;
  }
  return !username->empty();
}

static bool ShowLoginDialog_Kdialog(std::string *username, std::string *password) {
  // kdialog doesn't have a forms widget, so use two sequential prompts
  std::string ucmd = "kdialog --title 'Sign in to Spotify'"
                     " --inputbox 'Spotify username:' '" + *username + "' 2>/dev/null";
  std::string user = RunAndCapture(ucmd.c_str());
  if (user.empty()) return false;

  std::string pcmd = "kdialog --title 'Sign in to Spotify'"
                     " --password 'Password:' 2>/dev/null";
  std::string pass = RunAndCapture(pcmd.c_str());
  if (pass.empty()) return false;

  *username = user;
  *password = pass;
  return true;
}

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

bool ShowLoginDialog(PlatformWindow *parent, std::string *username, std::string *password) {
#if defined(__APPLE__)
  std::string user = ShowMacPrompt("Enter Spotify Username:", *username);
  if (user.empty()) return false;
  std::string pass = ShowMacPrompt("Enter Spotify Password:", *password, true);
  if (pass.empty()) return false;
  *username = user;
  *password = pass;
  return true;

#elif defined(__linux__)
  if (CommandExists("zenity"))
    return ShowLoginDialog_Zenity(username, password);
  if (CommandExists("kdialog"))
    return ShowLoginDialog_Kdialog(username, password);

  // Last resort: terminal prompt
  std::cout << "\n=== Sign in to Spotify ===" << std::endl;
  std::cout << "Username: ";
  if (!(std::cin >> *username)) return false;
  std::cout << "Password: ";
  if (!(std::cin >> *password)) return false;
  return true;

#else
  std::cout << "\n=== Sign in to Spotify ===" << std::endl;
  std::cout << "Username: ";
  if (!(std::cin >> *username)) return false;
  std::cout << "Password: ";
  if (!(std::cin >> *password)) return false;
  return true;
#endif
}

std::string ShowSearchDialog(PlatformWindow *parent, Tsp *tsp) {
#if defined(__APPLE__)
  return ShowMacPrompt("Search query:");

#elif defined(__linux__)
  if (CommandExists("zenity"))
    return ShowSearchDialog_Zenity("");
  if (CommandExists("kdialog"))
    return ShowSearchDialog_Kdialog("");

  std::cout << "\n=== Spotiamp Search ===" << std::endl;
  std::cout << "Enter search query: ";
  std::string q;
  std::cin >> q;
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
#if defined(__APPLE__)
  return ShowMacPrompt(message ? message : "", default_value ? default_value : "");

#elif defined(__linux__)
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
