#!/usr/bin/env bash
# Run before publishing: checks that nothing from the game is in the git tree or in the VPK.
#   tools/check_release.sh [path/to/spymouse.vpk]
cd "$(dirname "$0")/.." || exit 1
fail=0
if [ -d .git ]; then
  echo "== tracked files =="
  git ls-files
  bad=$(git ls-files | grep -E '(^data/|\.apk$|\.obb$|\.so$|\.mp4$|\.m4a$|libSpyMouse|dex_dump|ANALYSIS|PORTING|^src/gen/)') && { echo "GAME-DERIVED FILES TRACKED:"; echo "$bad"; fail=1; }
  big=$(git ls-files -z | xargs -0 ls -l 2>/dev/null | awk '$5 > 800000 {print $9, $5}'); [ -n "$big" ] && { echo "LARGE TRACKED FILES (check them):"; echo "$big"; }
else
  echo "(no git repository yet: run 'git init' first)"; fail=1
fi
vpk=${1:-build/vita/spymouse.vpk}
if [ -f "$vpk" ]; then
  echo "== $vpk =="
  python3 -m zipfile -l "$vpk"
  python3 - "$vpk" <<'EOF' || fail=1
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); ok = True
allowed = {"eboot.bin", "sce_sys/param.sfo", "sce_sys/icon0.png", "sce_sys/livearea/contents/bg.png",
           "sce_sys/livearea/contents/startup.png", "sce_sys/livearea/contents/template.xml"}
for i in z.infolist():
    if i.filename not in allowed or i.file_size > 2_000_000:
        print("UNEXPECTED FILE IN VPK:", i.filename, i.file_size); ok = False
sys.exit(0 if ok else 1)
EOF
  sha256sum "$vpk"
fi
[ $fail -eq 0 ] && echo "OK: no game files found" || echo "PROBLEMS FOUND"
exit $fail
