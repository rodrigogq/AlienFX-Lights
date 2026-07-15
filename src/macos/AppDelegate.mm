// AppDelegate.mm — macOS menu bar UI (Objective-C++).
//
// Same operating model as the Windows tray app: Go Light/Dim/Dark on top,
// one Color and Effect for all devices, an Input Devices mode for
// keyboards/mice, and per-device overrides. Starts passive (never touches
// the hardware until the user picks something), then owns and reapplies the
// state on wake / hotplug / display power events.
//
// Talks to the shared C++ AFXController directly (this is Objective-C++).
//
// This file is part of AlienFX-Lights and is distributed under the GNU GPLv3.

#import "AppDelegate.h"

#import <ServiceManagement/ServiceManagement.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDLib.h>

#include <vector>
#include <string>

#include "AFXController.h"

using namespace AlienFX_SDK;

namespace {

const uint8_t kDimLevel = 102; // Go Dim (~40%)

struct SysModeItem { const char* raw; NSString* title; };
struct InputModeItem { const char* raw; NSString* title; };
struct Preset { NSString* name; uint8_t r, g, b; };

NSString* const kSystemModeKey = @"systemMode";
NSString* const kColorHexKey = @"colorHex";
NSString* const kEffectKey = @"effect";
NSString* const kInputModeKey = @"inputDevicesMode";
NSString* const kDeviceModesKey = @"deviceModes";
NSString* const kTurnOffKey = @"turnOffWithDisplay";

// Exact palette shared with the Windows app; Alienware Cyan is the default.
const Preset kPresets[] = {
    { @"Alienware Cyan", 0, 255, 255 },
    { @"Alien Green",    0, 255, 50 },
    { @"White",          255, 255, 255 },
    { @"Red",            255, 0, 0 },
    { @"Orange",         255, 100, 0 },
    { @"Yellow",         255, 220, 0 },
    { @"Blue",           0, 60, 255 },
    { @"Purple",         150, 0, 255 },
    { @"Pink",           255, 0, 130 },
};

NSArray<NSString*>* const kEffectTitles = @[ @"Static Color", @"Pulse", @"Morph", @"Breathing", @"Spectrum" ];

NSString* DeviceKey(uint32_t deviceID) {
    return [NSString stringWithFormat:@"%04x:%04x", (deviceID >> 16) & 0xffff, deviceID & 0xffff];
}

} // namespace

@implementation AppDelegate {
    NSStatusItem* _statusItem;
    AFXController _controller;
    std::vector<AFXDeviceEntry> _devices;
    IONotificationPortRef _notifyPort;
    io_iterator_t _matchedIter;
    io_iterator_t _terminatedIter;
    BOOL _rescanPending;
    BOOL _active; // becomes YES after the first user action
}

#pragma mark Lifecycle

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    _statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
    NSImage* icon = [self statusImage];
    if (icon) {
        [icon setTemplate:YES]; // 'template' is a C++ keyword; use the setter in .mm
        _statusItem.button.image = icon;
    } else {
        _statusItem.button.title = @"AFX";
    }

    NSMenu* menu = [[NSMenu alloc] init];
    menu.delegate = self; // rebuilt in menuNeedsUpdate:
    _statusItem.menu = menu;

    [self startHotplugMonitoring];

    NSNotificationCenter* wc = [[NSWorkspace sharedWorkspace] notificationCenter];
    [wc addObserver:self selector:@selector(displaysDidSleep)
               name:NSWorkspaceScreensDidSleepNotification object:nil];
    [wc addObserver:self selector:@selector(systemDidWake)
               name:NSWorkspaceScreensDidWakeNotification object:nil];
    [wc addObserver:self selector:@selector(systemDidWake)
               name:NSWorkspaceDidWakeNotification object:nil];

    [self refreshDevices:NO]; // passive: enumerate only, don't touch the hardware
}

- (NSImage*)statusImage {
    NSString* path = [[NSBundle mainBundle] pathForResource:@"menuicon" ofType:@"png"];
    if (path) {
        NSImage* img = [[NSImage alloc] initWithContentsOfFile:path];
        [img setSize:NSMakeSize(18, 18)];
        return img;
    }
    if (@available(macOS 11.0, *))
        return [NSImage imageWithSystemSymbolName:@"lightbulb.fill" accessibilityDescription:@"AlienFX Lights"];
    return nil;
}

