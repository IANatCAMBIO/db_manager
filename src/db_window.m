/*
 * db_window.m — the window under the menu bar item (see db_window.h).
 *
 * Specification: the Database section of the Notes app's Settings window
 * (settings_window.m at commit 4dd8d29), carried over control for control.
 */

#import "db_window.h"

#import <ServiceManagement/ServiceManagement.h>

#import "shared_look.h"
#import "shared_ui.h"

#include "identity.h"
#include "look.h"

#include <time.h>
#include <unistd.h>

/* The health light: a ● tinted green only for a check that ran and
 * passed, red for one that failed, grey otherwise.                         */
static NSString * const health_light = @"●";

/* ── Small builders ─────────────────────────────────────────────────────── */

/*
 * plate_type — the plate's text at the status size (shared_look_status_font),
 * applied after the window's shared_ui_use_ui_font; the copy links keep
 * their own attributed titles.
 *
 * Inputs:
 *   root — the plate's grid, or a view inside it
 */
static void
plate_type(NSView *root)
{
    if ([root isKindOfClass:NSTextField.class]) {
        ((NSTextField *)root).font = [NSFont systemFontOfSize:shared_look_status_font];
    }
    for (NSView *v in root.subviews) {
        plate_type(v);
    }
}

/*
 * small_button — a small push button (the buttons sit under the lines
 * they act on, so they are quiet).
 *
 * Inputs:
 *   title  — its title
 *   target — the receiver
 *   action — the selector
 *
 * Output: the button.
 */
static NSButton *
small_button(NSString *title, id target, SEL action)
{
    NSButton *button = [NSButton buttonWithTitle:title target:target action:action];
    button.controlSize = NSControlSizeSmall;
    return button;
}

/*
 * row — views side by side.
 *
 * Inputs:
 *   views — the views
 *
 * Output: the stack.
 */
static NSStackView *
row(NSArray<NSView *> *views)
{
    NSStackView *stack = [NSStackView stackViewWithViews:views];
    stack.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    stack.spacing     = 6;
    return stack;
}

/*
 * indented — a view moved right under its heading.
 *
 * Inputs:
 *   view  — the view
 *   inset — how far
 *
 * Output: a stack holding it.
 */
static NSStackView *
indented(NSView *view, CGFloat inset)
{
    NSStackView *stack = [NSStackView stackViewWithViews:@[view]];
    stack.edgeInsets = NSEdgeInsetsMake(0, inset, 0, 0);
    return stack;
}

/*
 * login_enabled — whether the app is registered to start at login
 * (SMAppService; the registration belongs to the bundle that made it).
 *
 * Output: YES when it is.
 */
static BOOL
login_enabled(void)
{
    return SMAppService.mainAppService.status == SMAppServiceStatusEnabled;
}

/* ── The window ─────────────────────────────────────────────────────────── */

@interface cdb_window () <NSWindowDelegate>
@property (nonatomic, assign) cdb_service *service;
@property (nonatomic, strong) NSTextField *led;
@property (nonatomic, strong) NSTextField *health;
@property (nonatomic, strong) NSTextField *db_path;
@property (nonatomic, strong) NSTextField *db_data;
@property (nonatomic, strong) NSTextField *db_size;
@property (nonatomic, strong) NSTextField *sha;
@property (nonatomic, strong) NSButton    *sha_copy;      /* copies the whole digest */
@property (nonatomic, copy)   NSString    *sha_full;
@property (nonatomic, strong) NSButton    *update;
@property (nonatomic, strong) NSButton    *backup_on;
@property (nonatomic, strong) NSTextField *interval;
@property (nonatomic, strong) NSStepper   *interval_step;
@property (nonatomic, strong) NSTextField *keep;
@property (nonatomic, strong) NSStepper   *keep_step;
@property (nonatomic, strong) NSTextField *backup_path;
@property (nonatomic, strong) NSButton    *backup_choose;
@property (nonatomic, strong) NSButton    *backup_now;
@property (nonatomic, strong) NSTextField *peers;         /* who has the file open  */
@property (nonatomic, strong) NSTextField *leftovers;     /* what a crash left      */
@property (nonatomic, strong) NSTimer     *peer_timer;    /* the scan while shown   */
@property (nonatomic, strong) NSButton    *login_box;
@property (nonatomic, strong) NSTextField *status_line;
@property (nonatomic, strong) NSStackView *plate_box;     /* the plate and Update   */
@property (nonatomic, strong) NSGridView  *plate_grid;    /* the plate's lines      */
@property (nonatomic, strong) NSStackView *backup_buttons;
@end

