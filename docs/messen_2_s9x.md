# Mesen2 → SNES9x (`.mss` → `.009`)

Implemented in `convert.cpp::convert_mss_to_s9x` and the family of
`overlay_*` helpers it calls. snes9x's loader
(`snapshot.cpp::UnfreezeStateFromCopy`) requires every standard section in
a specific order, so the converter emits the full required list at exactly
the v12 sizes that snes9x reads — every section is zero-initialised first,
then overlay helpers stamp each field that the `.mss` carries.

## On-disk section layout (v12)

The output is a gzipped stream beginning with the magic header
`#!s9xsnp:0012\n`, followed by `<3-char tag>:<6 digit ascii size>:<bytes>`
sections in this exact order:

| Tag   | Size       | Source                          |
| ----- | ---------- | ------------------------------- |
| `NAM` | 1024       | ROM filename stem (info only)   |
| `CPU` | 48         | `overlay_cpu` + hard-coded defaults |
| `REG` | 16         | `overlay_reg`                   |
| `PPU` | 2652       | `overlay_ppu`                   |
| `DMA` | 152 (8×19) | `overlay_dma`                   |
| `VRA` | 65 536     | `ppu.vram` byte-identical       |
| `RAM` | 131 072    | `memoryManager.workRam` byte-identical |
| `SRA` | 131 072    | `cart.saveRam` (padded; emitted unconditionally to keep `CheckBlockName` happy) |
| `FIL` | 32 768     | `overlay_fil`                   |
| `SND` | 66 560     | `overlay_snd`                   |
| `CTL` | 91         | only `ver = 1` is written; rest defaults |
| `TIM` | 70         | hard-coded NTSC defaults; PAL switch if `vCounter > 261` |

## 65C816 CPU registers (`REG`, 16 B)

| `REG` offset | s9x field | mesen2 key                                   | Status |
| :----------: | --------- | -------------------------------------------- | :----: |
| 0            | `PB`      | `cpu.k`                                      |   ✓    |
| 1            | `DB`      | `cpu.dbr`                                    |   ✓    |
| 2-3          | `P.W`     | `cpu.ps`  +  bit 8 from `cpu.emulationMode`  |   ✓    |
| 4-5          | `A.W`     | `cpu.a`                                      |   ✓    |
| 6-7          | `D.W`     | `cpu.d`                                      |   ✓    |
| 8-9          | `S.W`     | `cpu.sp`                                     |   ✓    |
| 10-11        | `X.W`     | `cpu.x`                                      |   ✓    |
| 12-13        | `Y.W`     | `cpu.y`                                      |   ✓    |
| 14-15        | `PCw`     | `cpu.pc`                                     |   ✓    |

## CPU scheduling (`CPU`, 48 B)

| Offset | s9x field            | mesen2 source                            | Status |
| :----: | -------------------- | ---------------------------------------- | :----: |
| 0-3    | `Cycles`             | `memoryManager.hClock`                   |   ✓    |
| 4-7    | `PrevCycles`         | template default (0)                     |   ✗    |
| 8-11   | `V_Counter`          | `ppu.scanline`                           |   ✓    |
| 12-15  | `Flags`              | template default `SCAN_KEYS_FLAG` ($10)  |   ⚠    | Mesen2 doesn't carry s9x scheduler flags |
| 16-19  | `IRQPending`         | `cpu.irqSource ? 1 : 0`                  |   ✓    |
| 20-23  | `MemSpeed`           | template default `SLOW_ONE_CYCLE` (8)    |   ⚠    | s9x recomputes from PB on `S9xSetPCBase` |
| 24-27  | `MemSpeedx2`         | template default (16)                    |   ⚠    | same                                     |
| 28-31  | `FastROMSpeed`       | template default `FAST_ONE_CYCLE` (6)    |   ⚠    | derived from FIL[$420D] elsewhere        |
| 32-36  | `InDMA/InHDMA/InDMAorHDMA/InWRAMDMAorHDMA/HDMARanInDMA` | left 0                |   ⚠    | Snapshot loader resets these to 0 anyway |
| 37     | `WhichEvent`         | `memoryManager.nextEvent` → `HC_*` enum  |   ✓    | Mesen2 0/1/2/3 → s9x 4/6/2/3             |
| 38-41  | `NextEvent`          | `memoryManager.nextEventClock`           |   ✓    |
| 42     | `WaitingForInterrupt`| `cpu.waiOver` inverted                   |   ✓    |
| 43     | `NMIPending`         | `cpu.needNmi`                            |   ✓    |
| 44     | `IRQLine`            | template default (0)                     |   ✗    | Re-armed when s9x crosses `NextIRQTimer` |
| 45     | `IRQTransition`      | template default (0)                     |   ✗    |
| 46     | `IRQLastState`       | template default (0)                     |   ✗    |
| 47     | `IRQExternal`        | template default (0)                     |   ✗    |

