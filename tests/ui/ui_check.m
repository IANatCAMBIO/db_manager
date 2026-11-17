/*
 * ui_check.m — the window (src/db_window.h) built in-process on a scratch
 * database: its chrome (a titled window, no header bar), every line of
 * the plate filled once the launch check lands, the backup line's folder
 * and warning, the login row and the status line; light and dark PNG
 * snapshots; the window released on close.  Needs no Screen Recording or
 * Accessibility permission.
 *
 *   ui_check <snapshot dir>
 */

#import <Cocoa/Cocoa.h>

#import "db_window.h"

#include "check.h"
#include "identity.h"
#include "service.h"

/*
 * view_with_identifier — the first view under `root` with an identifier.
 *
 * Inputs:
 *   root       — where to look
 *   identifier — the identifier
 *
 * Output: the view, or nil.
 */
static NSView *
view_with_identifier(NSView *root, NSString *identifier)
{
    if ([root.identifier isEqualToString:identifier]) {
        return root;
    }
    for (NSView *v in root.subviews) {
        NSView *found = view_with_identifier(v, identifier);
        if (found != nil) {
            return found;
        }
    }
    return nil;
}

/*
 * snapshot — render a window, title bar included, to a PNG.
 *
 * Inputs:
 *   window — the window
 *   dir    — the folder
 *   name   — the file's name without extension
 *
 * Output: true when written.
 */
static bool
snapshot(NSWindow *window, NSString *dir, NSString *name)
{
    NSView *frame_view = window.contentView.superview;
    [frame_view layoutSubtreeIfNeeded];
    [window displayIfNeeded];
    NSBitmapImageRep *rep = [frame_view bitmapImageRepForCachingDisplayInRect:frame_view.bounds];
    [frame_view cacheDisplayInRect:frame_view.bounds toBitmapImageRep:rep];
    NSData *png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    return [png writeToFile:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".png"]]
                 atomically:YES];
}

int
main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: ui_check <snapshot dir>\n");
        return 2;
    }
    const char *path = check_sandbox();
    NSString   *dir  = @(argv[1]);
    [NSApplication sharedApplication];
    NSApp.activationPolicy = NSApplicationActivationPolicyAccessory;

    cdb_service_hooks hooks = { 0 };
    char             *err   = NULL;
    cdb_service      *s     = cdb_service_start(&hooks, NULL, &err);
    if (s == NULL) {
        fprintf(stderr, "cannot start the service: %s\n", err != NULL ? err : "?");
        return 2;
    }

    /* The window's work runs in a pool of its own, so that the lifetime
     * check below sees the window after every autoreleased reference to it
     * has gone.                                                            */
    __weak NSWindow *weak_win = nil;
    @autoreleasepool {
        cdb_window *controller = [[cdb_window alloc] init_with_service:s];
        NSWindow   *win        = controller.window;
        [controller showWindow:nil];

        check([win.title isEqualToString:@CDB_APP_NAME], "chrome: the window is titled");
        check((win.styleMask & NSWindowStyleMaskTitled) != 0 && !win.titlebarAppearsTransparent &&
              (win.styleMask & NSWindowStyleMaskFullSizeContentView) == 0 && win.titleVisibility == NSWindowTitleVisible,
              "chrome: an ordinary title bar, no header bar");
        check(![win isKindOfClass:NSPanel.class], "chrome: a window, not a panel");

        NSView *root = win.contentView;
        NSTextField *health = (NSTextField *)view_with_identifier(root, @"health");
        NSTextField *data   = (NSTextField *)view_with_identifier(root, @"data");
        NSTextField *backup = (NSTextField *)view_with_identifier(root, @"backup-path");
        NSTextField *status = (NSTextField *)view_with_identifier(root, @"status");
        NSButton    *login  = (NSButton *)view_with_identifier(root, @"login");
        NSTextField *peers  = (NSTextField *)view_with_identifier(root, @"peers");
        NSTextField *left   = (NSTextField *)view_with_identifier(root, @"leftovers");
        check(health && data && backup && status && login && peers && left, "plate: every line is present");
        check([health.stringValue isEqualToString:@"Checking…"], "plate: the launch check shows as Checking…");
        check([status.stringValue hasPrefix:@"Created a new database at "], "status: the start is announced");

        CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + 20.0;
        while (cdb_service_checking(s) && CFAbsoluteTimeGetCurrent() < end) {
            check_pump(NULL, 0.05);
        }
        [controller refresh];
        check([health.stringValue hasPrefix:@"Healthy — checked "], "plate: the verdict lands as Healthy");
        check([data.stringValue isEqualToString:@"0 notes, 0 tasks, 0 tags"], "plate: the counts of an empty database");
        check([backup.stringValue hasPrefix:@"Backing up to "] &&
              [backup.stringValue containsString:@"alongside the database"] &&
              [backup.stringValue containsString:@"No backup made yet"],
              "backups: the default folder beside the database, with its warning, and no backup yet");
        NSString *db_dir = @(path).stringByDeletingLastPathComponent;
        check([backup.stringValue containsString:db_dir], "backups: the folder named is the sandbox's");
        cdb_service_scan_peers(s);
        end = CFAbsoluteTimeGetCurrent() + 20.0;
        while (cdb_service_scanning(s) && CFAbsoluteTimeGetCurrent() < end) {
            check_pump(NULL, 0.05);
        }
        [controller refresh];
        check([peers.stringValue containsString:@"this app"], "connections: this process is listed as this app");
        check([left.stringValue hasPrefix:@"Nothing left behind"] ||
              [left.stringValue containsString:@"write-ahead log"],
              "connections: the leftovers line is filled");

        check(snapshot(win, dir, @"window-light"), "snapshot: light");
        win.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        check(snapshot(win, dir, @"window-dark"), "snapshot: dark");
        win.appearance = nil;

        weak_win = win;
        [win close];
    }
    check_pump(NULL, 0.2);
    check(weak_win == nil, "lifetime: the closed window is released");
    cdb_service_stop(s);
    return check_result();
}