@implementation cdb_window

/*
 * init_with_service: — see db_window.h.
 */
- (instancetype)
init_with_service:(cdb_service *)service
{
    NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 400, 300)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                backing:NSBackingStoreBuffered defer:NO];
    win.title              = @CDB_APP_NAME;
    win.releasedWhenClosed = NO;
    if (!(self = [super initWithWindow:win])) {
        return nil;
    }
    _service     = service;
    win.delegate = self;

    NSMutableArray<NSView *> *rows     = [NSMutableArray array];
    NSMutableArray<NSView *> *headings = [NSMutableArray array];
    NSMutableArray<NSView *> *ends     = [NSMutableArray array];   /* last row of a section */
    [headings addObject:[self add_database:rows]];
    [ends addObject:rows.lastObject];
    [headings addObject:[self add_connections:rows]];
    [ends addObject:rows.lastObject];
    [headings addObject:[self add_launch:rows]];
    [ends addObject:rows.lastObject];
    self.status_line = shared_ui_note(@"", cdb_look_text_w);
    self.status_line.identifier = @"status";
    [rows addObject:self.status_line];

    NSStackView *stack = [NSStackView stackViewWithViews:rows];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment   = NSLayoutAttributeLeading;
    stack.spacing     = cdb_look_row_gap;
    stack.edgeInsets  = NSEdgeInsetsMake(cdb_look_margin_top, cdb_look_margin, cdb_look_margin, cdb_look_margin);
    for (NSView *v in headings) {
        [stack setCustomSpacing:cdb_look_heading_gap afterView:v];
    }
    for (NSView *v in ends) {
        [stack setCustomSpacing:cdb_look_section_gap afterView:v];
    }
    /* The plate, Update and the backup buttons share the column's right
     * edge.                                                                */
    for (NSView *v in @[self.plate_box, self.backup_buttons]) {
        [v.trailingAnchor constraintEqualToAnchor:stack.trailingAnchor
                                         constant:-stack.edgeInsets.right].active = YES;
    }

    shared_ui_use_ui_font(stack);
    plate_type(self.plate_grid);
    self.sha.font = [NSFont monospacedSystemFontOfSize:shared_look_status_font weight:NSFontWeightRegular];
    /* The digest arrives from a worker after the window is sized: the line
     * is as wide as its head…tail form from the start, so it cannot widen
     * the plate past the window.                                           */
    NSString *digest_form = [NSString stringWithFormat:@"%@…%@",
                             [@"" stringByPaddingToLength:cdb_look_sha_head withString:@"0" startingAtIndex:0],
                             [@"" stringByPaddingToLength:cdb_look_sha_tail withString:@"0" startingAtIndex:0]];
    NSTextField *digest_probe = [NSTextField labelWithString:digest_form];
    digest_probe.font = self.sha.font;
    [self.sha.widthAnchor constraintGreaterThanOrEqualToConstant:ceil(digest_probe.fittingSize.width)].active = YES;

    [self refresh];                  /* the path decides the plate's width  */
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    NSView *content = [[NSView alloc] initWithFrame:NSZeroRect];
    [content addSubview:stack];
    shared_ui_pin_edges(stack, content);
    win.contentView = content;
    [stack layoutSubtreeIfNeeded];
    [win setContentSize:stack.fittingSize];
    [win center];
    return self;
}

/* ── Sections ─────────────────────────────────────────────────────────── */

