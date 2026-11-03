/* macOS desktop state (desktop.h): CGSessionCopyCurrentDictionary's
 * CGSSessionScreenIsLocked (set only while the screen is locked; not a
 * public constant, hence the string) and CGDisplayIsAsleep. Both answer in
 * a few ms while the screen is locked. */
#include "desktop.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

void desktop_state(int *locked, int *asleep)
{
	*locked = -1;
	CFDictionaryRef d = CGSessionCopyCurrentDictionary();
	if (d) {
		CFTypeRef v = CFDictionaryGetValue(d, CFSTR("CGSSessionScreenIsLocked"));
		*locked = v && CFGetTypeID(v) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)v);
		CFRelease(d);
	}
	*asleep = CGDisplayIsAsleep(CGMainDisplayID()) != 0;
}
