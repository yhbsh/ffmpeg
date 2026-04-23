# FFmpeg Fork

Streamlined FFmpeg build for Android, iOS, and macOS targets with mainstream
codecs only (H.264, HEVC, VP8/9, MPEG-4, MJPEG, AAC, MP3, Opus, FLAC, Vorbis,
ALAC, PCM) and the SMP/SRT/RTMP protocol stack.

## Building

```sh
clang -O2 -o build build.c
./build               # defaults to macos-arm64
./build -j14          # use 14 parallel workers
./build -v            # verbose (print every command)
```

Produces `ffmpeg`, `ffprobe`, and `ffplay` in the repo root plus per-library
`.a` archives.

### Dependencies

- clang (or any C compiler in `CC`)
- openssl (required by native SRT)
- SDL2 (for ffplay; skip by editing `build.c` and setting `build_ffplay=false`)

## Layout

- `build.c` — single-file build driver. Replaces autoconf + Make.
- `build_srcs.h` — generated per-library source lists.
- `config.h`, `config_components.h`, `libavutil/avconfig.h` — frozen
  configuration for the mainstream codec/format/protocol set.
- `libav*/`, `libsw*/`, `fftools/` — FFmpeg sources.

## License

LGPL 2.1+. See `LICENSE.md`.
