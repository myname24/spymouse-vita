#!/usr/bin/env python3
"""Assemble the folder to copy to the Vita (ux0:data/spymouse/) from YOUR OWN Spy Mouse HD Android files.

    python3 tools/prepare_data.py <SpyMouse.apk> <com.ea.spymouse_row/files folder> <output folder>

The APK supplies libSpyMouse.so (lib/armeabi/) and assets/; the folder is the game data the Android app downloads on first run
(res/, res_960x640/, Downloaded.indicate). Nothing is downloaded and nothing from the game ships with this project.
"""
import hashlib, os, shutil, sys, zipfile

SO_SIZE = 2902272
SO_SHA256 = "cb066bb7179dfa2e67a3034d565341afb6b71e2a502be07dff9cd919a00a121f"


def main():
    if len(sys.argv) != 4:
        print(__doc__); return 1
    apk, data, out = sys.argv[1:]
    if not os.path.isfile(apk): sys.exit("APK not found: " + apk)
    if not os.path.isdir(os.path.join(data, "res_960x640")): sys.exit("not the game data folder (no res_960x640 inside): " + data)
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(apk) as z:
        names = z.namelist()
        so = next((n for n in names if n.endswith("lib/armeabi/libSpyMouse.so")), None)
        if not so: sys.exit("lib/armeabi/libSpyMouse.so not found in the APK (this port needs the armeabi library)")
        blob = z.read(so)
        if len(blob) != SO_SIZE or hashlib.sha256(blob).hexdigest() != SO_SHA256:
            print("WARNING: libSpyMouse.so differs from the supported build (%d bytes, sha256 %s); the loader patches fixed addresses and will refuse it"
                  % (len(blob), hashlib.sha256(blob).hexdigest()[:16]))
        with open(os.path.join(out, "libSpyMouse.so"), "wb") as f: f.write(blob)
        n = 0
        for name in names:
            if name.startswith("assets/") and not name.endswith("/"):
                dst = os.path.join(out, name)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as f: f.write(z.read(name))
                n += 1
    print("libSpyMouse.so + %d asset files written" % n)
    dst = os.path.join(out, "files")
    if os.path.exists(dst): shutil.rmtree(dst)
    shutil.copytree(data, dst)
    print("game data copied to", dst)
    print("\nNow copy the whole folder to the Vita as ux0:data/spymouse/ (so ux0:data/spymouse/libSpyMouse.so exists).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
