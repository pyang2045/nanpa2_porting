/* The saves popup (dialog.h) on iOS: a UIAlertController over the key window's top view controller,
 * with the answer awaited by running the main run loop (SDL's view and its display link keep going).
 * NANPA2_RESET_POPUP=continue|remove-cancel|remove-delete answers by itself 2.5 s after each alert
 * shows (for screenshots and tests; set through SIMCTL_CHILD_ on the simulator). */
#import <UIKit/UIKit.h>

#include <stdlib.h>
#include <string.h>

#include "platform/dialog.h"

static UIViewController *top_vc(void)
{
	UIWindow *key = nil;
	for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
		if (![s isKindOfClass:UIWindowScene.class])
			continue;
		for (UIWindow *w in ((UIWindowScene *)s).windows)
			if (w.isKeyWindow || !key)
				key = w;
	}
	UIViewController *vc = key.rootViewController;
	while (vc.presentedViewController)
		vc = vc.presentedViewController;
	return vc;
}

static void spin(void)
{
	[[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
}

/* Show the alert and return the index of the button taken. timeout > 0: after that many seconds the
 * default button is taken, and "%d" in the message shows the seconds left. pre >= 0: that button is
 * taken by itself after 2.5 s. */
static int ask(NSString *message, NSArray<NSString *> *buttons, int def, int destructive, int cancel, int timeout, int pre)
{
	__block int result = -1;
	UIViewController *vc = top_vc();
	if (!vc)
		return def;
	UIAlertController *a = [UIAlertController alertControllerWithTitle:nil
								    message:[NSString stringWithFormat:message, timeout]
							     preferredStyle:UIAlertControllerStyleAlert];
	for (int i = 0; i < (int)buttons.count; i++) {
		UIAlertActionStyle st = i == destructive ? UIAlertActionStyleDestructive
				       : i == cancel	 ? UIAlertActionStyleCancel
							 : UIAlertActionStyleDefault;
		UIAlertAction *act = [UIAlertAction actionWithTitle:buttons[i]
							      style:st
							    handler:^(UIAlertAction *x) {
								    result = i;
							    }];
		[a addAction:act];
		if (i == def)
			a.preferredAction = act;
	}
	[vc presentViewController:a animated:YES completion:nil];
	NSDate *t0 = [NSDate date];
	int shown = timeout;
	while (result < 0) {
		spin();
		double el = -[t0 timeIntervalSinceNow];
		if (timeout > 0) {
			int left = timeout - (int)el;
			if (left < 0)
				left = 0;
			if (left != shown) {
				shown = left;
				a.message = [NSString stringWithFormat:message, left];
			}
			if (el >= timeout)
				result = def;
		}
		if (pre >= 0 && el >= 2.5)
			result = pre;
	}
	__block int dismissed = 0;
	if (a.presentingViewController)
		[a dismissViewControllerAnimated:YES completion:^{
			dismissed = 1;
		}];
	else
		dismissed = 1;
	while (!dismissed)
		spin();
	return result;
}

int dialog_saves_popup(void)
{
	const char *pre_env = getenv("NANPA2_RESET_POPUP");
	int pre1 = -1, pre2 = -1;
	if (pre_env) {
		if (!strcmp(pre_env, "continue"))
			pre1 = 0;
		else if (!strcmp(pre_env, "remove-cancel"))
			pre1 = 1, pre2 = 1;
		else if (!strcmp(pre_env, "remove-delete"))
			pre1 = 1, pre2 = 0;
	}
	int first = ask(@"刪除存檔並重新開始（可自訂名字）？\nRemove saves and start over to set your own name?\n\n"
			@"自動繼續 Auto-continue in %d s",
			@[ @"繼續 Continue", @"刪除存檔 Remove saves" ], 0, -1, -1, 5, pre1);
	if (first != 1)
		return 0;
	int second = ask(@"確定刪除所有存檔？此動作無法復原。\nDelete all saves? This can't be undone.",
			 @[ @"刪除 Delete", @"取消 Cancel" ], 1, 0, 1, 0, pre2);
	return second == 0;
}
