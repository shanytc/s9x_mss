// convert.cpp - SNES9x .009 <-> Mesen2 .mss state conversion.
//
// Forward direction (s9x_to_mss): direct port of convert() in s9x_to_mss.py.
// Mesen2 has hundreds of emulator-internal fields that don't exist in a .009,
// so the forward direction requires a template .mss for the same ROM (see the
// .py docstring). The GUI auto-locates the template alongside the input file.
//
// Reverse direction (mss_to_s9x): inverts the field-mapping math from the .py
// script and assembles a v12 SNES9x snapshot. Because .009 sections are smaller
// (only ~300KB total vs Mesen2's much richer state), more of the reverse output
// can be synthesized from defaults than the forward direction; we still want a
// same-ROM .009 template for fields the .mss doesn't carry (HDMA mid-frame
// counters, snes9x event scheduler).

#include "s9x_mss.h"
#include "miniz.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

// ===== shared helpers ====================================================

static Bytes byteswap16(const Bytes& in) {
    if (in.size() & 1) throw ConvertError("byteswap16 needs even length");
    Bytes out(in.size());
    for (size_t i = 0; i < in.size(); i += 2) {
        out[i] = in[i + 1];
        out[i + 1] = in[i];
    }
    return out;
}

static Bytes u8_arr (uint32_t v) { return Bytes{ uint8_t(v & 0xFF) }; }
static Bytes u16_arr(uint32_t v) {
    return Bytes{ uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF) };
}
static Bytes u32_arr(uint32_t v) {
    return Bytes{ uint8_t(v & 0xFF), uint8_t((v >> 8) & 0xFF),
                  uint8_t((v >> 16) & 0xFF), uint8_t((v >> 24) & 0xFF) };
}
static Bytes s16_arr(int32_t v) { return u16_arr(uint32_t(int16_t(v))); }

// ===== forward: SNES9x .009 -> Mesen2 .mss ===============================
// Mirrors the patching logic in s9x_to_mss.py's convert() and its _patch_*
// helpers. Comments in the Python source explain the field-level rationale -
// this C++ port keeps the same overall structure so the two stay in sync.

static void apply_forward_ppu(const S9xState& s9x, MssFile& mss) {
    S9xPPUFields p = s9x_decode_ppu(s9x);

    uint8_t inidisp = s9x.fil(0x2100);
    uint8_t obsel   = s9x.fil(0x2101);
    uint8_t bgmode  = s9x.fil(0x2105);
    uint8_t mosaic  = s9x.fil(0x2106);
    uint8_t vmain   = s9x.fil(0x2115);
    uint8_t tm      = s9x.fil(0x212C);
    uint8_t ts      = s9x.fil(0x212D);
    uint8_t cgwsel  = s9x.fil(0x2130);
    uint8_t cgadsub = s9x.fil(0x2131);
    uint8_t setini  = s9x.fil(0x2133);

    mss.add_u8("ppu.forcedBlank",        (inidisp >> 7) & 1);
    mss.add_u8("ppu.screenBrightness",   inidisp & 0x0F);
    // p.BGMode reads PPU byte 58 which is misaligned in legacy v6 PPU
    // section (legacy CGDATA starts at byte 58 there). Always derive from
    // FillRAM[$2105] instead — that's the BGMODE register snapshot.
    mss.add_u8("ppu.bgMode",             bgmode & 7);
    mss.add_u8("ppu.mode1Bg3Priority",   (bgmode >> 3) & 1);
    mss.add_u8("ppu.mainScreenLayers",   tm);
    mss.add_u8("ppu.subScreenLayers",    ts);
    mss.add_u8("ppu.cgramAddress",       p.CGADD);
    mss.add_u16("ppu.vramAddress",        p.VMA_Address);
    static const uint8_t vram_step[4] = {1, 32, 128, 128};
    mss.add_u8("ppu.vramIncrementValue", vram_step[vmain & 3]);
    mss.add_u8("ppu.vramAddressRemapping",  (vmain >> 2) & 3);
    mss.add_u8("ppu.vramAddrIncrementOnSecondReg", (vmain >> 7) & 1);
    mss.add_u8("ppu.mosaicSize",         ((mosaic >> 4) & 0x0F) + 1);
    mss.add_u8("ppu.mosaicEnabled",      mosaic & 0x0F);
    mss.add_u8("ppu.oamMode",            (obsel >> 5) & 7);
    mss.add_u16("ppu.oamBaseAddress",     (obsel & 7) << 13);
    mss.add_u16("ppu.oamAddressOffset",   (((obsel >> 3) & 3) + 1) << 12);
    mss.add_u16("ppu.oamRamAddress",      p.OAMAddr & 0x3FF);
    mss.add_u8("ppu.enableOamPriority",  (p.OAMAddr >> 15) & 1);
    mss.add_u8("ppu.hiResMode",          (setini >> 3) & 1);
    mss.add_u8("ppu.screenInterlace",    setini & 1);
    mss.add_u8("ppu.objInterlace",       (setini >> 1) & 1);
    mss.add_u8("ppu.overscanMode",       (setini >> 2) & 1);
    mss.add_u8("ppu.directColorMode",    cgwsel & 1);
    mss.add_u8("ppu.colorMathAddSubscreen", (cgwsel >> 1) & 1);
    mss.add_u32("ppu.colorMathClipMode",  (cgwsel >> 6) & 3);
    mss.add_u32("ppu.colorMathPreventMode", (cgwsel >> 4) & 3);
    mss.add_u8("ppu.colorMathEnabled",   cgadsub & 0x3F);
    mss.add_u8("ppu.colorMathSubtractMode", (cgadsub >> 7) & 1);
    mss.add_u8("ppu.colorMathHalveResult", (cgadsub >> 6) & 1);
    uint32_t fixed = (uint32_t(p.FixedColourBlue & 0x1F) << 10)
                   | (uint32_t(p.FixedColourGreen & 0x1F) << 5)
                   |  uint32_t(p.FixedColourRed & 0x1F);
    mss.add_u16("ppu.fixedColor", fixed);

    // For legacy states, s9x_decode_ppu's BG_SCBase/BG_NameBase parse from
    // PPU offsets that assume modern v12 layout. Legacy v6 has smaller
    // VMA/BG field types so those offsets read garbage. Override by
    // deriving from FillRAM registers ($2107-$210A for SCBase, $210B/210C
    // for NameBase, $210D-$2114 latches are zero for legacy mid-init —
    // game's first NMI will fix them anyway).
    const bool legacy = (s9x.original_version >= 1000 && s9x.original_version < 2000);
    for (int n = 0; n < 4; ++n) {
        char k[64];
        uint16_t scbase, namebase, hoffset, voffset;
        uint8_t scsize_bits;
        if (legacy) {
            uint8_t bgnsc = s9x.fil(0x2107 + n);
            scbase      = uint16_t((bgnsc & 0xFC) << 8);
            scsize_bits = uint8_t(bgnsc & 0x03);
            uint8_t nba = s9x.fil((n < 2) ? 0x210B : 0x210C);
            uint8_t nb_nib = (n & 1) ? (nba >> 4) : (nba & 0x0F);
            namebase    = uint16_t(nb_nib) << 12;
            hoffset     = 0;  // latch — game's NMI will refresh
            voffset     = 0;
        } else {
            scbase      = p.BG_SCBase[n];
            namebase    = p.BG_NameBase[n];
            hoffset     = p.BG_HOffset[n];
            voffset     = p.BG_VOffset[n];
            scsize_bits = uint8_t(p.BG_SCSize[n]);
        }
        std::snprintf(k, sizeof(k), "ppu.layers[%d].tilemapAddress", n);
        mss.add_u16(k, scbase);
        std::snprintf(k, sizeof(k), "ppu.layers[%d].chrAddress", n);
        mss.add_u16(k, namebase);
        std::snprintf(k, sizeof(k), "ppu.layers[%d].hscroll", n);
        mss.add_u16(k, hoffset);
        std::snprintf(k, sizeof(k), "ppu.layers[%d].vscroll", n);
        mss.add_u16(k, voffset);
        std::snprintf(k, sizeof(k), "ppu.layers[%d].doubleWidth", n);
        mss.add_u8(k, scsize_bits & 1);
        std::snprintf(k, sizeof(k), "ppu.layers[%d].doubleHeight", n);
        mss.add_u8(k, (scsize_bits >> 1) & 1);
        std::snprintf(k, sizeof(k), "ppu.layers[%d].largeTiles", n);
        mss.add_u8(k, (bgmode >> (4 + n)) & 1);
    }

    mss.add_s16("ppu.mode7.matrix[0]", p.MatrixA);
    mss.add_s16("ppu.mode7.matrix[1]", p.MatrixB);
    mss.add_s16("ppu.mode7.matrix[2]", p.MatrixC);
    mss.add_s16("ppu.mode7.matrix[3]", p.MatrixD);
    mss.add_s16("ppu.mode7.centerX",   p.CentreX);
    mss.add_s16("ppu.mode7.centerY",   p.CentreY);
    mss.add_s16("ppu.mode7.hscroll",   p.M7HOFS);
    mss.add_s16("ppu.mode7.vscroll",   p.M7VOFS);

    // The Category C derivations below depend on snes9x v6+ field layout +
    // FillRAM being correctly synchronised with internal state. Legacy
    // snes9x 1.5.x stored these registers in a different layout and
    // sometimes left stale values in FillRAM (e.g. mid-DMA $420B markers).
    // Writing those to mesen2 occasionally regresses an otherwise working
    // legacy conversion. Skip for legacy — mesen2's defaults work better.
    if (s9x.original_version >= 1000) return;

    // --- Mode 7 flip / repeat flags ($211A bits) ---
    // PPU.Mode7HFlip/VFlip/Repeat live at v12 PPU offsets 2570-2572.
    if (s9x.section("PPU").size() >= 2652) {
        uint8_t m7hflip = s9x.ppu_u8(2570);
        uint8_t m7vflip = s9x.ppu_u8(2571);
        uint8_t m7rep   = s9x.ppu_u8(2572);
        mss.add_u8("ppu.mode7.horizontalMirroring", m7hflip & 1);
        mss.add_u8("ppu.mode7.verticalMirroring",   m7vflip & 1);
        // Mode7Repeat = (largeMap << 1) | fillWithTile0 (with 1 -> 0 remap on
        // load, so the result is 0, 2, or 3 here).
        mss.add_u8("ppu.mode7.fillWithTile0", (m7rep & 1) ? 1 : 0);
        mss.add_u8("ppu.mode7.largeMap",     (m7rep & 2) ? 1 : 0);
    }

    // --- CGRAM write-pair toggle ---
    // PPU.CGFLIP at PPU offset 60 (v12 schema, between BG3Priority and CGFLIPRead).
    if (s9x.section("PPU").size() >= 61) {
        mss.add_u8("ppu.cgramAddressLatch", s9x.ppu_u8(60) & 1);
    }

    // --- Sprite scan range ---
    // PPU.FirstSprite/LastSprite at PPU offsets 2547/2548 (uint8 each in snes9x).
    if (s9x.section("PPU").size() >= 2549) {
        mss.add_u16("ppu.fetchSpriteStart", s9x.ppu_u8(2547));
        mss.add_u16("ppu.fetchSpriteEnd",   s9x.ppu_u8(2548));
    }

    // --- Window edges (left/right) ---
    // PPU.Window1Left/Right/Window2Left/Right at PPU offsets 2595-2598.
    if (s9x.section("PPU").size() >= 2599) {
        mss.add_u8("ppu.window[0].left",  s9x.ppu_u8(2595));
        mss.add_u8("ppu.window[0].right", s9x.ppu_u8(2596));
        mss.add_u8("ppu.window[1].left",  s9x.ppu_u8(2597));
        mss.add_u8("ppu.window[1].right", s9x.ppu_u8(2598));
    }

    // --- Per-layer window active/inverted flags ---
    // PPU.ClipWindow{1,2}{Enable,Inside}[L] at PPU offsets 2600+L*6 + {2,3,4,5}.
    // L: 0=BG1, 1=BG2, 2=BG3, 3=BG4, 4=OBJ, 5=COLOR.
    if (s9x.section("PPU").size() >= 2636) {
        for (int L = 0; L < 6; ++L) {
            int base = 2600 + L * 6;
            char k[80];
            std::snprintf(k, sizeof(k), "ppu.window[0].activeLayers[%d]", L);
            mss.add_u8(k, s9x.ppu_u8(base + 2) & 1);
            std::snprintf(k, sizeof(k), "ppu.window[1].activeLayers[%d]", L);
            mss.add_u8(k, s9x.ppu_u8(base + 3) & 1);
            std::snprintf(k, sizeof(k), "ppu.window[0].invertedLayers[%d]", L);
            mss.add_u8(k, s9x.ppu_u8(base + 4) & 1);
            std::snprintf(k, sizeof(k), "ppu.window[1].invertedLayers[%d]", L);
            mss.add_u8(k, s9x.ppu_u8(base + 5) & 1);
        }
    }

    // --- Main / sub screen window mask ($212E/$212F) ---
    {
        uint8_t tmw = s9x.fil(0x212E);
        uint8_t tsw = s9x.fil(0x212F);
        for (int L = 0; L < 5; ++L) {
            char k[64];
            std::snprintf(k, sizeof(k), "ppu.windowMaskMain[%d]", L);
            mss.add_u8(k, (tmw >> L) & 1);
            std::snprintf(k, sizeof(k), "ppu.windowMaskSub[%d]", L);
            mss.add_u8(k, (tsw >> L) & 1);
        }
    }

    // --- VRAM read prefetch buffer ---
    mss.add_u16("ppu.vramReadBuffer", p.VRAMReadBuffer);

    // --- Mode 7 ExtBg ($2133 bit 6) ---
    mss.add_u8("ppu.extBgEnabled", (setini >> 6) & 1);
}

