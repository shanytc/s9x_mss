# SNES9x → Mesen2 (`.009` → `.mss`)

Implemented in `convert.cpp::convert_s9x_to_mss` (read path: `s9x_format.cpp`,
write path: `mss_format.cpp`). The Mesen2 deserializer (`Utilities/Serializer.cpp`)
defaults unknown keys to their constructor values, so the converter emits only
the (key, value) entries it can derive from the `.009` snapshot — Mesen2 fills
in the rest.

The on-disk `.mss` layout: `MSS` magic, 8 × `uint32 LE` header (versions /
console type / framebuffer dimensions / compressed-FB size), the compressed
preview framebuffer, a length-prefixed ROM name, then a zlib-compressed state
blob. The blob is a flat sequence of `<NUL-terminated key>` + `<uint32 LE size>`
+ `<size bytes value>` records.

## Bulk memory

| SNES9x source                       | Mesen2 destination                  | Status | Notes                                                  |
| ----------------------------------- | ----------------------------------- | :----: | ------------------------------------------------------ |
| `RAM` section (128 KB)              | `memoryManager.workRam`             |   ✓    | Byte-identical                                         |
| `VRA` section (64 KB)               | `ppu.vram`                          |   ✓    | Byte-identical                                         |
| `PPU.OAMData` (544 B)               | `ppu.oamRam`                        |   ✓    | Byte-identical                                         |
| `PPU.CGDATA` (512 B BE)             | `ppu.cgram`                         |   ✓    | uint16 byte-swap (s9x stores BE, Mesen2 LE)            |
| `SRA` section                       | `cart.saveRam`                      |   ✓    | Heuristic: low-diversity SRA → emit empty (no battery) |
| `SND[0..65535]`                     | `spc.ram`                           |   ✓    | Byte-identical                                         |

## 65C816 CPU registers

| SNES9x source             | Mesen2 destination                            | Status | Notes                                            |
| ------------------------- | --------------------------------------------- | :----: | ------------------------------------------------ |
| `REG.A.W`                 | `cpu.a`                                       |   ✓    | 16-bit                                           |
| `REG.X.W`                 | `cpu.x`                                       |   ✓    | 16-bit                                           |
| `REG.Y.W`                 | `cpu.y`                                       |   ✓    | 16-bit                                           |
| `REG.S.W`                 | `cpu.sp`                                      |   ✓    | 16-bit                                           |
| `REG.D.W`                 | `cpu.d`                                       |   ✓    | Direct page                                      |
| `REG.PCw`                 | `cpu.pc`                                      |   ✓    | 16-bit                                           |
| `REG.DB`                  | `cpu.dbr`                                     |   ✓    | Data bank                                        |
| `REG.PB`                  | `cpu.k`                                       |   ✓    | Program bank                                     |
| `REG.P.W` low byte        | `cpu.ps`                                      |   ✓    |                                                  |
| `REG.P.W` bit 8           | `cpu.emulationMode`                           |   ✓    | E flag extracted from status word                |

## CPU scheduling / interrupt state

| SNES9x source             | Mesen2 destination                            | Status | Notes                                                |
| ------------------------- | --------------------------------------------- | :----: | ---------------------------------------------------- |
| `CPU.V_Counter`           | `ppu.scanline`, `internalRegisters.vCounter`  |   ✓    |                                                      |
| `CPU.Cycles`              | `memoryManager.hClock`                        |   ✓    | Per-scanline cycle counter                           |
| `CPU.WhichEvent`          | `memoryManager.nextEvent`                     |   ✓    | s9x `HC_*` enum → Mesen2 `SnesEventType`             |
| `CPU.NextEvent`           | `memoryManager.nextEventClock`                |   ✓    |                                                      |
| `CPU.NMIPending`          | `cpu.needNmi`                                 |   ✓    |                                                      |
| `CPU.WaitingForInterrupt` | `cpu.waiOver` (inverted)                      |   ✓    | s9x stores "waiting", Mesen2 stores "completed"      |
| `CPU.IRQPending`          | `cpu.irqSource`, `cpu.prevIrqSource`          |   ✓    |                                                      |
| `FillRAM[$4210]`          | `internalRegisters.nmiFlag`                   |   ✓    | bit 7 only                                           |
| `FillRAM[$4211]`          | `internalRegisters.irqFlag`                   |   ✓    | bit 7 only                                           |
| —                         | `memoryManager.masterClock`                   |   ⚠    | Synthesized: `V_Counter * 1364 + (Cycles & 7)`       |
| `CPU.IRQLine` / `IRQTransition` / `IRQLastState` / `IRQExternal` | —          |   ✗    | s9x-only scheduling state                            |
| `CPU.InDMA` / `InHDMA` / `InDMAorHDMA` / `HDMARanInDMA`           | —          |   ✗    | Mid-cycle DMA state                                  |