#pragma mark Settings (NSUserDefaults)

- (NSUserDefaults*)defaults { return [NSUserDefaults standardUserDefaults]; }

- (int)systemMode { // 0 Light, 1 Dim, 2 Dark
    NSString* raw = [[self defaults] stringForKey:kSystemModeKey];
    if ([raw isEqualToString:@"goDim"]) return 1;
    if ([raw isEqualToString:@"goDark"]) return 2;
    return 0;
}
- (void)setSystemMode:(int)mode {
    NSString* raw = mode == 1 ? @"goDim" : mode == 2 ? @"goDark" : @"goLight";
    [[self defaults] setObject:raw forKey:kSystemModeKey];
}

- (uint32_t)colorHex {
    id v = [[self defaults] objectForKey:kColorHexKey];
    return v ? (uint32_t)([v integerValue] & 0xffffff) : 0x00FFFF;
}
- (void)setColorHex:(uint32_t)hex { [[self defaults] setInteger:(NSInteger)(hex & 0xffffff) forKey:kColorHexKey]; }

- (int)effect {
    NSInteger e = [[self defaults] integerForKey:kEffectKey];
    return (e >= 0 && e <= 4) ? (int)e : 0;
}
- (void)setEffect:(int)e { [[self defaults] setInteger:e forKey:kEffectKey]; }

// Device mode strings: "alwaysOn" / "onKeyPress" / "off"; nil = default/follow.
- (NSString*)inputDevicesMode {
    NSString* raw = [[self defaults] stringForKey:kInputModeKey];
    return raw ?: @"alwaysOn";
}
- (void)setInputDevicesMode:(NSString*)raw { [[self defaults] setObject:raw forKey:kInputModeKey]; }

- (NSString*)deviceMode:(uint32_t)deviceID { // nil = default
    NSDictionary* d = [[self defaults] dictionaryForKey:kDeviceModesKey];
    return d[DeviceKey(deviceID)];
}
- (void)setDeviceMode:(NSString*)mode forDevice:(uint32_t)deviceID {
    NSMutableDictionary* d = [([[self defaults] dictionaryForKey:kDeviceModesKey] ?: @{}) mutableCopy];
    if (mode) d[DeviceKey(deviceID)] = mode; else [d removeObjectForKey:DeviceKey(deviceID)];
    [[self defaults] setObject:d forKey:kDeviceModesKey];
}

- (BOOL)turnOffWithDisplay {
    id v = [[self defaults] objectForKey:kTurnOffKey];
    return v ? [v boolValue] : YES;
}
- (void)setTurnOffWithDisplay:(BOOL)v { [[self defaults] setBool:v forKey:kTurnOffKey]; }

#pragma mark Menu