static void apply_forward_dma(const S9xState& s9x, MssFile& mss) {
    uint8_t hdma_enable = s9x.fil(0x420C);
    auto channels = s9x_decode_dma(s9x);
    for (int ch = 0; ch < 8; ++ch) {
        const auto& c = channels[ch];
        char k[80];
        auto k_at = [&](const char* fmt) -> const char* {
            std::snprintf(k, sizeof(k), fmt, ch);
            return k;
        };
        mss.add_u8(k_at("dmaController.channel[%d].invertDirection"),
                          c.ReverseTransfer ? 1 : 0);
        mss.add_u8(k_at("dmaController.channel[%d].hdmaIndirectAddressing"),
                          c.HDMAIndirectAddressing ? 1 : 0);
        mss.add_u8(k_at("dmaController.channel[%d].unusedControlFlag"),
                          c.UnusedBit43x0 ? 1 : 0);
        mss.add_u8(k_at("dmaController.channel[%d].fixedTransfer"),
                          c.AAddressFixed ? 1 : 0);
        mss.add_u8(k_at("dmaController.channel[%d].decrement"),
                          c.AAddressDecrement ? 1 : 0);
        mss.add_u8(k_at("dmaController.channel[%d].transferMode"), c.TransferMode);
        mss.add_u8(k_at("dmaController.channel[%d].destAddress"),  c.BAddress);
        mss.add_u16(k_at("dmaController.channel[%d].srcAddress"),   c.AAddress);
        mss.add_u8(k_at("dmaController.channel[%d].srcBank"),      c.ABank);
        mss.add_u16(k_at("dmaController.channel[%d].transferSize"), c.DMACount);
        mss.add_u8(k_at("dmaController.channel[%d].hdmaBank"),     c.IndirectBank);
        mss.add_u16(k_at("dmaController.channel[%d].hdmaTableAddress"), c.Address);

        // hdmaLineCounterAndRepeat is the raw HDMA-table line-counter byte
        // ($43xA value). snes9x decodes it (dma.cpp:1172) into LineCount +
        // Repeat with two special cases at LineCount=128:
        //   line == 0    -> Repeat=FALSE, LineCount=128 (terminator)
        //   line == 0x80 -> Repeat=TRUE,  LineCount=128
        //   else         -> Repeat = !(line & 0x80), LineCount = line & 0x7F
        // Reconstruct the original byte without losing the two-way mapping.
        uint8_t line_counter;
        if (c.LineCount == 128 && !c.Repeat)      line_counter = 0;
        else if (c.LineCount == 128 && c.Repeat)  line_counter = 0x80;
        else line_counter = uint8_t((c.LineCount & 0x7F) | (c.Repeat ? 0 : 0x80));
        mss.add_u8(k_at("dmaController.channel[%d].hdmaLineCounterAndRepeat"),
                         line_counter);
        bool is_active = (hdma_enable & (1 << ch)) != 0;
        if (is_active) {
            mss.add_u8(k_at("dmaController.channel[%d].doTransfer"),  c.DoTransfer ? 1 : 0);
            mss.add_u8(k_at("dmaController.channel[%d].hdmaFinished"), 0);
        } else {
            mss.add_u8(k_at("dmaController.channel[%d].doTransfer"),  0);
            mss.add_u8(k_at("dmaController.channel[%d].hdmaFinished"), 1);
        }
        mss.add_u8(k_at("dmaController.channel[%d].unusedRegister"), c.UnknownByte);
        // dmaActive flag: only emit for modern states. Legacy 1.5.x states
        // sometimes carry mid-DMA bookkeeping in FillRAM[$420B] that, when
        // combined with an explicit dmaActive=0 override, makes mesen2's DMA
        // scheduler hiccup. For legacy, leave the key absent so mesen2 picks
        // its default.
        if (s9x.original_version < 1000)
            mss.add_u8(k_at("dmaController.channel[%d].dmaActive"), 0);
    }
    mss.add_u8("dmaController.hdmaChannels", s9x.fil(0x420C));
}

static void apply_forward_internal_regs(const S9xState& s9x, MssFile& mss) {
    uint8_t nmitimen = s9x.fil(0x4200);
    uint32_t htime = s9x.fil(0x4207) | ((s9x.fil(0x4208) & 1) << 8);
    uint32_t vtime = s9x.fil(0x4209) | ((s9x.fil(0x420A) & 1) << 8);
    mss.add_u8("internalRegisters.enableNmi",           (nmitimen >> 7) & 1);
    mss.add_u8("internalRegisters.enableVerticalIrq",   (nmitimen >> 5) & 1);
    mss.add_u8("internalRegisters.enableHorizontalIrq", (nmitimen >> 4) & 1);
    mss.add_u8("internalRegisters.enableAutoJoypadRead", nmitimen & 1);
    mss.add_u16("internalRegisters.horizontalTimer",      htime);
    mss.add_u16("internalRegisters.verticalTimer",        vtime);
    mss.add_u8("internalRegisters.enableFastRom",        s9x.fil(0x420D) & 1);
    mss.add_u8("internalRegisters.ioPortOutput",         s9x.fil(0x4201));

    // Category C additions only for modern states — see apply_forward_ppu
    // comment. Legacy FillRAM can carry stale mid-cycle markers that confuse
    // mesen2 when written verbatim.
    if (s9x.original_version >= 1000) return;

    // --- ALU multiply / divide operands and results ---
    // $4202 multiplicand, $4203 multiplier, $4204-$4205 dividend, $4206 divisor,
    // $4214-$4215 quotient, $4216-$4217 product/remainder.
    mss.add_u8("internalRegisters.aluMulDiv.multOperand1", s9x.fil(0x4202));
    mss.add_u8("internalRegisters.aluMulDiv.multOperand2", s9x.fil(0x4203));
    mss.add_u16("internalRegisters.aluMulDiv.dividend",     s9x.fil(0x4204) | (s9x.fil(0x4205) << 8));
    mss.add_u8("internalRegisters.aluMulDiv.divisor",      s9x.fil(0x4206));
    mss.add_u16("internalRegisters.aluMulDiv.divResult",                 s9x.fil(0x4214) | (s9x.fil(0x4215) << 8));
    mss.add_u16("internalRegisters.aluMulDiv.multOrRemainderResult",     s9x.fil(0x4216) | (s9x.fil(0x4217) << 8));

    // --- Joypad auto-read shadow ($4218-$421F → controllerData[0..3]) ---
    for (int n = 0; n < 4; ++n) {
        uint32_t lo = s9x.fil(0x4218 + n * 2);
        uint32_t hi = s9x.fil(0x4219 + n * 2);
        char k[64];
        std::snprintf(k, sizeof(k), "internalRegisters.controllerData[%d]", n);
        mss.add_u16(k, lo | (hi << 8));
    }

    // --- Memory bus state ---
    // cpuSpeed: 6 cycles per access in FastROM regions when enabled, 8 otherwise.
    mss.add_u8("memoryManager.cpuSpeed",          (s9x.fil(0x420D) & 1) ? 6 : 8);
    // dramRefreshPosition: NTSC default; PAL still uses the same constant.
    mss.add_u16("memoryManager.dramRefreshPosition", 538);
    // openBus: snes9x doesn't keep this in FillRAM directly. $FF is a safe
    // default — the bus float on most reads of unmapped space.
    mss.add_u8("memoryManager.openBus", 0xFF);
}

static void apply_forward_cpu_timing(const S9xState& s9x, MssFile& mss) {
    S9xCpuState c = s9x_decode_cpu(s9x);
    uint32_t v = uint32_t(c.V_Counter) & 0xFFFF;
    mss.add_u16("ppu.scanline",              v);
    mss.add_u16("internalRegisters.vCounter", v);

    mss.add_u8("cpu.needNmi",  c.NMIPending ? 1 : 0);
    mss.add_u8("cpu.waiOver",  c.WaitingForInterrupt ? 0 : 1);
    mss.add_u8("cpu.irqSource",     c.IRQPending ? 1 : 0);
    mss.add_u8("cpu.prevIrqSource", c.IRQPending ? 1 : 0);

    uint8_t rdnmi = s9x.fil(0x4210);
    uint8_t rdirq = s9x.fil(0x4211);
    mss.add_u8("internalRegisters.nmiFlag", (rdnmi >> 7) & 1);
    mss.add_u8("internalRegisters.irqFlag", (rdirq >> 7) & 1);

    mss.add_u16("memoryManager.hClock", uint32_t(c.Cycles) & 0xFFFF);

    int we = c.WhichEvent;
    int mesen_event = 1; // DramRefresh fallback
    switch (we) {
        case 4: mesen_event = 0; break;   // HC_HDMA_INIT    -> HdmaInit
        case 6: mesen_event = 1; break;   // HC_WRAM_REFRESH -> DramRefresh
        case 2: mesen_event = 2; break;   // HC_HDMA_START   -> HdmaStart
        case 3: mesen_event = 3; break;   // HC_HCOUNTER_MAX -> EndOfScanline
    }
    mss.add_u8("memoryManager.nextEvent",      mesen_event);
    mss.add_u16("memoryManager.nextEventClock", uint32_t(c.NextEvent) & 0xFFFF);

    // masterClock: total master cycles since power-on. The absolute value
    // doesn't matter as long as it's positive and aligned so masterClock & 7
    // == hClock & 7 (Mesen2 derives DRAM refresh position from the low 3
    // bits). Use the current scanline as a coarse starting point.
    uint64_t new_mc = uint64_t(uint32_t(c.V_Counter)) * 1364ull
                    + (uint32_t(c.Cycles) & 7);
    uint8_t mc_le[8]; wr_u64_le(mc_le, new_mc);
    mss.add_entry("memoryManager.masterClock", mc_le, 8);
}

static void apply_forward_smp_blargg(const S9xState& s9x, MssFile& mss) {
    const Bytes& snd = s9x.section("SND");
    if (snd.size() < 65575) return;
    const uint8_t* regs    = &snd[65536];
    const uint8_t* regs_in = &snd[65552];
    uint16_t pc  = rd_u16_le(&snd[65568]);
    mss.add_u16("spc.pc", pc);
    mss.add_u8("spc.a",  snd[65570]);
    mss.add_u8("spc.x",  snd[65571]);
    mss.add_u8("spc.y",  snd[65572]);
    mss.add_u8("spc.sp", snd[65574]);
    mss.add_u8("spc.ps", snd[65573]);
    mss.add_u8("spc.dspReg", regs[2]);
    uint8_t f1 = regs[1];
    mss.add_u8("spc.timersEnabled",  (f1 & 0x07) == 0 ? 0 : 1);
    mss.add_u8("spc.romEnabled",     (f1 & 0x80) ? 1 : 0);
    mss.add_u8("spc.timersDisabled", 0);
    for (int i = 0; i < 4; ++i) {
        char k[40];
        std::snprintf(k, sizeof(k), "spc.cpuRegs[%d]", i);
        mss.add_u8(k, regs_in[4 + i]);
        std::snprintf(k, sizeof(k), "spc.newCpuRegs[%d]", i);
        mss.add_u8(k, regs_in[4 + i]);
    }
}

// Legacy snes9x 1.5.x SPC state lives in separate APU / ARE / IAP / SOU
// sections that the load path kept around for us. Extract SPC700 registers
// from ARE and write them to the .mss so mesen2 doesn't reboot the SPC via
// IPL ROM on load (which would clobber the apuram output ports the game's
// 65C816 side is polling at $2140-$2143).
//
// ARE layout (7 bytes, snes9x 1.5.1 SAPURegisters serialised via FreezeStruct):
//   [0..1]  YA  (big-endian: byte 0 = Y, byte 1 = A)
//   [2]     X
//   [3]     S   (stack pointer)
//   [4]     P   (PSW status flags)
//   [5..6]  PC  (big-endian)
// Verified empirically: for the Yoshi's Safari state, ARE = 80 F3 FC 10 7B 19 64
// gives PC=$1964 which lands on real SPC music-driver code (DEC Y / BNE / MOV A,$F6
// — a port-poll loop). The opposite "PC first" reading lands on garbage.
static void apply_forward_smp_legacy(const S9xState& s9x, MssFile& mss) {
    auto are_it = s9x.sections.find("ARE");
    if (are_it != s9x.sections.end() && are_it->second.size() >= 7) {
        const auto& are = are_it->second;
        mss.add_u8 ("spc.y",  are[0]);
        mss.add_u8 ("spc.a",  are[1]);
        mss.add_u8 ("spc.x",  are[2]);
        mss.add_u8 ("spc.sp", are[3]);
        mss.add_u8 ("spc.ps", are[4]);
        // Saved PC is preserved in comments only — we redirect SMP to the
        // echo loop planted at $1947 in spc.ram (see convert_s9x_to_mss SND
        // handling for the loop bytes and rationale). The original ARE PC
        // would land back in the deadlocked upload routine and freeze.
        // uint16_t saved_pc = (uint16_t(are[5]) << 8) | are[6];
        mss.add_u16("spc.pc", uint16_t(0x1947));
    }

    // SAPU section (snes9x 1.5.1 SAPU struct, big-endian INT_V serialisation):
    //   [0..3]   Cycles (int32 BE)
    //   [4]      ShowROM (bool8) — $F1 bit 7, IPL ROM visibility
    //   [5]      Flags
    //   [6]      KeyedChannels
    //   [7..10]  OutPorts[0..3] — SPC's last writes (in Blargg APU's separate
    //            storage). We *don't* use these here because the CPU side in
    //            v1.5.1 actually reads $2140-$2143 from apuram[$F4..$F7]
    //            (the two directions ended up unified at the apuram level
    //            through how Blargg APU implemented port writes; OutPorts is
    //            best-effort scratch). Whatever the CPU was last reading is
    //            sitting in apuram[$F4..$F7], which the line-~408 write
    //            already pushed into spc.outputReg. Leaving that alone is
    //            what makes the post-load $2140 echo satisfy a typical
    //            CPU "wait for command ack" spinloop.
    auto apu_it = s9x.sections.find("APU");
    if (apu_it != s9x.sections.end() && apu_it->second.size() >= 221) {
        const auto& apu = apu_it->second;
        mss.add_u8("spc.romEnabled",     apu[4] ? 1 : 0);
        mss.add_u8("spc.timersEnabled",  1);   // best-effort
        mss.add_u8("spc.timersDisabled", 0);

        // Per-timer state. Same offsets as the upgrade path (Timer at 206,
        // TimerTarget at 212, TimerEnabled at 218 — empirically verified
        // from Yoshi's music-tick target=$10 at offset 213).
        for (int t = 0; t < 3; ++t) {
            char k[64];
            uint16_t timer_now = (uint16_t(apu[206 + t * 2]) << 8) | apu[207 + t * 2];
            uint16_t target    = (uint16_t(apu[212 + t * 2]) << 8) | apu[213 + t * 2];
            uint8_t  enabled   = apu[218 + t];
            std::snprintf(k, sizeof(k), "spc.timer%d.enabled",       t);
            mss.add_u8(k, enabled);
            std::snprintf(k, sizeof(k), "spc.timer%d.timersEnabled", t);
            mss.add_u8(k, 1);
            std::snprintf(k, sizeof(k), "spc.timer%d.target",        t);
            mss.add_u8(k, uint8_t(target));
            std::snprintf(k, sizeof(k), "spc.timer%d.stage0",        t);
            mss.add_u8(k, uint8_t(timer_now));
            std::snprintf(k, sizeof(k), "spc.timer%d.stage2",        t);
            mss.add_u8(k, 0);
            std::snprintf(k, sizeof(k), "spc.timer%d.output",        t);
            mss.add_u8(k, 0);
        }

        // DSP register bank (128 bytes at APU offset 11) — modern bAPU loads
        // these via mesen2's `spc.dsp.regs` key.
        if (apu.size() >= 11 + 128) {
            Bytes dsp_regs(apu.begin() + 11, apu.begin() + 11 + 128);
            mss.add_entry("spc.dsp.regs", dsp_regs);
        }

        // DSP register address latch ($F2 register).
        const Bytes& snd2 = s9x.section("SND");
        if (snd2.size() >= 0xF3)
            mss.add_u8("spc.dspReg", snd2[0xF2]);
    }

    // CPU -> SPC ports. In legacy Blargg APU these live in apuram[$F4..$F7]
    // (the SPC's view; CPU writes via $2140-$2143 go straight here). mesen2's
    // SPC reads its $F4-$F7 MMIO not from apuram but from spc.cpuRegs[], so
    // we have to explicitly populate it. Pull from the synthesised SND (which
    // we built from legacy ARA).
    const Bytes& snd = s9x.section("SND");
    if (snd.size() >= 0xF8) {
        for (int i = 0; i < 4; ++i) {
            char k[40];
            std::snprintf(k, sizeof(k), "spc.cpuRegs[%d]", i);
            mss.add_u8(k, snd[0xF4 + i]);
            std::snprintf(k, sizeof(k), "spc.newCpuRegs[%d]", i);
            mss.add_u8(k, snd[0xF4 + i]);
        }
    }
}