/*
 * add_database: — the health plate and its Update button, then the
 * rotating backups.
 *
 * Inputs:
 *   rows — the column to append to
 *
 * Output: the section's heading.
 */
- (NSView *)
add_database:(NSMutableArray<NSView *> *)rows
{
    NSTextField *head = shared_ui_heading(@"Database");
    [rows addObject:head];

    self.led    = [NSTextField labelWithString:health_light];
    self.led.textColor = NSColor.tertiaryLabelColor;
    self.health = [NSTextField labelWithString:@""];
    self.health.identifier = @"health";
    self.db_path = [NSTextField wrappingLabelWithString:@""];
    self.db_path.selectable = YES;
    self.db_path.preferredMaxLayoutWidth = cdb_look_path_w;
    self.db_data = [NSTextField labelWithString:@""];
    self.db_data.identifier = @"data";
    self.db_size = [NSTextField labelWithString:@""];
    self.sha      = [NSTextField labelWithString:@"—"];
    self.sha_copy = shared_ui_copy_link(self, @selector(copy_sha:));

    NSArray<NSArray<NSView *> *> *cells = @[
        @[[NSTextField labelWithString:@"Health:"],           row(@[self.led, self.health])],
        @[[NSTextField labelWithString:@"Current database:"],
          row(@[self.db_path, shared_ui_copy_link(self, @selector(copy_path:))])],
        @[[NSTextField labelWithString:@"Data:"],             self.db_data],
        @[[NSTextField labelWithString:@"Size on disk:"],     self.db_size],
        @[[NSTextField labelWithString:@"SHA-256:"],          row(@[self.sha, self.sha_copy])],
    ];
    NSGridView *grid = self.plate_grid = [NSGridView gridViewWithViews:cells];
    grid.rowSpacing    = cdb_look_plate_row_gap;
    grid.columnSpacing = cdb_look_plate_col_gap;
    grid.rowAlignment  = NSGridRowAlignmentFirstBaseline;
    grid.translatesAutoresizingMaskIntoConstraints = NO;

    NSBox *plate = [[NSBox alloc] initWithFrame:NSZeroRect];
    plate.boxType            = NSBoxCustom;
    plate.fillColor          = NSColor.controlBackgroundColor;
    plate.borderColor        = NSColor.separatorColor;
    plate.borderWidth        = 1;
    plate.cornerRadius       = cdb_look_plate_radius;
    plate.contentViewMargins = NSMakeSize(cdb_look_plate_pad_h, cdb_look_plate_pad_v);
    plate.titlePosition      = NSNoTitle;
    [plate.contentView addSubview:grid];
    [NSLayoutConstraint activateConstraints:@[
        [grid.topAnchor      constraintEqualToAnchor:plate.contentView.topAnchor],
        [grid.bottomAnchor   constraintEqualToAnchor:plate.contentView.bottomAnchor],
        [grid.leadingAnchor  constraintEqualToAnchor:plate.contentView.leadingAnchor],
        [grid.trailingAnchor constraintLessThanOrEqualToAnchor:plate.contentView.trailingAnchor],
    ]];

    self.update = small_button(@"Update", self, @selector(update_health:));
    self.update.toolTip = @"Re-read every line above: run PRAGMA integrity_check and PRAGMA foreign_key_check "
                           "against this database, then re-count its notes, tasks and tags and re-read its "
                           "size and SHA-256.";
    NSStackView *plate_box = self.plate_box = [NSStackView stackViewWithViews:@[plate, self.update]];
    plate_box.orientation = NSUserInterfaceLayoutOrientationVertical;
    plate_box.alignment   = NSLayoutAttributeTrailing;
    plate_box.spacing     = 5;
    plate_box.edgeInsets  = NSEdgeInsetsMake(0, cdb_look_indent, 0, 0);
    [rows addObject:plate_box];

    shared_db *db = cdb_service_db(self.service);
    self.backup_on = [NSButton checkboxWithTitle:@"Back up the database automatically" target:self
                                          action:@selector(backup_toggled:)];
    self.backup_on.toolTip = @"Writes a verified copy of the database Notes and Tasks share into a folder "
                              "of your choice on a timer, keeping only the most recent few.  Worth pointing "
                              "at a disk INDEPENDENT of wherever the database itself lives, so one mishap "
                              "cannot take both.";
    [rows addObject:indented(self.backup_on, cdb_look_indent)];

    self.interval = [self number_field:SHARED_SETTING_BACKUP_INTERVAL_MIN
                                 value:(int)cdb_policy_value(db, SHARED_SETTING_BACKUP_INTERVAL_MIN)
                                   min:0 max:SHARED_BACKUP_INTERVAL_MIN_MAX
                                  step:SHARED_BACKUP_INTERVAL_MIN_STEP stepper:&_interval_step];
    self.interval.toolTip = @"Minutes between backups.  0 backs up only when you press Back Up Now.";
    self.keep = [self number_field:SHARED_SETTING_BACKUP_KEEP
                             value:(int)cdb_policy_value(db, SHARED_SETTING_BACKUP_KEEP)
                               min:SHARED_BACKUP_KEEP_MIN max:SHARED_BACKUP_KEEP_MAX step:1
                           stepper:&_keep_step];
    self.keep.toolTip = @"How many backup files to retain.  The oldest are removed once a NEW backup has "
                         "been verified, never before.";
    [rows addObject:indented(row(@[[NSTextField labelWithString:@"Every"], self.interval, self.interval_step,
                                   [NSTextField labelWithString:@"minutes, keeping"], self.keep, self.keep_step,
                                   [NSTextField labelWithString:@"files"]]), cdb_look_indent)];

    self.backup_path = shared_ui_note(@"", cdb_look_text_w);
    self.backup_path.identifier = @"backup-path";
    [rows addObject:indented(self.backup_path, cdb_look_indent)];
    self.backup_choose = small_button(@"Change Folder…", self, @selector(choose_backup_folder:));
    self.backup_now    = small_button(@"Back Up Now", self, @selector(backup_now:));
    self.backup_buttons = row(@[]);
    [self.backup_buttons addView:self.backup_choose inGravity:NSStackViewGravityTrailing];
    [self.backup_buttons addView:self.backup_now inGravity:NSStackViewGravityTrailing];
    [rows addObject:self.backup_buttons];
    return head;
}