- (void)menuNeedsUpdate:(NSMenu*)menu {
    [self refreshDevices:NO]; // AWCC may have been used in parallel; enumerate only
    [menu removeAllItems];

    // 1. System mode
    NSArray* sysTitles = @[ @"Go Light", @"Go Dim", @"Go Dark" ];
    int sys = [self systemMode];
    for (int i = 0; i < 3; i++) {
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:sysTitles[i] action:@selector(pickSystemMode:) keyEquivalent:@""];
        it.target = self; it.representedObject = @(i); it.state = (sys == i) ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:it];
    }
    [menu addItem:[NSMenuItem separatorItem]];

    // 2. Color
    NSMenu* colorMenu = [[NSMenu alloc] init];
    uint32_t hex = [self colorHex];
    for (const Preset& p : kPresets) {
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:p.name action:@selector(pickPreset:) keyEquivalent:@""];
        it.target = self; it.representedObject = @[ @(p.r), @(p.g), @(p.b) ];
        it.image = [self swatchR:p.r g:p.g b:p.b];
        uint32_t ph = ((uint32_t)p.r << 16) | ((uint32_t)p.g << 8) | p.b;
        it.state = (ph == hex) ? NSControlStateValueOn : NSControlStateValueOff;
        [colorMenu addItem:it];
    }
    [colorMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* custom = [[NSMenuItem alloc] initWithTitle:@"Custom…" action:@selector(pickCustomColor:) keyEquivalent:@""];
    custom.target = self; [colorMenu addItem:custom];
    NSMenuItem* colorRoot = [[NSMenuItem alloc] initWithTitle:@"Color" action:nil keyEquivalent:@""];
    colorRoot.submenu = colorMenu; [menu addItem:colorRoot];

    // 2b. Effect
    NSMenu* effectMenu = [[NSMenu alloc] init];
    int eff = [self effect];
    for (int i = 0; i < (int)kEffectTitles.count; i++) {
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:kEffectTitles[i] action:@selector(pickEffect:) keyEquivalent:@""];
        it.target = self; it.representedObject = @(i); it.state = (eff == i) ? NSControlStateValueOn : NSControlStateValueOff;
        [effectMenu addItem:it];
    }
    NSMenuItem* effectRoot = [[NSMenuItem alloc] initWithTitle:@"Effect" action:nil keyEquivalent:@""];
    effectRoot.submenu = effectMenu; [menu addItem:effectRoot];

    [menu addItem:[NSMenuItem separatorItem]];

    // 3. Input Devices mode
    NSMenu* inputMenu = [[NSMenu alloc] init];
    NSArray* inTitles = @[ @"Always On", @"On While Typing", @"Always Off" ];
    NSArray* inRaws = @[ @"alwaysOn", @"onKeyPress", @"off" ];
    NSString* inMode = [self inputDevicesMode];
    for (int i = 0; i < 3; i++) {
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:inTitles[i] action:@selector(pickInputMode:) keyEquivalent:@""];
        it.target = self; it.representedObject = inRaws[i];
        it.state = [inMode isEqualToString:inRaws[i]] ? NSControlStateValueOn : NSControlStateValueOff;
        [inputMenu addItem:it];
    }
    NSMenuItem* inputRoot = [[NSMenuItem alloc] initWithTitle:@"Input Devices" action:nil keyEquivalent:@""];
    inputRoot.submenu = inputMenu; [menu addItem:inputRoot];

    [menu addItem:[NSMenuItem separatorItem]];

    // 4. Per-device overrides
    if (_devices.empty()) {
        NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:@"No Alienware devices found" action:nil keyEquivalent:@""];
        it.enabled = NO; [menu addItem:it];
    } else {
        for (const AFXDeviceEntry& dev : _devices) {
            NSMenu* devMenu = [[NSMenu alloc] init];
            NSString* current = [self deviceMode:dev.deviceID]; // nil = default
            NSMutableArray* raws = [@[ @"default", @"alwaysOn" ] mutableCopy];
            NSMutableArray* titles = [@[ @"Default", @"Always On" ] mutableCopy];
            if (dev.supportsKeyPress) { [raws addObject:@"onKeyPress"]; [titles addObject:@"On While Typing"]; }
            [raws addObject:@"off"]; [titles addObject:@"Off"];
            for (int i = 0; i < (int)raws.count; i++) {
                NSMenuItem* it = [[NSMenuItem alloc] initWithTitle:titles[i] action:@selector(pickDeviceMode:) keyEquivalent:@""];
                it.target = self;
                it.representedObject = @{ @"id": @(dev.deviceID), @"mode": raws[i] };
                BOOL on = current ? [current isEqualToString:raws[i]] : [raws[i] isEqualToString:@"default"];
                it.state = on ? NSControlStateValueOn : NSControlStateValueOff;
                [devMenu addItem:it];
            }
            NSString* name = [NSString stringWithUTF8String:dev.name.c_str()];
            NSMenuItem* devRoot = [[NSMenuItem alloc] initWithTitle:name action:nil keyEquivalent:@""];
            devRoot.submenu = devMenu; [menu addItem:devRoot];
        }
    }

    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem* off = [[NSMenuItem alloc] initWithTitle:@"Turn Off With Display" action:@selector(toggleOffWithDisplay:) keyEquivalent:@""];
    off.target = self; off.state = [self turnOffWithDisplay] ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:off];
    NSMenuItem* refresh = [[NSMenuItem alloc] initWithTitle:@"Refresh Devices" action:@selector(refreshAction:) keyEquivalent:@"r"];
    refresh.target = self; [menu addItem:refresh];
    if ([NSBundle mainBundle].bundleIdentifier) {
        NSMenuItem* login = [[NSMenuItem alloc] initWithTitle:@"Start at Login" action:@selector(toggleLoginItem:) keyEquivalent:@""];
        login.target = self;
        if (@available(macOS 13.0, *))
            login.state = (SMAppService.mainAppService.status == SMAppServiceStatusEnabled) ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:login];
    }
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:@"Quit AlienFX Lights" action:@selector(terminate:) keyEquivalent:@"q"];
    quit.target = NSApp; [menu addItem:quit];
}