## PPU (`PPU`, 2652 B)

The v12 PPU section was off by 11 bytes for the entire end-of-section block
before the most recent fixes (Mode 7 matrix, ForcedBlanking, FixedColour,
Brightness, ScreenHeight, HDMA, VRAMReadBuffer). All offsets below reflect the
**v12 layout** (= v6 offsets + 1 for everything past CGSavedByte, plus the
VRAMReadBuffer tail).

### VMA / BG / OAM register fields

| Offset      | s9x field                      | mesen2 source                                                                    | Status |
| ----------- | ------------------------------ | -------------------------------------------------------------------------------- | :----: |
| 2-3         | `VMA.Address`                  | `ppu.vramAddress`                                                                |   ✓    |
| 14+n·11+0   | `BG[n].SCBase`                 | `ppu.layers[N].tilemapAddress`                                                   |   ✓    |
| 14+n·11+2   | `BG[n].HOffset`                | `ppu.layers[N].hscroll`                                                          |   ✓    |
| 14+n·11+4   | `BG[n].VOffset`                | `ppu.layers[N].vscroll`                                                          |   ✓    |
| 14+n·11+6   | `BG[n].BGSize`                 | `ppu.layers[N].largeTiles`                                                       |   ✓    |
| 14+n·11+7   | `BG[n].NameBase`               | `ppu.layers[N].chrAddress`                                                       |   ✓    |
| 14+n·11+9   | `BG[n].SCSize`                 | bit 0 `doubleWidth` · bit 1 `doubleHeight`                                       |   ✓    |
| 58          | `BGMode`                       | `ppu.bgMode`                                                                     |   ✓    |
| 59          | `BG3Priority`                  | `ppu.mode1Bg3Priority`                                                           |   ✓    |
| 60          | `CGFLIP`                       | `ppu.cgramAddressLatch`                                                          |   ✓    |
| 61          | `CGFLIPRead`                   | template default (0)                                                             |   ✗    | Transient read-latch toggle |
| 62          | `CGADD`                        | `ppu.cgramAddress`                                                               |   ✓    |
| 63          | `CGSavedByte` (v11+)           | template default (0)                                                             |   ✗    | CGRAM write-pair scratch     |
| 64-575      | `CGDATA[256]` (uint16 BE)      | `ppu.cgram` byte-swapped from LE                                                 |   ✓    |
| 576-1983    | `OBJ[128]` cached structs      | rebuilt by s9x on `IPPU.OBJChanged = TRUE` (set by snapshot loader)              |   ✓    |
| post+0      | `OBJThroughMain`               | —                                                                                |   ✗    | Should derive from FIL[$212C/$212D] bits |
| post+1      | `OBJThroughSub`                | —                                                                                |   ✗    |
| post+2      | `OBJAddition`                  | —                                                                                |   ✗    |
| post+3      | `OBJNameBase` (uint16)         | `ppu.oamBaseAddress`                                                             |   ✓    |
| post+5      | `OBJNameSelect` (uint16)       | `ppu.oamAddressOffset`                                                           |   ✓    |
| post+7      | `OBJSizeSelect`                | —                                                                                |   ✗    | Derivable from `ppu.oamMode` |
| post+8      | `OAMAddr` (uint16)             | `ppu.oamRamAddress` ‖ (`enableOamPriority` ? $8000)                              |   ✓    |
| post+10     | `SavedOAMAddr` (uint16)        | mirror of `oamRamAddress`                                                        |   ✓    |
| post+12     | `OAMPriorityRotation`          | `ppu.enableOamPriority`                                                          |   ✓    |
| post+13     | `OAMFlip`                      | template default                                                                 |   ✗    | Write-pair toggle             |
| post+14     | `OAMReadFlip`                  | template default                                                                 |   ✗    | Read-pair toggle              |
| post+15     | `OAMTileAddress` (uint16)      | template default                                                                 |   ✗    | Internal OAM ptr              |
| post+17     | `OAMWriteRegister` (uint16)    | `ppu.oamWriteBuffer` (low byte only)                                             |   ✓    |
| 2003-2546   | `OAMData[544]`                 | `ppu.oamRam`                                                                     |   ✓    |
| 2547        | `FirstSprite`                  | `ppu.fetchSpriteStart` (truncated)                                               |   ✓    |
| 2548        | `LastSprite`                   | `ppu.fetchSpriteEnd` (truncated)                                                 |   ✓    |
| 2549        | `HTimerEnabled`                | `internalRegisters.enableHorizontalIrq`                                          |   ✓    |
| 2550        | `VTimerEnabled`                | `internalRegisters.enableVerticalIrq`                                            |   ✓    |
| 2551-2552   | `HTimerPosition` (int16)       | computed from `horizontalTimer` via `S9xUpdateIRQPositions` formula              |   ✓    |
| 2553-2554   | `VTimerPosition` (int16)       | `internalRegisters.verticalTimer`                                                |   ✓    |
| 2555-2556   | `IRQHBeamPos` (uint16)         | `internalRegisters.horizontalTimer`                                              |   ✓    |
| 2557-2558   | `IRQVBeamPos` (uint16)         | `internalRegisters.verticalTimer`                                                |   ✓    |
| 2559-2569   | `HBeamFlip` / `VBeamFlip` / `H/VBeamPosLatched` / `Gun{H,V}Latch` / `HVBeamCounterLatched` | template default | ✗ | Gun latch + beam counter latches; transient |
| 2570        | `Mode7HFlip`                   | `ppu.mode7.horizontalMirroring`                                                  |   ✓    |
| 2571        | `Mode7VFlip`                   | `ppu.mode7.verticalMirroring`                                                    |   ✓    |
| 2572        | `Mode7Repeat`                  | derived: `(largeMap << 1) \| fillWithTile0`, remap 1→0                           |   ✓    |
| 2573-2588   | Matrix A/B/C/D, CentreX/Y, M7HOFS/VOFS | `ppu.mode7.matrix[0..3]`, `centerX/Y`, `hscroll/vscroll`                  |   ✓    |
| 2589        | `Mosaic`                       | `ppu.mosaicSize - 1` (snes9x stores `$2106` high-nibble form)                    |   ✓    |
| 2590        | `MosaicStart`                  | 0 (per-frame scratch)                                                            |   ✓    |
| 2591-2594   | `BGMosaic[4]`                  | per-bit decode of `ppu.mosaicEnabled`                                            |   ✓    |
| 2595        | `Window1Left`                  | `ppu.window[0].left`                                                             |   ✓    |
| 2596        | `Window1Right`                 | `ppu.window[0].right`                                                            |   ✓    |
| 2597        | `Window2Left`                  | `ppu.window[1].left`                                                             |   ✓    |
| 2598        | `Window2Right`                 | `ppu.window[1].right`                                                            |   ✓    |
| 2599        | `RecomputeClipWindows`         | hard `1` (force s9x to rebuild caches)                                           |   ✓    |
| 2600+L·6+0  | `ClipCounts[L]`                | left 0 (recomputed by s9x)                                                       |   ✓    |
| 2600+L·6+1  | `ClipWindowOverlapLogic[L]`    | left 0 (recomputed by s9x)                                                       |   ✓    |
| 2600+L·6+2  | `ClipWindow1Enable[L]`         | `ppu.window[0].activeLayers[L]`                                                  |   ✓    |
| 2600+L·6+3  | `ClipWindow2Enable[L]`         | `ppu.window[1].activeLayers[L]`                                                  |   ✓    |
| 2600+L·6+4  | `ClipWindow1Inside[L]`         | `ppu.window[0].invertedLayers[L]`                                                |   ✓    |
| 2600+L·6+5  | `ClipWindow2Inside[L]`         | `ppu.window[1].invertedLayers[L]`                                                |   ✓    |
| 2636        | `ForcedBlanking`               | `ppu.forcedBlank`                                                                |   ✓    |
| 2637-2639   | `FixedColour{Red,Green,Blue}`  | `ppu.fixedColor` split (5/5/5)                                                   |   ✓    |
| 2640        | `Brightness`                   | `ppu.screenBrightness`                                                           |   ✓    |
| 2641-2642   | `ScreenHeight` (uint16)        | `ppu.overscanMode ? 239 : 224`                                                   |   ✓    |
| 2643        | `Need16x8Mulitply`             | template default (0)                                                             |   ✗    | Mode-7 byte-pair multiply gate; recomputed |
| 2644        | `BGnxOFSbyte`                  | template default (0)                                                             |   ✗    | BG scroll write-pair scratch                |
| 2645        | `M7byte`                       | `ppu.mode7.valueLatch` would map here                                            |   ✗    | Mode-7 matrix write-pair scratch — not currently set |
| 2646        | `HDMA`                         | `dmaController.hdmaChannels`                                                     |   ✓    |
| 2647        | `HDMAEnded`                    | template default (0)                                                             |   ✗    |
| 2648-2649   | `OpenBus1` / `OpenBus2`        | template default (0)                                                             |   ✗    |
| 2650-2651   | `VRAMReadBuffer` (uint16)      | `ppu.vramReadBuffer`                                                             |   ✓    |