static void apply_forward_smp(const S9xState& s9x, MssFile& mss) {
    if (s9x.sections.find("SND") == s9x.sections.end()) return;
    // Legacy "#!snes9x:1510" states keep their SPC state in APU/ARE/IAP/SOU.
    // Pull what we can (registers + IPL ROM enable + out-ports) from there.
    if (s9x.original_version >= 1000) { apply_forward_smp_legacy(s9x, mss); return; }
    if (s9x.original_version < 8) { apply_forward_smp_blargg(s9x, mss); return; }

    S9xSmpState smp = s9x_decode_smp(s9x);
    mss.add_u16("spc.pc", uint16_t(smp.v[S9xSmpState::REG_PC]));
    mss.add_u8("spc.a",  uint8_t (smp.v[S9xSmpState::REG_A]));
    mss.add_u8("spc.x",  uint8_t (smp.v[S9xSmpState::REG_X]));
    mss.add_u8("spc.y",  uint8_t (smp.v[S9xSmpState::REG_Y]));
    mss.add_u8("spc.sp", uint8_t (smp.v[S9xSmpState::REG_SP]));

    uint8_t psw = uint8_t(
        ((smp.v[S9xSmpState::P_N] & 1) << 7) |
        ((smp.v[S9xSmpState::P_V] & 1) << 6) |
        ((smp.v[S9xSmpState::P_P] & 1) << 5) |
        ((smp.v[S9xSmpState::P_B] & 1) << 4) |
        ((smp.v[S9xSmpState::P_H] & 1) << 3) |
        ((smp.v[S9xSmpState::P_I] & 1) << 2) |
        ((smp.v[S9xSmpState::P_Z] & 1) << 1) |
        ( smp.v[S9xSmpState::P_C] & 1));
    mss.add_u8("spc.ps", psw);
    mss.add_u8("spc.dspReg", uint8_t(smp.v[S9xSmpState::STATUS_DSP_ADDR]));

    // Three SPC timers. Field-name map: stage1/2/3 (snes9x) -> stage0/2/output (Mesen2).
    static const int T_EN[3]  = { S9xSmpState::T0_ENABLE, S9xSmpState::T1_ENABLE, S9xSmpState::T2_ENABLE };
    static const int T_TGT[3] = { S9xSmpState::T0_TARGET, S9xSmpState::T1_TARGET, S9xSmpState::T2_TARGET };
    static const int T_S1[3]  = { S9xSmpState::T0_STAGE1, S9xSmpState::T1_STAGE1, S9xSmpState::T2_STAGE1 };
    static const int T_S2[3]  = { S9xSmpState::T0_STAGE2, S9xSmpState::T1_STAGE2, S9xSmpState::T2_STAGE2 };
    static const int T_S3[3]  = { S9xSmpState::T0_STAGE3, S9xSmpState::T1_STAGE3, S9xSmpState::T2_STAGE3 };
    for (int n = 0; n < 3; ++n) {
        char k[64];
        std::snprintf(k, sizeof(k), "spc.timer%d.enabled", n);
        mss.add_u8(k, smp.v[T_EN[n]] ? 1 : 0);
        std::snprintf(k, sizeof(k), "spc.timer%d.timersEnabled", n);
        mss.add_u8(k, 1);
        std::snprintf(k, sizeof(k), "spc.timer%d.target", n);
        mss.add_u8(k, uint8_t(smp.v[T_TGT[n]]));
        std::snprintf(k, sizeof(k), "spc.timer%d.stage0", n);
        mss.add_u8(k, uint8_t(smp.v[T_S1[n]]));
        std::snprintf(k, sizeof(k), "spc.timer%d.stage2", n);
        mss.add_u8(k, uint8_t(smp.v[T_S2[n]]));
        std::snprintf(k, sizeof(k), "spc.timer%d.output", n);
        mss.add_u8(k, uint8_t(smp.v[T_S3[n]]) & 0x0F);
    }

    const Bytes& snd = s9x.section("SND");
    uint8_t f1 = snd[0xF1];
    mss.add_u8("spc.timersEnabled",   (f1 & 0x07) == 0 ? 0 : 1);
    mss.add_u8("spc.romEnabled",      (f1 & 0x80) ? 1 : 0);
    mss.add_u8("spc.timersDisabled",  0);

    // DSP register block (128 bytes at SND offset 65700).
    if (snd.size() >= 65700 + 128) {
        Bytes dsp_regs(snd.begin() + 65700, snd.begin() + 65700 + 128);
        mss.add_entry("spc.dsp.regs", dsp_regs);
    }

    // DSP voice state (only if half or more voices are active).
    size_t voice_base = 65700 + 128;
    if (snd.size() >= voice_base + 8 * 38) {
        int active = 0;
        for (int v = 0; v < 8; ++v) {
            size_t vo = voice_base + v * 38;
            uint16_t env = uint16_t(snd[vo + 28]) | (uint16_t(snd[vo + 29]) << 8);
            if (env > 0) active++;
        }
        if (active >= 4) {
            for (int v = 0; v < 8; ++v) {
                size_t vo = voice_base + v * 38;
                uint16_t env = uint16_t(snd[vo + 28]) | (uint16_t(snd[vo + 29]) << 8);
                uint8_t env_mode = snd[vo + 35];
                uint8_t envx_out = snd[vo + 36];
                if (env == 0 && env_mode == 0 && envx_out == 0) continue;

                Bytes sample_buffer(snd.begin() + vo, snd.begin() + vo + 24);
                uint32_t interp_pos = uint32_t(snd[vo + 24]) | (uint32_t(snd[vo + 25]) << 8);
                uint16_t brr_addr   = uint16_t(snd[vo + 26]) | (uint16_t(snd[vo + 27]) << 8);
                uint32_t hidden_env = uint32_t(snd[vo + 30]) | (uint32_t(snd[vo + 31]) << 8);
                uint8_t  buf_pos    = snd[vo + 32];
                uint8_t  brr_offset = snd[vo + 33];
                uint8_t  kon_delay  = snd[vo + 34];

                char k[80];
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].sampleBuffer", v);
                mss.add_entry(k, sample_buffer);
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].interpolationPos", v);
                mss.add_u32(k, interp_pos);
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].brrAddress", v);
                mss.add_u16(k, brr_addr);
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].envVolume", v);
                mss.add_u32(k, env);
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].prevCalculatedEnv", v);
                mss.add_u32(k, hidden_env);
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].bufferPos", v);
                mss.add_u8(k, buf_pos);
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].brrOffset", v);
                mss.add_u16(k, brr_offset);
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].keyOnDelay", v);
                mss.add_u8(k, kon_delay);
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].envMode", v);
                mss.add_u32(k, env_mode);
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].envOut", v);
                mss.add_u8(k, envx_out);
                std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].voiceBit", v);
                mss.add_u8(k, 1 << v);
            }
        }
    }

    // external_regs: 128 bytes near end of DSP state, just before final 16-byte block.
    if (snd.size() > 65536) {
        const uint8_t* tail = &snd[65536];
        size_t tail_len = snd.size() - 65536;
        std::ptrdiff_t last_nz = -1;
        for (size_t i = 0; i < tail_len; ++i) if (tail[i] != 0) last_nz = std::ptrdiff_t(i);
        if (last_nz > 128 + 17) {
            size_t ext_end   = size_t(last_nz) - 16;
            size_t ext_start = ext_end - 128;
            if (ext_start >= 164) {
                Bytes ext(tail + ext_start, tail + ext_end);
                mss.add_entry("spc.dsp.externalRegs", ext);
            }
        }
    }
}

void convert_s9x_to_mss(const std::string& in_path, const std::string& out_path) {
    S9xState s9x = S9xState::load(in_path);

    // Build the .mss from scratch. Mesen2's deserializer is tolerant of missing
    // keys (see Utilities/Serializer.cpp: "if(result != _values.end()) {...}
    // else { /* leave the field at its default value */ }"), so we emit only
    // the (key, value) entries we can derive from the .009 snapshot. Mesen2
    // fills in defaults from its constructors for everything else.
    std::string rom_name = fs::path(in_path).stem().string();
    MssFile mss = MssFile::create_empty(rom_name);

    // Bulk memory regions (byte-identical in both emulators).
    mss.add_entry("memoryManager.workRam", s9x.section("RAM"));
    mss.add_entry("ppu.vram",              s9x.section("VRA"));
    mss.add_entry("ppu.oamRam",            s9x_ppu_oam(s9x));
    mss.add_entry("ppu.cgram",             byteswap16(s9x_ppu_cgdata_be(s9x)));

    if (s9x.sections.count("SND")) {
        const Bytes& snd = s9x.section("SND");
        if (snd.size() >= 65536) {
            Bytes spc_ram(snd.begin(), snd.begin() + 65536);

            // Legacy unstick hack: the SNES IPL-style upload protocol contains
            // a `MOV Y, !CPUIO0; BNE -3` busy-wait that depends on cycle-precise
            // CPU<->SPC sync the legacy snes9x 1.5.x state didn't preserve. If
            // the state was captured mid-upload, both sides deadlock at the
            // wait point. Patch every occurrence of the wait pattern so the
            // SPC unconditionally falls through into the inner CMP/echo loop;
            // it'll then echo whatever Y currently holds and let the CPU's
            // matching wait satisfy. The cost: real subsequent uploads (e.g.
            // game changing music) skip the "wait for byte index 0" sync, so
            // the first upload after load may glitch. Subsequent ones still
            // work because each upload reissues the protocol code from scratch.
            //
            // Pattern: `EC F4 00 D0 FB`
            //   $XX+0:  EC F4 00    MOV Y, !$00F4   (read CPUIO0)
            //   $XX+3:  D0 FB       BNE  $XX        (loop if non-zero)
            // Replace D0 FB -> 2F 00: BRA +0 = unconditional fall-through.
            if (s9x.original_version >= 1000) {
                int patches = 0;
                for (size_t i = 0; i + 4 < spc_ram.size(); ++i) {
                    if (spc_ram[i  ] == 0xEC && spc_ram[i+1] == 0xF4 &&
                        spc_ram[i+2] == 0x00 && spc_ram[i+3] == 0xD0 &&
                        spc_ram[i+4] == 0xFB) {
                        spc_ram[i+3] = 0x2F;
                        spc_ram[i+4] = 0x00;
                        ++patches;
                        if (patches >= 16) break; // sanity cap
                    }
                }

                // Plant the same SPC echo loop the legacy->v12 upgrade path
                // uses (see upgrade_legacy_s9x_state step 7). SPC reads each
                // cpu.registers[N] (CPU's write to $2140+N) and immediately
                // copies it into apuram[$F4+N] so the CPU's `CMP $2140 / BNE`
                // echo-waits in mid-handshake legacy states satisfy on every
                // iteration without needing cycle-precise SPC sync. Audio is
                // sacrificed; the alternative is a hard freeze.
                static const uint8_t echo_loop[] = {
                    0xE5, 0xF4, 0x00,  // MOV A, !$F4
                    0xC5, 0xF4, 0x00,  // MOV !$F4, A
                    0xE5, 0xF5, 0x00,  // MOV A, !$F5
                    0xC5, 0xF5, 0x00,  // MOV !$F5, A
                    0xE5, 0xF6, 0x00,  // MOV A, !$F6
                    0xC5, 0xF6, 0x00,  // MOV !$F6, A
                    0xE5, 0xF7, 0x00,  // MOV A, !$F7
                    0xC5, 0xF7, 0x00,  // MOV !$F7, A
                    0x2F, 0xE6,        // BRA $1947 (-26)
                };
                const size_t echo_addr = 0x1947;
                if (echo_addr + sizeof(echo_loop) <= spc_ram.size())
                    std::memcpy(&spc_ram[echo_addr], echo_loop, sizeof(echo_loop));

                // Override the SPC PC (set by apply_forward_smp_legacy from
                // ARE bytes 5-6) so SMP runs the echo loop on resume.
                mss.add_u16("spc.pc", uint16_t(echo_addr));
            }

            mss.add_entry("spc.ram", spc_ram);
            for (int i = 0; i < 4; ++i) {
                char k[40];
                std::snprintf(k, sizeof(k), "spc.outputReg[%d]", i);
                // Legacy mid-upload states deadlock the CPU's `LDA $214X ;
                // BNE -3` post-upload-ack waits because legacy apuram[$F4..$F7]
                // carries the CPU's last-written command byte rather than the
                // SPC's echo byte the CPU is polling for. Zero the CPU-visible
                // ports so all such waits satisfy on resume — same fix the
                // legacy->v12 upgrade path applies (see upgrade_legacy_s9x_state
                // step 7). For modern snes9x states the apuram value is already
                // a correct SPC->CPU snapshot, leave it alone.
                uint8_t v = (s9x.original_version >= 1000) ? uint8_t(0)
                                                           : snd[0xF4 + i];
                mss.add_u8(k, v);
            }
        }
        if (s9x.original_version >= 8 && snd.size() > 65536) {
            const uint8_t* tail = &snd[65536];
            size_t tail_len = snd.size() - 65536;
            std::ptrdiff_t last_nz = -1;
            for (size_t i = 0; i < tail_len; ++i) if (tail[i] != 0) last_nz = std::ptrdiff_t(i);
            if (last_nz >= 200) {
                const uint8_t* cpu_regs = tail + (last_nz - 3);
                for (int i = 0; i < 4; ++i) {
                    char k[40];
                    std::snprintf(k, sizeof(k), "spc.cpuRegs[%d]", i);
                    mss.add_u8(k, cpu_regs[i]);
                    std::snprintf(k, sizeof(k), "spc.newCpuRegs[%d]", i);
                    mss.add_u8(k, cpu_regs[i]);
                }
            }
        }
    }

    // cart.saveRam: snes9x always writes a fixed-size SRA section (= the ROM's
    // declared Memory.SRAM_SIZE) even for cartridges without battery-backed
    // RAM, filling it with WRAM-init garbage. Mesen2 sizes its cart.saveRam
    // per ROM, so a wrong size here mismatches Mesen2's expected size and can
    // wedge load. Best-effort heuristic: if the SRA bytes look like a single
    // repeated fill pattern (no diversity), treat the ROM as having no SRAM
    // and emit a 0-byte cart.saveRam entry (matching what Mesen2 writes for
    // non-SRAM ROMs). Otherwise pass through the real SRA bytes.
    if (s9x.sections.count("SRA")) {
        const Bytes& sra = s9x.section("SRA");
        if (sra.empty()) {
            mss.add_entry("cart.saveRam", nullptr, 0);
        } else {
            // Count distinct byte values - "real" SRAM tends to have many
            // different byte values; uninitialized SRA fills look like one
            // or two repeating bytes.
            uint64_t seen[4] = {0, 0, 0, 0};
            for (uint8_t b : sra) seen[b >> 6] |= 1ull << (b & 63);
            int distinct = 0;
            for (int i = 0; i < 4; ++i)
                for (uint64_t m = seen[i]; m; m &= m - 1) distinct++;
            if (distinct <= 4) {
                // Looks like uninitialized fill -> ROM has no SRAM.
                mss.add_entry("cart.saveRam", nullptr, 0);
            } else {
                mss.add_entry("cart.saveRam", sra);
            }
        }
    }

    // 65C816 registers.
    S9xRegisters r = s9x_decode_registers(s9x);
    mss.add_entry("cpu.a",   u16_arr(r.A));
    mss.add_entry("cpu.x",   u16_arr(r.X));
    mss.add_entry("cpu.y",   u16_arr(r.Y));
    mss.add_entry("cpu.sp",  u16_arr(r.S));
    mss.add_entry("cpu.d",   u16_arr(r.D));
    mss.add_entry("cpu.pc",  u16_arr(r.PC));
    mss.add_entry("cpu.dbr", u8_arr (r.DB));
    mss.add_entry("cpu.k",   u8_arr (r.PB));
    mss.add_entry("cpu.ps",  u8_arr (r.P & 0xFF));
    mss.add_entry("cpu.emulationMode", u8_arr((r.P & 0x100) ? 1 : 0));

    apply_forward_ppu(s9x, mss);
    apply_forward_dma(s9x, mss);
    apply_forward_internal_regs(s9x, mss);
    apply_forward_cpu_timing(s9x, mss);
    apply_forward_smp(s9x, mss);

    mss.save(out_path);
}

