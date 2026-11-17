/* ===========================================================================
 * identity.h — who this build is: the ONE place each name is spelled
 *
 * camb.io Database is the menu bar app that creates and looks after the
 * SQLite file Notes and Tasks share.  Renaming it is an edit to this file
 * and nothing else: the Makefile reads the bundle name, identifier and
 * icon out of it (see `ident` there), and every module includes it.  Keep
 * each definition on one line in the form `#define NAME "value"` — that is
 * what the Makefile parses.
 * =========================================================================== */

#ifndef CDB_IDENTITY_H
#define CDB_IDENTITY_H

/* The name shown to the user: the window title, the menu, tool panels. */
#define CDB_APP_NAME "camb.io Database Manager"

/* The bundle is <CDB_BUNDLE_NAME>.app and its executable is
 * Contents/MacOS/<CDB_BUNDLE_NAME>.                                         */
#define CDB_BUNDLE_NAME "camb.io_db_mgr"

/* CFBundleIdentifier; also the login item's identity (SMAppService).       */
#define CDB_BUNDLE_ID "com.cambio.db"

/* The app's icon, the one image file in the bundle's Resources (from
 * icons/): the coin.  Finder gets it as an .icns through CFBundleIconFile
 * (the Makefile builds that from the PNG); the menu bar item and the About
 * panel load the PNG by this name (shared_ui_app_icon).                     */
#define CDB_APP_ICON "coin.png"

/* The lock that keeps the daemon to one copy per database: a file beside
 * the database, held with flock() for the life of the process.             */
#define CDB_LOCK_FILENAME "camb.io_db_mgr.lock"

/* The Info.plist key the Makefile stamps with the build time (UTC, ISO
 * 8601), shown in the About panel.                                          */
#define CDB_BUILD_DATE_KEY "CambioDbBuildDate"

#endif /* CDB_IDENTITY_H */