## DMA / HDMA (`DMA`, 8 × 19 B)

| Offset (per ch) | s9x field             | mesen2 source                                          | Status |
| :-------------: | --------------------- | ------------------------------------------------------ | :----: |
| 0               | `ReverseTransfer`     | `dmaController.channel[N].invertDirection`             |   ✓    |
| 1               | `HDMAIndirectAddressing` | `dmaController.channel[N].hdmaIndirectAddressing`   |   ✓    |
| 2               | `UnusedBit43x0`       | `dmaController.channel[N].unusedControlFlag`           |   ✓    |
| 3               | `AAddressFixed`       | `dmaController.channel[N].fixedTransfer`               |   ✓    |
| 4               | `AAddressDecrement`   | `dmaController.channel[N].decrement`                   |   ✓    |
| 5               | `TransferMode`        | `dmaController.channel[N].transferMode`                |   ✓    |
| 6               | `BAddress`            | `dmaController.channel[N].destAddress`                 |   ✓    |
| 7-8             | `AAddress` (uint16)   | `dmaController.channel[N].srcAddress`                  |   ✓    |
| 9               | `ABank`               | `dmaController.channel[N].srcBank`                     |   ✓    |
| 10-11           | `DMACount` (uint16)   | `dmaController.channel[N].transferSize`                |   ✓    |
| 12              | `IndirectBank`        | `dmaController.channel[N].hdmaBank`                    |   ✓    |
| 13-14           | `Address` (uint16)    | `dmaController.channel[N].hdmaTableAddress`            |   ✓    |
| 15              | `Repeat`              | derived from `hdmaLineCounterAndRepeat`                |   ✓    |
| 16              | `LineCount`           | derived from `hdmaLineCounterAndRepeat`                |   ✓    |
| 17              | `UnknownByte`         | `dmaController.channel[N].unusedRegister`              |   ✓    |
| 18              | `DoTransfer`          | `dmaController.channel[N].doTransfer`                  |   ✓    |