// ===== reverse: Mesen2 .mss -> SNES9x .009 ===============================
//
// Mirrors the forward direction's shape: load the source .mss, load a same-ROM
// .009 template, and overlay the .mss-derived fields onto the template. The
// template carries snes9x-internal state that has no clean counterpart in the
// .mss (mid-frame HDMA bookkeeping, snes9x event scheduler, voice mid-decode
// state, FillRAM bytes outside the standard I/O register range).

// Read N bytes from a key, padded with zeros if shorter/absent.
static Bytes mss_bytes(const MssFile& mss, const std::string& key, size_t want) {
    auto it = mss.index.find(key);
    if (it == mss.index.end()) return Bytes(want, 0);
    Bytes out(want, 0);
    size_t n = std::min<size_t>(want, it->second.size);
    std::memcpy(out.data(), &mss.state[it->second.off], n);
    return out;
}
static uint32_t mss_u8 (const MssFile& mss, const std::string& key) {
    auto it = mss.index.find(key);
    if (it == mss.index.end() || it->second.size < 1) return 0;
    return mss.state[it->second.off];
}
static uint32_t mss_u16(const MssFile& mss, const std::string& key) {
    auto it = mss.index.find(key);
    if (it == mss.index.end() || it->second.size < 2) return 0;
    return rd_u16_le(&mss.state[it->second.off]);
}
static uint32_t mss_u32(const MssFile& mss, const std::string& key) {
    auto it = mss.index.find(key);
    if (it == mss.index.end() || it->second.size < 4) return 0;
    return rd_u32_le(&mss.state[it->second.off]);
}
static int32_t mss_s16(const MssFile& mss, const std::string& key) {
    return int16_t(mss_u16(mss, key));
}

// ---- in-place section overlay helpers (operate on the template's bytes) ----
static void put_u8_at  (Bytes& s, size_t off, uint32_t v) {
    if (off < s.size()) s[off] = uint8_t(v);
}
static void put_u16be (Bytes& s, size_t off, uint32_t v) {
    if (off + 1 < s.size()) wr_u16_be(&s[off], uint16_t(v));
}
static void put_u32be (Bytes& s, size_t off, uint32_t v) {
    if (off + 3 < s.size()) wr_u32_be(&s[off], v);
}
static void put_s16be (Bytes& s, size_t off, int32_t v) {
    put_u16be(s, off, uint32_t(uint16_t(int16_t(v))));
}
static void put_s32be (Bytes& s, size_t off, int32_t v) {
    put_u32be(s, off, uint32_t(v));
}
static void put_le_s32(Bytes& s, size_t off, int32_t v) {
    if (off + 3 < s.size()) wr_u32_le(&s[off], uint32_t(v));
}
static void put_slice (Bytes& s, size_t off, const Bytes& src) {
    if (off + src.size() > s.size()) return;
    std::memcpy(&s[off], src.data(), src.size());
}

// ---- per-section overlays --------------------------------------------------
// Each overlay_* function takes the matching section from the template .009
// (which must already exist with the v12 byte layout) and writes only the
// fields the .mss carries, leaving every other byte alone.

static void overlay_reg(Bytes& reg, const MssFile& mss) {
    if (reg.size() < 16) reg.assign(16, 0);
    put_u8_at (reg, 0,  mss_u8 (mss, "cpu.k"));
    put_u8_at (reg, 1,  mss_u8 (mss, "cpu.dbr"));
    uint16_t P = uint16_t(mss_u8(mss, "cpu.ps"))
               | (mss_u8(mss, "cpu.emulationMode") ? 0x100 : 0);
    put_u16be (reg, 2,  P);
    put_u16be (reg, 4,  mss_u16(mss, "cpu.a"));
    put_u16be (reg, 6,  mss_u16(mss, "cpu.d"));
    put_u16be (reg, 8,  mss_u16(mss, "cpu.sp"));
    put_u16be (reg, 10, mss_u16(mss, "cpu.x"));
    put_u16be (reg, 12, mss_u16(mss, "cpu.y"));
    put_u16be (reg, 14, mss_u16(mss, "cpu.pc"));
}

static void overlay_cpu(Bytes& cpu, const MssFile& mss) {
    if (cpu.size() < 48) return;
    int32_t cycles = int32_t(mss_u16(mss, "memoryManager.hClock"));
    put_s32be(cpu, 0,  cycles);                          // Cycles
    put_s32be(cpu, 8,  int32_t(mss_u16(mss, "ppu.scanline"))); // V_Counter
    put_s32be(cpu, 16, mss_u8(mss, "cpu.irqSource") ? 1 : 0);  // IRQPending

    uint8_t mesen_event = uint8_t(mss_u8(mss, "memoryManager.nextEvent"));
    uint8_t s9x_event = cpu[37];   // keep template default
    switch (mesen_event) {
        case 0: s9x_event = 4; break; // HdmaInit -> HC_HDMA_INIT
        case 1: s9x_event = 6; break; // DramRefresh -> HC_WRAM_REFRESH
        case 2: s9x_event = 2; break; // HdmaStart -> HC_HDMA_START
        case 3: s9x_event = 3; break; // EndOfScanline -> HC_HCOUNTER_MAX
    }
    put_u8_at(cpu, 37, s9x_event);
    put_s32be(cpu, 38, int32_t(mss_u16(mss, "memoryManager.nextEventClock")));
    put_u8_at(cpu, 42, mss_u8(mss, "cpu.waiOver") ? 0 : 1);
    put_u8_at(cpu, 43, mss_u8(mss, "cpu.needNmi"));
    // Leave IRQLine/Transition/LastState/External (44..47), MemSpeed (20..31)
    // and Flags (12..15) at template values - they describe scheduling state
    // the .mss doesn't capture.
}

static void overlay_ppu(Bytes& ppu, const MssFile& mss) {
    if (ppu.size() < 2652) return;

    // VMA group
    put_u16be(ppu, 2,  mss_u16(mss, "ppu.vramAddress"));

    for (int n = 0; n < 4; ++n) {
        int base = 14 + n * 11;
        char k[64];
        std::snprintf(k, sizeof(k), "ppu.layers[%d].tilemapAddress", n);
        put_u16be(ppu, base + 0,  mss_u16(mss, k));
        std::snprintf(k, sizeof(k), "ppu.layers[%d].hscroll", n);
        put_u16be(ppu, base + 2,  mss_u16(mss, k));
        std::snprintf(k, sizeof(k), "ppu.layers[%d].vscroll", n);
        put_u16be(ppu, base + 4,  mss_u16(mss, k));
        std::snprintf(k, sizeof(k), "ppu.layers[%d].largeTiles", n);
        put_u8_at(ppu, base + 6,  mss_u8(mss, k) & 1);
        std::snprintf(k, sizeof(k), "ppu.layers[%d].chrAddress", n);
        put_u16be(ppu, base + 7,  mss_u16(mss, k));
        std::snprintf(k, sizeof(k), "ppu.layers[%d].doubleWidth", n);
        uint16_t scsize = uint16_t(mss_u8(mss, k) & 1);
        std::snprintf(k, sizeof(k), "ppu.layers[%d].doubleHeight", n);
        scsize |= uint16_t((mss_u8(mss, k) & 1) << 1);
        put_u16be(ppu, base + 9, scsize);
    }
    put_u8_at(ppu, 58, mss_u8(mss, "ppu.bgMode") & 7);
    put_u8_at(ppu, 59, mss_u8(mss, "ppu.mode1Bg3Priority") & 1);
    put_u8_at(ppu, 60, mss_u8(mss, "ppu.cgramAddressLatch") & 1);   // CGFLIP
    put_u8_at(ppu, 62, mss_u8(mss, "ppu.cgramAddress"));

    // CGDATA: Mesen2 little-endian -> snes9x big-endian.
    Bytes cgram = mss_bytes(mss, "ppu.cgram", 512);
    Bytes cgbe  = byteswap16(cgram);
    put_slice(ppu, PPU_CGDATA_OFF, cgbe);

    // OAM post fields at PPU_CGDATA_OFF + 512 + PPU_OBJ_LEN = 1984.
    int post = PPU_CGDATA_OFF + 512 + PPU_OBJ_LEN;
    uint32_t oam_addr = mss_u16(mss, "ppu.oamRamAddress");
    put_u16be(ppu, post + 3, mss_u16(mss, "ppu.oamBaseAddress"));
    put_u16be(ppu, post + 5, mss_u16(mss, "ppu.oamAddressOffset"));
    put_u16be(ppu, post + 8, oam_addr
                            | (mss_u8(mss, "ppu.enableOamPriority") ? 0x8000 : 0));
    put_u16be(ppu, post + 10, oam_addr);                            // SavedOAMAddr
    put_u8_at(ppu, post + 12, mss_u8(mss, "ppu.enableOamPriority")); // OAMPriorityRotation
    put_u16be(ppu, post + 17, mss_u8(mss, "ppu.oamWriteBuffer"));    // OAMWriteRegister (low byte only)

    // OAMData (544 bytes uint8) at PPU_OAMDATA_OFF (2003).
    Bytes oam = mss_bytes(mss, "ppu.oamRam", 544);
    put_slice(ppu, PPU_OAMDATA_OFF, oam);

    // FirstSprite / LastSprite (uint8 each, snes9x's sprite-scan range).
    put_u8_at(ppu, 2547, mss_u16(mss, "ppu.fetchSpriteStart") & 0x7F);
    put_u8_at(ppu, 2548, mss_u16(mss, "ppu.fetchSpriteEnd")   & 0x7F);

    // HIRQ / VIRQ enable + position.
    uint32_t irq_h = mss_u16(mss, "internalRegisters.horizontalTimer");
    uint32_t irq_v = mss_u16(mss, "internalRegisters.verticalTimer");
    put_u8_at (ppu, 2549, mss_u8(mss, "internalRegisters.enableHorizontalIrq"));
    put_u8_at (ppu, 2550, mss_u8(mss, "internalRegisters.enableVerticalIrq"));
    // HTimerPosition (cycle position) — mirrors S9xUpdateIRQPositions math:
    //   PPU.HTimerPosition = irq_h * ONE_DOT_CYCLE + IRQTriggerCycles
    //                       - (irq_h ? 0 : ONE_DOT_CYCLE)
    //                       + (irq_h > 322 ? ONE_DOT_CYCLE/2 : 0)
    //                       + (irq_h > 326 ? ONE_DOT_CYCLE/2 : 0)
    {
        const int ONE_DOT_CYCLE = 4;
        const int IRQTriggerCycles = 14;
        int htp = int(irq_h) * ONE_DOT_CYCLE + IRQTriggerCycles;
        htp -= irq_h ? 0 : ONE_DOT_CYCLE;
        htp += (irq_h > 322) ? (ONE_DOT_CYCLE / 2) : 0;
        htp += (irq_h > 326) ? (ONE_DOT_CYCLE / 2) : 0;
        put_u16be(ppu, 2551, uint16_t(htp));            // HTimerPosition
        put_u16be(ppu, 2553, uint16_t(irq_v));          // VTimerPosition
    }
    put_u16be(ppu, 2555, irq_h);                        // IRQHBeamPos
    put_u16be(ppu, 2557, irq_v);                        // IRQVBeamPos

    // Mode7 flip + repeat. snes9x stores repeat as 2-bit code from $211A bits 6-7,
    // remapping the otherwise-unreachable value 1 to 0:
    //   00 = wrap, 10 = fill tile 0, 11 = fill backdrop color.
    // Mesen2 splits this into two bools: fillWithTile0 (bit 6) and largeMap (bit 7).
    put_u8_at(ppu, 2570, mss_u8(mss, "ppu.mode7.horizontalMirroring") & 1);
    put_u8_at(ppu, 2571, mss_u8(mss, "ppu.mode7.verticalMirroring")   & 1);
    {
        uint8_t fill = mss_u8(mss, "ppu.mode7.fillWithTile0") & 1;
        uint8_t big  = mss_u8(mss, "ppu.mode7.largeMap")      & 1;
        uint8_t code = uint8_t((big << 1) | fill);
        if (code == 1) code = 0;   // snes9x's same remap
        put_u8_at(ppu, 2572, code);
    }

    // Mosaic. Mesen2: mosaicEnabled = 4-bit mask of BGs that participate;
    //                mosaicSize     = 1..16 pixel block size.
    // snes9x:   PPU.Mosaic = block size - 1 (the register $2106 high nibble form),
    //           BGMosaic[N] = per-BG enable flag.
    {
        uint8_t msz = mss_u8(mss, "ppu.mosaicSize") & 0x0F;
        uint8_t men = mss_u8(mss, "ppu.mosaicEnabled") & 0x0F;
        put_u8_at(ppu, 2589, msz ? uint8_t(msz - 1) : 0);   // Mosaic
        put_u8_at(ppu, 2590, 0);                            // MosaicStart (per-frame scratch)
        for (int n = 0; n < 4; ++n)
            put_u8_at(ppu, 2591 + n, (men >> n) & 1);       // BGMosaic[n]
    }

    // Window left / right pair (8 px each).
    put_u8_at(ppu, 2595, mss_u8(mss, "ppu.window[0].left"));
    put_u8_at(ppu, 2596, mss_u8(mss, "ppu.window[0].right"));
    put_u8_at(ppu, 2597, mss_u8(mss, "ppu.window[1].left"));
    put_u8_at(ppu, 2598, mss_u8(mss, "ppu.window[1].right"));

    // Force snes9x to recompute its ClipCounts / ClipWindowOverlapLogic caches
    // from the per-layer flags below on the next render.
    put_u8_at(ppu, 2599, 1);                                // RecomputeClipWindows

    // Per-layer window enables. snes9x stores 6 layers (BG1..BG4, OBJ, COLOR),
    // 6 bytes each starting at offset 2600 in the order
    //   ClipCounts, OverlapLogic, W1Enable, W2Enable, W1Inside, W2Inside.
    // Counts + OverlapLogic get recomputed (RecomputeClipWindows above).
    for (int L = 0; L < 6; ++L) {
        int base = 2600 + L * 6;
        char k[80];
        std::snprintf(k, sizeof(k), "ppu.window[0].activeLayers[%d]", L);
        put_u8_at(ppu, base + 2, mss_u8(mss, k) & 1);       // ClipWindow1Enable
        std::snprintf(k, sizeof(k), "ppu.window[1].activeLayers[%d]", L);
        put_u8_at(ppu, base + 3, mss_u8(mss, k) & 1);       // ClipWindow2Enable
        std::snprintf(k, sizeof(k), "ppu.window[0].invertedLayers[%d]", L);
        put_u8_at(ppu, base + 4, mss_u8(mss, k) & 1);       // ClipWindow1Inside
        std::snprintf(k, sizeof(k), "ppu.window[1].invertedLayers[%d]", L);
        put_u8_at(ppu, base + 5, mss_u8(mss, k) & 1);       // ClipWindow2Inside
    }

    // Mode7 matrix (v12 PPU layout — MatrixA starts at 2573, not 2562).
    put_s16be(ppu, 2573, mss_s16(mss, "ppu.mode7.matrix[0]"));
    put_s16be(ppu, 2575, mss_s16(mss, "ppu.mode7.matrix[1]"));
    put_s16be(ppu, 2577, mss_s16(mss, "ppu.mode7.matrix[2]"));
    put_s16be(ppu, 2579, mss_s16(mss, "ppu.mode7.matrix[3]"));
    put_s16be(ppu, 2581, mss_s16(mss, "ppu.mode7.centerX"));
    put_s16be(ppu, 2583, mss_s16(mss, "ppu.mode7.centerY"));
    put_s16be(ppu, 2585, mss_s16(mss, "ppu.mode7.hscroll"));
    put_s16be(ppu, 2587, mss_s16(mss, "ppu.mode7.vscroll"));

    // End-of-section fields, v12 layout (each was off by 11 bytes before).
    put_u8_at(ppu, 2636, mss_u8(mss, "ppu.forcedBlank"));
    uint32_t fixed = mss_u16(mss, "ppu.fixedColor");
    put_u8_at(ppu, 2637, fixed & 0x1F);
    put_u8_at(ppu, 2638, (fixed >> 5) & 0x1F);
    put_u8_at(ppu, 2639, (fixed >> 10) & 0x1F);
    put_u8_at(ppu, 2640, mss_u8(mss, "ppu.screenBrightness"));
    put_u16be(ppu, 2641, mss_u8(mss, "ppu.overscanMode") ? 239 : 224);
    put_u8_at(ppu, 2646, mss_u8(mss, "dmaController.hdmaChannels"));
    put_u16be(ppu, 2650, mss_u16(mss, "ppu.vramReadBuffer"));    // VRAMReadBuffer
}

