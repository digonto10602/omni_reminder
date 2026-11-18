# System packages installed for this project

Every system package installed while building omni-reminder on this machine.
All build and runtime dependencies (qt6-base, qt6-declarative, qt6-multimedia,
qt6-svg, qt6-wayland, ffmpeg, libnotify, pipewire-pulse, base-devel,
superwhisper-bin) were already present, so none were added.

| Date | Package | Version | Source | Why |
| --- | --- | --- | --- | --- |
| 2026-11-17 | omni-reminder | 0.1.0-1 | built locally from `pkgbuild/` (`makepkg -f`, then `pkexec pacman -U`) | the app itself, so it is on PATH and in the app launcher |
| 2026-11-17 | omni-reminder | 0.1.0-2 | built locally from `pkgbuild/` (`makepkg -f`, then `pkexec pacman -U`) | fix: Esc closes the overlay after an edit |