## FillRAM shadows (`FIL`, 32 KB)

snes9x reads many PPU/CPU register shadows from `FillRAM` directly during
emulation (e.g. `gfx.cpp` reads `FillRAM[$212c]` to decide BG enable each
frame). The values below ARE the convenient short-cuts — for any byte
not overlaid here, FillRAM is zero-initialised, and snes9x will re-derive it
the first time the game writes the matching MMIO register.

| FIL addr   | mesen2 source                                                              | Status |
| ---------- | -------------------------------------------------------------------------- | :----: |
| `$2100`    | `ppu.forcedBlank` (bit 7) ‖ `ppu.screenBrightness` (low nibble)            |   ✓    |
| `$2101`    | `oamMode` · `oamAddressOffset` / 0x1000 · `oamBaseAddress` >> 13           |   ✓    |
| `$2105`    | `bgMode` · `mode1Bg3Priority` · per-layer `largeTiles`                     |   ✓    |
| `$2106`    | `mosaicEnabled` (low nibble) ‖ (`mosaicSize - 1`) (high nibble)            |   ✓    |
| `$2107-$210A` | `tilemapAddress` · `doubleWidth` · `doubleHeight` (BGn-SC)              |   ✓    |
| `$210B-$210C` | per-layer `chrAddress` packed as nibbles                                |   ✓    |
| `$2115`    | `vramIncrementValue` (→ low 2 bits) · `vramAddressRemapping` · `vramAddrIncrementOnSecondReg` |   ✓    |
| `$2116-$2117` | `ppu.vramAddress` (LE)                                                  |   ✓    |
| `$211A`    | mode7 `horizontalMirroring` · `verticalMirroring` · `fillWithTile0` · `largeMap` |   ✓    |
| `$2123`    | window[0/1] active/inverted bits for BG1+BG2                              |   ✓    |
| `$2124`    | window[0/1] active/inverted bits for BG3+BG4                              |   ✓    |
| `$2125`    | window[0/1] active/inverted bits for OBJ+COLOR                            |   ✓    |
| `$2126-$2127` | `ppu.window[0].left` / `.right`                                         |   ✓    |
| `$2128-$2129` | `ppu.window[1].left` / `.right`                                         |   ✓    |
| `$212C`    | `ppu.mainScreenLayers`                                                    |   ✓    |
| `$212D`    | `ppu.subScreenLayers`                                                     |   ✓    |
| `$212E`    | `ppu.windowMaskMain[0..4]` packed                                         |   ✓    |
| `$212F`    | `ppu.windowMaskSub[0..4]` packed                                          |   ✓    |
| `$2130`    | `directColorMode` · `colorMathAddSubscreen` · `colorMathPreventMode` · `colorMathClipMode` |   ✓    |
| `$2131`    | `colorMathEnabled` · `colorMathHalveResult` · `colorMathSubtractMode`     |   ✓    |
| `$2133`    | `screenInterlace` · `objInterlace` · `overscanMode` · `hiResMode`         |   ✓    |
| `$4200`    | `enableAutoJoypadRead` · `enableHorizontalIrq` · `enableVerticalIrq` · `enableNmi` |   ✓    |
| `$4201`    | `internalRegisters.ioPortOutput`                                          |   ✓    |
| `$4202-$4203` | ALU `multOperand1` / `multOperand2`                                    |   ✓    |
| `$4204-$4205` | ALU `dividend` (16-bit)                                                |   ✓    |
| `$4206`    | ALU `divisor`                                                             |   ✓    |
| `$4207-$4208` | `horizontalTimer` (LE)                                                 |   ✓    |
| `$4209-$420A` | `verticalTimer` (LE)                                                   |   ✓    |
| `$420C`    | `dmaController.hdmaChannels`                                              |   ✓    |
| `$420D`    | `internalRegisters.enableFastRom`                                         |   ✓    |
| `$4210`    | `internalRegisters.nmiFlag` (bit 7 in)                                    |   ✓    |
| `$4211`    | `internalRegisters.irqFlag` (bit 7 in)                                    |   ✓    |
| `$4214-$4215` | ALU `divResult`                                                        |   ✓    |
| `$4216-$4217` | ALU `multOrRemainderResult`                                            |   ✓    |
| `$4218-$421F` | —                                                                      |   ✗    | Auto-joypad shadows; s9x rewrites at `V == ScreenHeight + 3` |

