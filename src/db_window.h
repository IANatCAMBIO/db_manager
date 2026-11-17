/*
 * db_window.h — the window under the menu bar item: the Database section
 * of Notes' and Tasks' old Settings, now in one place.
 *
 * The plate (health, the file, what it holds, its size and digest) and
 * Update; the rotating backups (the switch, the schedule, where they
 * land, Change Folder…, Back Up Now); who has the database open and what
 * a crash left behind; whether the app starts at login; and a status
 * line for the latest event.  Every control writes through
 * at once — the policy to shared_settings, where the backups read it — so
 * there is no OK or Apply.  Main thread only.
 */

#import <Cocoa/Cocoa.h>

#include "service.h"

@interface cdb_window : NSWindowController

/*
 * init_with_service: — build the window, sized to its content, and fill
 * it from the service.
 *
 * Inputs:
 *   service — the daemon (borrowed; it outlives the window)
 *
 * Output: the controller.
 */
- (instancetype)init_with_service:(cdb_service *)service;

/*
 * refresh — every line from the service: the plate, the backup line, the
 * buttons' state, the status line.  Called when the service says a fact
 * moved and whenever the window is shown.
 */
- (void)refresh;

/*
 * say: — show a one-line event in the status line at the bottom.
 *
 * Inputs:
 *   message — the line
 */
- (void)say:(NSString *)message;

/*
 * show_below: — place the window under a view in the menu bar (its right
 * edge on the view's), bring the app forward and make the window key,
 * and read the file's size and digest again.
 *
 * Inputs:
 *   anchor — the menu bar item's button; nil centres the window instead
 */
- (void)show_below:(NSView *)anchor;

@end
