# Spy Mouse HD for PS Vita

A PS Vita port of **Spy Mouse HD** (Firemint / EA, 2010-2011). It does not contain any game code or data: it loads the game's own
Android library (`libSpyMouse.so`) from **your** copy of the game and emulates the small part of Android it needs (JNI, libc,
OpenGL ES 1.1 on top of vitaGL, touch input, audio). It is the same idea as the other "so loader" ports (GTA SA, Mass Effect,
Dead Space ...).

> You must own the game. The game is no longer sold, so you need an Android copy you already have: the APK and the extra data
> folder the game downloads on first launch. **Nothing from the game is included in this repository or in the releases.**

## Status

- Boots, shows the menus, plays the levels at 30 fps with music and sound effects, touch controls.
- Tested on real hardware only by the author. Please report what works and what does not.
- The intro and outro videos are skipped (they are MPEG-4 Part 2, which the Vita has no hardware decoder for).
- No online features (EA's servers are long gone), ads and social buttons do nothing.

## Screenshots<img width="960" height="544" alt="2026-10-17-165750" src="https://github.com/user-attachments/assets/769f4fc2-c8e4-4050-8cf2-63897799bf43" />
<img width="960" height="544" alt="2026-10-17-165801" src="https://github.com/user-attachments/assets/34f55a4a-0bd4-4e22-b966-ed927ddd9b09" />


## What you need

1. A hacked PS Vita / PSTV (HENkaku + Enso or similar).
2. The **kubridge** kernel plugin (tested with v0.3.1; the same plugin every other so-loader port needs): put `kubridge.skprx` in
   `ur0:tai/` and list it under `*KERNEL` in `ur0:tai/config.txt`, then reboot. It is available from VitaDB and from the downloads of
   those other ports.
3. **libshacccg.suprx** installed (the shader compiler, `ur0:/data/libshacccg.suprx`; extract it with ShaRKBR33D or any of the
   usual guides). vitaGL needs it.
4. The **Spy Mouse HD Android files** (armeabi build, the `libSpyMouse.so` that is exactly 2,902,272 bytes):
   - the game APK (`lib/armeabi/libSpyMouse.so` and `assets/` are used from it),
   - the game data folder `Android/data/com.ea.spymouse_row/files` (about 175 MB: `res/`, `res_960x640/`, `Downloaded.indicate`).

### Where `libSpyMouse.so` comes from

You do not build it and it is not downloaded: it is the native library inside the game's APK (an APK is a zip file; the library is
`lib/armeabi/libSpyMouse.so`). `tools/prepare_data.py` extracts it for you and checks its size and hash. By hand: rename a copy of the APK
to `.zip`, open it and take `lib/armeabi/libSpyMouse.so` and the `assets/` folder.

If you have the game on an Android device (or emulator), you can get both parts with adb (the paths can differ on newer Android,
where `Android/data` is protected; a file manager or an emulator works too):

```
adb shell pm path com.ea.spymouse_row            # prints the APK path(s)
adb pull <that path> SpyMouse.apk
adb pull /sdcard/Android/data/com.ea.spymouse_row/files ./spymouse_files
```

## Installing

1. Install `spymouse.vpk` from the releases page with VitaShell.
2. Build the data folder on your PC from your own files:

   ```
   python3 tools/prepare_data.py SpyMouse.apk path/to/com.ea.spymouse_row/files out/
   ```

   (or copy by hand, see the layout below).
3. Copy the result to the Vita so that it looks like this:

   ```
   ux0:data/spymouse/libSpyMouse.so
   ux0:data/spymouse/assets/          (from the APK: EAMCore.ini, GameApplication.ini, downloadcontent/ ...)
   ux0:data/spymouse/files/res/
   ux0:data/spymouse/files/res_960x640/
   ux0:data/spymouse/files/Downloaded.indicate
   ```
4. Start **Spy Mouse HD** from the LiveArea. The first start takes a few seconds before the picture appears.

`ux0:data/spymouse/log.txt` is written on every run; send it with bug reports. If the game closes right away the last lines of that
file say why (missing or wrong `libSpyMouse.so`, kubridge not installed ...).

## Controls

Touch screen (front panel), like the Android version. Tap, drag and swipe on the 960x544 screen.

## Optional switches

Create an empty file with one of these names in `ux0:data/spymouse/` to turn a diagnostic on:

| File | Effect |
|---|---|
| `verbose.txt` | log every file open, GL call and Java call (slow) |
| `noaudio.txt` | turn the sound off |
| `check.txt` | scan the game data at start-up and list files that are 0 bytes (bad copies) |
| `overlay.txt` | draw debug squares over the picture |

## Building

You need the [VitaSDK](https://vitasdk.org) with `vitaGL`, `vitashark`, `mathneon` and `taiHEN` installed, Python 3, and the
**supported `libSpyMouse.so` placed in `data/libSpyMouse.so`** (the build derives the import tables and the audio patches from it, which
is also why no generated files are committed).

```
./build.sh          # -> build/vita/spymouse.vpk
```

## How it works

See [docs/TECHNICAL.md](docs/TECHNICAL.md): ELF loader, bionic-to-newlib shims, fake JNI/Java, allocator and vitaGL fixes, and the
translator that turns the game's ARM VFP short-vector audio mixer code into scalar code the Cortex-A9 can run.

## Credits

- TheOfficialFloW and everyone behind the Vita so-loader technique and `kubridge` (the stub libraries in `third_party/kubridge` are
  built from its import definitions).
- Rinnegatamante for vitaGL, and the VitaSDK / taiHEN teams.
- Firemint / EA made the game. This project is not affiliated with or endorsed by them.

## License

GPL-3.0-or-later (see `LICENSE`), because it links vitaGL. The game, its name and its artwork belong to their owners; the LiveArea
images in `livearea/` are taken from the game's own title and icon art.
