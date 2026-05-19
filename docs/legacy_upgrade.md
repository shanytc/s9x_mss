# Legacy snes9x 1.5.x → Modern snes9x v12 upgrade (`#!snes9x:NNNN` → `#!s9xsnp:0012`)

Implemented in `convert.cpp::upgrade_legacy_s9x_state`. Reads a pre-v6
snes9x 1.5.x state (header `#!snes9x:1510` etc.), applies the legacy
section transforms in `s9x_format.cpp::upgrade_legacy_section` followed
by the standard v6→v12 promoter in `upgrade_section_to_v12`, then layers
in a series of state-side fixes derived empirically against actual
test states. The result is a modern v12 `.000` file that modern snes9x
builds (v1.62+) can load.

The same legacy state also goes through `convert_s9x_to_mss` for the
Mesen2 path, which has parallel fixes — the mss path tolerates
mid-init states more gracefully than snes9x.

## The chain of fixes

Loading a legacy mid-audio-init state in modern snes9x exposes a long
chain of subtle issues, each of which manifests as a different freeze
or render glitch until the underlying cause is found. The chain in
order of discovery:

### 1. SPC echo-loop unstick (`upgrade_legacy_s9x_state` step 7)

Legacy mid-upload states catch the CPU in audio-handshake spinloops
like `CMP $2140 / BNE -3`. Modern bAPU is cycle-precise; the legacy
state's SPC was in a tightly-synced echo dance with the CPU that
doesn't translate. CPU loops forever waiting on a byte the SPC will
never send.

Fix: plant a 26-byte SPC routine at apuram offset `$1947` that mirrors
every `cpu.registers[N]` byte into `apuram[$F4+N]` on every iteration.
Force `SMP.regs.pc = $1947`. CPU's handshake loops then satisfy on the
SPC's next pass through the echo loop (~30 SPC cycles). Audio dies
(SPC is no longer running the real audio engine), but the game runs.

```text
$1947  E5 F4 00   MOV A, !$F4    ; A = cpu.registers[0]
$194A  C5 F4 00   MOV !$F4, A    ; apuram[$F4] = A
... (repeats for $F5, $F6, $F7)
$195F  2F E6      BRA $1947
```

The same loop is planted in `convert_s9x_to_mss` for the mss path
(line ~675).

### 2. FillRAM `$4200` NMI enable (step 7a)

Legacy mid-audio-init typically captures `NMITIMEN=$00` (NMI disabled
while SPC upload is in progress). Modern snes9x then never vectors to
the game's NMI handler on vblank, so the game's main-loop never runs.

Fix: `FIL[$4200] |= $80` to force NMI on. The other bits (V/H-IRQ,
joypad-enable) are preserved.

### 3. APU timing accumulators (step 7b)

`spc::reference_time`, `spc::remainder`, `dsp.clock` at SND offsets
`66342..66353` are left over from the legacy capture. Modern snes9x
treats them as "how far behind the APU is from CPU.Cycles" and tries
to fast-forward the APU to catch up — generating tens of thousands of
samples in one shot, overflowing the audio buffer, and making
`CXAudio2::ProcessSound` block the main thread for 1000ms per scanline.

Fix: zero the 12-byte timing tail in SND.

### 4. PPU pre-CGDATA rebuild + CGDATA shift (step 7b'')

Legacy v6 snes9x stored VMA / WRAM / BG[N] with smaller field types
than modern v12 (likely `uint8` instead of `uint16` for the high-bit
VMA fields, `uint16` instead of `uint32` for `WRAM`). Net: legacy v6
pre-CGDATA fits in 58 bytes; modern v12 needs 64 bytes pre-CGDATA
(VMA + WRAM + BG + BGMode/.../CGADD + CGSavedByte).

Just inserting `CGSavedByte` at byte 63 (as `upgrade_section_to_v12`
does) leaves the legacy bytes 0..57 sitting where modern reads VMA /
WRAM / BG, but **with the wrong field sizes** — every field after the
first 1-2 bytes reads garbage.

Fix: rebuild bytes 0..63 of the v12 PPU section directly from FillRAM
registers (which are byte-correct and version-stable):

| Modern v12 PPU offset | Field           | Source                       |
| --------------------- | --------------- | ---------------------------- |
| 0                     | VMA.High        | `(FIL[$2115] >> 7) & 1`      |
| 1                     | VMA.Increment   | VMAIN bits 0-1 lookup table  |
| 2-3                   | VMA.Address     | `FIL[$2116] \| (FIL[$2117]<<8)` |
| 10-13                 | WRAM            | `FIL[$2181/$2182/$2183]`     |
| 14 + n×11             | BG[n].SCBase    | `(FIL[$2107+n] & 0x7C) << 8` |
| 21 + n×11             | BG[n].NameBase  | nibble of `FIL[$210B/$210C]` |
| 58                    | BGMode          | `FIL[$2105] & 7`             |
| 59                    | BG3Priority     | `(FIL[$2105] >> 3) & 1`      |
| 62                    | CGADD           | `FIL[$2121]`                 |
| 63                    | CGSavedByte     | 0                            |