## PPU registers (derived from `FillRAM` shadows)

| SNES9x source                  | Mesen2 destination                                                                  | Status |
| ------------------------------ | ----------------------------------------------------------------------------------- | :----: |
| `FillRAM[$2100]` INIDISP       | `ppu.forcedBlank`, `ppu.screenBrightness`                                           |   ✓    |
| `FillRAM[$2101]` OBSEL         | `ppu.oamMode`, `ppu.oamBaseAddress`, `ppu.oamAddressOffset`                         |   ✓    |
| `PPU.OAMAddr` (from `$2102/$2103`) | `ppu.oamRamAddress`, `ppu.enableOamPriority` (bit 15)                            |   ✓    |
| `FillRAM[$2105]` BGMODE        | `ppu.bgMode`, `ppu.mode1Bg3Priority`, `ppu.layers[N].largeTiles`                    |   ✓    |
| `FillRAM[$2106]` MOSAIC        | `ppu.mosaicSize`, `ppu.mosaicEnabled`                                               |   ✓    |
| `PPU.BG[0..3]` (snes9x struct) | `ppu.layers[N].tilemapAddress / chrAddress / hscroll / vscroll / doubleWidth / doubleHeight` | ✓ |
| `FillRAM[$2115]` VMAIN         | `ppu.vramIncrementValue`, `ppu.vramAddressRemapping`, `ppu.vramAddrIncrementOnSecondReg` |   ✓    |
| `PPU.VMA.Address`              | `ppu.vramAddress`                                                                   |   ✓    |
| `PPU.CGADD`                    | `ppu.cgramAddress`                                                                  |   ✓    |
| `FillRAM[$212C/$212D]`         | `ppu.mainScreenLayers`, `ppu.subScreenLayers`                                       |   ✓    |
| `FillRAM[$2130]` CGWSEL        | `ppu.directColorMode`, `ppu.colorMathAddSubscreen`, `ppu.colorMathClipMode`, `ppu.colorMathPreventMode` | ✓ |
| `FillRAM[$2131]` CGADSUB       | `ppu.colorMathEnabled`, `ppu.colorMathHalveResult`, `ppu.colorMathSubtractMode`     |   ✓    |
| `FillRAM[$2133]` SETINI        | `ppu.hiResMode`, `ppu.screenInterlace`, `ppu.objInterlace`, `ppu.overscanMode`      |   ✓    |
| `PPU.FixedColour{Red,Green,Blue}` | `ppu.fixedColor` (packed 15-bit BGR)                                             |   ✓    |
| `PPU.Matrix{A,B,C,D}`, `Centre{X,Y}`, `M7{HOFS,VOFS}` | `ppu.mode7.matrix[0..3]`, `centerX/Y`, `hscroll/vscroll` | ✓ |
| `PPU.HTimerEnabled` / `VTimerEnabled` | —                                                                          |   ✗    | Not emitted; Mesen2 derives from $4200 |
| `PPU.IRQHBeamPos` / `IRQVBeamPos` | —                                                                              |   ✗    | Not emitted; derived from FillRAM $4207-$420A |
| `PPU.Mode7{HFlip,VFlip,Repeat}`   | —                                                                              |   ✗    | Should map to `ppu.mode7.{horizontal,vertical}Mirroring`, `fillWithTile0`, `largeMap` |
| `PPU.Window1{Left,Right}` / `Window2{Left,Right}` | —                                                              |   ✗    | Should map to `ppu.window[N].left/.right` |
| `PPU.ClipWindow{1,2}{Enable,Inside}[0..5]` | —                                                                     |   ✗    | Should map to `ppu.window[N].activeLayers[L]` / `invertedLayers[L]` |
| `PPU.Mosaic` / `MosaicStart` / `BGMosaic[4]` | —                                                                  |   ✗    | Mosaic mask bits emitted via `$2106`; per-BG enable not re-emitted to Mesen2 keys |
| `PPU.{FirstSprite,LastSprite}` | —                                                                                   |   ✗    | Should map to `ppu.fetchSpriteStart/End` |
| `PPU.VRAMReadBuffer`           | —                                                                                   |   ✗    | Should map to `ppu.vramReadBuffer` |
| `PPU.SavedOAMAddr` / `OAMFlip` / `OAMReadFlip` / `OAMTileAddress` / `OAMWriteRegister` | — |   ✗    | Transient OAM-write latches |
| `PPU.CGFLIP` / `CGFLIPRead` / `CGSavedByte` | —                                                                      |   ✗    | CGRAM write-pair toggles |