static void overlay_dma(Bytes& dma, const MssFile& mss) {
    if (dma.size() < 8 * 19) return;
    for (int ch = 0; ch < 8; ++ch) {
        int b = ch * 19;
        char k[80];
        auto rd_u8f = [&](const char* fmt) -> uint32_t {
            std::snprintf(k, sizeof(k), fmt, ch); return mss_u8(mss, k);
        };
        auto rd_u16f = [&](const char* fmt) -> uint32_t {
            std::snprintf(k, sizeof(k), fmt, ch); return mss_u16(mss, k);
        };
        put_u8_at(dma, b + 0,  rd_u8f ("dmaController.channel[%d].invertDirection"));
        put_u8_at(dma, b + 1,  rd_u8f ("dmaController.channel[%d].hdmaIndirectAddressing"));
        put_u8_at(dma, b + 2,  rd_u8f ("dmaController.channel[%d].unusedControlFlag"));
        put_u8_at(dma, b + 3,  rd_u8f ("dmaController.channel[%d].fixedTransfer"));
        put_u8_at(dma, b + 4,  rd_u8f ("dmaController.channel[%d].decrement"));
        put_u8_at(dma, b + 5,  rd_u8f ("dmaController.channel[%d].transferMode"));
        put_u8_at(dma, b + 6,  rd_u8f ("dmaController.channel[%d].destAddress"));
        put_u16be(dma, b + 7,  rd_u16f("dmaController.channel[%d].srcAddress"));
        put_u8_at(dma, b + 9,  rd_u8f ("dmaController.channel[%d].srcBank"));
        put_u16be(dma, b + 10, rd_u16f("dmaController.channel[%d].transferSize"));
        put_u8_at(dma, b + 12, rd_u8f ("dmaController.channel[%d].hdmaBank"));
        put_u16be(dma, b + 13, rd_u16f("dmaController.channel[%d].hdmaTableAddress"));
        // Decode hdmaLineCounterAndRepeat into snes9x's Repeat + LineCount
        // exactly as dma.cpp:1172 does, including the two LineCount=128 cases:
        //   line == 0    -> Repeat=FALSE, LineCount=128 (terminator)
        //   line == 0x80 -> Repeat=TRUE,  LineCount=128
        //   else         -> Repeat = !(line & 0x80), LineCount = line & 0x7F
        uint8_t line = uint8_t(rd_u8f("dmaController.channel[%d].hdmaLineCounterAndRepeat"));
        uint8_t repeat, line_count;
        if (line == 0)         { repeat = 0; line_count = 128; }
        else if (line == 0x80) { repeat = 1; line_count = 128; }
        else                   { repeat = (line & 0x80) ? 0 : 1; line_count = line & 0x7F; }
        put_u8_at(dma, b + 15, repeat);
        put_u8_at(dma, b + 16, line_count);
        put_u8_at(dma, b + 17, rd_u8f("dmaController.channel[%d].unusedRegister"));
        put_u8_at(dma, b + 18, rd_u8f("dmaController.channel[%d].doTransfer"));
    }
}

static void overlay_fil(Bytes& fil, const MssFile& mss) {
    auto put = [&](int addr, uint8_t v) {
        if (addr >= 0 && addr < int(fil.size())) fil[addr] = v;
    };

    // PPU register shadows ($2100..$2133)
    put(0x2100, uint8_t(((mss_u8(mss, "ppu.forcedBlank") & 1) << 7) |
                        (mss_u8(mss, "ppu.screenBrightness") & 0x0F)));

    {
        uint8_t obj_mode = mss_u8(mss, "ppu.oamMode") & 7;
        uint32_t obj_base = mss_u16(mss, "ppu.oamBaseAddress");
        uint32_t obj_off  = mss_u16(mss, "ppu.oamAddressOffset");
        uint8_t  size_sel = uint8_t(((obj_off >> 12) - 1) & 3);
        uint8_t  name_sel = uint8_t((obj_base >> 13) & 7);
        put(0x2101, uint8_t((obj_mode << 5) | (size_sel << 3) | name_sel));
    }
    put(0x2105, uint8_t(
        (mss_u8(mss, "ppu.bgMode") & 7) |
        ((mss_u8(mss, "ppu.mode1Bg3Priority") & 1) << 3) |
        ((mss_u8(mss, "ppu.layers[0].largeTiles") & 1) << 4) |
        ((mss_u8(mss, "ppu.layers[1].largeTiles") & 1) << 5) |
        ((mss_u8(mss, "ppu.layers[2].largeTiles") & 1) << 6) |
        ((mss_u8(mss, "ppu.layers[3].largeTiles") & 1) << 7)));
    put(0x2106, uint8_t(
        (mss_u8(mss, "ppu.mosaicEnabled") & 0x0F) |
        ((((mss_u8(mss, "ppu.mosaicSize") - 1) & 0x0F) << 4))));

    {
        uint8_t step = uint8_t(mss_u8(mss, "ppu.vramIncrementValue"));
        uint8_t v_lo = (step == 1 ? 0 : step == 32 ? 1 : 2);
        put(0x2115, uint8_t(
            v_lo |
            ((mss_u8(mss, "ppu.vramAddressRemapping") & 3) << 2) |
            ((mss_u8(mss, "ppu.vramAddrIncrementOnSecondReg") & 1) << 7)));
    }
    put(0x212C, uint8_t(mss_u8(mss, "ppu.mainScreenLayers")));
    put(0x212D, uint8_t(mss_u8(mss, "ppu.subScreenLayers")));

    // $2123..$2125 — per-layer window enable / invert. Layer index map for the
    // packed nibbles: $2123 = BG1+BG2, $2124 = BG3+BG4, $2125 = OBJ+COLOR.
    // snes9x derives PPU.ClipWindowN{Enable,Inside}[L] from these on write.
    auto win_active   = [&](int w, int L) -> uint8_t {
        char k[64]; std::snprintf(k, sizeof(k), "ppu.window[%d].activeLayers[%d]", w, L);
        return mss_u8(mss, k) & 1;
    };
    auto win_inverted = [&](int w, int L) -> uint8_t {
        char k[64]; std::snprintf(k, sizeof(k), "ppu.window[%d].invertedLayers[%d]", w, L);
        return mss_u8(mss, k) & 1;
    };
    auto pack_win = [&](int La, int Lb) -> uint8_t {
        return uint8_t(
            (win_inverted(0, La) << 0) | (win_active(0, La) << 1) |
            (win_inverted(1, La) << 2) | (win_active(1, La) << 3) |
            (win_inverted(0, Lb) << 4) | (win_active(0, Lb) << 5) |
            (win_inverted(1, Lb) << 6) | (win_active(1, Lb) << 7));
    };
    put(0x2123, pack_win(0, 1));   // BG1, BG2
    put(0x2124, pack_win(2, 3));   // BG3, BG4
    put(0x2125, pack_win(4, 5));   // OBJ, COLOR

    // $212E / $212F — main / sub screen window mask. 5 layers (BG1..BG4, OBJ).
    {
        uint8_t main = 0, sub = 0;
        for (int L = 0; L < 5; ++L) {
            char k[64];
            std::snprintf(k, sizeof(k), "ppu.windowMaskMain[%d]", L);
            main |= uint8_t((mss_u8(mss, k) & 1) << L);
            std::snprintf(k, sizeof(k), "ppu.windowMaskSub[%d]", L);
            sub  |= uint8_t((mss_u8(mss, k) & 1) << L);
        }
        put(0x212E, main);
        put(0x212F, sub);
    }

    // $211A M7SEL — Mode 7 H/V flip + screen-over bits.
    put(0x211A, uint8_t(
        (mss_u8(mss, "ppu.mode7.horizontalMirroring") & 1) |
        ((mss_u8(mss, "ppu.mode7.verticalMirroring") & 1) << 1) |
        ((mss_u8(mss, "ppu.mode7.fillWithTile0")     & 1) << 6) |
        ((mss_u8(mss, "ppu.mode7.largeMap")          & 1) << 7)));

    // $2126..$2129 — window 1/2 left/right edges.
    put(0x2126, uint8_t(mss_u8(mss, "ppu.window[0].left")));
    put(0x2127, uint8_t(mss_u8(mss, "ppu.window[0].right")));
    put(0x2128, uint8_t(mss_u8(mss, "ppu.window[1].left")));
    put(0x2129, uint8_t(mss_u8(mss, "ppu.window[1].right")));

    put(0x2130, uint8_t(
        (mss_u8(mss, "ppu.directColorMode") & 1) |
        ((mss_u8(mss, "ppu.colorMathAddSubscreen") & 1) << 1) |
        ((mss_u32(mss, "ppu.colorMathPreventMode") & 3) << 4) |
        ((mss_u32(mss, "ppu.colorMathClipMode")    & 3) << 6)));
    put(0x2131, uint8_t(
        (mss_u8(mss, "ppu.colorMathEnabled") & 0x3F) |
        ((mss_u8(mss, "ppu.colorMathHalveResult") & 1) << 6) |
        ((mss_u8(mss, "ppu.colorMathSubtractMode") & 1) << 7)));
    put(0x2133, uint8_t(
        (mss_u8(mss, "ppu.screenInterlace") & 1) |
        ((mss_u8(mss, "ppu.objInterlace") & 1) << 1) |
        ((mss_u8(mss, "ppu.overscanMode") & 1) << 2) |
        ((mss_u8(mss, "ppu.hiResMode") & 1) << 3)));

    // CPU internal register shadows ($4200..$420D)
    put(0x4200, uint8_t(
        (mss_u8(mss, "internalRegisters.enableAutoJoypadRead") & 1) |
        ((mss_u8(mss, "internalRegisters.enableHorizontalIrq") & 1) << 4) |
        ((mss_u8(mss, "internalRegisters.enableVerticalIrq")   & 1) << 5) |
        ((mss_u8(mss, "internalRegisters.enableNmi")           & 1) << 7)));
    put(0x4201, uint8_t(mss_u8(mss, "internalRegisters.ioPortOutput")));
    {
        uint32_t htime = mss_u16(mss, "internalRegisters.horizontalTimer");
        uint32_t vtime = mss_u16(mss, "internalRegisters.verticalTimer");
        put(0x4207, uint8_t(htime & 0xFF));
        put(0x4208, uint8_t((htime >> 8) & 1));
        put(0x4209, uint8_t(vtime & 0xFF));
        put(0x420A, uint8_t((vtime >> 8) & 1));
    }
    put(0x420C, uint8_t(mss_u8(mss, "dmaController.hdmaChannels")));
    put(0x420D, uint8_t(mss_u8(mss, "internalRegisters.enableFastRom")));
    // RDNMI / TIMEUP: bit 7 holds the latched flag.
    if (0x4210 < int(fil.size())) {
        uint8_t orig = fil[0x4210];
        uint8_t nmi  = uint8_t(mss_u8(mss, "internalRegisters.nmiFlag")) & 1;
        fil[0x4210] = uint8_t((orig & 0x7F) | (nmi << 7));
    }
    if (0x4211 < int(fil.size())) {
        uint8_t orig = fil[0x4211];
        uint8_t irq  = uint8_t(mss_u8(mss, "internalRegisters.irqFlag")) & 1;
        fil[0x4211] = uint8_t((orig & 0x7F) | (irq << 7));
    }

    // BG addressing register shadows ($2107..$210C). The PPU section preserves
    // the structured BG_SCBase / BG_NameBase fields, but games occasionally
    // read these register-page bytes directly, so reconstruct them.
    //   $2107..$210A BGnSC: bits 0-1 = scsize, bits 2-7 = tilemap >> 10
    for (int n = 0; n < 4; ++n) {
        char k[64];
        std::snprintf(k, sizeof(k), "ppu.layers[%d].tilemapAddress", n);
        uint32_t tmap = mss_u16(mss, k);
        std::snprintf(k, sizeof(k), "ppu.layers[%d].doubleWidth", n);
        uint8_t dw = uint8_t(mss_u8(mss, k) & 1);
        std::snprintf(k, sizeof(k), "ppu.layers[%d].doubleHeight", n);
        uint8_t dh = uint8_t(mss_u8(mss, k) & 1);
        uint8_t scsize = uint8_t((dh << 1) | dw);
        put(0x2107 + n, uint8_t(((tmap >> 10) << 2) | (scsize & 3)));
    }
    //   $210B BG1/BG2 chr base, $210C BG3/BG4 chr base. Each nibble = addr/0x1000.
    {
        uint32_t c0 = mss_u16(mss, "ppu.layers[0].chrAddress") / 0x1000;
        uint32_t c1 = mss_u16(mss, "ppu.layers[1].chrAddress") / 0x1000;
        uint32_t c2 = mss_u16(mss, "ppu.layers[2].chrAddress") / 0x1000;
        uint32_t c3 = mss_u16(mss, "ppu.layers[3].chrAddress") / 0x1000;
        put(0x210B, uint8_t((c0 & 0x0F) | ((c1 & 0x0F) << 4)));
        put(0x210C, uint8_t((c2 & 0x0F) | ((c3 & 0x0F) << 4)));
    }
    // VRAM address shadow ($2116/$2117) — last write before the load.
    {
        uint32_t va = mss_u16(mss, "ppu.vramAddress");
        put(0x2116, uint8_t(va & 0xFF));
        put(0x2117, uint8_t((va >> 8) & 0xFF));
    }

    // ALU multiply/divide register shadows ($4202..$4206 input, $4214..$4217
    // output). Mesen2 stores these as internalRegisters.aluMulDiv.*.
    put(0x4202, uint8_t(mss_u8(mss, "internalRegisters.aluMulDiv.multOperand1")));
    put(0x4203, uint8_t(mss_u8(mss, "internalRegisters.aluMulDiv.multOperand2")));
    {
        uint32_t dividend = mss_u16(mss, "internalRegisters.aluMulDiv.dividend");
        put(0x4204, uint8_t(dividend & 0xFF));
        put(0x4205, uint8_t((dividend >> 8) & 0xFF));
    }
    put(0x4206, uint8_t(mss_u8(mss, "internalRegisters.aluMulDiv.divisor")));
    {
        uint32_t div_res = mss_u16(mss, "internalRegisters.aluMulDiv.divResult");
        put(0x4214, uint8_t(div_res & 0xFF));
        put(0x4215, uint8_t((div_res >> 8) & 0xFF));
        uint32_t mult_res = mss_u16(mss, "internalRegisters.aluMulDiv.multOrRemainderResult");
        put(0x4216, uint8_t(mult_res & 0xFF));
        put(0x4217, uint8_t((mult_res >> 8) & 0xFF));
    }
}

