# Acheron — Queen

**AI Video Dubber** — a single-file GTK3 application in C that transcribes, translates, and re-dubs videos entirely offline/locally.

Version: **v1.0.0**
Source: `win-1.c` (~8700 lines, single translation unit)
Platforms: **Linux** (Arch, primary target) and **Windows** (MSYS2 UCRT64 / MINGW64)

---

## What it does

Drop in a video, press a button, get back a fully dubbed version with translated speech, optional burned-in subtitles, and background-music ducking.

The full pipeline:

1. **Extract audio** from video (FFmpeg → 16 kHz WAV).
2. **Transcribe** speech with OpenAI Whisper (tiny → large-v3).
3. **Translate** segments via one of four engines: Google Translate, Argos Translate (offline), Ollama, or Mistral.
4. **Synthesize** new speech with Microsoft Edge TTS (Khmer voices built-in: Sreymom / Piseth).
5. **Mix** dubbed audio back over the original video, with optional music ducking, subtitle burn-in, and logo overlay.

---

## Features

- **Full pipeline automation** — single-click run from video in → dubbed video out.
- **Speaker diarization + voice mapping** — auto-assigns female/male voices per detected speaker.
- **Auto-speed TTS** — re-times synthesized speech to fit each source segment.
- **Automated background-music ducking** — FFmpeg volume envelope dips BG audio during speech (with fade-in/out).
- **Burned subtitle support** — ASS format with configurable font size; choose original or translated text.
- **Live subtitle preview** — preview a single segment before committing the full render.
- **Batch processing** — queue multiple videos to process sequentially.
- **Logo overlay** — add watermark/logo to output.
- **Audio visualizer** — waveform view.
- **Merge tool** — splice clips together.
- **Multi-engine translation** — switch between Google, Argos (offline), Ollama, or Mistral.
- **Voice cloning hook** — pluggable script path for custom cloning backends.
- **Recent files** — last 5 opened videos surfaced in a menu.
- **GStreamer video preview** — embedded playback with timeline scrubbing.
- **Persistent config** — settings live in `~/.config/ai_dubber/config.json`.
- **Structured logging** — mutex-safe logs to `~/.local/share/ai_dubber/ai_dubber.log`.

---

## Dependencies

### Runtime (Arch Linux)

```bash
sudo pacman -S gtk3 gstreamer gst-plugins-good gst-plugins-bad ffmpeg python noto-fonts-extra
pip install openai-whisper deep-translator edge-tts --break-system-packages
```

### Runtime (Windows / MSYS2 UCRT64)

```bash
pacman -S mingw-w64-ucrt-x86_64-gtk3 \
          mingw-w64-ucrt-x86_64-gstreamer \
          mingw-w64-ucrt-x86_64-gst-plugins-good \
          mingw-w64-ucrt-x86_64-gst-plugins-bad \
          mingw-w64-ucrt-x86_64-ffmpeg \
          mingw-w64-ucrt-x86_64-python \
          mingw-w64-ucrt-x86_64-winpthreads-git
pip install openai-whisper deep-translator edge-tts
```

`bash.exe` (from MSYS2 or Git for Windows) must be on `PATH` — the dub/mix pipeline shells out to bash.

---

## Build

### Linux (bash)

```bash
gcc $(pkg-config --cflags gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0) \
    win-1.c -o queen \
    $(pkg-config --libs gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0) \
    -lpthread -lm -Wall -O2
```

### Linux (fish)

```fish
gcc (pkg-config --cflags gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 | string split " ") \
    win-1.c -o queen \
    (pkg-config --libs gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0 | string split " ") \
    -lpthread -lm -Wall -O2
```

### Windows (MSYS2 UCRT64)

```bash
gcc $(pkg-config --cflags gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0) \
    win-1.c -o queen.exe \
    $(pkg-config --libs gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0) \
    -lpthread -lm -Wall -O2
```

---

## Run

```bash
./queen
```

Then:

1. **Open** a video (mp4 / mkv / avi / mov / webm / flv).
2. Pick a **Whisper model**, **target language**, and **dub voice**.
3. Click **Transcribe → Translate → Dub**, or run the **Full Pipeline**.
4. Review segments in the table; double-click a row to preview.
5. **Export** the dubbed video.

Config and logs:
- Config: `~/.config/ai_dubber/config.json`
- Log: `~/.local/share/ai_dubber/ai_dubber.log`

---

## Project layout

```
.
├── win-1.c           # single-file source (this build)
└── README.md
```

Source is sectioned by banner comments (Section 1 — Constants, Section 3 — Logging, … through Section 19 — main). All cross-platform code is behind `#ifdef _WIN32` shims near the top of the file.

---

## Architecture notes

- **Single translation unit.** No build system, no headers. `gcc win-1.c` produces the binary.
- **Threading.** Long-running work (extract / transcribe / translate / TTS) runs on worker threads; progress is posted back to the GTK main loop via `g_idle_add`.
- **Subprocess model.** Python tools (Whisper, translators, edge-tts) are invoked via `g_spawn_sync` / bash scripts — no Python C-API embedding.
- **Paths.** On Windows, `TMPDIR` is resolved once at startup from `g_get_tmp_dir()` and normalized to forward slashes; every temp path is built with `snprintf("%s/...", TMPDIR, ...)`.
- **File I/O wrappers.** `xunlink`, `xchmod`, `xaccess` go through GLib for UTF-8 path safety on Windows.

---

## License

MIT License

Copyright (c) 2026 Acheron-HSR

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## Credits

- **OpenAI Whisper** — speech-to-text
- **deep-translator** — translation backends
- **Microsoft Edge TTS** — neural voices
- **FFmpeg** — audio/video processing
- **GStreamer** — video playback
- **GTK3** — UI toolkit
