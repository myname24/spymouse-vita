# Release checklist

1. **Test the final build on the Vita**: fresh install of `build/vita/spymouse.vpk`, then
   - LiveArea icon, background and start button look right,
   - game starts, menu, a level, sound, touch,
   - level load time (compare `log.txt` "since last" lines with the earlier run),
   - start once with `libSpyMouse.so` renamed: the game should close and `log.txt` should say `MISSING ...`.
2. `./build.sh` (needs `data/libSpyMouse.so`), then `tools/check_release.sh` - it must print `OK: no game files found`.
3. Take 3-4 screenshots (menu, gameplay, LiveArea bubble).
4. Commit under your own identity and push (nothing here has been committed or pushed for you):

   ```bash
   cd ~/vita-sm
   git config user.name  "<your name>"
   git config user.email "<your email>"
   git commit -m "Spy Mouse HD for PS Vita v1.0"
   git remote add origin git@github.com:<you>/spymouse-vita.git
   git push -u origin main
   git tag v1.0 && git push origin v1.0
   ```
5. On GitHub: Releases -> new release from tag `v1.0`, paste `docs/release/GITHUB_RELEASE.md`, attach `build/vita/spymouse.vpk`
   and put its SHA-256 in the notes (or `gh release create v1.0 build/vita/spymouse.vpk -F docs/release/GITHUB_RELEASE.md`).
6. Post to Reddit using `docs/release/REDDIT_POST.md` with the GitHub link and screenshots.

## Decisions for you

- **Artwork**: `livearea/` uses the game's own icon and title art (the usual practice for ports). If you would rather not ship EA art,
  replace the four files in `livearea/sce_sys/` with your own (icon 128x128, bg 840x500, startup 280x158, 8-bit PNG) and rebuild.
- **License**: GPL-3.0-or-later, because the binary links vitaGL (LGPL/GPL). Change only after checking that.
- **Repository name**: `spymouse-vita` is a suggestion.
