#import <AppKit/AppKit.h>

#include <math.h>
#include "SDL.h"
#include "macos_ui.h"

static NSString *const Disk1Key = @"HardDisk1Path";
static NSString *const Disk2Key = @"HardDisk2Path";
static NSString *const DisplayScaleKey = @"DisplayScale";

static SDL_Window *freebeeWindow;
static macos_paste_callback freebeePasteCallback;

static void set_display_scale(double scale)
{
	if (freebeeWindow == NULL || scale <= 0.0)
		return;

	SDL_SetWindowFullscreen(freebeeWindow, 0);
	SDL_SetWindowSize(freebeeWindow, (int)lround(720.0 * scale),
									 (int)lround(348.0 * scale));
	SDL_SetWindowPosition(freebeeWindow, SDL_WINDOWPOS_CENTERED,
									 SDL_WINDOWPOS_CENTERED);
	[[NSUserDefaults standardUserDefaults] setDouble:scale forKey:DisplayScaleKey];
}

@interface FreeBeeMenuController : NSObject
@end

@implementation FreeBeeMenuController

- (void)showSettings:(id)sender
{
	(void)sender;
	NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
	NSString *disk1 = [defaults stringForKey:Disk1Key];
	NSString *disk2 = [defaults stringForKey:Disk2Key];
	if (disk1 == nil)
		disk1 = @"Bundled/default disk";
	if (disk2 == nil)
		disk2 = @"Not selected";
	NSAlert *alert = [[NSAlert alloc] init];
	alert.messageText = @"FreeBee Settings";
	alert.informativeText = [NSString stringWithFormat:
		@"Boot disk: %@\nSecond disk: %@\n\nUse the Machine and View menus to change these settings.",
		disk1, disk2];
	[alert addButtonWithTitle:@"OK"];
	[alert runModal];
}

- (void)selectDisk:(NSMenuItem *)sender
{
	NSInteger drive = sender.tag;
	NSOpenPanel *panel = [NSOpenPanel openPanel];
	panel.title = drive == 0 ? @"Select FreeBee Boot Disk" : @"Select FreeBee Second Disk";
	panel.canChooseDirectories = NO;
	panel.canChooseFiles = YES;
	panel.allowsMultipleSelection = NO;
	if ([panel runModal] != NSModalResponseOK)
		return;
	if (![[NSFileManager defaultManager] isWritableFileAtPath:panel.URL.path]) {
		NSAlert *error = [[NSAlert alloc] init];
		error.messageText = @"The disk image is not writable";
		error.informativeText = @"FreeBee needs read/write access to a hard-disk image.";
		[error addButtonWithTitle:@"OK"];
		[error runModal];
		return;
	}

	NSString *key = drive == 0 ? Disk1Key : Disk2Key;
	[[NSUserDefaults standardUserDefaults] setObject:panel.URL.path forKey:key];

	NSAlert *alert = [[NSAlert alloc] init];
	alert.messageText = @"Disk selection saved";
	alert.informativeText = @"Quit and reopen FreeBee to use the selected disk. The running root disk was not changed.";
	[alert addButtonWithTitle:@"OK"];
	[alert runModal];
}

- (void)useBundledDisk:(id)sender
{
	(void)sender;
	[[NSUserDefaults standardUserDefaults] removeObjectForKey:Disk1Key];
	NSAlert *alert = [[NSAlert alloc] init];
	alert.messageText = @"Default disk selected";
	alert.informativeText = @"Quit and reopen FreeBee to use the default disk in Application Support.";
	[alert addButtonWithTitle:@"OK"];
	[alert runModal];
}

- (void)paste:(id)sender
{
	(void)sender;
	NSString *text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
	if (text != nil && freebeePasteCallback != NULL)
		freebeePasteCallback(text.UTF8String);
}

- (void)setScale:(NSMenuItem *)sender
{
	set_display_scale((double)sender.tag / 100.0);
}

- (void)fitToScreen:(id)sender
{
	(void)sender;
	NSRect frame = NSScreen.mainScreen.visibleFrame;
	double scale = fmin(frame.size.width / 720.0, frame.size.height / 348.0);
	set_display_scale(floor(scale * 4.0) / 4.0);
}