- (NSImage*)swatchR:(uint8_t)r g:(uint8_t)g b:(uint8_t)b {
    NSSize size = NSMakeSize(14, 14);
    return [NSImage imageWithSize:size flipped:NO drawingHandler:^BOOL(NSRect rect) {
        NSColor* c = [NSColor colorWithSRGBRed:r/255.0 green:g/255.0 blue:b/255.0 alpha:1];
        [c setFill];
        NSBezierPath* path = [NSBezierPath bezierPathWithOvalInRect:NSInsetRect(rect, 1, 1)];
        [path fill];
        [[NSColor separatorColor] setStroke];
        path.lineWidth = 1; [path stroke];
        return YES;
    }];
}

#pragma mark Actions

- (void)pickSystemMode:(NSMenuItem*)sender {
    _active = YES;
    [self setSystemMode:(int)[sender.representedObject integerValue]];
    [self applyStateToAll];
}
- (void)pickInputMode:(NSMenuItem*)sender {
    _active = YES;
    [self setInputDevicesMode:sender.representedObject];
    [self applyStateToAll];
}
- (void)pickEffect:(NSMenuItem*)sender {
    _active = YES;
    [self setEffect:(int)[sender.representedObject integerValue]];
    if ([self systemMode] == 2) [self setSystemMode:0]; // picking an effect turns lights on
    [self applyStateToAll];
}
- (void)pickPreset:(NSMenuItem*)sender {
    NSArray* rgb = sender.representedObject;
    [self setColorR:[rgb[0] intValue] g:[rgb[1] intValue] b:[rgb[2] intValue]];
}
- (void)pickCustomColor:(NSMenuItem*)sender {
    NSColorPanel* panel = [NSColorPanel sharedColorPanel];
    panel.showsAlpha = NO;
    [panel setTarget:self];
    [panel setAction:@selector(customColorChanged:)];
    uint32_t hex = [self colorHex];
    panel.color = [NSColor colorWithSRGBRed:((hex>>16)&0xff)/255.0 green:((hex>>8)&0xff)/255.0 blue:(hex&0xff)/255.0 alpha:1];
    [NSApp activateIgnoringOtherApps:YES];
    [panel makeKeyAndOrderFront:nil];
}
- (void)customColorChanged:(NSColorPanel*)panel {
    NSColor* c = [panel.color colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (c) [self setColorR:(uint8_t)(c.redComponent*255) g:(uint8_t)(c.greenComponent*255) b:(uint8_t)(c.blueComponent*255)];
}
- (void)pickDeviceMode:(NSMenuItem*)sender {
    _active = YES;
    NSDictionary* info = sender.representedObject;
    uint32_t deviceID = (uint32_t)[info[@"id"] unsignedIntegerValue];
    NSString* mode = info[@"mode"];
    [self setDeviceMode:([mode isEqualToString:@"default"] ? nil : mode) forDevice:deviceID];
    [self applyStateToDeviceID:deviceID];
}
- (void)toggleOffWithDisplay:(NSMenuItem*)sender { [self setTurnOffWithDisplay:![self turnOffWithDisplay]]; }
- (void)refreshAction:(NSMenuItem*)sender { [self refreshDevices:_active]; }
- (void)toggleLoginItem:(NSMenuItem*)sender {
    if (@available(macOS 13.0, *)) {
        NSError* err = nil;
        if (SMAppService.mainAppService.status == SMAppServiceStatusEnabled)
            [SMAppService.mainAppService unregisterAndReturnError:&err];
        else
            [SMAppService.mainAppService registerAndReturnError:&err];
        if (err) NSLog(@"Login item change failed: %@", err);
    }
}

- (void)setColorR:(uint8_t)r g:(uint8_t)g b:(uint8_t)b {
    _active = YES;
    [self setColorHex:((uint32_t)r << 16) | ((uint32_t)g << 8) | b];
    if ([self systemMode] == 2) [self setSystemMode:0]; // picking a color turns lights on
    [self applyStateToAll];
}

#pragma mark Device state

- (void)refreshDevices:(BOOL)applyState {
    _devices = _controller.Rescan();
    if (applyState) [self applyStateToAll];
}

// Resolved mode: per-device override, else Input Devices mode for
// keyboards/mice, else Always On.
- (NSString*)resolvedModeFor:(const AFXDeviceEntry&)dev {
    NSString* m = [self deviceMode:dev.deviceID];
    if (m) return m;
    if (AFXIsInputDevice(dev.apiVersion)) return [self inputDevicesMode];
    return @"alwaysOn";
}

- (void)applyStateTo:(const AFXDeviceEntry&)dev {
    int sys = [self systemMode];
    NSString* dm = [self resolvedModeFor:dev];
    AFXMode mode; uint8_t level = 255;
    if (sys == 2 || [dm isEqualToString:@"off"]) {
        mode = AFXMode::Off;
    } else {
        mode = [dm isEqualToString:@"onKeyPress"] ? AFXMode::OnKeyPress : AFXMode::AlwaysOn;
        if (sys == 1) level = kDimLevel;
    }
    uint32_t hex = [self colorHex];
    _controller.ApplyMode(dev.deviceID, mode, (hex>>16)&0xff, (hex>>8)&0xff, hex&0xff,
                          level, (AFXEffect)[self effect]);
}

- (void)applyStateToDeviceID:(uint32_t)deviceID {
    for (const AFXDeviceEntry& d : _devices)
        if (d.deviceID == deviceID) { [self applyStateTo:d]; return; }
}

- (void)applyStateToAll {
    for (const AFXDeviceEntry& d : _devices) [self applyStateTo:d];
}

- (void)applyOffToAll {
    for (const AFXDeviceEntry& d : _devices)
        _controller.ApplyMode(d.deviceID, AFXMode::Off, 0, 0, 0);
}

#pragma mark Sleep / wake

- (void)displaysDidSleep {
    if (_active && [self turnOffWithDisplay]) [self applyOffToAll];
}
- (void)systemDidWake {
    if (!_active) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{ [self refreshDevices:YES]; });
}