static void overlay_snd(Bytes& snd, const MssFile& mss) {
    if (snd.size() < 65536) return;

    // First 64 KB = apuram. Replace with spc.ram, but stamp the I/O page so
    // the SPC's view of the four hardware ports matches the .mss snapshot.
    //   apuram[$F4..$F7] is the SPC->CPU direction (what the CPU reads at
    //     $2140-$2143). In Mesen2 this is spc.outputReg[N], NOT spc.cpuRegs.
    //   apuram[$F1] is the SPC control register; we reconstruct it from the
    //     per-timer enabled flags and spc.romEnabled.
    //   apuram[$F2] is the DSP register-address latch (spc.dspReg).
    Bytes spc_ram = mss_bytes(mss, "spc.ram", 65536);
    for (int i = 0; i < 4; ++i) {
        char k[40];
        std::snprintf(k, sizeof(k), "spc.outputReg[%d]", i);
        spc_ram[0xF4 + i] = uint8_t(mss_u8(mss, k));
    }
    uint8_t f1 = 0;
    if (mss_u8(mss, "spc.timer0.enabled")) f1 |= 0x01;
    if (mss_u8(mss, "spc.timer1.enabled")) f1 |= 0x02;
    if (mss_u8(mss, "spc.timer2.enabled")) f1 |= 0x04;
    if (mss_u8(mss, "spc.romEnabled"))     f1 |= 0x80;
    spc_ram[0xF1] = f1;
    spc_ram[0xF2] = uint8_t(mss_u8(mss, "spc.dspReg"));
    std::memcpy(snd.data(), spc_ram.data(), 65536);

    // SMP state (40 little-endian int32s at offset 65536).
    if (snd.size() >= 65536 + S9xSmpState::COUNT_ * 4) {
        int32_t smp[S9xSmpState::COUNT_] = {0};

        // Preserve template values for fields the .mss doesn't carry (clock,
        // opcode_number, opcode_cycle, rd, wr, dp, sp, ya, bit, etc.) - copy
        // them out first, then overwrite only the ones we know.
        for (int i = 0; i < S9xSmpState::COUNT_; ++i)
            smp[i] = int32_t(rd_u32_le(&snd[65536 + i * 4]));

        smp[S9xSmpState::REG_PC] = int32_t(mss_u16(mss, "spc.pc"));
        smp[S9xSmpState::REG_SP] = int32_t(mss_u8 (mss, "spc.sp"));
        smp[S9xSmpState::REG_A]  = int32_t(mss_u8 (mss, "spc.a"));
        smp[S9xSmpState::REG_X]  = int32_t(mss_u8 (mss, "spc.x"));
        smp[S9xSmpState::REG_Y]  = int32_t(mss_u8 (mss, "spc.y"));
        uint8_t ps = uint8_t(mss_u8(mss, "spc.ps"));
        smp[S9xSmpState::P_N] = (ps >> 7) & 1;
        smp[S9xSmpState::P_V] = (ps >> 6) & 1;
        smp[S9xSmpState::P_P] = (ps >> 5) & 1;
        smp[S9xSmpState::P_B] = (ps >> 4) & 1;
        smp[S9xSmpState::P_H] = (ps >> 3) & 1;
        smp[S9xSmpState::P_I] = (ps >> 2) & 1;
        smp[S9xSmpState::P_Z] = (ps >> 1) & 1;
        smp[S9xSmpState::P_C] =  ps       & 1;
        smp[S9xSmpState::STATUS_DSP_ADDR]      = int32_t(mss_u8(mss, "spc.dspReg"));
        smp[S9xSmpState::STATUS_IPLROM_ENABLE] = int32_t(mss_u8(mss, "spc.romEnabled"));
        static const int T_E[3] = {S9xSmpState::T0_ENABLE, S9xSmpState::T1_ENABLE, S9xSmpState::T2_ENABLE};
        static const int T_T[3] = {S9xSmpState::T0_TARGET, S9xSmpState::T1_TARGET, S9xSmpState::T2_TARGET};
        static const int T_1[3] = {S9xSmpState::T0_STAGE1, S9xSmpState::T1_STAGE1, S9xSmpState::T2_STAGE1};
        static const int T_2[3] = {S9xSmpState::T0_STAGE2, S9xSmpState::T1_STAGE2, S9xSmpState::T2_STAGE2};
        static const int T_3[3] = {S9xSmpState::T0_STAGE3, S9xSmpState::T1_STAGE3, S9xSmpState::T2_STAGE3};
        for (int n = 0; n < 3; ++n) {
            char k[64];
            std::snprintf(k, sizeof(k), "spc.timer%d.enabled", n);
            smp[T_E[n]] = int32_t(mss_u8(mss, k));
            std::snprintf(k, sizeof(k), "spc.timer%d.target", n);
            smp[T_T[n]] = int32_t(mss_u8(mss, k));
            std::snprintf(k, sizeof(k), "spc.timer%d.stage0", n);
            smp[T_1[n]] = int32_t(mss_u8(mss, k));
            std::snprintf(k, sizeof(k), "spc.timer%d.stage2", n);
            smp[T_2[n]] = int32_t(mss_u8(mss, k));
            std::snprintf(k, sizeof(k), "spc.timer%d.output", n);
            smp[T_3[n]] = int32_t(mss_u8(mss, k));
        }
        for (int i = 0; i < S9xSmpState::COUNT_; ++i)
            put_le_s32(snd, 65536 + i * 4, smp[i]);
    }

    // DSP register block (128 bytes at offset 65700).
    if (snd.size() >= 65700 + 128) {
        Bytes dsp = mss_bytes(mss, "spc.dsp.regs", 128);
        put_slice(snd, 65700, dsp);
    }

    // DSP voice state. Each of the 8 voices is 38 bytes starting at offset
    // 65828 (= 65700 + 128). snes9x's SPC_DSP requires brr_offset to be a
    // valid BRR byte index (1, 3, 5, 7 for "mid-block", or 1 between blocks).
    // A zero-filled voice state with brr_offset=0 will trigger an assertion
    // in SPC_DSP.cpp::voice_V3 the moment the voice tries to decode -
    // (brr_offset += 2) reaches 2, then 4, ..., eventually 10 (!= 9, the
    // brr_block_size). Mesen2's _brrOffset defaults to 1 (DspVoice.h), so
    // pre-fill every voice with 1 then overlay any per-voice keys the .mss
    // carries on top.
    const size_t VOICE_BASE = 65700 + 128;   // 65828
    for (int v = 0; v < 8; ++v) {
        size_t vo = VOICE_BASE + v * 38;
        if (vo + 38 > snd.size()) break;

        // Safe defaults: env=0 (silent), brr_offset=1 (idle between blocks).
        // Everything else stays zero from the section init.
        snd[vo + 33] = 1;  // brr_offset default

        char k[80];

        std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].sampleBuffer", v);
        auto it = mss.index.find(k);
        if (it != mss.index.end() && it->second.size <= 24)
            std::memcpy(&snd[vo], &mss.state[it->second.off], it->second.size);

        std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].interpolationPos", v);
        uint32_t interp = mss_u32(mss, k);
        snd[vo + 24] = uint8_t(interp & 0xFF);
        snd[vo + 25] = uint8_t((interp >> 8) & 0xFF);

        std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].brrAddress", v);
        uint32_t brr_addr = mss_u16(mss, k);
        snd[vo + 26] = uint8_t(brr_addr & 0xFF);
        snd[vo + 27] = uint8_t((brr_addr >> 8) & 0xFF);

        std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].envVolume", v);
        uint32_t env = mss_u32(mss, k);
        snd[vo + 28] = uint8_t(env & 0xFF);
        snd[vo + 29] = uint8_t((env >> 8) & 0xFF);

        std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].prevCalculatedEnv", v);
        uint32_t hidden_env = mss_u32(mss, k);
        snd[vo + 30] = uint8_t(hidden_env & 0xFF);
        snd[vo + 31] = uint8_t((hidden_env >> 8) & 0xFF);

        std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].bufferPos", v);
        snd[vo + 32] = uint8_t(mss_u8(mss, k));

        // brr_offset: keep our default of 1 unless the .mss explicitly stores
        // a different valid value. Coerce to 1 for any out-of-range value to
        // avoid the SPC_DSP assertion (only odd 1..7 are safe; 9 means "just
        // finished a block" which the very next sample will reset).
        std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].brrOffset", v);
        if (mss.index.count(k)) {
            uint8_t bo = uint8_t(mss_u16(mss, k) & 0xFF);
            if (bo == 0 || bo > 9 || (bo & 1) == 0) bo = 1;
            snd[vo + 33] = bo;
        }

        std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].keyOnDelay", v);
        snd[vo + 34] = uint8_t(mss_u8(mss, k));

        std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].envMode", v);
        snd[vo + 35] = uint8_t(mss_u32(mss, k) & 0xFF);

        std::snprintf(k, sizeof(k), "spc.dsp.voices[%d].envOut", v);
        snd[vo + 36] = uint8_t(mss_u8(mss, k));
        // vo + 37 is copier.extra() byte - keep zero.
    }

    // external_regs: Mesen2's DspState::ExternalRegs (128 bytes). Their snes9x
    // SND position depends on the size of the voice/echo/misc blocks; the
    // simplest stable reference is the .mss key. Located 128 bytes before the
    // final SND tail (16 bytes of ref_time/remainder/dsp.clock/cpu.regs +
    // 1 byte trailing copier.extra). For v12: 66358 - 16 - 1 - 128 = 66213.
    auto ext_it = mss.index.find("spc.dsp.externalRegs");
    if (ext_it != mss.index.end() && ext_it->second.size == 128 &&
        snd.size() >= 66213 + 128) {
        std::memcpy(&snd[66213], &mss.state[ext_it->second.off], 128);
    }

    // CPU::registers (CPU->SPC port writes, what the SPC sees at $F4-$F7
    // for *reads*) lives in the SND tail at a fixed offset for v12 snapshots.
    // Layout per apu.cpp::S9xAPUSaveState:
    //   [0..65535]       apuram
    //   [65536..65695]   SMP::save_state (40 LE int32 = 160 bytes)
    //   [65696..66341]   DSP::save_state (646 bytes total via SPC_DSP::copy_state)
    //   [66342..66345]   reference_time
    //   [66346..66349]   remainder
    //   [66350..66353]   dsp.clock
    //   [66354..66357]   cpu.registers <-- here
    //   [66358..]        zero padding to SPC_SAVE_STATE_BLOCK_SIZE (66560)
    // The DSP block size was verified against a real v12 .009.
    if (snd.size() >= 66358) {
        for (int i = 0; i < 4; ++i) {
            char k[40];
            std::snprintf(k, sizeof(k), "spc.cpuRegs[%d]", i);
            snd[66354 + i] = uint8_t(mss_u8(mss, k));
        }
    }
}

int detect_rom_region_pal(const std::string& rom_path) {
    std::ifstream f(rom_path, std::ios::binary);
    if (!f) return -1;
    // SNES ROM headers are at $7FC0 (LoROM) or $FFC0 (HiROM), preceded by an
    // optional 512-byte copier header that we must skip. Read enough bytes to
    // cover both possibilities plus the copier header.
    std::vector<uint8_t> data(0x10200);
    f.read(reinterpret_cast<char*>(data.data()), data.size());
    size_t n = size_t(f.gcount());
    if (n < 0x8000) return -1;

    size_t skip = (n % 1024 == 512) ? 512 : 0;

    auto check_header = [&](size_t base) -> int {
        if (base + 0xE0 > n) return -1;
        uint8_t makeup = data[base + 0xD5];
        // Plausibility: top nibble of makeup byte should be 0x20..0x3F for
        // standard map modes (LoROM/HiROM/ExHiROM/FastROM combinations).
        if ((makeup & 0xE0) != 0x20) return -1;
        uint8_t region = data[base + 0xD9];
        // Region byte: 0/1/D/F/10 = NTSC family, 2..C/11 = PAL family.
        if (region == 0x00 || region == 0x01 || region == 0x0D ||
            region == 0x0F || region == 0x10)
            return 0;
        if ((region >= 0x02 && region <= 0x0C) || region == 0x11)
            return 1;
        return -1;
    };
    int r = check_header(skip + 0x7FC0);   // LoROM header position
    if (r >= 0) return r;
    return check_header(skip + 0xFFC0);    // HiROM header position
}

