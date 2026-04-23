# FFmpeg Fork

Streamlined FFmpeg build for Android, iOS, and macOS targets with mainstream
codecs only (H.264, HEVC, VP8/9, MPEG-4, MJPEG, AAC, MP3, Opus, FLAC, Vorbis,
ALAC, PCM) and the SMP/SRT/RTMP protocol stack.

## Building

```sh
clang -O2 -o build build.c
./build              # default: macos-arm64
./build -j14         # 14 parallel workers
./build -v           # verbose (print every command)
./build clean        # remove all artifacts
./build ios-arm64    # see caveats below
./build android-arm64
```

Produces `ffmpeg`, `ffprobe`, `ffplay` in the repo root plus per-library `.a`
archives. Clean build on an M-series Mac: ~25s with 14 workers.

### Dependencies (macOS)

- clang (Xcode command-line tools)
- openssl — homebrew `openssl@3` (for native SRT)
- SDL2 — for ffplay (drop `build_ffplay` in the target if unwanted)

### Cross-compile caveats

`config.h` and `config_components.h` are currently frozen for macOS arm64
(`ARCH_AARCH64=1`, macOS-specific `HAVE_*` values). To build for Android or
iOS you need target-specific replacements — run upstream FFmpeg's configure
on the right toolchain to regenerate, then swap the header in.

The Android and iOS targets in `build.c` are wired up with the correct
compiler paths and link flags; they just can't compile until a matching
`config.h` is in place.

Openssl locations are hardcoded in `build.c` under `OPENSSL_MACOS`,
`OPENSSL_ANDROID`, and `ANDROID_NDK` — adjust for your machine.

## Layout

- `build.c`, `build_srcs.h` — build driver + generated source lists.
- `config.h`, `config_components.h`, `libavutil/avconfig.h`,
  `libavutil/ffversion.h` — frozen configuration.
- `libavcodec/codec_list.c`, `libavformat/muxer_list.c`, etc. — frozen
  registry files listing the enabled components.
- `libav*/`, `libsw*/`, `fftools/` — FFmpeg sources.
- `compat/` — minimal platform shims (Android, atomics, dispatch_semaphore,
  float, stdbit, va_copy).

## License

LGPL 2.1+. See `LICENSE.md`.
