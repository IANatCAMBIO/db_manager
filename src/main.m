#import <Cocoa/Cocoa.h>
#import "app_delegate.h"

/*
 * main — the app's entry point: the application and its delegate, which
 * starts the daemon once AppKit is up.
 *
 * Inputs:
 *   argc — argument count (unused: the app takes no arguments)
 *   argv — argument vector (unused)
 *
 * Output: does not return (the app ends in applicationShouldTerminate:).
 */
int
main(int argc, char *argv[])
{
    @autoreleasepool {
        NSApplication    *app      = [NSApplication sharedApplication];
        cdb_app_delegate *delegate = [[cdb_app_delegate alloc] init];
        app.delegate               = delegate;
        [app run];
    }
    return 0;
}
