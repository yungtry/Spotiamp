#include <string>
#include <vector>
#import <Cocoa/Cocoa.h>

class PlatformWindow;
struct Tsp;

std::string ShowSearchDialog(PlatformWindow *parent, Tsp *tsp) {
  @autoreleasepool {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Spotiamp Search"];
    [alert addButtonWithTitle:@"Search"];
    [alert addButtonWithTitle:@"Cancel"];
    
    NSTextField *searchField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 22)];
    [[searchField cell] setPlaceholderString:@"Enter search query..."];
    
    [alert setAccessoryView:searchField];
    
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
    
    NSInteger button = [alert runModal];
    if (button == NSAlertFirstButtonReturn) {
      return [[searchField stringValue] UTF8String];
    }
    return "";
  }
}

std::string ShowTextInputDialog(PlatformWindow *parent, const char *title,
                                const char *message, const char *default_value) {
  @autoreleasepool {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:[NSString stringWithUTF8String:title ? title : "Spotiamp"]];
    if (message && message[0])
      [alert setInformativeText:[NSString stringWithUTF8String:message]];
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];

    NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 420, 22)];
    [[field cell] setPlaceholderString:@"Spotify playlist link"];
    if (default_value)
      [field setStringValue:[NSString stringWithUTF8String:default_value]];

    [alert setAccessoryView:field];

    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];

    NSInteger button = [alert runModal];
    if (button == NSAlertFirstButtonReturn)
      return [[field stringValue] UTF8String];
    return "";
  }
}

void AutoCompleteCopy() {
}

int ShowMacListPrompt(const std::vector<std::string> &items) {
  @autoreleasepool {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Spotiamp Menu"];
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    
    NSComboBox *comboBox = [[NSComboBox alloc] initWithFrame:NSMakeRect(0, 0, 300, 26)];
    [comboBox setNumberOfVisibleItems:15];
    [comboBox setCompletes:YES];
    
    for (const auto &item : items) {
      [comboBox addItemWithObjectValue:[NSString stringWithUTF8String:item.c_str()]];
    }
    
    [comboBox selectItemAtIndex:0];
    [alert setAccessoryView:comboBox];
    
    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
    
    NSInteger button = [alert runModal];
    if (button == NSAlertFirstButtonReturn) {
      return (int)[comboBox indexOfSelectedItem] + 1;
    }
    return 0;
  }
}