/*
 * number_field:value:min:max:step:stepper: — a small number field with
 * its stepper, for one integer backup setting; either writes the setting.
 *
 * Inputs:
 *   key      — the shared_settings key
 *   value    — its current value
 *   min, max — the range (the field clamps to it)
 *   step     — the stepper's increment
 *   stepper  — receives the stepper
 *
 * Output: the field (its identifier is the key).
 */
- (NSTextField *)
number_field:(const char *)key value:(int)value min:(int)min max:(int)max step:(int)step
     stepper:(NSStepper * __strong *)stepper
{
    NSNumberFormatter *fmt = [[NSNumberFormatter alloc] init];
    fmt.minimum      = @(min);
    fmt.maximum      = @(max);
    fmt.allowsFloats = NO;
    NSTextField *field = [NSTextField textFieldWithString:@""];
    field.formatter    = fmt;
    field.integerValue = MAX(min, MIN(max, value));
    field.identifier   = @(key);
    field.alignment    = NSTextAlignmentRight;
    field.target       = self;
    field.action       = @selector(number_changed:);
    [field.widthAnchor constraintEqualToConstant:cdb_look_number_w].active = YES;
    NSStepper *s = [[NSStepper alloc] initWithFrame:NSZeroRect];
    s.minValue     = min;
    s.maxValue     = max;
    s.increment    = step;
    s.integerValue = field.integerValue;
    s.identifier   = @(key);
    s.target       = self;
    s.action       = @selector(number_changed:);
    *stepper = s;
    return field;
}

