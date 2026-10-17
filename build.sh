#!/usr/bin/env bash
# Build the Vita VPK: ./build.sh   ->  build/vita/spymouse.vpk
cd "$(dirname "$0")" || exit 1
export VITASDK=${VITASDK:-/usr/local/vitasdk}
export PATH=$VITASDK/bin:$PATH
SO=${SO:-data/libSpyMouse.so}
mkdir -p build/vita src/gen
arm-vita-eabi-nm -g --defined-only $VITASDK/arm-vita-eabi/lib/libvitaGL.a 2>/dev/null | awk '$3 ~ /^(gl|vgl)/ {print $3}' | sort -u > build/vitagl_syms.txt
python3 tools/gen.py "$SO" build/vitagl_syms.txt | head -1 || exit 1
# VFP short-vector -> scalar patches for the EA audio mixer (see tools/vfpvec.py); the cave lives in the page after the data segment
python3 tools/vfpvec.py gen "$SO" src/gen/vfp_patch.c 2f6000 || exit 1
# text padding (src/gen/textpad.c): vita-elf-create appends ~3.5 KB of SCE data after the code segment and the data segment starts
# at the next 64 KB boundary; when the code ends just below one it fails with "Cannot allocate N bytes for SCE data at end of
# segment 0; segment 1 overlaps". Then measure the gap, pad the code past the boundary and link again (same fix as the iOS loader).
[ -f src/gen/textpad.c ] || printf 'const unsigned char sm_text_pad[4] __attribute__((used)) = { 0 };\n' > src/gen/textpad.c
(cd build/vita && cmake -DCMAKE_BUILD_TYPE=Release ../.. > cmake.log && make > make.log 2>&1); st=$?
if grep -q "segment 1 overlaps" build/vita/make.log; then
  read -r VA FS < <(arm-vita-eabi-readelf -lW build/vita/spymouse | awk '$1=="LOAD" {print $3, $5; exit}')
  END=$(( VA + FS ))
  OLD=$(grep -o '\[[0-9]*\]' src/gen/textpad.c | tr -d '[]')
  GAP=$(( 0x10000 - (END & 0xffff) ))
  PAD=$(( OLD + GAP + 0x100 ))
  echo "code segment ends $(printf %#x $END), $GAP bytes below a 64 KB boundary: padding by $PAD"
  printf 'const unsigned char sm_text_pad[%d] __attribute__((used)) = { 0 };\n' $PAD > src/gen/textpad.c
  (cd build/vita && make > make.log 2>&1); st=$?
fi
grep -E 'error|warning: ' build/vita/make.log | head -30
[ $st -eq 0 ] || { tail -15 build/vita/make.log; exit 1; }
ls -la build/vita/spymouse.vpk
mkdir -p build/elf && cp build/vita/spymouse "build/elf/spymouse-$(date +%m%d-%H%M).elf"