## DMA / HDMA

| SNES9x source                          | Mesen2 destination                                                          | Status |
| -------------------------------------- | --------------------------------------------------------------------------- | :----: |
| `DMA[N].ReverseTransfer`               | `dmaController.channel[N].invertDirection`                                  |   ✓    |
| `DMA[N].HDMAIndirectAddressing`        | `dmaController.channel[N].hdmaIndirectAddressing`                           |   ✓    |
| `DMA[N].UnusedBit43x0`                 | `dmaController.channel[N].unusedControlFlag`                                |   ✓    |
| `DMA[N].AAddressFixed`                 | `dmaController.channel[N].fixedTransfer`                                    |   ✓    |
| `DMA[N].AAddressDecrement`             | `dmaController.channel[N].decrement`                                        |   ✓    |
| `DMA[N].TransferMode`                  | `dmaController.channel[N].transferMode`                                     |   ✓    |
| `DMA[N].BAddress`                      | `dmaController.channel[N].destAddress`                                      |   ✓    |
| `DMA[N].AAddress` / `ABank`            | `dmaController.channel[N].srcAddress` / `srcBank`                           |   ✓    |
| `DMA[N].DMACount`                      | `dmaController.channel[N].transferSize`                                     |   ✓    |
| `DMA[N].IndirectBank` / `Address`      | `dmaController.channel[N].hdmaBank` / `hdmaTableAddress`                    |   ✓    |
| `DMA[N].Repeat` + `LineCount`          | `dmaController.channel[N].hdmaLineCounterAndRepeat`                         |   ✓    | Recomposes `$43xA` raw byte incl. the two `LineCount=128` special cases |
| `DMA[N].DoTransfer` (HDMA active only) | `dmaController.channel[N].doTransfer` / `hdmaFinished`                      |   ✓    | Driven by `$420C` bit                                              |
| `DMA[N].UnknownByte`                   | `dmaController.channel[N].unusedRegister`                                   |   ✓    |
| `FillRAM[$420C]` HDMAEN                | `dmaController.hdmaChannels`                                                |   ✓    |
| `CPU.HDMARanInDMA`                     | —                                                                           |   ✗    | s9x-only HDMA-during-DMA tracking                                  |

## Internal registers / I/O

| SNES9x source            | Mesen2 destination                                                          | Status |
| ------------------------ | --------------------------------------------------------------------------- | :----: |
| `FillRAM[$4200]` NMITIMEN| `internalRegisters.enableNmi`, `enableVerticalIrq`, `enableHorizontalIrq`, `enableAutoJoypadRead` | ✓ |
| `FillRAM[$4201]` WRIO    | `internalRegisters.ioPortOutput`                                            |   ✓    |
| `FillRAM[$4207..$420A]`  | `internalRegisters.horizontalTimer`, `internalRegisters.verticalTimer`      |   ✓    |
| `FillRAM[$420D]` MEMSEL  | `internalRegisters.enableFastRom`                                           |   ✓    |
| `FillRAM[$4202..$4206]`  | `internalRegisters.aluMulDiv.{multOperand1,multOperand2,dividend,divisor}`  |   ✗    | Not emitted forward; left at Mesen2 default |
| `FillRAM[$4214..$4217]`  | `internalRegisters.aluMulDiv.{divResult,multOrRemainderResult}`             |   ✗    | Not emitted forward |
| `FillRAM[$4218..$421F]`  | `controlManager.controllerData[]` (or similar)                              |   ✗    | Auto-joypad shadows; Mesen2 re-reads on next scanline |

## SPC700 / APU