void convert_mss_to_s9x(const std::string& in_path, const std::string& out_path,
                        RegionOverride region, const std::string& rom_path) {
    MssFile mss = MssFile::load(in_path);

    // Build a fresh v12 SNES9x snapshot from scratch. snes9x's loader reads
    // a strict ordered list of required sections (NAM, CPU, REG, PPU, DMA,
    // VRA, RAM, SRA, FIL, SND, CTL - see snapshot.cpp::UnfreezeStateFromCopy)
    // and exits with WRONG_FORMAT if any one is missing or the wrong size.
    // We emit every required section sized exactly per the snes9x source,
    // zero-fill the bytes, then run the surgical overlay_* helpers to write
    // the fields the .mss carries. snes9x re-derives most state-machine
    // counters on the first frame after load, so a zero-default base is OK.
    S9xState s9x;
    s9x.version = 12;
    s9x.original_version = 12;

    // Section sizes (from snes9x source - all v12 layout):
    //   NAM: PATH_MAX = 1024 bytes (port.h:116 - 1024 on the canonical build)
    //   CPU: 48 bytes (SnapCPU @ v12, after v6→v7 + v10→v11 upgrades)
    //   REG: 16 bytes (SnapRegisters: PB,DB,P,A,D,S,X,Y,PC)
    //   PPU: 2652 bytes (SnapPPU @ v12 + CGSavedByte + VRAMReadBuffer)
    //   DMA: 8 * 19 = 152 bytes
    //   VRA: 64 KB (Memory.VRAM)
    //   RAM: 128 KB (Memory.RAM)
    //   SRA: Memory.SRAM_SIZE - varies per ROM; emit only if .mss has it
    //   FIL: 32 KB (Memory.FillRAM)
    //   SND: SPC_SAVE_STATE_BLOCK_SIZE = 1024 * 65 = 66560 (apu.h:14)
    //   CTL: 91 bytes (SControlSnapshot serialized fields)
    //   TIM: 70 bytes (SnapTimings @ v12)
    s9x.sections["NAM"] = Bytes(1024,      0);
    s9x.sections["CPU"] = Bytes(48,        0);
    s9x.sections["REG"] = Bytes(16,        0);
    s9x.sections["PPU"] = Bytes(2652,      0);
    s9x.sections["DMA"] = Bytes(8 * 19,    0);
    s9x.sections["VRA"] = mss_bytes(mss, "ppu.vram",               64 * 1024);
    s9x.sections["RAM"] = mss_bytes(mss, "memoryManager.workRam", 128 * 1024);
    s9x.sections["FIL"] = Bytes(0x8000,    0);
    s9x.sections["SND"] = Bytes(66560,     0);   // SPC_SAVE_STATE_BLOCK_SIZE
    s9x.sections["CTL"] = Bytes(91,        0);
    s9x.sections["TIM"] = Bytes(70,        0);

    // NAM: ROM filename + NUL. snes9x doesn't check this against the loaded
    // ROM; it's mostly informational. Use the input filename stem as a hint.
    {
        std::string stem = fs::path(in_path).stem().string();
        auto& nam = s9x.sections["NAM"];
        size_t n = std::min(stem.size(), nam.size() - 1);
        std::memcpy(nam.data(), stem.data(), n);
        nam[n] = 0;
    }

    // SRA: snes9x's loader (UnfreezeBlockCopy + CheckBlockName) requires the
    // SRA section to be present and have a non-zero size in its header. If we
    // omit it (or write size=0), CheckBlockName returns false, UnfreezeBlockCopy
    // returns 0, and S9xMessageFromResult falls through to the misleading
    // "ROM image not found" message. Always emit 131072 bytes (Memory.SRAM_SIZE
    // upper bound = 128 KB). snes9x reads only Memory.SRAM_SIZE bytes from the
    // section at load time and discards the rest, so over-sizing is safe.
    {
        Bytes sra(131072, 0);
        auto it = mss.index.find("cart.saveRam");
        if (it != mss.index.end() && it->second.size > 0) {
            size_t n = std::min<size_t>(it->second.size, sra.size());
            std::memcpy(sra.data(), &mss.state[it->second.off], n);
        }
        s9x.sections["SRA"] = std::move(sra);
    }

    // TIM section: snes9x's STimings is initialized in memmap.cpp at ROM load
    // to ROM-region-specific constants (NTSC = 262 V_Max, PAL = 312). When
    // loading a state, UnfreezeStructFromCopy OVERWRITES Timings from the file,
    // so a zero-filled TIM gives H_Max_Master = 0 - scanlines never end, the
    // CPU is stuck forever, the game freezes. Fill with snes9x's documented
    // defaults from snes9x.h (SNES_CYCLES_PER_SCANLINE = 1364, SNES_HBLANK_*
    // = 1096/4, SNES_HDMA_INIT_HC = 20, SNES_HDMA_START_HC = 1106,
    // SNES_RENDER_START_HC = 512, V_Max = 262 NTSC). FreezeStruct field order
    // (snapshot.cpp SnapTimings[]) - bytes are big-endian int32 except
    // InterlaceField (offset 44) and APUAllowTimeOverflow (offset 65) which
    // are 1-byte bool8.
    {
        auto& tim = s9x.sections["TIM"];
        wr_u32_be(&tim[0],  1364);            // H_Max_Master
        wr_u32_be(&tim[4],  1364);            // H_Max
        wr_u32_be(&tim[8],  262);             // V_Max_Master (NTSC default)
        wr_u32_be(&tim[12], 262);             // V_Max
        wr_u32_be(&tim[16], 1096);            // HBlankStart
        wr_u32_be(&tim[20], 4);               // HBlankEnd
        wr_u32_be(&tim[24], 20);              // HDMAInit
        wr_u32_be(&tim[28], 1106);            // HDMAStart
        wr_u32_be(&tim[32], 0xFFFF);          // NMITriggerPos (no NMI pending)
        wr_u32_be(&tim[36], 538);             // WRAMRefreshPos (SNES_WRAM_REFRESH_HC_v2)
        wr_u32_be(&tim[40], 512);             // RenderPos
        tim[44] = 0;                          // InterlaceField (bool8)
        wr_u32_be(&tim[45], 18);              // DMACPUSync
        wr_u32_be(&tim[49], 24);              // NMIDMADelay
        wr_u32_be(&tim[53], 0);               // IRQFlagChanging
        wr_u32_be(&tim[57], 0);               // APUSpeedup
        wr_u32_be(&tim[61], 14);              // IRQTriggerCycles
        tim[65] = 0;                          // APUAllowTimeOverflow (bool8)
        wr_u32_be(&tim[66], 0x0FFFFFFFu);     // NextIRQTimer = "no IRQ scheduled"

        // Region selection priority:
        //   1. Explicit override (ForceNTSC / ForcePAL) — caller knows best.
        //   2. ROM header parse — exact answer for any standard cart.
        //   3. V counter heuristic — anything > 261 must be PAL.
        // Mesen2 doesn't carry ROM region in the .mss, hence the fallback chain.
        bool pal = false;
        if (region == RegionOverride::ForcePAL) {
            pal = true;
        } else if (region == RegionOverride::ForceNTSC) {
            pal = false;
        } else {
            int rom_says = rom_path.empty() ? -1 : detect_rom_region_pal(rom_path);
            if (rom_says == 1)       pal = true;
            else if (rom_says == 0)  pal = false;
            else {
                uint32_t v_counter   = mss_u16(mss, "internalRegisters.vCounter");
                uint32_t v_scanline  = mss_u16(mss, "ppu.scanline");
                uint32_t v_timer_pos = mss_u16(mss, "internalRegisters.verticalTimer");
                pal = (v_counter > 261 || v_scanline > 261 || v_timer_pos > 261);
            }
        }
        if (pal) {
            wr_u32_be(&tim[8],  312);         // V_Max_Master (PAL)
            wr_u32_be(&tim[12], 312);
        }
    }

    // CPU defaults the overlay won't touch. snes9x updates these on every
    // memory access so the absolute starting values don't matter much, but
    // SCAN_KEYS_FLAG (bit 4) is set every frame to schedule controller polls -
    // missing it costs one frame of input lag after load.
    {
        auto& cpu = s9x.sections["CPU"];
        wr_u32_be(&cpu[12], 0x10);            // Flags = SCAN_KEYS_FLAG
        wr_u32_be(&cpu[20], 8);               // MemSpeed (SLOW_ONE_CYCLE default)
        wr_u32_be(&cpu[24], 16);              // MemSpeedx2
        wr_u32_be(&cpu[28], 6);               // FastROMSpeed (FAST_ONE_CYCLE)
    }

    // CTL: SControlSnapshot. ver=1 (current); rest are pad-read state that
    // snes9x re-derives on the next auto-joypad cycle.
    s9x.sections["CTL"][0] = 1;               // ver

    // Now overlay everything the .mss carries onto these fresh sections.
    overlay_reg(s9x.section("REG"), mss);
    overlay_cpu(s9x.section("CPU"), mss);
    overlay_ppu(s9x.section("PPU"), mss);
    overlay_dma(s9x.section("DMA"), mss);
    overlay_fil(s9x.section("FIL"), mss);
    overlay_snd(s9x.section("SND"), mss);

    s9x.save(out_path);
}

// ===== Legacy snes9x 1.5.x -> modern snes9x v12 upgrade ==================
//
// S9xState::load already normalises CPU/PPU/TIM to v6 layout and synthesises
// a 64 KB-only SND from the legacy ARA section, leaving the rest of the SND
// (SMP state, DSP regs, ports) zeroed. We need to fill those in from the
// legacy APU / ARE / IAP sections so modern snes9x can resume the SPC.

