/*
 * app_delegate.h — the application: the menu bar item, its menu, the
 * window under it, and the daemon's lifetime.
 *
 * camb.io Database Manager has no Dock icon and no window of its own until the
 * item is clicked (LSUIElement).  Launch starts the service (service.h),
 * which adopts or creates the shared database and arms the backup poll
 * and the daily integrity check; quitting stops it.  Main thread only.
 */

#import <Cocoa/Cocoa.h>

@interface cdb_app_delegate : NSObject <NSApplicationDelegate>
@end
