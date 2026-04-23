# FFmpeg Fork

Streamlined FFmpeg build for Android, iOS, and macOS targets with mainstream
codecs only (H.264, HEVC, VP8/9, MPEG-4, MJPEG, AAC, MP3, Opus, FLAC, Vorbis,
ALAC, PCM) and the SMP/SRT/RTMP protocol stack.

## Building

```sh
clang -O2 -o build build.c
./build                  # default: macos-arm64
./build -j14             # 14 parallel workers
./build -v               # verbose (print every command)
./build clean            # remove all artifacts
./build macos-arm64
./build android-arm64
./build ios-arm64
```

Produces `ffmpeg`, `ffprobe`, `ffplay` (macOS only) in the repo root plus
per-library `.a` archives. Clean build on an M-series Mac: ~25s with 14
workers per target.

## Target status

| Target | Status | Produces |
|---|---|---|
| `macos-arm64` | ✅ full build (ffmpeg + ffprobe + ffplay) | Mach-O arm64 |
| `android-arm64` | ✅ ffmpeg + ffprobe (no ffplay; no SDL on Android) | ELF aarch64 |
| `ios-arm64` | ⚠️ compiles, needs iOS-built openssl to link | Mach-O arm64 |

### Paths to adjust in `build.c`

- `OPENSSL_MACOS` — homebrew openssl@3 prefix
- `OPENSSL_ANDROID` — directory with Android-built `lib/libcrypto.a`,
  `lib/libssl.a`, `include/openssl/`
- `ANDROID_NDK` + `ANDROID_API` — NDK toolchain

### iOS openssl

`ios-arm64` target links against `-lssl -lcrypto` but uses homebrew paths
by default. Cross-compile openssl for iOS (arm64) first, then point the
target's `.extra_ldflags` at it, and disable `-lssl -lcrypto` in homebrew
search paths.

## Dependencies

### macOS
- clang (Xcode command-line tools)
- openssl — homebrew `openssl@3` (for native SRT)
- SDL2 — for ffplay (set `build_ffplay = false` in target to drop)

### Android
- NDK 26+ (tested with 29.0.14033849)
- Openssl static libs cross-compiled for aarch64-linux-android
- `compat/android/stdio_shim.c` bridges `stderr`/`stdin`/`stdout` from
  older NDK stdio to bionic's `__sF[]`.

## Layout

- `build.c`, `build_srcs.h` — build driver + generated source lists.
- `config-<target>.h` — per-target configure output (HAVE_*, ARCH_*,
  OS_NAME, etc). `build.c` copies the chosen one to `config.h` before
  compilation.
- `config_components-<target>.h` — per-target component enable table.
- `libavdevice/indev_list-<target>.c`, `outdev_list-<target>.c` —
  per-target device list (macOS: AVFoundation + AudioToolbox;
  Android: lavfi only; iOS: AVFoundation only).
- `libavutil/avconfig.h`, `libavutil/ffversion.h` — frozen, target-agnostic.
- `libav*/`, `libsw*/`, `fftools/` — FFmpeg sources.
- `compat/` — platform shims (android, atomics, dispatch_semaphore, float,
  stdbit, va_copy).

## License

LGPL 2.1+. See `LICENSE.md`.