void upgrade_legacy_s9x_state(const std::string& in_path,
                              const std::string& out_path) {
    S9xState s9x = S9xState::load(in_path);
    if (s9x.original_version < 1000)
        throw ConvertError("not a legacy snes9x 1.5.x state — already in a modern format snes9x can load");

    if (!s9x.sections.count("SND"))
        throw ConvertError("legacy state has no synthesised SND — load logic regression?");

    Bytes& snd = s9x.section("SND");
    if (snd.size() < 66358)
        snd.resize(66560, 0);

    auto apu_it = s9x.sections.find("APU");
    auto are_it = s9x.sections.find("ARE");

    // 1. SPC <-> CPU port plumbing. Earlier versions swapped apuram[$F4..$F7]
    //    with SAPU.OutPorts on the theory that legacy / modern stored the two
    //    directions on different sides. In practice the CPU side at save time
    //    was polling $2140 for the value sitting in legacy apuram[$F4..$F7]
    //    (e.g. $1B for Yoshi's Safari) — which means whatever model legacy
    //    used, that byte IS what the CPU expects to read after resume. Modern
    //    bAPU's CPU-side $2140 read returns apuram[$F4], so leave the legacy
    //    apuram bytes alone and mirror them into cpu.registers (= what the
    //    SPC reads at $F4 MMIO) below in step 5. Without this the upgraded
    //    state immediately deadlocks: CPU reads $2140=$00 (from SAPU.OutPorts)
    //    but is comparing against A.low=$1B.
    uint8_t legacy_apuram_F4_F7[4] = { snd[0xF4], snd[0xF5], snd[0xF6], snd[0xF7] };

    // 2. SMP state (41 LE int32) at offset 65536.
    //    ARE layout (snes9x 1.5.1 SAPURegisters, big-endian INT_V):
    //      [0..1] YA (Y high, A low) — byte 0 = Y, byte 1 = A
    //      [2]    X
    //      [3]    S (stack pointer)
    //      [4]    P (PSW)
    //      [5..6] PC (big-endian)
    if (are_it != s9x.sections.end() && are_it->second.size() >= 7
        && snd.size() >= 65536 + 41 * 4) {
        const auto& are = are_it->second;
        int32_t smp[41] = {0};
        smp[3] = (int32_t(are[5]) << 8) | are[6];   // REG_PC
        smp[4] = are[3];                            // REG_SP
        smp[5] = are[1];                            // REG_A
        smp[6] = are[2];                            // REG_X
        smp[7] = are[0];                            // REG_Y
        uint8_t psw = are[4];
        smp[8]  = (psw >> 7) & 1;   // P_N
        smp[9]  = (psw >> 6) & 1;   // P_V
        smp[10] = (psw >> 5) & 1;   // P_P
        smp[11] = (psw >> 4) & 1;   // P_B
        smp[12] = (psw >> 3) & 1;   // P_H
        smp[13] = (psw >> 2) & 1;   // P_I
        smp[14] = (psw >> 1) & 1;   // P_Z
        smp[15] = psw & 1;          // P_C

        if (apu_it != s9x.sections.end() && apu_it->second.size() >= 11) {
            smp[16] = apu_it->second[4] ? 1 : 0;    // STATUS_IPLROM_ENABLE
        }
        // STATUS_DSP_ADDR: last value written to SPC $F2 (DSP address latch).
        // Legacy Blargg APU stored this in apuram[$F2]; modern bAPU keeps it
        // in status.dsp_addr (= SMP state index 17). Source the latch byte
        // from our synthesised apuram which still holds the legacy $F2.
        smp[17] = snd[0xF2];                        // STATUS_DSP_ADDR
        smp[18] = snd[0xF8];                        // STATUS_RAM00F8
        smp[19] = snd[0xF9];                        // STATUS_RAM00F9

        // Timers from SAPU. Empirically verified by finding Yoshi's music-tick
        // target=$0010 at offset 213 (so TimerTarget[0] BE is at 212-213).
        // The legacy struct has 3 bytes of padding/uninitialised ExtraRAM
        // tail between byte 202 and the Timer array (despite the documented
        // ExtraRAM[64] suggesting Timer starts at 203). Actual layout:
        //   [206..211] Timer[3]        (3 x uint16 BE — current counter)
        //   [212..217] TimerTarget[3]  (3 x uint16 BE — target)
        //   [218..220] TimerEnabled[3] (3 x bool8)
        if (apu_it != s9x.sections.end() && apu_it->second.size() >= 221) {
            const auto& apu = apu_it->second;
            static const int T_BASE[3] = { 20, 25, 30 };
            for (int t = 0; t < 3; ++t) {
                int32_t timer_now = (int32_t(apu[206 + t * 2]) << 8) | apu[207 + t * 2];
                int32_t target    = (int32_t(apu[212 + t * 2]) << 8) | apu[213 + t * 2];
                int b = T_BASE[t];
                smp[b + 0] = apu[218 + t];  // T?_ENABLE
                smp[b + 1] = target;        // T?_TARGET
                smp[b + 2] = timer_now;     // T?_STAGE1 (current counter)
                smp[b + 3] = 0;             // T?_STAGE2
                smp[b + 4] = 0;             // T?_STAGE3 (4-bit output)
            }
        }

        for (int i = 0; i < 41; ++i)
            wr_u32_le(&snd[65536 + i * 4], uint32_t(smp[i]));
    }

    // 3. DSP register bank (128 bytes) at offset 65700.
    //    Legacy SAPU.DSP[128] sits at APU offset 11.
    if (apu_it != s9x.sections.end() && apu_it->second.size() >= 11 + 128
        && snd.size() >= 65700 + 128) {
        std::memcpy(&snd[65700], &apu_it->second[11], 128);
    }

    // 4. DSP voice state (8 * 38 bytes from offset 65828): zero with safe
    //    brrOffset = 1 to avoid the SPC_DSP voice_V3 assertion in snes9x.
    {
        const size_t voice_base = 65828;
        for (int v = 0; v < 8; ++v) {
            size_t vo = voice_base + v * 38;
            if (vo + 38 > snd.size()) break;
            std::memset(&snd[vo], 0, 38);
            snd[vo + 33] = 1;
        }
    }

    // 5. cpu.registers[4] at offset 66354 = CPU->SPC ports. Legacy held
    //    these in apuram[$F4..$F7] (CPU's writes); we saved those before
    //    overwriting apuram with SAPU.OutPorts in step 1.
    if (snd.size() >= 66358) {
        snd[66354] = legacy_apuram_F4_F7[0];
        snd[66355] = legacy_apuram_F4_F7[1];
        snd[66356] = legacy_apuram_F4_F7[2];
        snd[66357] = legacy_apuram_F4_F7[3];
    }

    // 6. Apply the same SPC-busy-wait unstick patch the mss path uses.
    //    Legacy snes9x 1.5.x states often catch the SPC in the SNES IPL-style
    //    "MOV Y, !CPUIO0 ; BNE -3" upload wait. Modern snes9x doesn't capture
    //    cycle-precise CPU<->SPC sync either (same as mesen2), so without
    //    patching the SPC also deadlocks on load. Replace every occurrence of
    //    the wait pattern (D0 FB after EC F4 00) with BRA +0 (2F 00) so the
    //    SPC falls through into the inner CMP/echo loop at $1947 instead of
    //    spinning at the outer wait.
    {
        int patches = 0;
        for (size_t i = 0; i + 4 < snd.size() && i + 4 < 65536; ++i) {
            if (snd[i]   == 0xEC && snd[i+1] == 0xF4 &&
                snd[i+2] == 0x00 && snd[i+3] == 0xD0 &&
                snd[i+4] == 0xFB) {
                snd[i+3] = 0x2F;
                snd[i+4] = 0x00;
                if (++patches >= 16) break;
            }
        }
    }

    // 7. Plant an SPC-side echo loop in apuram and force SMP PC to it.
    //    Legacy 1.5.x states often capture the CPU mid-audio-upload where
    //    the standard wait pattern is:
    //
    //        STA $2140                ; send byte
    //        ...
    //        CMP $2140  ; CD 40 21    ; wait for SPC to echo it back
    //        BNE -3     ; D0 FB
    //
    //    Each iteration polls for a *different* echo value (the bytes
    //    shift through A.low via XBA + LDA [$91],Y + INC A), so no single
    //    state-side apuram value satisfies all the waits. The SPC code
    //    that would echo is itself stuck in a mid-byte loop because the
    //    cycle-precise CPU<->SPC sync legacy snes9x didn't preserve.
    //
    //    Bypass the whole mess by replacing the broken SPC routine with a
    //    tight echo loop: SPC reads each cpu.registers[N] (CPU's write to
    //    $2140+N) and immediately copies it into apuram[$F4+N] (what CPU
    //    reads at $2140+N). The CPU's CMP/BNE waits satisfy within ~32
    //    SPC cycles. Audio is lost — the alternative is a hard freeze.
    //
    //    $1947 is chosen because it's where Yoshi's Safari's mid-upload
    //    routine sits (and similarly-stuck states tend to land in the
    //    same upload-protocol area of apuram); overwriting it is safe
    //    because that routine is already deadlocked.
    {
        static const uint8_t echo_loop[] = {
            0xE5, 0xF4, 0x00,  // MOV A, !$F4
            0xC5, 0xF4, 0x00,  // MOV !$F4, A
            0xE5, 0xF5, 0x00,  // MOV A, !$F5
            0xC5, 0xF5, 0x00,  // MOV !$F5, A
            0xE5, 0xF6, 0x00,  // MOV A, !$F6
            0xC5, 0xF6, 0x00,  // MOV !$F6, A
            0xE5, 0xF7, 0x00,  // MOV A, !$F7
            0xC5, 0xF7, 0x00,  // MOV !$F7, A
            0x2F, 0xE6,        // BRA $1947 (-26)
        };
        const size_t echo_addr = 0x1947;
        if (echo_addr + sizeof(echo_loop) <= 65536)
            std::memcpy(&snd[echo_addr], echo_loop, sizeof(echo_loop));

        // Override SMP PC (smp state int32[3], at SND offset 65536+12=65548).
        if (snd.size() >= 65552)
            wr_u32_le(&snd[65548], uint32_t(echo_addr));
    }

    // Keep apuram[$F4..$F7] zeroed at boot so the very first CPU CMP after
    // resume reads 0; the echo loop above repopulates them with valid
    // values once SPC executes its first iteration.
    snd[0xF4] = 0;
    snd[0xF5] = 0;
    snd[0xF6] = 0;
    snd[0xF7] = 0;

    // 7a. Force NMI enable in the FillRAM I/O register shadow. Legacy
    //     mid-audio-init states often have $4200 = $00 (NMI disabled
    //     while uploading SPC code so the upload protocol isn't disturbed
    //     by vblank). In legacy snes9x the game's resume path eventually
    //     re-enables NMI via STA $4200, but modern snes9x is stricter and
    //     gets parked in main-loop subroutines that depend on a vblank
    //     NMI driver to advance. Setting bit 7 (NMI enable) lets the
    //     game's NMI vector fire on the next vblank — which restarts the
    //     frame loop at $80:CBA0 (INC frame counter -> JSL chain -> BIT
    //     $4212 vblank wait -> RTI/RTL).
    //
    //     We OR with $80 instead of overwriting so we preserve the
    //     legacy V/H-IRQ enable bits (5-4) and joypad-read bit (0) the
    //     legacy state did capture.
    if (s9x.sections.count("FIL")) {
        Bytes& fil = s9x.section("FIL");
        if (fil.size() > 0x4200) {
            fil[0x4200] |= 0x80;
        }
    }

    // 7b. Zero APU side timing accumulators so modern snes9x doesn't
    //     think it needs to catch the APU up to a stale CPU cycle count
    //     on load. SND tail layout per apu.cpp::S9xAPUSaveState:
    //       [66342..66345]  spc::reference_time
    //       [66346..66349]  spc::remainder
    //       [66350..66353]  dsp.clock
    //
    //     CPU side Cycles / PrevCycles / V_Counter are INTENTIONALLY LEFT
    //     ALONE — verified against a working .mss-converted .000 state
    //     which has CPU.Cycles=$330 / V_Counter=$B4 and runs fine in modern
    //     snes9x. Zeroing those caused the resume to land at scanline 0
    //     instead of the legacy save's actual scanline (12 for Yoshi's
    //     Safari), which seems to confuse modern snes9x's frame state.
    if (snd.size() >= 66354) {
        std::memset(&snd[66342], 0, 12);
    }

    // 7b''. Rebuild PPU pre-CGDATA fields (bytes 0..63) from FIL register
    //       state. Legacy snes9x 1.5.x had a different PPU struct layout
    //       in this region (smaller VMA/WRAM/BG field types), so legacy
    //       PPU bytes 0..57 don't map to modern v12 PPU bytes 0..63
    //       correctly. The 6-byte mismatch shifts CGDATA from its
    //       expected v12 offset 64 down to 59, and modern snes9x then
    //       reads VMA / BG / BGMode / CGADD from wrong positions —
    //       result: corrupt BG tile data pointers and a yellow/pink
    //       palette-fallback render (see Yoshi's Safari boss screen).
    //
    //       Reconstruct the 64 pre-CGDATA bytes from FIL register
    //       state (which we have correctly preserved). HOffset / VOffset
    //       latches aren't in FIL — we default them to 0; first frame
    //       after resume may show a 1-frame BG-scroll glitch which the
    //       game's NMI handler corrects immediately on next vblank.
    if (s9x.sections.count("PPU") && s9x.sections.count("FIL")) {
        Bytes& ppu = s9x.section("PPU");
        const Bytes& fil = s9x.section("FIL");
        if (ppu.size() >= 64 + 512 && fil.size() >= 0x2140) {
            auto u16be = [&ppu](size_t off, uint16_t v) {
                ppu[off] = uint8_t(v >> 8); ppu[off+1] = uint8_t(v & 0xFF);
            };
            // Save existing CGDATA bytes (currently at wrong offset 59
            // due to the 5-byte shift) so we can reposition them.
            // After insertion of 5 bytes, CGDATA should land at byte 64.
            Bytes cgdata(ppu.begin() + 59, ppu.begin() + 59 + 512);
            Bytes tail(ppu.begin() + 59 + 512, ppu.end());

            // Zero pre-CGDATA region fully.
            std::memset(&ppu[0], 0, 64);

            // VMA struct (10 bytes total).
            uint8_t vmain = fil[0x2115];
            ppu[0] = (vmain & 0x80) ? 1 : 0;                  // VMA.High (bool8)
            // Increment value per VMAIN bits 0-1: 00=1, 01=32, 10/11=128
            static const uint8_t vma_inc_table[4] = { 1, 32, 128, 128 };
            ppu[1] = vma_inc_table[vmain & 0x03];             // VMA.Increment
            u16be(2, fil[0x2116] | (uint16_t(fil[0x2117]) << 8)); // VMA.Address
            // Mask1, FullGraphicCount, Shift — derived from VMAIN remap bits
            // 2-3 for full-graphics rotation. We don't have these exactly;
            // 0 is safe for non-rotation modes.
            u16be(4, 0xFFFF);                                  // VMA.Mask1
            u16be(6, 0);                                       // VMA.FullGraphicCount
            u16be(8, 0);                                       // VMA.Shift

            // WRAM (uint32 BE at offset 10..13) — derived from $2181-$2183
            uint32_t wram_addr = uint32_t(fil[0x2181]) |
                                 (uint32_t(fil[0x2182]) << 8) |
                                 (uint32_t(fil[0x2183] & 1) << 16);
            ppu[10] = uint8_t(wram_addr >> 24);
            ppu[11] = uint8_t(wram_addr >> 16);
            ppu[12] = uint8_t(wram_addr >> 8);
            ppu[13] = uint8_t(wram_addr & 0xFF);

            // BG[0..3]: 11 bytes each at offsets 14, 25, 36, 47.
            for (int n = 0; n < 4; ++n) {
                size_t bg = 14 + n * 11;
                uint8_t bgnsc = fil[0x2107 + n];                  // $2107..$210A
                uint16_t scbase = (uint16_t)((bgnsc & 0xFC) << 8);
                u16be(bg + 0, scbase);                            // SCBase
                u16be(bg + 2, 0);                                  // HOffset (latch — default 0)
                u16be(bg + 4, 0);                                  // VOffset (latch — default 0)
                ppu[bg + 6] = (fil[0x2105] >> (4 + n)) & 1;       // BGSize
                // NameBase: BG12NBA / BG34NBA each pack 2 BGs in nibbles
                uint8_t nba = (n < 2) ? fil[0x210B] : fil[0x210C];
                uint8_t nb_nib = (n & 1) ? (nba >> 4) : (nba & 0x0F);
                u16be(bg + 7, uint16_t(nb_nib) << 12);            // NameBase
                u16be(bg + 9, bgnsc & 0x03);                       // SCSize
            }

            // BGMode, BG3Priority, CGFLIP, CGFLIPRead, CGADD at offsets 58-62.
            uint8_t bgmode = fil[0x2105];
            ppu[58] = bgmode & 0x07;                              // BGMode
            ppu[59] = (bgmode >> 3) & 1;                          // BG3Priority
            ppu[60] = 0;                                          // CGFLIP
            ppu[61] = 0;                                          // CGFLIPRead
            ppu[62] = fil[0x2121];                                // CGADD
            ppu[63] = 0;                                          // CGSavedByte

            // Restore CGDATA + the rest of the section at correct offsets.
            // ppu is currently 2652 bytes; CGDATA goes to 64..575; tail
            // continues from byte 576. Since legacy gave us 5 bytes too few
            // before CGDATA, the original tail starts 5 bytes earlier. We
            // need to slide it.
            std::memcpy(&ppu[64], cgdata.data(), 512);
            // The original tail (everything after CGDATA in legacy=2649-byte
            // PPU) has its own structure that lined up with modern v12 once
            // CGDATA is moved. Copy back.
            size_t tail_dst = 64 + 512;
            size_t tail_len = std::min(tail.size(), ppu.size() - tail_dst);
            if (tail_len > 0)
                std::memcpy(&ppu[tail_dst], tail.data(), tail_len);
        }
    }

    // 7b'''. Zero the OBJ[128] sprite array (PPU bytes 576..1983) and
    //        the OAMData region. Legacy v6 PPU's OBJ struct had smaller
    //        field types (e.g. uint8 HPos instead of int16) so legacy
    //        OBJ bytes don't map to modern v12's 11-byte-per-OBJ layout.
    //        Result: VFlip/HFlip/Priority/Palette read as garbage values
    //        (e.g. $95 for VFlip which should only ever be 0 or 1),
    //        sprites render with corrupt tile/palette indices.
    //
    //        Fix is benign: zero the whole sprite-table region. Most
    //        games' NMI handler DMA-copies OAM from WRAM ($0200-$03FF
    //        typically) into PPU OAM each vblank via $4300+/$420B,
    //        which immediately repopulates PPU.OBJ[] correctly. So
    //        sprites are blank for at most 1 frame post-resume.
    //
    //        Modern v12 PPU layout post-CGDATA:
    //          [64..575]      CGDATA (512 bytes)
    //          [576..1983]    OBJ[128] (1408 bytes, 11 bytes each)
    //          [1984..2527]   OAMData (544 bytes) + intermediate fields
    //          [2528..2641]   timer / mode 7 / window / clip fields
    //          [2641..2642]   ScreenHeight (set by step 7b' below)
    if (s9x.sections.count("PPU")) {
        Bytes& ppu = s9x.section("PPU");
        if (ppu.size() >= 1984) {
            std::memset(&ppu[576], 0, 1408);  // OBJ[128]
        }
    }

    // 7b'. Override PPU.ScreenHeight in the upgraded PPU section. Legacy
    //      snes9x 1.5.x had a different PPU struct layout, and the byte
    //      that ends up at v12 offset 2641 (= ScreenHeight, uint16 BE)
    //      after the v6→v12 promoter's byte-shift is some unrelated
    //      legacy field. Modern snes9x reads it as ScreenHeight and gets
    //      values like $6403 (= 25603) which makes the vblank-entry check
    //      `V_Counter == ScreenHeight + 1` never match — vblank handler
    //      never runs, SCAN_KEYS_FLAG never set, S9xMainLoop never returns,
    //      Windows marks snes9x as Not Responding.
    //
    //      Hardcode the correct value: 224 for NTSC standard, 239 if the
    //      game enabled overscan via $2133 bit 2 (= FIL[$2133] & 0x04).
    if (s9x.sections.count("PPU")) {
        Bytes& ppu = s9x.section("PPU");
        if (ppu.size() >= 2643) {
            uint8_t setini = 0;
            if (s9x.sections.count("FIL")) {
                const Bytes& fil = s9x.section("FIL");
                if (fil.size() > 0x2133) setini = fil[0x2133];
            }
            uint16_t sh = (setini & 0x04) ? 239 : 224;
            ppu[2641] = uint8_t(sh >> 8);   // BE: high byte first
            ppu[2642] = uint8_t(sh & 0xFF);
        }
    }

    // 7c. Clear CPU.Flags. Legacy snes9x 1.5.x reused some bits in this
    //     field for purposes that modern snes9x interprets very
    //     differently — most damagingly bit 2 (0x04). Modern snes9x
    //     treats this bit as SINGLE_STEP_FLAG (see snes9x.h), which puts
    //     the emulator into "step one instruction per frame call"
    //     debugger mode. Verified on Yoshi's Safari: legacy Flags=$14
    //     (= SCAN_KEYS_FLAG | SINGLE_STEP_FLAG in modern terms) made the
    //     game appear permanently frozen even though CPU was advancing
    //     ~1 instruction every 16ms.
    //
    //     CPU.Flags lives at offset 12 of the CPU section (4-byte BE
    //     INT_V) in both v6 and v12 layouts. None of these flag bits are
    //     game-state — they're emulator-internal (debugger, frame
    //     advance, key-scan flags). Clearing them is always safe.
    if (s9x.sections.count("CPU")) {
        Bytes& cpu = s9x.section("CPU");
        if (cpu.size() >= 16) {
            cpu[12] = cpu[13] = cpu[14] = cpu[15] = 0;
        }
    }

    // 8. Drop the legacy-only sections so save() emits a clean modern state.
    for (const char* tag : { "APU", "ARE", "IAP", "SOU" })
        s9x.sections.erase(tag);

    s9x.version = 12;
    s9x.original_version = 12;
    s9x.save(out_path);
}

// ===== output naming =====================================================

std::string tag_output_path(const std::string& path, const std::string& tag) {
    fs::path p = path;
    std::string stem = p.stem().string();
    std::string ext  = p.extension().string();
    if (stem.find(tag) != std::string::npos) return path;
    fs::path out = p.parent_path() / (stem + tag + ext);
    return out.string();
}
