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

### Bonus: legacy snes9x 1.5.x → modern snes9x v12 upgrade

Side A also has an **Upgrade legacy → v12** button that only enables when the
dropped file uses the pre-v6 `#!snes9x:NNNN` header (snes9x 1.5.0 / 1.5.1
era). Current snes9x builds can't load those states directly; clicking the
button writes a `_upgraded` copy in the modern `#!s9xsnp:0012` format that
modern snes9x will load. Has nothing to do with mesen2 — pure snes9x format
migration.

What's preserved exactly: VRAM, WRAM, SRAM, OAM RAM, CGRAM, 65C816 registers,
SPC700 RAM. What's reconstructed (because legacy v6 structs had different
field sizes than modern v12): PPU pre-CGDATA fields (VMA / WRAM / BG[N]
config / BGMode / CGADD) — all derived from the FillRAM register snapshot.
PPU OBJ control fields (`OBJNameBase` / `OBJNameSelect` / `OBJSizeSelect`)
derived from `FillRAM[$2101]` using snes9x's `(val & 3) << 14` formula.

What's sacrificed: audio. The SPC's normal handshake with the CPU is
cycle-precise and not preserved across the legacy → modern format change,
so the converter plants a 26-byte SPC echo loop at apuram `$1947` and forces
`SMP.PC = $1947`. The loop mirrors `cpu.registers[N]` back to
`apuram[$F4+N]` so any CPU `CMP $2140 / BNE` audio-handshake spinloop
satisfies and the game runs. Real audio replay is dead until the game
re-uploads its SPC audio engine (most games don't).

Best with **gameplay states**, not mid-init / title-screen states. A
mid-init save captures the game before it has finished uploading some
VRAM data (BG tile graphics for the upcoming scene); after upgrade the
game runs cleanly but BG layers can render blank because that VRAM data
was never in the saved file. See `docs/legacy_upgrade.md` for the full
fix chain, the empirical findings, and the diagnostic CPU-debugger
dialog (separate `debugger` branch in the snes9x-latest repo).

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

For the **legacy → v12 upgrade path** specifically (separate from the
mesen2 conversions):

- Audio replay is sacrificed by design (echo-loop replaces the SPC's audio
  engine) to escape cycle-precise CPU↔SPC handshake spinloops that don't
  survive the format change.
- Mid-init / title-screen legacy states can produce playable but
  rendering-incomplete upgrades — sprites render correctly but BG layers
  can be blank if the legacy save happened before the game finished
  uploading scene BG tile graphics to VRAM. State-side editing can't
  manufacture VRAM data the legacy state didn't capture. See
  `docs/legacy_upgrade.md` for the empirical findings.
- Save during gameplay (post-init) for clean upgrades.
