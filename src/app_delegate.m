/*
 * app_delegate.m — the application (see app_delegate.h)
 */

#import "app_delegate.h"
#import "db_window.h"

#import <ServiceManagement/ServiceManagement.h>

#import "shared_ui.h"

#include "identity.h"
#include "service.h"

#include <signal.h>

/* The About panel's credits. */
static NSString * const about_motto = @"The one database Notes and Tasks share, looked after.";

/* The menu bar item's artwork: the coin, at the menu bar's icon size. */
static const CGFloat item_icon_size = 18.0;

/* The About panel's artwork. */
static const CGFloat about_icon_size = 128.0;

@interface cdb_app_delegate ()
@property (nonatomic, strong) NSStatusItem *status_item;
@property (nonatomic, strong) NSMenu       *item_menu;       /* on a right or ⌃ click */
@property (nonatomic, strong) cdb_window   *window;
@property (nonatomic, assign) cdb_service  *service;         /* owned; NULL before start */
@property (nonatomic, copy)   NSArray<dispatch_source_t> *signal_sources;  /* live for the process */
@end

/* ── The service's hooks ────────────────────────────────────────────────── */

/*
 * hook_state_changed — a fact the window shows moved: repaint it.
 *
 * Inputs:
 *   service — the service (unused)
 *   ctx     — the delegate
 */
static void
hook_state_changed(cdb_service *service, void *ctx)
{
    cdb_app_delegate *d = (__bridge cdb_app_delegate *)ctx;
    [d.window refresh];
}

/*
 * hook_status — a one-line event: the window's status line and the menu
 * bar item's tooltip.
 *
 * Inputs:
 *   service — the service (unused)
 *   message — the line
 *   ctx     — the delegate
 */
static void
hook_status(cdb_service *service, const char *message, void *ctx)
{
    cdb_app_delegate *d = (__bridge cdb_app_delegate *)ctx;
    [d.window say:@(message)];
    d.status_item.button.toolTip = [NSString stringWithFormat:@CDB_APP_NAME @"\n%s", message];
}

/*
 * hook_problem — something the user must see (the integrity check found
 * damage): a tool panel, over the window when it is up.
 *
 * Inputs:
 *   service — the service (unused)
 *   title   — the headline
 *   message — the detail
 *   ctx     — the delegate
 */
static void
hook_problem(cdb_service *service, const char *title, const char *message, void *ctx)
{
    cdb_app_delegate *d = (__bridge cdb_app_delegate *)ctx;
    [NSApp activate];
    shared_ui_notice(d.window.window.isVisible ? d.window.window : nil, @(title), @(message), nil);
}

@implementation cdb_app_delegate

/* ── Launch ──────────────────────────────────────────────────────────────── */

/*
 * applicationDidFinishLaunching: — the menu bar, the signals, the service
 * (which adopts or creates the database), then the menu bar item.  A
 * service that cannot start is reported in a tool panel and the app quits:
 * there is deliberately no fallback to another location.
 *
 * Inputs:
 *   n — NSApplicationDidFinishLaunchingNotification (unused)
 */
- (void)
applicationDidFinishLaunching:(NSNotification *)n
{
    [self build_menu_bar];
    [self watch_signals];

    cdb_service_hooks hooks = {
        .state_changed = hook_state_changed,
        .status        = hook_status,
        .problem       = hook_problem,
        .ctx           = (__bridge void *)self,
    };
    char         *why  = NULL;       /* why the service did not start       */
    cdb_open_kind kind = cdb_open_adopted;
    self.service = cdb_service_start(&hooks, &kind, &why);
    if (self.service == NULL) {
        [NSApp activate];
        shared_ui_notice(nil, @"Could not start " @CDB_APP_NAME, why != NULL ? @(why) : @"Unknown error.",
                         ^{ [NSApp terminate:nil]; });
        free(why);
        return;
    }
    NSLog(@"[launch] %s %s", kind == cdb_open_created ? "created" : "adopted", cdb_service_path(self.service));

    [self build_status_item];
    self.window = [[cdb_window alloc] init_with_service:self.service];
    hook_status(self.service, cdb_service_status(self.service), (__bridge void *)self);
}

/*
 * build_status_item — the coin in the menu bar.  A left click toggles the
 * window; a right or ⌃ click opens the menu.
 */