| SNES9x source                              | Mesen2 destination                                                  | Status |
| ------------------------------------------ | ------------------------------------------------------------------- | :----: |
| `SND[0xF4..0xF7]`                          | `spc.outputReg[0..3]`                                               |   ✓    | SPC→CPU port direction                                |
| Tail 4 bytes of SND (cpu.registers)        | `spc.cpuRegs[0..3]`, `spc.newCpuRegs[0..3]`                         |   ✓    | CPU→SPC port direction                                |
| SMP `REG_PC/A/X/Y/SP/PS`                   | `spc.pc/a/x/y/sp/ps`                                                |   ✓    | PSW reconstructed from P_N..P_C bits                  |
| SMP `STATUS_DSP_ADDR`                      | `spc.dspReg`                                                        |   ✓    |                                                       |
| `SND[$F1]` bit 0/1/2 + bit 7               | `spc.timersEnabled`, `spc.romEnabled`                               |   ✓    |                                                       |
| SMP `T{0,1,2}_{ENABLE,TARGET,STAGE1,STAGE2,STAGE3}` | `spc.timer{0,1,2}.{enabled,target,stage0,stage2,output}`   |   ✓    | s9x stage1/2/3 → Mesen2 stage0/2/output               |
| SND[65700..65827] (DSP regs)               | `spc.dsp.regs`                                                      |   ✓    | 128 bytes byte-identical                              |
| SND[~66213..66340] (external_regs)         | `spc.dsp.externalRegs`                                              |   ✓    | Located by SND-tail offset heuristic                  |
| SND voice block (38 B/voice × 8)           | `spc.dsp.voices[N].{sampleBuffer,interpolationPos,brrAddress,envVolume,prevCalculatedEnv,bufferPos,brrOffset,keyOnDelay,envMode,envOut,voiceBit}` | ⚠ | **Skipped if <4 voices active** to avoid corrupting mid-transition driver state |
| SMP `STATUS_IPLROM_ENABLE`                 | `spc.romEnabled`                                                    |   ✓    |                                                       |
| `SND[$F8] / $F9`                           | `spc.ram00f8`, `spc.ram00f9`                                        |   ✗    | SPC scratch bytes; not currently re-emitted to Mesen2 keys |
| SMP `RD/WR/DP/SP/YA/BIT`                   | —                                                                   |   ✗    | Mid-instruction latch state                           |
| SMP `OPCODE_NUMBER/OPCODE_CYCLE/CLOCK`     | —                                                                   |   ✗    | Mid-instruction position                              |
| DSP echo history                           | —                                                                   |   ✗    | Echo ring buffer not in s9x snapshot                  |
| DSP `t_*` intermediate registers           | —                                                                   |   ✗    | Mid-sample pipeline state                             |

## Coprocessors (not transferred)

| SNES9x source     | Mesen2 destination | Status |
| ----------------- | ------------------ | :----: |
| `SFX` (SuperFX)   | —                  |   ✗    |
| `SA1` / `SAR`     | —                  |   ✗    |
| `DP1` / `DP2` / `DP4` (DSP-1/2/4) | —  |   ✗    |
| `CX4` (Cx4)       | —                  |   ✗    |
| `OBC` / `OBM`     | —                  |   ✗    |
| `ST0` / `ST1` / `ST2` (Seta) | —       |   ✗    |
| `S71` (SPC7110)   | —                  |   ✗    |
| `SRT` / `CLK` (S-RTC) | —              |   ✗    |
| `BSX`             | —                  |   ✗    |
| `MSU` (MSU-1)     | —                  |   ✗    |

## Legacy `#!snes9x:` magic (snes9x 1.5.0 / 1.5.1)

snes9x 1.5.1 and earlier wrote save states with a different magic and section
layout. The converter accepts these on a best-effort basis:

| Detail                | Legacy (`#!snes9x:1510`)                                                | Modern (`#!s9xsnp:NNNN`)  |
| --------------------- | ------------------------------------------------------------------------ | ------------------------- |
| Header magic          | `#!snes9x:NNNN\n` (NNNN = 1500/1510/1520)                                | `#!s9xsnp:0006`–`0012`   |
| `CPU` size            | 58 bytes (trailing 2 dropped; first 16 bytes — Cycles/PrevCycles/V_Counter/Flags — are taken at face value, the rest is best-effort) | 48 bytes (v12) |
| `TIM` size            | 57 bytes (legacy = v6 minus trailing APUSpeedup; +4 B APUSpeedup→61 (v6), then v6→v12 chain) | 70 bytes (v12)            |
| `PPU` size            | 2649 bytes (v6-compatible)                                               | 2652 bytes (v12)          |
| Audio                 | 5 separate sections: `APU` (236 B), `ARE` (7 B), `ARA` (65 536 B), `SOU` (1281 B), `IAP` (45 B) | unified `SND` (66 560 B) |
| `GBJ` (Game-Boy joypad), `SHO` (preview screenshot) | present                                                 | absent                    |

How we handle it:

1. **Magic + version normalization.** `S9xState::load` accepts both magics. For
   legacy files the internal `version` is forced to 6 so the existing v6→v12
   section promoter handles the rest of the upgrade.