#pragma mark Hotplug (IOKit)

static void AFXDrain(io_iterator_t iter) { io_object_t o; while ((o = IOIteratorNext(iter))) IOObjectRelease(o); }

static void AFXChanged(void* refcon, io_iterator_t iter) {
    AFXDrain(iter);
    AppDelegate* self = (__bridge AppDelegate*)refcon;
    [self scheduleRescan];
}

- (void)scheduleRescan {
    if (_rescanPending) return;
    _rescanPending = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.0 * NSEC_PER_SEC)),
                   dispatch_get_main_queue(), ^{
        self->_rescanPending = NO;
        [self refreshDevices:self->_active];
    });
}

- (void)startHotplugMonitoring {
    _notifyPort = IONotificationPortCreate(kIOMainPortDefault);
    if (!_notifyPort) return;
    IONotificationPortSetDispatchQueue(_notifyPort, dispatch_get_main_queue());
    IOServiceAddMatchingNotification(_notifyPort, kIOFirstMatchNotification,
        IOServiceMatching(kIOHIDDeviceKey), AFXChanged, (__bridge void*)self, &_matchedIter);
    if (_matchedIter) AFXDrain(_matchedIter);
    IOServiceAddMatchingNotification(_notifyPort, kIOTerminatedNotification,
        IOServiceMatching(kIOHIDDeviceKey), AFXChanged, (__bridge void*)self, &_terminatedIter);
    if (_terminatedIter) AFXDrain(_terminatedIter);
}

@end