## TIM section (70 B)

| Offset | Field                  | Source             | Status |
| :----: | ---------------------- | ------------------ | :----: |
| 0-3    | `H_Max_Master`         | hard `1364`        |   ⚠    | NTSC; PAL not auto-detected |
| 4-7    | `H_Max`                | hard `1364`        |   ⚠    | same                        |
| 8-11   | `V_Max_Master`         | `262` (NTSC) or `312` (PAL if `vCounter > 261` ‖ `scanline > 261` ‖ `verticalTimer > 261`) |   ⚠    | PAL detection is a heuristic; saves taken before V=262 in PAL boot are misclassified as NTSC |
| 12-15  | `V_Max`                | mirrors `V_Max_Master` |   ⚠    |
| 16-19  | `HBlankStart`          | hard `1096`        |   ✓    |
| 20-23  | `HBlankEnd`            | hard `4`           |   ✓    |
| 24-27  | `HDMAInit`             | hard `20`          |   ✓    |
| 28-31  | `HDMAStart`            | hard `1106`        |   ✓    |
| 32-35  | `NMITriggerPos`        | hard `0xFFFF`      |   ✓    | "no NMI pending"            |
| 36-39  | `WRAMRefreshPos`       | hard `538`         |   ✓    |
| 40-43  | `RenderPos`            | hard `512`         |   ✓    |
| 44     | `InterlaceField`       | template default   |   ✗    |
| 45-48  | `DMACPUSync`           | hard `18`          |   ✓    |
| 49-52  | `NMIDMADelay`          | hard `24`          |   ✓    |
| 53-56  | `IRQFlagChanging`      | hard `0`           |   ✓    |
| 57-60  | `APUSpeedup`           | hard `0`           |   ✓    |
| 61-64  | `IRQTriggerCycles`     | hard `14`          |   ✓    |
| 65     | `APUAllowTimeOverflow` | hard `0`           |   ✓    |
| 66-69  | `NextIRQTimer`         | hard `0x0FFFFFFF`  |   ✓    | "no IRQ scheduled"          |