After rebuilding, CGDATA[0..511] needs to land at modern v12 offset
64. Legacy stored CGDATA[0..4] at bytes 58-62 (raw legacy) and
CGDATA[5..511] at bytes 63-569; `upgrade_section_to_v12` shifts those
to modern bytes 58-62 and 64-570 with a `CGSavedByte` gap at byte 63.
To produce contiguous CGDATA at the correct modern offset:

```cpp
Bytes cgdata;
cgdata.insert(cgdata.end(), ppu.begin() + 58, ppu.begin() + 63);   // CGDATA[0..4]
cgdata.insert(cgdata.end(), ppu.begin() + 64, ppu.begin() + 64 + 507); // CGDATA[5..511]
memcpy(&ppu[64], cgdata.data(), 512);
```

Plus shift the post-CGDATA region (OBJ tables, OAM, etc.) forward by
5 bytes to compensate for the size growth in the pre-CGDATA region.

### 5. TIM section `WRAMRefreshPos` byte offsets (`s9x_format.cpp` line 86)

Original `upgrade_legacy_section` for `TIM` had:

```cpp
v6tim[37] = 0x02; v6tim[38] = 0x1A;  // intended: WRAMRefreshPos = 538
```

`WRAMRefreshPos` lives at v6 bytes 36-39 (uint32 BE). For value
`538 = $0000_021A`, the bytes should be `00 00 02 1A` — set at v6tim
indices 38 and 39, not 37 and 38. The bug placed `0x02` and `0x1A`
one byte too early, producing `$0002_1A00 = 137728` instead of `538`.

Result: the H-event scheduler stalls at `HC_WRAM_REFRESH_EVENT`
waiting for `CPU.Cycles >= 137728` (which doesn't happen within a
1364-cycle scanline). Each scanline takes the full 1,000,000-cycle
deadlock-detection budget instead of 1364. Emulator runs ~100× slower
than realtime and looks frozen.

Fix: `v6tim[38] = 0x02; v6tim[39] = 0x1A;` — correct BE byte positions.

### 6. Hardcode `PPU.ScreenHeight = 224` (step 7b')

Legacy v6 PPU's `ScreenHeight` field doesn't map cleanly to modern v12
offset 2641 (uint16 BE) due to field-size differences accumulating
through the section. Modern snes9x ends up reading garbage like
`$6403 = 25603` and the vblank-entry check
`if (V_Counter == ScreenHeight + 1)` never fires. With it never
firing, `SCAN_KEYS_FLAG` never gets set, `S9xMainLoop` never returns,
Windows marks the process as **Not Responding**.

Fix: hardcode `224` (or `239` if `FIL[$2133]` overscan bit is set).

### 7. Clear `CPU.Flags` (step 7c)

Legacy reused some `CPU.Flags` bits for different purposes than
modern. Notably bit 2 in legacy was some debugging flag; in modern
it's `SINGLE_STEP_FLAG`. Legacy state with `Flags=$14` translates to
modern as `SCAN_KEYS_FLAG | SINGLE_STEP_FLAG` and puts the emulator
into one-instruction-per-frame debugger mode — CPU advances 1-3 bytes
per second, looks frozen.

Fix: zero all of `CPU.Flags` on upgrade. None of the modern flag bits
are game-state (they're emulator-internal debugger / UI flags).

### 8. OBJ control fields (step 7b'''')

Modern v12 PPU section bytes `1984..1991` hold `OBJThroughMain`,
`OBJThroughSub`, `OBJAddition`, `OBJNameBase` (uint16 BE),
`OBJNameSelect` (uint16 BE), `OBJSizeSelect`. Legacy v6's smaller
per-OBJ struct (`SOBJ` was ~8 bytes vs modern 11) pushes `OBJControl`
~384 bytes earlier in the section. After our +5-byte post-CGDATA
shift, the bytes that land at modern offsets 1984+ are random
mid-OAMData garbage. snes9x reads `OBJNameBase=$0000` and sprites
draw with tile data from the wrong VRAM region.

Fix: derive `OBJNameBase / OBJNameSelect / OBJSizeSelect` from
`FIL[$2101]` using **snes9x's** formula (not Mesen2's):