2. **Compatible-layout sections** (`REG`, `DMA`, `VRA`, `RAM`, `SRA`, `FIL`,
   `PPU`) are taken byte-identical. The `PPU`'s 2649-byte body is v6-compatible
   and goes through the same `CGSavedByte` + `VRAMReadBuffer` insertion path
   that other v6 states use.
3. **CPU (58 → 56 → 48 bytes).** Trailing two legacy bytes are dropped, then
   the v6→v12 promoter takes the result the rest of the way. The first 16
   bytes (`Cycles`, `PrevCycles`, `V_Counter`, `Flags`) map cleanly; the
   middle bytes (`IRQPending`/`MemSpeed`/…) come from legacy data that *may
   or may not* have the same field at the same offset — close enough for the
   first-frame re-derivation to fix up.
4. **TIM (57 → 61 → 70 bytes).** Legacy TIM lacks the trailing 4 bytes that v6
   adds (`APUSpeedup`); we append 4 zero bytes to reach v6's 61-byte size, then
   the v6→v12 promoter appends `IRQTriggerCycles=14`, `APUAllowTimeOverflow=0`,
   and `NextIRQTimer=0x0fffffff`. Result: scanline / event timing constants
   (`H_Max`, `V_Max`, `HBlankStart`, `HDMAInit`, `HDMAStart`, `WRAMRefreshPos`,
   etc.) are correct; `IRQTriggerCycles` is hard-coded rather than sourced from
   the state (snes9x 1.5.1 didn't store it).
5. **apuram (`ARA` → `SND[0..65535]`).** The 64 KB SPC memory is byte-identical
   between legacy and modern and is copied wholesale into the synthetic SND.
6. **SPC700 registers (`ARE` → `spc.pc/a/y/x/sp/ps`).** The 7-byte legacy ARE
   section is parsed as `PC` (uint16 BE) + `A` + `Y` + `X` + `SP` + `PSW`
   (the SAPURegisters union from snes9x 1.5.1). All six fields are written
   straight to the .mss so mesen2 resumes the SPC mid-execution instead of
   rebooting it via IPL ROM (which would clobber `apuram[$F4..$F7]` — the
   ports the game polls at `$2140-$2143`).
7. **SPC control byte (`APU[4]` → `spc.romEnabled`, `APU[7..10]` →
   `spc.outputReg[0..3]`).** From the SAPU struct: `ShowROM` flag, OutPorts
   array. `spc.timersEnabled` is set optimistically to 1.
8. **DSP voice / echo state, timer counters, IAPU internals.** Not extracted
   — mesen2 keeps its default DSP/timer state and the music driver re-fills
   it from apuram on the next tick. Audio may glitch for a fraction of a
   second after load.
9. **`GBJ` and `SHO` are dropped** — Game Boy joypad shadow and preview
   screenshot have no Mesen2 equivalent.
7. **`GBJ` and `SHO` are dropped** — Game Boy joypad shadow and preview
   screenshot have no Mesen2 equivalent.

Probe label: `SNES9x legacy v1.5.x (#!snes9x:1510) — best-effort conversion,
audio may glitch briefly`.

## Known limitations

- **Mid-transition audio.** Voice state is only emitted when ≥ 4 voices are
  active. Two-voice fade-outs and intro/handshake moments are skipped to avoid
  putting Mesen2's voice driver into an unreachable state.
- **WRAM init pattern.** snes9x fills uninitialised WRAM with one pattern,
  Mesen2 with another. The ~75 KB of WRAM the game never wrote to will differ
  between the converted state and a native Mesen2 save. Harmless — by definition
  the game doesn't read those bytes.
- **`memoryManager.masterClock`** is synthesised, not preserved (s9x doesn't
  store a master-cycle counter). The constraint Mesen2 enforces — `masterClock
  & 7 == hClock & 7` — is satisfied.
- **Coprocessor states aren't synced.** A `.mss` made from a SuperFX/SA-1/DSP-N
  game will run with the coprocessor at Mesen2's reset defaults, which is
  almost certainly wrong.
- **Mid-DMA / mid-HDMA cycle.** If snes9x's `CPU.InDMA` or `CPU.InHDMA` is set
  at save time, the CPU is paused mid-transfer. We don't propagate this; first
  frame after load may show one-frame DMA artifacts.
- **PPU window state** (left/right edges, per-layer active/inverted bits,
  clip-window cached counts) currently isn't emitted forward. If a Mesen2-side
  load reads `ppu.window[*]` keys directly rather than rebuilding from
  FillRAM register writes, the first rendered frame may have wrong window
  clipping until the game writes `$2123`/`$2124`/`$2125`/`$2126-$2129`.
