# SNES9x <-> Mesen2 save state converter

A native Win32 GUI for converting save states between SNES9x (`.009` /
`.oops`) and Mesen2 (`.mss`). **No template files needed** — both directions
synthesize the destination file from scratch using only the source file's
contents and what the two emulators' source code tells us about their save
state formats.

## How it works

**Forward (`.009` → `.mss`)**: Mesen2's deserializer (`Utilities/Serializer.cpp`)
is tolerant of missing keys — if a key isn't in the loaded state, the
corresponding field stays at its constructor default. So the converter
builds a fresh `.mss` with the SNES `ConsoleType=0` header and emits only
the (key, value) entries it can derive from the `.009`: bulk memory
(`workRam`, `vram`, `oamRam`, `cgram`), SPC RAM, 65C816 registers, PPU
registers, DMA channels, internal-register shadows ($4200/$4207/$420C…),
SMP state, and DSP regs. Everything else Mesen2 fills in from defaults.

**Reverse (`.mss` → `.009`)**: snes9x's loader (`snapshot.cpp::UnfreezeStateFromCopy`)
requires every standard section in a specific order, so the converter emits
the full required list (`NAM`, `CPU`, `REG`, `PPU`, `DMA`, `VRA`, `RAM`,
`SRA` if applicable, `FIL`, `SND`, `CTL`, `TIM`) at the exact v12 sizes
read from snes9x source. Each section is zero-initialized, then the same
field-overlay helpers from the forward direction (run inversely) write
back whatever the `.mss` carries.

## Building (Visual Studio 2022)

Three options, pick whichever you prefer:

1. **Open the .sln**: double-click `s9x_mss_gui.sln`. Targets `v143` toolset,
   C++17, /SUBSYSTEM:WINDOWS; x64 default and Win32 also available. Output
   goes to `build\$(Platform)\$(Configuration)\s9x_mss_gui.exe`.
2. **CMake (Open Folder)**: *File > Open > Folder...* on this directory —
   VS auto-detects `CMakeLists.txt`.
3. **Command-line**: open a "Developer Command Prompt for VS 2022" and run
   `build.bat`. Output is `s9x_mss_gui.exe` in the project root.

## Using it

The window has two drop zones:

- **Left (Side A)** — drop a `.009` or `.oops`; output is written next to it
  with `_from_snes9x` inserted before the `.mss` extension.
- **Right (Side B)** — drop a `.mss`; output is `_from_mesen2.009`.

Browse buttons exist for non-drag flow. After a successful conversion, the
"Open output folder" button opens Explorer with the output selected.

## Files

| File                | Purpose                                           |
| ------------------- | ------------------------------------------------- |
| `gui.cpp`           | Win32 GUI (two side-by-side drop zones)           |
| `convert.cpp`       | Forward + reverse conversion (from-scratch synth) |
| `s9x_format.cpp`    | `.009` / `.oops` snapshot parser/writer + decoders |
| `mss_format.cpp`    | `.mss` parser/writer + zlib helpers + sparse-emit |
| `s9x_mss.h`         | Shared declarations                               |
| `miniz.c` / `.h`    | Vendored single-file zlib replacement             |
| `s9x_to_mss.py`     | Reference Python port (the original forward tool) |
| `s9x_mss_gui.sln`   | Visual Studio 2022 solution                       |
| `s9x_mss_gui.vcxproj` / `.filters` | VS project                         |
| `CMakeLists.txt`    | CMake build alternative                           |
| `build.bat`         | Command-line MSVC build                           |

## Limitations

Both directions inherit the limitations called out at the top of
`s9x_to_mss.py`, plus an additional one from the from-scratch approach:

- Audio may glitch for a fraction of a second after load. The converter
  transfers SPC RAM + DSP regs + per-voice state (when ≥ 4 voices are
  active), but per-voice mid-BRR-decode state isn't fully preserved.
- Mid-frame HDMA / DMA in the source state won't transfer; the first frame
  after load may show one-frame artifacts.
- Only the standard SNES memory model is converted. Special chips (SuperFX,
  SA-1, DSP-1, etc.) aren't synthesized — converted states for these games
  may not load.
- Because no template carries Mesen2-internal fields, audio (DSP voice
  envelopes), the event scheduler queue, and mid-cycle state machines start
  at their boot defaults rather than mid-game state. Most games recover by
  the next frame; some might briefly stutter or visually glitch.
