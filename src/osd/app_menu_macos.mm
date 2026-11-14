// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#include "osd/app_menu.h"

#include "osd/i18n.h"

#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>

namespace {

Uint32 g_settings_event = 0;

}  // namespace

/// The target of the menu item: pushes an SDL event, so the main loop
/// opens the settings in its own time rather than from inside Cocoa's
/// dispatch.
@interface KeishoMenuTarget : NSObject
- (void)openSettings:(id)sender;
@end

@implementation KeishoMenuTarget
- (void)openSettings:(id)sender
{
    (void)sender;
    if (g_settings_event == 0) return;
    SDL_Event event;
    SDL_zero(event);
    event.type = g_settings_event;
    SDL_PushEvent(&event);
}
@end

namespace sm2::osd {

u32 install_app_menu()
{
    static KeishoMenuTarget* target = nil;
    if (g_settings_event != 0) return g_settings_event;
    g_settings_event = SDL_RegisterEvents(1);
    if (g_settings_event == 0) return 0;

    NSMenu* main = [NSApp mainMenu];
    if (main == nil || [main numberOfItems] == 0) return g_settings_event;
    NSMenu* app = [[main itemAtIndex:0] submenu];
    if (app == nil) return g_settings_event;

    target = [[KeishoMenuTarget alloc] init];
    // SDL already lays out the app menu with a disabled "Settings..." (Cmd+,)
    // item for the app to claim: take it over. Failing that, add one after
    // "About", as the system's own apps place it.
    for (NSMenuItem* existing in [app itemArray]) {
        if ([[existing keyEquivalent] isEqualToString:@","]) {
            [existing setTarget:target];
            [existing setAction:@selector(openSettings:)];
            [existing setEnabled:YES];
            return g_settings_event;
        }
    }
    NSString*   title = [NSString stringWithUTF8String:i18n::tr("Settings...")];
    NSMenuItem* item  = [[NSMenuItem alloc] initWithTitle:title action:@selector(openSettings:) keyEquivalent:@","];
    [item setTarget:target];
    const NSInteger at = [app numberOfItems] > 1 ? 1 : [app numberOfItems];
    [app insertItem:[NSMenuItem separatorItem] atIndex:at];
    [app insertItem:item atIndex:at + 1];
    return g_settings_event;
}

}  // namespace sm2::osd