```cpp
PPU.OBJNameBase   = (OBSEL & 3) << 14;        // (OBSEL & 3) * $4000
PPU.OBJNameSelect = ((OBSEL >> 3) & 3) << 13; // * $2000
PPU.OBJSizeSelect = (OBSEL >> 5) & 7;
```

Verified against a working modern state: `FIL[$2101] = $03` →
`OBJNameBase = $C000` (= `(3 & 3) << 14`), not `$6000`. Python tool's
Mesen2-direction formula `(OBSEL & 7) << 13` produces `$6000` for
the same input — different internal representation between emulators.

### 9. OBJ[128] zeroing (step 7b''')

Legacy v6's `SOBJ` struct fields don't map to modern v12's
11-byte-per-OBJ layout (legacy `HPos / VPos / Name` were `uint8` not
`int16/uint16/uint16`). After post-CGDATA shift, the bytes at modern
offsets 576..1983 (= OBJ[128]) are misaligned: `HFlip/VFlip` read
values like `$95` instead of 0/1, etc. Sprite rendering pipeline
gets garbage attribute bytes.

Fix: zero the 1408 bytes of OBJ table. Game's NMI handler does OAM
DMA from WRAM each vblank (typical SNES idiom), which repopulates
the OBJ table within one frame.

### 10. Mss-path BG layer keys (`apply_forward_ppu`)

For the mss path, `S9xPPUFields` decoded via `s9x_decode_ppu` reads
BG fields from PPU offsets that assume modern v12 layout (e.g.
`BG_SCBase[n]` at `14 + n*11`). For legacy states those offsets are
wrong. Derive from FillRAM instead when source is legacy:

```cpp
if (legacy) {
    scbase   = (FIL[0x2107+n] & 0xFC) << 8;
    scsize   = FIL[0x2107+n] & 0x03;
    namebase = (nibble of FIL[$210B/$210C]) << 12;
} else {
    scbase   = p.BG_SCBase[n];
    namebase = p.BG_NameBase[n];
    ...
}
```

Same for `ppu.bgMode` / `ppu.mode1Bg3Priority` from `FIL[$2105]`.

### 11. Mss-path CGDATA stitch (`s9x_ppu_cgdata_be`)

Mirror of fix #4 for the mss path. Stitch CGDATA[0..4] (bytes 58-62)
with CGDATA[5..511] (bytes 64-570) skipping CGSavedByte at byte 63.

## What works

| Behavior on resume                                  | Mid-init legacy state           |
| --------------------------------------------------- | ------------------------------- |
| Emulator doesn't freeze / hang / "Not Responding"   | ✓                               |
| UI is responsive                                    | ✓                               |
| CPU runs at full 60 fps                             | ✓                               |
| NMI fires each vblank                               | ✓                               |
| Main character sprite (OBJ) renders cleanly         | ✓                               |
| Player sprite (Yoshi for Y. Safari) renders cleanly | ✓                               |
| Palette (CGRAM) correct                             | ✓                               |
| BG tile maps point to correct VRAM addresses        | ✓                               |
| Audio plays                                         | ✗ (echo loop replaces SPC)      |
| Background layers render correctly                  | ✗ partial (mid-init limitation) |
| HUD bars / score / time display                     | ✗ (BG-rendered, see above)      |

## What doesn't, and why

For Yoshi's Safari mid-audio-init: even after the full fix chain,
loading the upgrade in modern snes9x produces a clean Bowser + Yoshi
sprite render but with **missing BG layers** (no boss arena walls,
no HUD). Forensic comparison against a known-good gameplay state
loaded in the same snes9x build shows:

| Region          | Upgrade hash | Working hash | Identical? |
| --------------- | ------------ | ------------ | ---------- |
| **CGRAM**       | 0xB78D22F0   | 0xB78D22F0   | yes        |
| TileCache[2bpp] | 0xD2063DC5   | 0xD2063DC5   | yes        |
| TileCache[8bpp] | 0x4D7705C5   | 0x4D7705C5   | yes        |
| **VRAM**        | 0x0A9DD0F8   | 0x39444781   | **no**     |
| WRAM            | 0x1F45C412   | 0x180EC086   | no         |
| OAM             | 0x4632B0A0   | 0x234255AB   | no         |
| FillRAM         | 0xB889DD4A   | 0x37F28568   | no         |
| TileCache[4bpp] | 0x30067223   | 0xF137F439   | **no**     |

(Hashes are FNV-1a 32-bit over the whole region; see `wsnes9x.cpp`
CPU debugger dialog on the `debugger` branch.)

