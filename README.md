# Terminal Notifier

A Linux C++17 terminal dashboard with three side-by-side panes:

- Left: Microsoft Teams desktop notification summaries and message previews.
- Centre: Outlook desktop notification summaries and email previews.
- Right: upcoming events from a published Outlook ICS feed, including recurring
  events, exclusions, moved occurrences, cancellations and all-day events.

## Build and Run

On Ubuntu/Debian, install the following packages yourself if they are missing:

```sh
sudo apt install build-essential cmake libncurses-dev libdbus-1-dev libcurl4-openssl-dev libical-dev
```

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/terminal-notifier
```

This workspace already has a built executable. Missing development headers were
extracted into the ignored `.deps/` directory without installing system packages.
CMake automatically finds that directory, using the installed shared libraries.

Your provided ICS URL is configured in the ignored, private
`terminal-notifier.conf` file. Do not publish this file: a published ICS URL is a
bearer secret that grants calendar access to anyone who has it. The program does
not print the URL, event titles, or event locations in calendar diagnostic mode.

Run inside a terminal in your logged-in graphical desktop session, as your normal
user, not with sudo, SSH, a container, or a disconnected session bus. Keep Teams
and Outlook PWAs running and allow notifications in both apps, the browser, and
the desktop. The minimum terminal size is 72 columns by 12 rows; 120 by 30 or
larger is recommended. Times are displayed in your local timezone.

Ongoing calendar events are marked `ONGOING` in green. The earliest future event
is marked `NEXT` in yellow with a countdown to its start; simultaneous next events
are all highlighted. Labels and titles also use bold reversed text for monochrome
terminals. Highlights update automatically as time passes, without a feed refresh.
All-day events follow the same rules, and overlapping ongoing events are all marked.

A new Teams/Outlook notification, a calendar event starting (including an event
already ongoing at launch), or a new/changed calendar entry after the initial
load triggers a full-width red bar at the bottom. It flashes for five seconds on,
five seconds off until you press `a`. Acknowledgement does not clear notifications
or events; later activity re-arms the alert. Unchanged calendar refreshes do not
re-trigger it, and the initial load of future events is treated as a baseline.
The normal keyboard footer returns during the off phase and after acknowledgement.

## Controls

| Key | Action |
| --- | --- |
| Tab / Shift+Tab / 1 / 2 / 3 | Select a pane |
| Up / Down / j / k | Scroll selected pane |
| PageUp / PageDown | Scroll one screen |
| Home / End | Jump to beginning or end |
| r | Refresh calendar |
| c | Clear selected Teams or email pane |
| a | Acknowledge and stop the flashing activity alert |
| q / Ctrl+C | Quit |

## PWA Notification Matching

The app passively observes `org.freedesktop.Notifications.Notify` on the session
D-Bus using `BecomeMonitor`. It does not replace your notification daemon,
dismiss toasts, or change browser settings. Notifications continue to display
normally on the desktop.

Chromium often reports only its generic browser name and desktop entry, even for
installed PWAs. Routing recognizes Outlook and Teams website origins in the
`x-kde-origin-name` hint or at the start of the browser notification body,
including Chromium's hyperlink-formatted context. A recognized website origin
takes precedence over subject keywords, so an Outlook email about Teams stays
in the email pane. Unrelated websites and ordinary message text are not matched.

As a fallback, routing looks for `teams`, `msteams`, or `outlook` in the app name,
`desktop-entry` hint, or notification title. To discover an opaque PWA identifier
or check website-origin routing, run:

```sh
./build/terminal-notifier --inspect
```

Trigger a new Teams message or Outlook notification while this command is
running. It prints only the app name, desktop entry, recognized Microsoft website
origin, and routing result, never the message title or body. An Outlook browser
notification should show `origin="outlook.office.com" lane=outlook` (or another
recognized Outlook domain). Previously missed notifications cannot be replayed.
For unidentified PWAs, pass the unique identifier as a case-insensitive substring:

```sh
./build/terminal-notifier --teams-match chrome-TEAMS_APP_ID --outlook-match chrome-OUTLOOK_APP_ID
```

These flags are repeatable and also match notification titles. Do not match the
generic browser name unless you want all that browser's notifications routed to
one pane. To persist these rules, use repeatable `teams_match=IDENTIFIER` and
`outlook_match=IDENTIFIER` entries in your configuration. Your private workspace
configuration already includes identifiers from your installed Teams and Outlook
PWAs. If Teams and Outlook expose identical generic metadata without an
identifying title or website context, D-Bus alone cannot reliably distinguish them. Separate browser
profiles/apps or Microsoft Graph integration would be needed. Unmatched
notifications are counted in the footer and are not retained.

## Calendar Configuration

Configuration precedence, from lowest to highest: default configuration file,
`TERMINAL_NOTIFIER_ICS_URL` environment variable, command-line options. The default
file is `terminal-notifier.conf` in the working directory when present, otherwise
`$XDG_CONFIG_HOME/terminal-notifier/config` or
`~/.config/terminal-notifier/config`. Use `--config FILE` for an explicit path.
The example configuration shows the supported `key=value` settings. There is no
calendar URL command-line flag, to avoid putting this secret in process listings.

```sh
./build/terminal-notifier --check-calendar
./build/terminal-notifier --calendar-file /path/to/calendar.ics
./build/terminal-notifier --refresh 120 --days 30
./build/terminal-notifier --no-calendar
./build/terminal-notifier --demo
```

Calendar downloads use verified HTTPS, HTTPS-only redirects, a 15-second timeout,
and an 8 MiB response limit. The default refresh interval is five minutes and
the agenda covers the next 14 days. Downloads run off the UI thread. Failed
refreshes retain the previous agenda with an explicit stale/error indication;
there is no disk cache. Shutdown may take up to 15 seconds if a download is in
progress. libical handles ICS escaping, folded lines, timezones and recurrence
rules. Floating/date-only values use the local timezone. Sub-hourly recurrence
rules and `RANGE=THISANDFUTURE` edits are rejected with an explicit error rather
than silently producing an incorrect agenda.

## Scope and Limitations

- This is a notification viewer, not a Teams client or a full email client. It
  cannot fetch chat history, list the mailbox, mark mail read, or send messages.
  Full mailbox/chat integration needs Microsoft Graph OAuth and tenant consent.
- It receives new notifications only while running, keeping the latest 200 per
  pane in memory. It does not recover old desktop notifications or persist mail,
  chat, or calendar contents. Notification bodies are shown as supplied; markup
  may be visible literally. Terminal control characters are removed.
- Preview content depends on app privacy settings. Apps that suppress background
  notifications or hide previews cannot provide missing content to this app.
- Notification monitoring may be denied by desktop D-Bus policy. The status bar
  reports that failure while the calendar remains usable. After a session-bus
  disconnect, restart the program. There is no silent fallback to taking over the
  notification service.
- Browsers using a sandboxed portal rather than the standard notification
  interface may not expose identifiable notifications. Use `--inspect` to verify
  what actually reaches your desktop notification service.
- Published Outlook feeds can be delayed or show only availability, depending on
  your organization's sharing policy. The app cannot bypass that policy. Revoke
  and republish the ICS URL in Outlook if it has been exposed.

## Tests

`ctest` covers notification routing and bounds, Chromium body/hyperlink/origin-hint
routing and lookalike domains, recurrence/exclusion/cancellation
handling, VTIMEZONE overrides, local floating times, notification capture through
a separate `dbus-run-session`, and pane rendering/keyboard/resize checks in
pseudo-terminals at 120x30, 72x12 and 40x8. It never injects synthetic notifications
into your actual desktop. `--demo` exercises all three panes without bus access
or downloads.