# Technical notes

How the port works, for people who want to read, fix or reuse it.

## Overview

`libSpyMouse.so` is the Android build of the game (ARMv5TE, softfp, GLES 1.1, no EGL). The Vita process loads it at a fixed address
with kubridge, resolves its imports to host functions, fakes the Java side through JNI, and drives it with the same calls the Android
activity makes.

| File | Role |
|---|---|
| `src/elfload.c` | ELF32 ARM loader at `0x98000000` (`kuKernelAllocMemBlock`, `kuKernelCpuUnrestrictedMemcpy`): REL relocations, `.init_array`, `DT_HASH` exports, `.ARM.exidx`; also applies the guest patches (zeroing `operator new`, VFP caves) |
| `src/imports.c` | the game's libc/libm/pthread/stdio imports: softfp wrappers, bionic `stat`/`dirent` layouts, open flags, 16-bit wchar, `__sF`, ctype tables, 4-byte pthread objects, path remap, missing-path cache, whole-file RAM cache |
| `src/glwrap.c`, `tools/gen.py` | GLES 1.1 on vitaGL: float-argument wrappers, per-texture parameter replay, colour-masked clear, short/byte/fixed client arrays converted to float |
| `src/jni.c`, `src/java.c` | fake JavaVM/JNIEnv (233 slots) and the emulated Java classes: AssetManager/InputStream, storage dirs, display, device info, AudioTrack |
| `src/audio.c` | ring buffer + 48 kHz `sceAudioOut` thread with linear resampling |
| `src/main.c` | start sequence, touch, frame loop, logging |
| `tools/vfpvec.py` | VFP short-vector to scalar translator (below) |
| `tools/prepare_data.py` | builds the Vita data folder from the user's own APK and data folder |

## Start sequence

`EAThread.Init`, `EAIO/rwfilesystem/StorageDirectory` startups, `PlayerAndroid.StartupNativeImpl`, `EAMAudioCoreWrapper.NativeStartup`,
`AndroidEAAudioCore.Init`, `MainActivity.NativeOnCreate/OnResume`, `AndroidRenderer.NativeOnSurfaceCreated/Changed(960, 544)`,
`NativeOnWindowFocusChanged(true)`, then `NativeOnDrawFrame` in a loop on the main thread. Touch goes to
`TouchSurfaceAndroid.NativeOnPointerEvent(eventType, moduleId = 1000, pointerId, x, y)` with the front panel scaled to 960x544.

## Things that were not obvious

- **Allocator**: the game's `operator new` tail-calls the EA pool allocator, which hands out uninitialised memory that Android's
  allocator would have zeroed (render objects read fields they never wrote). The four `operator new` variants are patched to call
  `memset` on the result. Host `malloc`/`realloc` zero too.
- **vitaGL**: texture parameters set before upload are reset by `glTexImage2D` (replayed afterwards); `glClear` ignores
  `glColorMask` (the game ends frames with a colour-masked clear, which blanked the screen); `GL_SHORT/BYTE/FIXED` arrays are converted.
- **Audio**: EA Audio Core mixes to 16-bit stereo and calls `AudioTrack.write(short[], off, len)` from its own thread. `Init(track, a,
  b, c)`: the 4th argument is the sample rate (the mixer sleeps `512 * 500 / rate` ms between blocks), `b * 512` is the array size.
  The mixer uses the ARM **VFP short-vector** mode (`FPSCR.LEN`), which the Vita's Cortex-A9 does not have.
- **Vita output**: reusing one buffer for `sceAudioOutOutput` crackles; four rotating 64-byte-aligned buffers are clean.
- **Memory**: `_newlib_heap_size_user` 208 MB, main thread stack 8 MB, guest threads at least 1 MB, `ATTRIBUTE2=12` (365 MB).

## VFP short-vector translation (`tools/vfpvec.py`)

The mixer sets `FPSCR.LEN` with an immediate (`vmrs; bic; orr #LEN<<16; vmsr`) and then runs ordinary-looking VFP data-processing
instructions that the hardware repeats LEN times over register banks. The tool finds every such region statically (27 regions, LEN 8
or 4), replaces each vector instruction by a branch to a cave that holds the LEN scalar instructions, and turns the `orr` into a nop.

Rules implemented (ARM ARM, VFPv2): `s0-s7` is the scalar bank; if the destination is there the operation is scalar; otherwise the
destination and first source step through their 8-register bank (wrapping), and the second source stays fixed if it is in bank 0.
**Conversions (`vcvt`), compares and loads/stores are always scalar** - expanding `vcvt` was the bug behind the "noise" in the first
audio builds (the wrap-around re-converted registers that were already floats).

## Performance notes

- Quiet, buffered logging by default; `verbose.txt` turns the firehose on.
- Missing-path cache: the game probes several non-existing paths before every asset.
- Whole-file RAM cache for read-only files up to 8 MB (48 MB total): one open/fstat/read/close per file, then `fread`/`fseek`
  from memory through `funopen`; the game seeks to the end and back for every size query, and each seek on a real `FILE` throws
  away the buffer and goes back to the memory card.

## Not done

Video playback (MPEG-4 Part 2 needs a transcode to H.264 for `sceAvPlayer`), a few GLES calls (`glLogicOp`, `glPointParameter*`,
`glSampleCoverage`, `glPointSizePointerOES`), on-screen error dialogs (errors go to `log.txt`).
