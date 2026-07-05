#include <string>
#include <vector>
#import <Cocoa/Cocoa.h>

class PlatformWindow;
struct Tsp;

bool ShowLoginDialog(PlatformWindow *parent, std::string *username, std::string *password) {
  @autoreleasepool {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Sign in to Spotify"];
    [alert setInformativeText:@"Enter your Spotify username and password."];
    [alert addButtonWithTitle:@"Sign In"];
    [alert addButtonWithTitle:@"Cancel"];

    // Container view
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 72)];

    // Labels
    NSTextField *userLabel = [NSTextField labelWithString:@"Username:"];
    NSTextField *passLabel = [NSTextField labelWithString:@"Password:"];
    [userLabel setAlignment:NSTextAlignmentRight];
    [passLabel setAlignment:NSTextAlignmentRight];
    [userLabel setFrame:NSMakeRect(0, 44, 80, 20)];
    [passLabel setFrame:NSMakeRect(0, 12, 80, 20)];

    // Input fields
    NSTextField *userField = [[NSTextField alloc] initWithFrame:NSMakeRect(88, 42, 232, 24)];
    [[userField cell] setPlaceholderString:@"Spotify username"];
    [userField setStringValue:[NSString stringWithUTF8String:username->c_str()]];

    NSSecureTextField *passField = [[NSSecureTextField alloc] initWithFrame:NSMakeRect(88, 10, 232, 24)];
    [[passField cell] setPlaceholderString:@"Password"];
    [passField setStringValue:[NSString stringWithUTF8String:password->c_str()]];

    [view addSubview:userLabel];
    [view addSubview:passLabel];
    [view addSubview:userField];
    [view addSubview:passField];

    [alert setAccessoryView:view];

    // Focus the username field (or password if username already filled)
    if (username->empty())
      [alert.window makeFirstResponder:userField];
    else
      [alert.window makeFirstResponder:passField];

    [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];

    NSInteger button = [alert runModal];
    if (button == NSAlertFirstButtonReturn) {
      *username = [[userField stringValue] UTF8String];
      *password = [[passField stringValue] UTF8String];
      return true;
    }
    return false;
  }
}

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
