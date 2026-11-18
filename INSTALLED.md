# System packages installed for this project

Every system package installed while building omni-reminder on this machine.
All build and runtime dependencies (qt6-base, qt6-declarative, qt6-multimedia,
qt6-svg, qt6-wayland, ffmpeg, libnotify, pipewire-pulse, base-devel,
superwhisper-bin) were already present, so none were added.

| Date | Package | Version | Source | Why |
| --- | --- | --- | --- | --- |
| 2026-11-17 | omni-reminder | 0.1.0-1 | built locally from `pkgbuild/` (`makepkg -f`, then `pkexec pacman -U`) | the app itself, so it is on PATH and in the app launcher |
| 2026-11-17 | omni-reminder | 0.1.0-2 | built locally from `pkgbuild/` (`makepkg -f`, then `pkexec pacman -U`) | fix: Esc closes the overlay after an edit |
| 2026-11-17 | omni-reminder | 0.2.0-1 | built locally from `pkgbuild/` (`makepkg -f`, then `pkexec pacman -U`) | two-way Google Calendar and Tasks sync; no new system packages were needed (Qt Network was already used, `libsecret`/`secret-tool` was already installed) |
| 2026-11-18 | omni-reminder | 0.2.1-1 | built locally from `pkgbuild/` (`makepkg -f`, then `pkexec pacman -U`) | Sync now button and `S` key in the overlay; no new system packages |
| 2026-11-18 | omni-reminder | 0.2.2-1 | built locally from `pkgbuild/` (`makepkg -f`, then `pkexec pacman -U`) | voice memos take their due time from what was said (English and Bengali); no new system packages |