/*
 * add_connections: — who has the database open, and what a crash left.
 * A connection does not outlive its process, so the first line is the
 * whole truth about who is connected; the second names what CAN be left
 * behind — log content, a backup claim, an unfinished backup folder — or
 * says that nothing is.
 *
 * Inputs:
 *   rows — the column to append to
 *
 * Output: the section's heading.
 */
- (NSView *)
add_connections:(NSMutableArray<NSView *> *)rows
{
    NSTextField *head = shared_ui_heading(@"Connections");
    [rows addObject:head];
    self.peers = shared_ui_note(@"", cdb_look_text_w);
    self.peers.identifier = @"peers";
    self.peers.textColor  = NSColor.labelColor;
    self.peers.toolTip = @"Every process of yours that has the database file, or its write-ahead log, "
                          "open right now, found by looking through their open files.  A crashed app "
                          "is never listed: macOS closes its files and releases its locks.";
    [rows addObject:indented(self.peers, cdb_look_indent)];
    self.leftovers = shared_ui_note(@"", cdb_look_text_w);
    self.leftovers.identifier = @"leftovers";
    [rows addObject:indented(self.leftovers, cdb_look_indent)];
    return head;
}

/*
 * add_launch: — whether the app starts at login.  A daemon that makes the
 * backups is no use when it is not running, so the switch sits here, in
 * the window, not only in the menu.
 *
 * Inputs:
 *   rows — the column to append to
 *
 * Output: the section's heading.
 */
- (NSView *)
add_launch:(NSMutableArray<NSView *> *)rows
{
    NSTextField *head = shared_ui_heading(@"Launch");
    [rows addObject:head];
    self.login_box = [NSButton checkboxWithTitle:@"Start " @CDB_APP_NAME @" at login" target:self
                                          action:@selector(login_toggled:)];
    self.login_box.identifier = @"login";
    self.login_box.toolTip = @"Backups and the daily integrity check happen only while " @CDB_APP_NAME
                              @" is running.  This registers it as a login item (System Settings ▸ "
                              @"General ▸ Login Items lists it).";
    [rows addObject:indented(self.login_box, cdb_look_indent)];
    return head;
}

/* ── Refreshing ───────────────────────────────────────────────────────── */

/*
 * refresh — see db_window.h.
 */
- (void)
refresh
{
    [self refresh_plate];
    [self refresh_backup];
    [self refresh_connections];
    self.login_box.state = login_enabled() ? NSControlStateValueOn : NSControlStateValueOff;
    const char *status = cdb_service_status(self.service);
    self.status_line.stringValue = status != NULL ? @(status) : @"";
}

/*
 * refresh_plate — every line of the health plate from the service: the
 * verdict as recorded (never run here), the file, its counts, and the
 * size and digest of the last probe.
 */
- (void)
refresh_plate
{
    cdb_service *s   = self.service;
    shared_db   *db  = cdb_service_db(s);
    char        *summary = NULL;
    char        *detail  = NULL;
    int light = cdb_health_words(cdb_service_health(s), cdb_service_checking(s),
                                 (int64_t)time(NULL), &summary, &detail);
    self.led.textColor      = light > 0 ? NSColor.systemGreenColor
                            : light < 0 ? NSColor.systemRedColor : NSColor.tertiaryLabelColor;
    self.health.stringValue = summary != NULL ? @(summary) : @"";
    self.health.toolTip     = detail != NULL ? @(detail) : nil;
    free(summary);
    free(detail);
    self.update.enabled      = !cdb_service_checking(s);
    self.db_path.stringValue = @(cdb_service_path(s));

    cdb_totals totals;
    cdb_totals_read(db, &totals, NULL);
    self.db_data.stringValue = [NSString stringWithFormat:@"%lld note%s, %lld task%s, %lld tag%s",
                                (long long)totals.notes, totals.notes == 1 ? "" : "s",
                                (long long)totals.tasks, totals.tasks == 1 ? "" : "s",
                                (long long)totals.tags,  totals.tags  == 1 ? "" : "s"];

    if (cdb_service_probing(s)) {
        self.db_size.stringValue = @"…";
        self.sha.stringValue     = @"…";
        self.sha_copy.enabled    = NO;
        return;
    }
    int64_t size = cdb_service_file_size(s);
    self.db_size.stringValue = size >= 0 ? shared_ui_format_bytes(size) : @"unknown";
    const char *sha = cdb_service_sha256(s);
    [self show_sha:sha != NULL ? @(sha) : nil];
}

