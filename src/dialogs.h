#include <string>
#include "window.h"
#include "spotifyamp.h"

std::string ShowSearchDialog(PlatformWindow *parent, Tsp *tsp);
std::string ShowTextInputDialog(PlatformWindow *parent, const char *title,
                                const char *message, const char *default_value);

void AutoCompleteCopy();
