// port/gdx_macos_ui.mm — macOS-native UI helpers: NSOpenPanel file picker and NSAlert dialogs.
// Objective-C++ (.mm) so we can call the AppKit classes directly from the port's C++ code.
// These mirror the Win32 GetOpenFileNameW / MessageBoxW calls in rom_buffer.cpp / gdx_firstboot.cpp.

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "gdx_macos_ui.h"

// Runs an NSOpenPanel modal file picker filtered to N64 ROM extensions.
// Returns the selected path as a UTF-8 C string (caller must free()), or nullptr if cancelled.
char* gdx_macos_pick_rom(void) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        [panel setTitle:@"Select F-Zero X ROM"];
        [panel setMessage:@"Choose a big-endian .z64 ROM image (US or PAL)"];
        [panel setAllowsMultipleSelection:NO];
        [panel setCanChooseDirectories:NO];
        [panel setCanChooseFiles:YES];

        // UTType-based filtering (macOS 11+). The ROM extensions are not system-known UTTypes,
        // so we create them dynamically from the file extensions.
        if (@available(macOS 11.0, *)) {
            NSArray<UTType*>* types = @[
                [UTType typeWithFilenameExtension:@"z64"],
                [UTType typeWithFilenameExtension:@"n64"],
                [UTType typeWithFilenameExtension:@"v64"],
            ];
            [panel setAllowedContentTypes:types];
        } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [panel setAllowedFileTypes:@[@"z64", @"n64", @"v64"]];
#pragma clang diagnostic pop
        }

        if ([panel runModal] != NSModalResponseOK) {
            return nullptr;
        }

        NSURL* url = [[panel URLs] firstObject];
        if (url == nil) {
            return nullptr;
        }

        const char* path = [[url path] UTF8String];
        if (path == nullptr) {
            return nullptr;
        }
        return strdup(path);
    }
}

// Shows a modal alert dialog (informational or error).
void gdx_macos_alert(const char* title, const char* message, int isError) {
    @autoreleasepool {
        NSAlert* alert = [[NSAlert alloc] init];
        [alert setMessageText:[NSString stringWithUTF8String:title]];
        [alert setInformativeText:[NSString stringWithUTF8String:message]];
        [alert setAlertStyle:(isError ? NSAlertStyleCritical : NSAlertStyleInformational)];
        [alert addButtonWithTitle:@"OK"];

        // Ensure we have an NSApplication instance for the modal run loop.
        if ([NSApp mainMenu] == nil) {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        }

        [alert runModal];
    }
}

#endif // __APPLE__