/*
 * show_sha: — the digest line of the plate: its head and tail, the whole
 * of it in the tooltip and on a click.
 *
 * Inputs:
 *   full — the hex digest; nil when the file could not be read
 */
- (void)
show_sha:(NSString *)full
{
    self.sha_full = full;
    if (self.sha_full.length > cdb_look_sha_head + cdb_look_sha_tail) {
        self.sha.stringValue  = [NSString stringWithFormat:@"%@…%@",
                                 [self.sha_full substringToIndex:cdb_look_sha_head],
                                 [self.sha_full substringFromIndex:self.sha_full.length - cdb_look_sha_tail]];
        self.sha.toolTip      = [NSString stringWithFormat:@"%@\n\nThe file as it stands.  A database in use "
                                 "changes with the next edit, so this moves.", self.sha_full];
        self.sha_copy.enabled = YES;
    } else {
        self.sha.stringValue  = @"—";
        self.sha.toolTip      = @"The database file could not be read.";
        self.sha_copy.enabled = NO;
    }
}

/*
 * refresh_backup — the backup controls follow the master switch, and the
 * folder line names where backups land, with a warning when that folder
 * goes with the database's, when the last backup was made
 * (backup_last_at) and why the last pass failed (backup_last_error) —
 * both can be set at once: a copy made whose pruning of old backups
 * failed.
 */
- (void)
refresh_backup
{
    cdb_service *s  = self.service;
    shared_db   *db = cdb_service_db(s);
    BOOL on = cdb_policy_value(db, SHARED_SETTING_BACKUP_ENABLED) != 0;
    self.backup_on.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    for (NSControl *c in @[self.interval, self.interval_step, self.keep, self.keep_step, self.backup_choose]) {
        c.enabled = on;
    }
    self.backup_now.enabled = on && !cdb_service_backing_up(s);

    char *why = NULL;
    char *dir = shared_db_backup_dir(db, &why);
    if (dir == NULL) {
        self.backup_path.stringValue = [NSString stringWithFormat:@"No backup folder: %@",
                                        why != NULL ? @(why) : @"unknown error"];
        free(why);
        return;
    }
    NSString *db_dir = @(cdb_service_path(s)).stringByDeletingLastPathComponent;
    NSString *text   = cdb_path_inside(dir, db_dir.fileSystemRepresentation)
        ? [NSString stringWithFormat:@"Backing up to %s\n⚠ alongside the database — change it to "
                                      "survive losing that folder", dir]
        : [NSString stringWithFormat:@"Backing up to %s", dir];
    free(dir);
    int64_t last_at = cdb_policy_value(db, SHARED_SETTING_BACKUP_LAST_AT);
    if (last_at > 0) {
        char *stamp = cdb_stamp(last_at, (int64_t)time(NULL));
        text = [text stringByAppendingFormat:@"\nLast backup %s", stamp];
        free(stamp);
    } else {
        text = [text stringByAppendingString:@"\nNo backup made yet"];
    }
    char *last_error = NULL;         /* why the last backup failed, if it did */
    if (shared_settings_get(db, SHARED_SETTING_BACKUP_LAST_ERROR, &last_error, NULL) == 1 &&
        last_error[0] != '\0') {
        text = [text stringByAppendingFormat:@"\n⚠ The last backup pass reported: %s", last_error];
    }
    free(last_error);
    self.backup_path.stringValue = text;
}

/*
 * refresh_connections — who has the database open, from the last scan,
 * and what a crash left behind, read now.
 */
