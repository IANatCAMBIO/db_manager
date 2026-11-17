/* ===========================================================================
 * look.h — every size, spacing and timing of camb.io Database Manager, in one place
 *
 * Plain C: the engine (service.h) reads the timings, the window the rest.
 * Colours are the system's (shared_look.h for the shared ones).
 * =========================================================================== */

#ifndef CDB_LOOK_H
#define CDB_LOOK_H

/* ── Timings (service.h) ──────────────────────────────────────────────────── */

/* How long after launch the first backup poll runs: soon, so a backup that
 * fell due while nothing was running is made at once, but after the launch
 * integrity check has had its say.                                          */
static const double cdb_look_first_poll_s = 5.0;

/* How often the integrity check runs on its own, after the one at launch
 * (Update runs it at any time).  Once a day: PRAGMA integrity_check walks
 * every page, so it is not something to run every minute.                   */
static const double cdb_look_check_interval_s = 24.0 * 60.0 * 60.0;

/* How often the window, while it is up, scans for the processes holding
 * the database open.                                                         */
static const double cdb_look_peer_scan_s = 5.0;

/* ── The window (db_window.h) ─────────────────────────────────────────────── */

static const double cdb_look_text_w       = 340.0;  /* wrapped text: the backup folder line  */
static const double cdb_look_path_w       = 220.0;  /* the database path, wrapped in the plate */
static const double cdb_look_indent       = 12.0;   /* a section's controls under its heading */
static const double cdb_look_margin       = 20.0;   /* the column's left and right margins   */
static const double cdb_look_margin_top   = 16.0;
static const double cdb_look_row_gap      = 6.0;    /* between rows                          */
static const double cdb_look_heading_gap  = 8.0;    /* under a heading                       */
static const double cdb_look_section_gap  = 18.0;   /* between sections                      */
static const double cdb_look_plate_pad_h  = 10.0;   /* the plate's border to its text        */
static const double cdb_look_plate_pad_v  = 8.0;
static const double cdb_look_plate_radius = 6.0;
static const double cdb_look_plate_col_gap = 8.0;   /* a plate label to its value            */
static const double cdb_look_plate_row_gap = 2.0;
static const double cdb_look_number_w     = 52.0;   /* the interval and retention fields     */
static const double cdb_look_below_item   = 4.0;    /* the window's top, under the menu bar item */

/* How much of the 64-character digest the plate shows: enough to compare
 * at a glance, short enough for one line.  The whole of it is in the
 * tooltip and is what "copy" copies.                                        */
static const unsigned cdb_look_sha_head = 8;
static const unsigned cdb_look_sha_tail = 4;

#endif /* CDB_LOOK_H */
