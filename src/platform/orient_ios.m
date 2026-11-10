/* iOS/iPadOS: keep the interface landscape, either side. iPadOS 26+ may ignore the supported-orientation
 * masks while the device's rotation lock is off (Apple forums 811216, 801140), and UIRequiresFullScreen is
 * deprecated (TN3192). Measured on an iPad (27.2): UIWindowScene requestGeometryUpdate for an orientation
 * is accepted but only acted on at the next device-orientation event, so a flip cannot be forced after the
 * fact. The system's own rotation handles the 180 degree turn between the landscape sides, so:
 *
 *   - unlocked while the device is on a landscape side (or flat/unknown): the system flips immediately;
 *   - locked, through UIViewController.prefersInterfaceOrientationLocked (TN3192 "Request scene orientation
 *     lock"; SDL's view controller class gets the method at runtime), as soon as the device notification
 *     reports portrait or portrait upside down, before the system's rotation;
 *   - unlocked again when the device is back on a landscape side;
 *   - safety net: if the interface is ever portrait, ask the scene for landscape and say so (portrait-recover).
 *
 * The lock is a request; the presenter letterboxes and maps touch through the window rectangle, so a
 * refused lock cannot distort. Every change is logged as orient[...]. Needs iOS 26 (scene geometry API). */
#import <UIKit/UIKit.h>

/* Everything here uses iOS 26 scene-geometry API; orient_ios_init returns before touching it on older
 * systems (the deployment target is 13), so the unguarded-availability warnings are silenced. */
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <SDL3/SDL.h>
#include <objc/runtime.h>

#include "orient_map.h"

static SDL_Window *g_win;
static BOOL g_lock;
static int g_last_land = 3; /* the last landscape interface side seen (3 or 4) */
static int g_last_iface;
static Uint64 g_dev_ns; /* when the last device-orientation notification arrived */

static UIWindow *ui_window(void)
{
	return (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(g_win),
							  SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
}

static BOOL prefers_locked(id self, SEL _cmd) { return g_lock; }

static int current(void)
{
	return (int)ui_window().windowScene.effectiveGeometry.interfaceOrientation;
}

static void log_state(const char *why)
{
	UIWindowScene *s = ui_window().windowScene;
	if (!s) {
		SDL_Log("orient[%s]: no scene", why);
		return;
	}
	UIWindowSceneGeometry *g = s.effectiveGeometry;
	CGRect b = g.coordinateSpace.bounds;
	SDL_Log("orient[%s]: interface=%ld device=%ld scene=%.0fx%.0f locked=%d wants_lock=%d resizing=%d", why,
		(long)g.interfaceOrientation, (long)UIDevice.currentDevice.orientation, b.size.width, b.size.height,
		(int)g.isInterfaceOrientationLocked, (int)g_lock, (int)g.isInteractivelyResizing);
}

static void set_lock(BOOL on, const char *why)
{
	if (g_lock == on)
		return;
	g_lock = on;
	UIViewController *vc = ui_window().rootViewController;
	SEL sel = NSSelectorFromString(@"setNeedsUpdateOfPrefersInterfaceOrientationLocked");
	if ([vc respondsToSelector:sel])
		[vc performSelector:sel];
	log_state(why);
}

static void request_landscape(int side)
{
	UIWindowScene *s = ui_window().windowScene;
	UIInterfaceOrientationMask m = side == 4 ? UIInterfaceOrientationMaskLandscapeLeft
						 : UIInterfaceOrientationMaskLandscapeRight;
	[ui_window().rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
	UIWindowSceneGeometryPreferencesIOS *p =
		[[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:m];
	[s requestGeometryUpdateWithPreferences:p
				   errorHandler:^(NSError *e) {
					   SDL_Log("orient[request-error]: %s", e.localizedDescription.UTF8String);
				   }];
	[UIViewController attemptRotationToDeviceOrientation];
}

static BOOL device_portrait(void)
{
	UIDeviceOrientation d = UIDevice.currentDevice.orientation;
	return d == UIDeviceOrientationPortrait || d == UIDeviceOrientationPortraitUpsideDown;
}

/* Called on a device notification and on window/scene events. */
static void settle(void)
{
	int io = current();
	if (io != g_last_iface) {
		double ms = g_dev_ns ? (double)(SDL_GetTicksNS() - g_dev_ns) / 1e6 : -1;
		SDL_Log("orient[iface-change]: %d -> %d, %.0f ms after the last device notification", g_last_iface, io, ms);
		g_last_iface = io;
		log_state("iface");
	}
	if (UIInterfaceOrientationIsLandscape((UIInterfaceOrientation)io)) {
		g_last_land = io;
		if (device_portrait())
			set_lock(YES, "lock");
		else if (orient_interface_for_device((int)UIDevice.currentDevice.orientation))
			set_lock(NO, "unlock");
	} else {
		/* lost the race (or launched held in portrait): never stay there */
		log_state("portrait-recover");
		request_landscape(g_last_land);
	}
}

static void device_changed(void)
{
	g_dev_ns = SDL_GetTicksNS();
	log_state("device");
	settle();
	/* back on a landscape side while locked: unlock now so the system rotates; ask for the side too */
	int t = orient_interface_for_device((int)UIDevice.currentDevice.orientation);
	if (t && t != current()) {
		set_lock(NO, "unlock");
		request_landscape(t);
	}
}

static bool SDLCALL watch(void *ud, SDL_Event *e)
{
	switch (e->type) {
	case SDL_EVENT_WINDOW_RESIZED:
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_FOCUS_GAINED:
	case SDL_EVENT_DID_ENTER_FOREGROUND:
	case SDL_EVENT_WINDOW_EXPOSED:
		dispatch_async(dispatch_get_main_queue(), ^{ settle(); });
		break;
	}
	return true;
}

void orient_ios_init(SDL_Window *win)
{
	g_win = win;
	if (@available(iOS 26.0, *)) {
	} else {
		return;
	}
	UIViewController *vc = ui_window().rootViewController;
	if (!vc)
		return;
	Class c = object_getClass(vc);
	class_addMethod(c, NSSelectorFromString(@"prefersInterfaceOrientationLocked"), (IMP)prefers_locked, "B@:");
	SDL_Log("orient: class %s, os %s", class_getName(c), UIDevice.currentDevice.systemVersion.UTF8String);
	g_last_iface = current();
	[UIDevice.currentDevice beginGeneratingDeviceOrientationNotifications];
	[NSNotificationCenter.defaultCenter addObserverForName:UIDeviceOrientationDidChangeNotification
							object:nil
							 queue:NSOperationQueue.mainQueue
						    usingBlock:^(NSNotification *n) { device_changed(); }];
	SDL_AddEventWatch(watch, NULL);
	log_state("init");
	settle();
}
