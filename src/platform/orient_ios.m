/* iOS/iPadOS: keep the interface landscape. iPadOS 26 deprecates UIRequiresFullScreen and may ignore the
 * supported-orientation masks while the device's rotation lock is off (Apple forums 811216, 801140), so
 * the mask alone is not enough. Apple's documented request (TN3192, "Request scene orientation lock") is
 * UIViewController.prefersInterfaceOrientationLocked, which SDL's view controller does not implement; it
 * is added to that class through the ObjC runtime. The scene is first asked for landscape through
 * requestGeometryUpdate; once it is landscape the lock is requested, so the UI stays at that orientation
 * while the device turns. The lock is a request, the system may refuse it (Stage Manager, Split View):
 * the presenter then letterboxes and maps touch through the window rectangle, so nothing distorts.
 * Every orientation or size change is logged. */
#import <UIKit/UIKit.h>

/* Everything here uses iOS 26 scene-geometry API; orient_ios_init returns before touching it on older
 * systems (the deployment target is 13), so the unguarded-availability warnings are silenced. */
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
#include <SDL3/SDL.h>
#include <objc/runtime.h>

#include "orient_map.h"

static SDL_Window *g_win;
static BOOL g_lock;
static BOOL g_flip;          /* a flip to the other landscape side is under way: the lock stays off */
static unsigned g_gen;       /* debounce / timeout generation */
static UIInterfaceOrientation g_target;