- (void)
build_status_item
{
    self.status_item = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
    NSStatusBarButton *button = self.status_item.button;
    button.image  = shared_ui_app_icon(@CDB_APP_ICON, item_icon_size);
    button.target = self;
    button.action = @selector(item_clicked:);
    [button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    button.toolTip = @CDB_APP_NAME;

    NSMenu *menu = [[NSMenu alloc] initWithTitle:@CDB_APP_NAME];
    [menu addItemWithTitle:@"Show Database Window" action:@selector(show_window:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Check Database Now" action:@selector(check_now:) keyEquivalent:@""];
    [menu addItemWithTitle:@"Back Up Now" action:@selector(backup_now:) keyEquivalent:@""];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItemWithTitle:@"About " @CDB_APP_NAME action:@selector(show_about:) keyEquivalent:@""];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItemWithTitle:@"Quit " @CDB_APP_NAME action:@selector(terminate:) keyEquivalent:@"q"];
    for (NSMenuItem *item in menu.itemArray) {
        if (item.action != @selector(terminate:)) {
            item.target = self;
        }
    }
    self.item_menu = menu;
}

/*
 * build_menu_bar — a menu bar for the window's sake: an app without one
 * has no ⌘Q, no ⌘W and no Edit shortcuts in its text fields.  It is never
 * seen (LSUIElement).
 */
- (void)
build_menu_bar
{
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@""];

    NSMenuItem *app_item = [bar addItemWithTitle:@CDB_APP_NAME action:nil keyEquivalent:@""];
    NSMenu     *app_menu = [[NSMenu alloc] initWithTitle:@CDB_APP_NAME];
    [[app_menu addItemWithTitle:@"About " @CDB_APP_NAME action:@selector(show_about:) keyEquivalent:@""]
        setTarget:self];
    [app_menu addItem:NSMenuItem.separatorItem];
    [app_menu addItemWithTitle:@"Quit " @CDB_APP_NAME action:@selector(terminate:) keyEquivalent:@"q"];
    app_item.submenu = app_menu;

    NSMenuItem *edit_item = [bar addItemWithTitle:@"Edit" action:nil keyEquivalent:@""];
    NSMenu     *edit_menu = [[NSMenu alloc] initWithTitle:@"Edit"];
    [edit_menu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    [edit_menu addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
    [edit_menu addItem:NSMenuItem.separatorItem];
    [edit_menu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit_menu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit_menu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit_menu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    edit_item.submenu = edit_menu;

    NSMenuItem *window_item = [bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""];
    NSMenu     *window_menu = [[NSMenu alloc] initWithTitle:@"Window"];
    [window_menu addItemWithTitle:@"Close" action:@selector(performClose:) keyEquivalent:@"w"];
    window_item.submenu = window_menu;

    NSApp.mainMenu = bar;
}

/*
 * watch_signals — turn SIGTERM (make install, pkill, logout), SIGINT
 * (Ctrl-C on `make run`) and SIGHUP (its terminal closing) into an ordinary
 * Quit, so the service stops in order: a backup pass in flight finishes
 * on its own, the database is closed, the lock released.  The sources
 * live for the whole process.
 */
- (void)
watch_signals
{
    static const int signals[] = { SIGTERM, SIGINT, SIGHUP };
    NSMutableArray<dispatch_source_t> *sources = [NSMutableArray array];
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) {
        signal(signals[i], SIG_IGN);     /* the dispatch source sees it instead */
        dispatch_source_t source = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)signals[i], 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(source, ^{ [NSApp terminate:nil]; });
        dispatch_resume(source);
        [sources addObject:source];
    }
    self.signal_sources = sources;
}

/* ── The item ────────────────────────────────────────────────────────────── */

/*
 * item_clicked: — the menu bar item: a right or ⌃ click pops the menu, a
 * left click shows the window under the item, or hides it when it is up.
 *
 * Inputs:
 *   sender — the item's button
 */
- (void)
item_clicked:(id)sender
{
    NSEvent *event = NSApp.currentEvent;
    if (event.type == NSEventTypeRightMouseUp || (event.modifierFlags & NSEventModifierFlagControl)) {
        /* The item shows its menu itself when it has one: lend it the
         * menu for this click, so a plain click still reaches here.       */
        self.status_item.menu = self.item_menu;
        [self.status_item.button performClick:nil];
        self.status_item.menu = nil;
        return;
    }
    if (self.window.window.isVisible && self.window.window.isKeyWindow) {
        [self.window.window orderOut:nil];
    } else {
        [self.window show_below:self.status_item.button];
    }
}

/*
 * show_window: — the menu's Show Database Window.
 *
 * Inputs:
 *   sender — the item (unused)
 */
- (void)
show_window:(id)sender
{
    [self.window show_below:self.status_item.button];
}

/*
 * check_now: — the menu's Check Database Now: the integrity check, its
 * verdict in the item's tooltip (and a tool panel when it is bad).
 *
 * Inputs:
 *   sender — the item (unused)
 */
- (void)
check_now:(id)sender
{
    cdb_service_check_now(self.service);
}

/*
 * backup_now: — the menu's Back Up Now.
 *
 * Inputs:
 *   sender — the item (unused)
 */
- (void)
backup_now:(id)sender
{
    cdb_service_backup_now(self.service);
}

/*
 * show_about: — the standard About panel, as the other apps show it: the
 * coin, the motto, "camb.io · BSD License", and "built <date>".
 *
 * Inputs:
 *   sender — the item (unused)
 */
- (void)
show_about:(id)sender
{
    [NSApp activate];
    shared_ui_show_about(about_motto, shared_ui_app_icon(@CDB_APP_ICON, about_icon_size), @CDB_BUILD_DATE_KEY);
}

/* ── Quit ────────────────────────────────────────────────────────────────── */

/*
 * applicationShouldTerminate: — THE way out (the menu, logout, SIGTERM,
 * SIGINT and SIGHUP all arrive here): the service stops — timers gone,
 * database closed, lock released.
 *
 * Inputs:
 *   sender — NSApplication (unused)
 *
 * Output: NSTerminateNow.
 */
- (NSApplicationTerminateReply)
applicationShouldTerminate:(NSApplication *)sender
{
    cdb_service_stop(self.service);
    self.service = NULL;
    NSLog(@"[quit] database closed");
    return NSTerminateNow;
}

/*
 * applicationSupportsSecureRestorableState: — the app keeps no restorable
 * state; answering YES avoids AppKit's launch warning.
 *
 * Inputs:
 *   app — NSApplication (unused)
 *
 * Output: YES.
 */
- (BOOL)
applicationSupportsSecureRestorableState:(NSApplication *)app
{
    return YES;
}

@end
