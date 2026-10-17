# Reddit post (r/vitahacks; check the sub's rules and flair first)

**Title:** `[Release] Spy Mouse HD (Firemint/EA) ported to PS Vita - needs your own Android copy`

**Body:**

I made a port of Spy Mouse HD for the Vita. Like the other so-loader ports (GTA SA, Mass Effect, Dead Space...) it does not contain any
game files: it loads the game's own Android library from *your* copy and emulates the bits of Android it needs, with the graphics
going through vitaGL.

**What works**
- Menus and gameplay at 30 fps with music and sound effects
- Touch controls (the game is designed for touch)
- Quick level loading

**What doesn't**
- Intro/outro videos are skipped
- Online features / ads / social buttons are dead (EA's servers are gone)

**What you need**
- Hacked Vita, `kubridge.skprx` and `libshacccg.suprx`
- The Android APK and the game data folder (`com.ea.spymouse_row/files`, ~175 MB). The game has been delisted, so you need a copy you already have.
- A helper script in the repo builds the folder to copy from your APK + data folder.

Download and instructions: **<GitHub link>**

Screenshots / video: **<links>**

It was tested by me on one Vita only, so reports are welcome - please attach `ux0:data/spymouse/log.txt` when something doesn't work.

Credits: TheOfficialFloW and the so-loader community, Rinnegatamante (vitaGL), VitaSDK/taiHEN. Not affiliated with EA or Firemint.

---

Notes for posting
- Do not link or offer the APK / data files, in the post or in comments. Reply "dump your own copy" if asked.
- Attach 3-4 screenshots (menu, a level, the LiveArea bubble) and, if possible, a 20-30 s gameplay clip.
- Pin a comment with the requirements and the troubleshooting line about `log.txt`.