- (void)toggleFullScreen:(id)sender
{
	(void)sender;
	if (freebeeWindow == NULL)
		return;
	Uint32 flags = SDL_GetWindowFlags(freebeeWindow);
	SDL_SetWindowFullscreen(freebeeWindow,
		(flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

- (void)quit:(id)sender
{
	(void)sender;
	SDL_Event event;
	SDL_zero(event);
	event.type = SDL_QUIT;
	SDL_PushEvent(&event);
}

@end

static NSMenuItem *menu_item(NSString *title, SEL action, NSString *key,
								 FreeBeeMenuController *controller)
{
	NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
	item.target = controller;
	return item;
}

double macos_ui_saved_scale(void)
{
	NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
	if ([defaults objectForKey:DisplayScaleKey] == nil)
		return 0.0;
	return [defaults doubleForKey:DisplayScaleKey];
}

const char *macos_ui_disk_path(int drive)
{
	NSString *key = drive == 0 ? Disk1Key : Disk2Key;
	NSString *path = [[NSUserDefaults standardUserDefaults] stringForKey:key];
	return path.length == 0 ? NULL : path.fileSystemRepresentation;
}

void macos_ui_init(SDL_Window *window, macos_paste_callback paste_callback)
{
	static FreeBeeMenuController *controller;
	freebeeWindow = window;
	freebeePasteCallback = paste_callback;
	controller = [[FreeBeeMenuController alloc] init];

	NSMenu *menuBar = [[NSMenu alloc] initWithTitle:@""];
	[NSApp setMainMenu:menuBar];

	NSMenuItem *appRoot = [[NSMenuItem alloc] initWithTitle:@"FreeBee" action:nil keyEquivalent:@""];
	NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"FreeBee"];
	[appMenu addItem:menu_item(@"Settings…", @selector(showSettings:), @",", controller)];
	[appMenu addItem:[NSMenuItem separatorItem]];
	[appMenu addItem:menu_item(@"Quit FreeBee", @selector(quit:), @"q", controller)];
	appRoot.submenu = appMenu;
	[menuBar addItem:appRoot];

	NSMenuItem *editRoot = [[NSMenuItem alloc] initWithTitle:@"Edit" action:nil keyEquivalent:@""];
	NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
	[editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"]];
	[editMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"]];
	[editMenu addItem:menu_item(@"Paste", @selector(paste:), @"v", controller)];
	editRoot.submenu = editMenu;
	[menuBar addItem:editRoot];

	NSMenuItem *machineRoot = [[NSMenuItem alloc] initWithTitle:@"Machine" action:nil keyEquivalent:@""];
	NSMenu *machineMenu = [[NSMenu alloc] initWithTitle:@"Machine"];
	NSMenuItem *disk1 = menu_item(@"Select Boot Disk…", @selector(selectDisk:), @"", controller);
	disk1.tag = 0;
	[machineMenu addItem:disk1];
	NSMenuItem *disk2 = menu_item(@"Select Second Disk…", @selector(selectDisk:), @"", controller);
	disk2.tag = 1;
	[machineMenu addItem:disk2];
	[machineMenu addItem:menu_item(@"Use Bundled Boot Disk", @selector(useBundledDisk:), @"", controller)];
	machineRoot.submenu = machineMenu;
	[menuBar addItem:machineRoot];

	NSMenuItem *viewRoot = [[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
	NSMenu *viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
	struct { __unsafe_unretained NSString *title; NSInteger scale; NSString *key; } scales[] = {
		{ @"Actual Size", 100, @"1" }, { @"1.5×", 150, @"" },
		{ @"2×", 200, @"2" }, { @"3×", 300, @"3" }
	};
	for (unsigned int i = 0; i < sizeof(scales) / sizeof(scales[0]); i++) {
		NSMenuItem *item = menu_item(scales[i].title, @selector(setScale:), scales[i].key, controller);
		item.tag = scales[i].scale;
		[viewMenu addItem:item];
	}
	[viewMenu addItem:menu_item(@"Fit to Screen", @selector(fitToScreen:), @"0", controller)];
	[viewMenu addItem:[NSMenuItem separatorItem]];
	[viewMenu addItem:menu_item(@"Toggle Full Screen", @selector(toggleFullScreen:), @"f", controller)];
	viewRoot.submenu = viewMenu;
	[menuBar addItem:viewRoot];
}