VRAM differs even though the first 16 bytes at each BG's SCBase /
NameBase match — meaning the difference is in VRAM bytes we sampled
elsewhere. Most likely the boss-scene BG tile-graphics blocks that
the legacy save was taken **before the game had finished
uploading**. After load, the game continues executing but the code
path that does the BG VRAM upload has already moved on (the game
expected to have done it earlier and doesn't re-trigger). So VRAM
holds permanently-incomplete tile data; tiles render as palette
color 0 (blank); BG layers look empty.

The 4bpp tile cache hash mirrors this: BG1/BG2 cached tiles are
decoded from the incomplete VRAM, so they're cached as blanks.

This is **the fundamental mid-init limitation**: the legacy state
captures a moment where critical VRAM data hadn't been uploaded yet,
and no amount of state-side editing can manufacture that data.

## Recommended use

**For best results, save the legacy state during normal gameplay**
(not at title screen / not mid-audio-init). A gameplay save has
fully-populated VRAM, OAM, WRAM, and CPU at the game's main loop —
all fixes still apply, and rendering matches snes9x's render of an
equivalent native v12 state.

For mid-init states the upgrade tool does its best: emulator runs,
sprites render correctly, audio is sacrificed to escape the
cycle-precise SPC handshake, and BG rendering is best-effort.

## Debugging tools

A read-only modeless CPU debugger dialog was added to the snes9x
build (Emulation → Debugger…) on a separate `debugger` git branch.
It refreshes every 200 ms and shows:

- 65C816 state: PB:PC, peeked instruction bytes, A/X/Y/S/D/DB/P
- Cycles, V_Counter, PrevCycles
- SPC700 state: PC/A/X/Y/SP
- APU ports both directions (`apuram[$F4..$F7]` and `cpu.registers`)
- Live FillRAM PPU registers: INIDISP, OBSEL, BGMODE, BGnSC, NBA, TM,
  TS, TMW, NMITIMEN, HDMAEN — with decoded SCBase / NameBase
  addresses
- Live VRAM tilemap (first 16 bytes at each BG's SCBase)
- Live VRAM tile-graphics data (first 16 bytes at each BG's NameBase)
- Live CGRAM (first 8 colors)
- FNV-1a 32-bit hashes of whole VRAM, CGRAM, OAM, WRAM, FillRAM, and
  the `IPPU.TileCached[]` arrays

The hash row is what made the VRAM-divergence finding above possible
— it lets two states be compared "is this whole region identical?"
without dumping kilobytes of bytes.

Critical implementation note: instruction-byte peek uses
`Memory.Map[]` page lookup directly. `S9xGetByte()` routes through
the full CPU read path including `S9xDoHEventProcessing`, which is
**unsafe to call from the UI thread** — it causes reentrant event
processing → infinite recursion in the dialog. The same trap applies
to any UI-thread read of emulator state; access via the cached
page tables only.

## Specific fix locations

| Fix                                  | File                                              | Function / step                       |
| ------------------------------------ | ------------------------------------------------- | ------------------------------------- |
| SPC echo loop + force PC             | `convert.cpp::upgrade_legacy_s9x_state`           | step 7                                |
| SPC echo loop + force PC (mss path)  | `convert.cpp::convert_s9x_to_mss` ~line 675       | inline in SND handler                 |
| NMI enable                           | `convert.cpp::upgrade_legacy_s9x_state`           | step 7a                               |
| APU timing zero                      | `convert.cpp::upgrade_legacy_s9x_state`           | step 7b                               |
| PPU pre-CGDATA rebuild + shift       | `convert.cpp::upgrade_legacy_s9x_state`           | step 7b''                             |
| ScreenHeight = 224                   | `convert.cpp::upgrade_legacy_s9x_state`           | step 7b'                              |
| OBJ control fields                   | `convert.cpp::upgrade_legacy_s9x_state`           | step 7b''''                           |
| OBJ[128] zeroing                     | `convert.cpp::upgrade_legacy_s9x_state`           | step 7b'''                            |
| CPU.Flags clear                      | `convert.cpp::upgrade_legacy_s9x_state`           | step 7c                               |
| WRAMRefreshPos byte offsets          | `s9x_format.cpp::upgrade_legacy_section`          | line ~86                              |
| Mss-path BG layer keys               | `convert.cpp::apply_forward_ppu`                  | legacy branch in BG loop              |
| Mss-path CGDATA stitch               | `s9x_format.cpp::s9x_ppu_cgdata_be`               | legacy branch                         |
| Mss-path SPC PC override             | `convert.cpp::apply_forward_smp_legacy`           | `add_u16("spc.pc", 0x1947)`           |
| Mss-path OBJ-port zero               | `convert.cpp::convert_s9x_to_mss` ~line 708       | `spc.outputReg` legacy branch         |