- (void)
refresh_connections
{
    cdb_service     *s     = self.service;
    size_t           count = 0;
    const cdb_peer  *peers = cdb_service_peers(s, &count);
    pid_t            me    = getpid();
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (size_t i = 0; i < count; i++) {
        [names addObject:peers[i].pid == me
            ? @"this app"
            : [NSString stringWithFormat:@"%s (pid %d)", peers[i].name, (int)peers[i].pid]];
    }
    self.peers.stringValue = names.count == 0
        ? (cdb_service_scanning(s) ? @"Looking for apps with the database open\u2026"
                                   : @"No process has the database open")
        : [@"Open in " stringByAppendingString:[names componentsJoinedByString:@", "]];

    cdb_leftovers left;
    int64_t       now = (int64_t)time(NULL);
    cdb_leftovers_read(cdb_service_db(s), cdb_service_path(s), now, &left);
    char *text = cdb_leftovers_words(&left, cdb_service_backing_up(s), now);
    self.leftovers.stringValue = @(text);
    free(text);
}

/*
 * say: — see db_window.h.
 */
- (void)
say:(NSString *)message
{
    self.status_line.stringValue = message;
}

/*
 * show_below: — see db_window.h.
 */
- (void)
show_below:(NSView *)anchor
{
    NSWindow *win = self.window;
    if (anchor != nil && anchor.window != nil) {
        NSRect item    = [anchor.window convertRectToScreen:[anchor convertRect:anchor.bounds toView:nil]];
        NSRect visible = (anchor.window.screen ?: NSScreen.mainScreen).visibleFrame;
        NSSize size    = win.frame.size;
        CGFloat x = MAX(NSMinX(visible), MIN(NSMaxX(item) - size.width, NSMaxX(visible) - size.width));
        CGFloat y = MAX(NSMinY(visible) + size.height, NSMinY(item) - cdb_look_below_item);
        [win setFrameTopLeftPoint:NSMakePoint(x, y)];
    }
    [self refresh];
    cdb_service_probe(self.service);
    [self scan_peers:nil];
    if (self.peer_timer == nil) {
        self.peer_timer = [NSTimer scheduledTimerWithTimeInterval:cdb_look_peer_scan_s target:self
                                                         selector:@selector(scan_peers:) userInfo:nil
                                                          repeats:YES];
    }
    [NSApp activate];
    [self showWindow:nil];
    [win makeKeyAndOrderFront:nil];
}

/*
 * scan_peers: — the scan for the processes holding the database open,
 * while the window is up; the list lands through the service's
 * state_changed.
 *
 * Inputs:
 *   sender — the timer, or nil
 */
- (void)
scan_peers:(id)sender
{
    cdb_service_scan_peers(self.service);
}

/*
 * windowWillClose: — the window is going: the scan stops with it.
 *
 * Inputs:
 *   note — the notification (unused)
 */
- (void)
windowWillClose:(NSNotification *)note
{
    [self.peer_timer invalidate];
    self.peer_timer = nil;
}

/* ── Actions ──────────────────────────────────────────────────────────── */

/*
 * update_health: — Update: the integrity check on a worker (its verdict
 * lands in the status line, problems in a tool panel) and the probe of the
 * file; the plate shows "Checking…" meanwhile.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
update_health:(id)sender
{
    cdb_service_check_now(self.service);
}

/*
 * copy_sha: — the full digest onto the clipboard.
 *
 * Inputs:
 *   sender — the link (unused)
 */
- (void)
copy_sha:(id)sender
{
    if (self.sha_full == nil) {
        return;
    }
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:self.sha_full forType:NSPasteboardTypeString];
    [self say:@"SHA-256 copied to the clipboard"];
}

/*
 * copy_path: — the plate's "copy" beside the path: the database's path
 * to the clipboard.
 *
 * Inputs:
 *   sender — the link (unused)
 */
- (void)
copy_path:(id)sender
{
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:@(cdb_service_path(self.service)) forType:NSPasteboardTypeString];
    [self say:@"Database path copied to the clipboard"];
}