## CTL section (91 B)

| Offset | Field                  | Source             | Status |
| :----: | ---------------------- | ------------------ | :----: |
| 0      | `ver`                  | hard `1`           |   ✓    |
| 1-2    | `port1_read_idx[2]`    | template (0)       |   ✗    | s9x rebuilds on next auto-joypad cycle |
| 7-8    | `port2_read_idx[2]`    | template (0)       |   ✗    | same                                   |
| 13-14  | `mouse_speed[2]`       | template (0)       |   ✗    |
| 15     | `justifier_select`     | template (0)       |   ✗    |
| 24     | `pad_read`             | template (0)       |   ✗    |
| 25     | `pad_read_last`        | template (0)       |   ✗    |
| 26-85  | `internal[60]`         | template (0)       |   ✗    |
| 86-90  | `internal_macs[5]`     | template (0)       |   ✗    |

## SND / APU (`SND`, 66 560 B)

`SPC_SAVE_STATE_BLOCK_SIZE = 1024 * 65`. Layout (from `apu/apu.cpp`):

| Offset      | Block                                              | mesen2 source                                                  | Status |
| ----------- | -------------------------------------------------- | -------------------------------------------------------------- | :----: |
| 0-65535     | `apuram` (64 KB)                                   | `spc.ram`, with `[0xF4..0xF7]` patched from `spc.outputReg[0..3]`, `[0xF1]` reconstructed from `spc.timer{0,1,2}.enabled` + `spc.romEnabled`, `[0xF2]` from `spc.dspReg` | ✓ |
| 65536-65699 | `SMP::save_state` (41 LE int32 = 164 B)            | `spc.pc/sp/a/x/y/dspReg/romEnabled` + per-timer fields + `cpu.ps` bits decomposed into P_N..P_C |   ✓    |
| 65700-65827 | DSP register block (128 B)                         | `spc.dsp.regs`                                                 |   ✓    |
| 65828-66131 | DSP voice state (8 voices × 38 B)                  | `spc.dsp.voices[N].{sampleBuffer,interpolationPos,brrAddress,envVolume,prevCalculatedEnv,bufferPos,brrOffset,keyOnDelay,envMode,envOut}` plus `brrOffset` coerced to a safe value (1, 3, 5, 7) to avoid the snes9x `SPC_DSP::voice_V3` assertion when default 0 / out-of-range | ✓ |
| 66132-66212 | Echo history + various `t_*` intermediates         | template (0)                                                   |   ✗    | Mesen2 doesn't expose these                          |
| 66213-66340 | `external_regs` (128 B)                            | `spc.dsp.externalRegs`                                         |   ✓    |
| 66342-66345 | `reference_time` (int32 LE)                        | template (0)                                                   |   ✗    | s9x cycle-tracking; recomputes                       |
| 66346-66349 | `remainder` (int32 LE)                             | template (0)                                                   |   ✗    |
| 66350-66353 | `dsp.clock` (int32 LE)                             | template (0)                                                   |   ✗    |
| 66354-66357 | `cpu.registers[4]` (= APU IN ports)                | `spc.cpuRegs[0..3]`                                            |   ✓    |
| 66358-66559 | zero padding                                       | —                                                              |   ✓    |

