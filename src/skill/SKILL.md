---
name: omni-reminder
description: >
  Read, add, and remove the user's reminders in Omni Reminder, the Omarchy
  reminder overlay. Use when asked to list reminders, remind the user of
  something at a time, or clear one. Triggers: omni-reminder, reminder list,
  remind me, voice memo reminders.
---

# Omni Reminder

`omni-reminder` keeps reminders in `~/.local/share/omni-reminder/reminders.json`
(voice clips in `clips/`). Never edit the file by hand; use the commands, which
lock it and save atomically. The running daemon picks changes up on its own.

```bash
omni-reminder list --json                       # every reminder, with id, text, due, clip
omni-reminder add "Call the dentist" --due "2026-12-01T14:00"
omni-reminder add "Stretch" --due +45m          # also: 18:30, tomorrow 9:00, in 10 min
omni-reminder remove 7                          # removes the reminder and its clip
omni-reminder open                              # show the overlay to the user
```

Each command exits 0 on success and 1 on failure with the reason on stderr.
A reminder with a due time raises a desktop notification when it is due.
Voice clips are deleted 30 days after recording or when all clips pass 100 MB
(oldest first); the reminder stays and shows "clip expired".

If the user connected Google (`omni-reminder sync status`), reminders also sync
with their Google Calendar (timed) and Google Tasks (untimed) within minutes;
only text, due time and done ever leave the computer. `omni-reminder sync now`
syncs at once. Signing in (`sync login`) needs the user at a browser.