/*
 * store_policy:value: — write one backup-policy integer to shared_settings;
 * a failure is said in the status line.
 *
 * Inputs:
 *   key   — SHARED_SETTING_BACKUP_ENABLED, _INTERVAL_MIN or _KEEP
 *   value — the new value
 */
- (void)
store_policy:(const char *)key value:(int64_t)value
{
    char *why = NULL;                /* the module's message                */
    if (cdb_policy_store(cdb_service_db(self.service), key, value, &why) != 0) {
        [self say:[NSString stringWithFormat:@"Could not store %s: %s", key,
                   why != NULL ? why : "unknown error"]];
    }
    free(why);
}

/*
 * backup_toggled: — the backup switch, written to shared_settings (the
 * poll reads it there).
 *
 * Inputs:
 *   sender — the checkbox
 */
- (void)
backup_toggled:(NSButton *)sender
{
    [self store_policy:SHARED_SETTING_BACKUP_ENABLED value:sender.state == NSControlStateValueOn ? 1 : 0];
    [self refresh_backup];
}

/*
 * number_changed: — the interval or retention field or stepper: the pair
 * agree and the value is written to shared_settings.
 *
 * Inputs:
 *   sender — the field or stepper (identifier = key)
 */
- (void)
number_changed:(NSControl *)sender
{
    BOOL         is_interval = [sender.identifier isEqualToString:@SHARED_SETTING_BACKUP_INTERVAL_MIN];
    NSTextField *field       = is_interval ? self.interval : self.keep;
    NSStepper   *stepper     = is_interval ? self.interval_step : self.keep_step;
    NSInteger    value       = MAX((NSInteger)stepper.minValue,
                                   MIN((NSInteger)stepper.maxValue, sender.integerValue));
    field.integerValue   = value;
    stepper.integerValue = value;
    [self store_policy:sender.identifier.UTF8String value:value];
}

/*
 * choose_backup_folder: — pick where backups go, starting where they go
 * now, and store it (a refused folder is reported in a tool panel).
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
choose_backup_folder:(id)sender
{
    char     *dir_c = shared_db_backup_dir(cdb_service_db(self.service), NULL);
    NSString *dir   = dir_c != NULL ? @(dir_c) : nil;
    free(dir_c);
    __weak cdb_window *ws = self;
    shared_ui_pick_path(SharedPickFolder, @"Choose Backup Folder", @"Select", nil, dir, ^(NSString *path) {
        cdb_window *s = ws;
        if (path == nil || s == nil) {
            return;
        }
        char *why = NULL;            /* the module's reason for refusing it */
        if (shared_db_set_backup_dir(cdb_service_db(s.service), path.fileSystemRepresentation, &why) != 0) {
            shared_ui_notice(s.window, @"That folder cannot be used", why != NULL ? @(why) : @"Unknown error.", nil);
            free(why);
        }
        [s refresh_backup];
    });
}

/*
 * backup_now: — one backup pass; the outcome is reported in the status
 * line.
 *
 * Inputs:
 *   sender — the button (unused)
 */
- (void)
backup_now:(id)sender
{
    cdb_service_backup_now(self.service);
    [self refresh_backup];
}

/*
 * login_toggled: — register or unregister the app as a login item.  A
 * refusal (macOS asks the user to allow it under System Settings the
 * first time, or the bundle moved) is shown and the box put back.
 *
 * Inputs:
 *   sender — the checkbox
 */
- (void)
login_toggled:(NSButton *)sender
{
    NSError *error = nil;
    BOOL     on    = sender.state == NSControlStateValueOn;
    BOOL     ok    = on ? [SMAppService.mainAppService registerAndReturnError:&error]
                        : [SMAppService.mainAppService unregisterAndReturnError:&error];
    if (!ok) {
        shared_ui_notice(self.window, on ? @"Could not add the login item" : @"Could not remove the login item",
                         error.localizedDescription, nil);
    }
    sender.state = login_enabled() ? NSControlStateValueOn : NSControlStateValueOff;
}

@end