Coverage detail for the SMP state (41 LE int32s at offset 65536, schema in `s9x_mss.h`):

| Index field            | mesen2 source                  | Status |
| ---------------------- | ------------------------------ | :----: |
| `CLOCK`                | template default               |   ✗    |
| `OPCODE_NUMBER` / `OPCODE_CYCLE` | template default     |   ✗    | Mid-instruction position |
| `REG_PC`               | `spc.pc`                       |   ✓    |
| `REG_SP/A/X/Y`         | `spc.sp/a/x/y`                 |   ✓    |
| `P_N..P_C`             | bit-decomposed from `spc.ps`   |   ✓    |
| `STATUS_IPLROM_ENABLE` | `spc.romEnabled`               |   ✓    |
| `STATUS_DSP_ADDR`      | `spc.dspReg`                   |   ✓    |
| `STATUS_RAM00F8/00F9`  | template default               |   ✗    |
| `T{0,1,2}_ENABLE`      | `spc.timer{N}.enabled`         |   ✓    |
| `T{0,1,2}_TARGET`      | `spc.timer{N}.target`          |   ✓    |
| `T{0,1,2}_STAGE1`      | `spc.timer{N}.stage0`          |   ✓    |
| `T{0,1,2}_STAGE2`      | `spc.timer{N}.stage2`          |   ✓    |
| `T{0,1,2}_STAGE3`      | `spc.timer{N}.output`          |   ✓    |
| `RD/WR/DP/SP/YA/BIT`   | template default               |   ✗    | Mid-instruction latches |

## Coprocessors (not synthesised)

The reverse converter emits no `SFX`/`SA1`/`SAR`/`DP1`/`DP2`/`DP4`/`CX4`/
`OBC`/`OBM`/`ST0`/`ST1`/`ST2`/`S71`/`SRT`/`CLK`/`BSX`/`MSU` sections. snes9x's
loader tolerates their absence (each section's failure is gated on the
matching `Settings.*` flag), so plain-ROM games load cleanly. A `.mss` made
from a SuperFX/SA-1/DSP-N game will run with the coprocessor sitting at
snes9x's reset defaults, which is almost certainly wrong for anything beyond
the first frame.

## Known limitations

- **PAL/NTSC detection.** Mesen2 doesn't store the ROM region in the state
  blob — it derives region from the loaded ROM at runtime. Our `TIM` writes
  `V_Max=262` (NTSC) unless `vCounter`, `scanline`, or `verticalTimer`
  exceed 261. Saves captured during early scanlines of a PAL game will be
  misclassified as NTSC, making the SNES side run ~19 % too fast even though
  snes9x's host frame-pacing (driven by `Settings.PAL`) still targets 50 fps.
  Workaround: pass `--pal` on the command line, or read the ROM file's
  `$FFD9` region byte.

- **PrevCycles, scheduler flags, IRQ-edge state.** None of these are
  preserved in the Mesen2 state. snes9x re-derives them from FillRAM and the
  next event/scanline transition; in practice this means one frame of slight
  scheduling drift after load.

- **Mid-instruction state.** snes9x's `OPCODE_NUMBER` / `OPCODE_CYCLE` and
  the SMP's `RD/WR/DP/SP/YA/BIT` latches aren't in the Mesen2 state. Mesen2
  saves at instruction boundaries so this is harmless in practice.

- **Echo history & DSP `t_*` pipeline.** snes9x preserves the echo ring
  buffer and many mid-sample intermediates; Mesen2 doesn't expose them. The
  first echo cycle after load reads zeros until the buffer fills, producing
  a ~1-frame dry-out of echo/reverb.

- **Window cache rebuild.** Per-layer `ClipCounts` and `ClipWindowOverlapLogic`
  are left zero and `RecomputeClipWindows = 1` is asserted so snes9x rebuilds
  on first render.

- **OAM internal latches** (`OAMFlip`, `OAMReadFlip`, `OAMTileAddress`)
  start at 0. snes9x's `IPPU.OBJChanged = TRUE` (set by the snapshot loader)
  triggers a full OAMData re-parse on first render, so the visible sprite
  list is correct from frame 1.

- **CGRAM write-pair latch** (`CGSavedByte`) starts at 0. If a save was
  taken mid-`$2122` write pair, the next CGRAM write may use a stale low
  byte. Vanishingly rare in practice.