static UIWindow *ui_window(void)
{
	return (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(g_win),
							  SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
}

static BOOL prefers_locked(id self, SEL _cmd) { return g_lock; }

static void log_state(const char *why)
{
	UIWindow *w = ui_window();
	UIWindowScene *s = w.windowScene;
	if (!s) {
		SDL_Log("orient[%s]: no scene", why);
		return;
	}
	UIWindowSceneGeometry *g = s.effectiveGeometry;
	CGRect b = g.coordinateSpace.bounds;
	SDL_Log("orient[%s]: interface=%ld device=%ld scene=%.0fx%.0f locked=%d wants_lock=%d resizing=%d",
		why, (long)g.interfaceOrientation, (long)UIDevice.currentDevice.orientation, b.size.width,
		b.size.height, (int)g.isInterfaceOrientationLocked, (int)g_lock, (int)g.isInteractivelyResizing);
}

static void apply(const char *why)
{
	UIWindow *w = ui_window();
	UIWindowScene *s = w.windowScene;
	UIViewController *vc = w.rootViewController;
	if (!s || !vc)
		return;
	UIInterfaceOrientation io = s.effectiveGeometry.interfaceOrientation;
	BOOL landscape = UIInterfaceOrientationIsLandscape(io);
	if (!landscape) {
		g_lock = NO;
		[vc setNeedsUpdateOfSupportedInterfaceOrientations];
		UIWindowSceneGeometryPreferencesIOS *p = [[UIWindowSceneGeometryPreferencesIOS alloc]
			initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
		[s requestGeometryUpdateWithPreferences:p
					   errorHandler:^(NSError *e) {
						   SDL_Log("orient: requestGeometryUpdate failed: %s",
							   e.localizedDescription.UTF8String);
					   }];
	} else if (!g_lock && !g_flip) {
		g_lock = YES;
	}
	if ([vc respondsToSelector:NSSelectorFromString(@"setNeedsUpdateOfPrefersInterfaceOrientationLocked")])
		[vc performSelector:NSSelectorFromString(@"setNeedsUpdateOfPrefersInterfaceOrientationLocked")];
	log_state(why);
}

static void notify_lock(void)
{
	UIViewController *vc = ui_window().rootViewController;
	SEL sel = NSSelectorFromString(@"setNeedsUpdateOfPrefersInterfaceOrientationLocked");
	if ([vc respondsToSelector:sel])
		[vc performSelector:sel];
}

/* The interface orientation the current device side calls for; 0 = no landscape side. */
static int wanted(void)
{
	return orient_interface_for_device((int)UIDevice.currentDevice.orientation);
}

static int current(void)
{
	return (int)ui_window().windowScene.effectiveGeometry.interfaceOrientation;
}

/* End of a flip: the scene reached the wanted side (by our request or by itself) or the timeout ran out;
 * lock again at whatever landscape side it is on. */
static void flip_done(const char *why)
{
	g_flip = NO;
	g_gen++;
	g_lock = UIInterfaceOrientationIsLandscape((UIInterfaceOrientation)current());
	notify_lock();
	log_state(why);
}

static BOOL g_req_logged;

/* One geometry request for the wanted side. Sent only once the scene reports itself unlocked (a request
 * made while it is still locked is dropped), and again on every poll tick until the interface matches. */
static void flip_request(int t)
{
	UIWindowScene *s = ui_window().windowScene;
	UIInterfaceOrientationMask m = t == UIInterfaceOrientationLandscapeLeft
					       ? UIInterfaceOrientationMaskLandscapeLeft
					       : UIInterfaceOrientationMaskLandscapeRight;
	UIWindowSceneGeometryPreferencesIOS *p =
		[[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:m];
	[ui_window().rootViewController setNeedsUpdateOfSupportedInterfaceOrientations];
	[s requestGeometryUpdateWithPreferences:p
				   errorHandler:^(NSError *e) {
					   SDL_Log("orient[flip-request-error]: %s", e.localizedDescription.UTF8String);
				   }];
	if (!g_req_logged) {
		g_req_logged = YES;
		SDL_Log("orient[flip-request]: target=%d", t);
	}
}

static void flip_poll(unsigned gen, int left)
{
	if (gen != g_gen || !g_flip)
		return;
	int w = wanted();
	if (w == 0 || current() == w) { /* arrived, or the device left the landscape sides: never fight the system */
		flip_done("flip-reached");
		return;
	}
	if (left <= 0) {
		flip_done("flip-timeout");
		return;
	}
	if (!ui_window().windowScene.effectiveGeometry.isInterfaceOrientationLocked || left < 22) /* or ~0.75 s passed */
		flip_request(w);
	else
		notify_lock(); /* still locked: ask again for the (now off) preference */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), dispatch_get_main_queue(),
		       ^{ flip_poll(gen, left - 1); });
}

/* After the debounce: the device is on a landscape side that differs from the interface. */
static void flip_check(unsigned gen)
{
	if (gen != g_gen)
		return; /* a newer device change superseded this one */
	int t = wanted();
	if (t == 0) { /* portrait, face up/down, unknown: never unlock; settle a flip in progress */
		if (g_flip)
			flip_poll(gen, 0);
		return;
	}
	if (t == current()) {
		if (g_flip)
			flip_done("flip-moot"); /* the device came back to the side it was on */
		return;
	}
	UIWindowScene *s = ui_window().windowScene;
	if (!s)
		return;
	g_flip = YES;
	g_target = (UIInterfaceOrientation)t;
	g_lock = NO;
	notify_lock();
	log_state("flip-unlock");
	g_req_logged = NO;
	flip_poll(gen, 27); /* 4 s at 150 ms: the system can take a while after the unlock */
}

/* Device turned: only the two landscape sides matter (orient_map.h). */
static void device_changed(void)
{
	if (wanted() == 0) {
		log_state("device-ignored");
		return;
	}
	unsigned gen = ++g_gen;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(),
		       ^{ flip_check(gen); }); /* debounce 0.3 s */
}

static bool SDLCALL watch(void *ud, SDL_Event *e)
{
	switch (e->type) {
	case SDL_EVENT_WINDOW_RESIZED:
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_FOCUS_GAINED:
	case SDL_EVENT_DID_ENTER_FOREGROUND:
	case SDL_EVENT_WINDOW_EXPOSED:
		dispatch_async(dispatch_get_main_queue(), ^{ apply("event"); });
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
	UIWindow *w = ui_window();
	UIViewController *vc = w.rootViewController;
	if (!vc)
		return;
	Class c = object_getClass(vc);
	class_addMethod(c, NSSelectorFromString(@"prefersInterfaceOrientationLocked"), (IMP)prefers_locked, "B@:");
	SDL_Log("orient: sdk-built, class %s, os %s", class_getName(c),
		UIDevice.currentDevice.systemVersion.UTF8String);
	[UIDevice.currentDevice beginGeneratingDeviceOrientationNotifications];
	[NSNotificationCenter.defaultCenter addObserverForName:UIDeviceOrientationDidChangeNotification
							object:nil
							 queue:NSOperationQueue.mainQueue
						    usingBlock:^(NSNotification *n) { device_changed(); }];
	SDL_AddEventWatch(watch, NULL);
	apply("init");
}
