# GitHub release text (v1.0)

Tag: `v1.0`   Title: `Spy Mouse HD for PS Vita v1.0`   Attach: `spymouse.vpk` (+ its SHA-256 in the notes)

---

First public release of the Spy Mouse HD port for PS Vita.

**This release contains no game files.** It is a loader: it runs the game's own Android library from your copy of the game.
You need the Android APK and the game's data folder (see the README).

### What works
- Menus, all gameplay at 30 fps, music and sound effects, touch controls.
- Level loading is quick (small files are read into RAM in one go instead of many tiny memory-card reads).

### Known limitations
- Intro and outro videos are skipped.
- No online features, ads or social buttons (the servers are gone).
- Only the supported Android library (`libSpyMouse.so`, 2,902,272 bytes, armeabi) works; anything else is refused with a message in `log.txt`.

### Requirements
- Hacked Vita/PSTV with `kubridge.skprx` and `libshacccg.suprx` installed
- Your own Spy Mouse HD APK + `com.ea.spymouse_row/files` data folder (about 175 MB)

### Install
1. Install `spymouse.vpk` with VitaShell.
2. `python3 tools/prepare_data.py SpyMouse.apk <files folder> out/` and copy `out/` to `ux0:data/spymouse/` (or copy by hand as described in the README).
3. Start the game from the LiveArea.

If something fails, attach `ux0:data/spymouse/log.txt` to the issue.

SHA-256 of `spymouse.vpk`: `<paste the output of tools/check_release.sh>`
