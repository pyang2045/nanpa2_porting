/* Folder picker (dialog.h): NSOpenPanel, a directory or a file (the .iso). */
#include "dialog.h"

#import <AppKit/AppKit.h>
#include <stdio.h>

int dialog_saves_popup(void)
{
	return 0; /* iOS only for now (dialog_ios.m) */
}

int dialog_pick_folder(const char *prompt, char *out, size_t cap)
{
	@autoreleasepool {
		[NSApplication sharedApplication];
		/* a Finder launch is active already; a terminal launch is not, and
		 * the panel would open behind the terminal */
		[NSApp activateIgnoringOtherApps:YES];
		NSOpenPanel *p = [NSOpenPanel openPanel];
		p.canChooseDirectories = YES;
		p.canChooseFiles = YES; /* a .iso image of the CD; launch.c validates */
		p.allowsMultipleSelection = NO;
		p.canCreateDirectories = NO;
		p.prompt = @"選擇";
		p.message = prompt ? [NSString stringWithUTF8String:prompt] : @"";
		if ([p runModal] != NSModalResponseOK || !p.URL)
			return 0;
		const char *s = p.URL.fileSystemRepresentation;
		if (!s || snprintf(out, cap, "%s", s) >= (int)cap)
			return 0;
		return 1;
	}
}
