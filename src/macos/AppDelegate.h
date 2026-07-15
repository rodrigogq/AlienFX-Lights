// AppDelegate.h — macOS menu bar app (Objective-C++), talking to the shared
// C++ AFXController directly. No Swift, no Obj-C bridge layer.
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#pragma once

#import <AppKit/AppKit.h>

@interface AppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate>
@end
