// s9x_format.cpp - SNES9x .009 snapshot read/write and field decoders.
// Mirrors the parsing in s9x_to_mss.py (which mirrors snes9x's snapshot.cpp).
#include "s9x_mss.h"
#include "miniz.h"
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>

// SnapPPU[] derived offsets (from snes9x/snapshot.cpp + ppu.h sizes).
const int PPU_CGDATA_OFF  = 10 /*VMA*/ + 4 /*WRAM*/ + 44 /*BG*/ + 6 /*pre-CG*/;   // 64
const int PPU_OBJ_LEN     = 128 * (2 + 2 + 1 + 1 + 2 + 1 + 1 + 1);                 // 1408
const int PPU_OAMDATA_OFF = PPU_CGDATA_OFF + 512 + PPU_OBJ_LEN
                            + (1 + 1 + 1 + 2 + 2 + 1 + 2 + 2 + 1 + 1 + 1 + 2 + 2); // 2003

// Convert a snes9x 1.5.x legacy section to the v6 equivalent layout, so the
// existing v6 -> v12 upgrader can take it the rest of the way. Legacy magic is
// "#!snes9x:NNNN" (NNNN = 1500/1510/1520...), not the modern "#!s9xsnp:NNNN".
//
// Key differences vs v6:
//   CPU: 58 bytes (legacy) vs 56 (v6) vs 48 (v12). Layout adds two trailing
//        bytes we drop; the first 56 bytes happen to map closely enough to the
//        v6 schema (Cycles..WaitingForInterrupt at the same offsets) that the
//        existing v6 -> v12 path consumes the result.
//   TIM: 57 bytes (legacy) vs 61 (v6). Legacy lacks the IRQTriggerCycles +
//        APUAllowTimeOverflow pair appended in v7; pad with safe defaults.
//   PPU: 2649 bytes in both — directly compatible.
//   REG/DMA/VRA/RAM/SRA/FIL: byte-identical layout.
//   APU/ARE/ARA/SOU/IAP: legacy Blargg-era audio, split across five sections.
//        We synthesise a minimal SND: copy apuram (ARA) into [0..65535] and
//        leave the SMP / DSP state at zeros — the SPC reboots from the apuram
//        contents on first frame, audio glitches briefly but the game advances.
static Bytes upgrade_legacy_section(const std::string& tag, Bytes raw) {
    if (tag == "CPU" && raw.size() == 58) {
        // Legacy snes9x 1.5.x CPU section uses a different in-memory layout
        // than v6: bytes 0..15 (Cycles, PrevCycles, V_Counter, Flags) match,
        // but bytes 16+ in legacy are PACKED differently (no CPU_IRQActive
        // pad byte at 16, different IRQPending position, plus serialised
        // pointers like WaitAddress that don't survive cross-build). Just
        // truncating to 56 bytes feeds garbage to the v6 -> v12 promoter
        // (symptom: MemSpeed = 1536, NextEvent = -240, etc.) which makes
        // modern snes9x deadlock on load. Rebuild the section in v6 layout
        // with the safe fields we can confidently copy from legacy + sane
        // defaults for everything else. snes9x re-derives most CPU
        // scheduling state on the first frame post-load, so defaults are
        // fine for those fields.
        Bytes v6cpu(56, 0);
        // Bytes 0..15: Cycles, PrevCycles, V_Counter, Flags — compatible across versions.
        std::memcpy(&v6cpu[0], &raw[0], 16);
        // Byte 16: CPU_IRQActive (v6 obsolete byte, stripped by v6 -> v12 promoter).
        v6cpu[16] = 0;
        // Bytes 17..20: IRQPending — leave 0; legacy IRQ state is unreliable
        // and snes9x re-derives from FillRAM on first frame.
        // Bytes 21..24: MemSpeed = SLOW_ONE_CYCLE (8 cycles, byte-swapped int32 BE).
        v6cpu[24] = 8;
        // Bytes 25..28: MemSpeedx2 = 16.
        v6cpu[28] = 16;
        // Bytes 29..32: FastROMSpeed = ONE_CYCLE (6).
        v6cpu[32] = 6;
        // Bytes 33..37: InDMA/InHDMA/InDMAorHDMA/InWRAMDMAorHDMA/HDMARanInDMA — all 0.
        // Byte 38: WhichEvent = HC_RENDER_EVENT (5) — safe scheduler entry point.
        v6cpu[38] = 5;
        // Bytes 39..42: NextEvent = Timings.RenderPos = 512 (int32 BE).
        v6cpu[39] = 0; v6cpu[40] = 0; v6cpu[41] = 0x02; v6cpu[42] = 0x00;
        // Byte 43: WaitingForInterrupt — 0 (most legacy states aren't mid-WAI).
        // Bytes 44..55: v6 obsolete WaitAddress/WaitCounter/PBPCAtOpcodeStart — 0,
        // stripped by v6 -> v12 promoter anyway.
        return v6cpu;
    }
    if (tag == "TIM" && raw.size() == 57) {
        // Legacy snes9x 1.5.x stored different live timing state in TIM
        // bytes 44..56 (DMACPUSync=18 OK, but NMIDMADelay had garbage like
        // $0002A171 and IRQFlagChanging had $12AE — clearly not the modern
        // semantics). RenderPos at offset 40-43 also differed (192 vs 512).
        // Build a clean v6 TIM (61 bytes) with safe modern constants and
        // copy ONLY the first 8 fields from legacy (H_Max_Master..HDMAStart,
        // which are dimensions of the scanline and identical across versions).
        // The v6 -> v12 promoter then appends IRQTriggerCycles +
        // APUAllowTimeOverflow + NextIRQTimer.
        Bytes v6tim(61, 0);
        // bytes 0..31: H_Max_Master..HDMAStart (8 int32 BE constants) from legacy.
        std::memcpy(&v6tim[0], &raw[0], 32);
        // bytes 32-35: NMITriggerPos = 0xFFFF (no NMI pending).
        v6tim[34] = 0xFF; v6tim[35] = 0xFF;
        // bytes 36-39: WRAMRefreshPos = 538 = 0x0000021A (big-endian).
        // Byte indices into the 4-byte BE field: 36=MSB, 37, 38, 39=LSB.
        // So 0x02 goes at byte 38, 0x1A at byte 39 — NOT 37/38.
        v6tim[38] = 0x02; v6tim[39] = 0x1A;
        // bytes 40-43: RenderPos = 512 (modern SNES_RENDER_START_HC).
        v6tim[42] = 0x02;
        // byte 44: InterlaceField = 0.
        // bytes 45-48: DMACPUSync = 18.
        v6tim[48] = 18;
        // bytes 49-52: NMIDMADelay = 24.
        v6tim[52] = 24;
        // bytes 53-56: IRQFlagChanging = 0 (already).
        // bytes 57-60: APUSpeedup = 0 (already, will be added by v6->v12 promoter
        //              via the IRQTriggerCycles append).
        return v6tim;
    }
    if (tag == "CTL" && raw.size() == 86) {
        // Legacy CTL (snes9x 1.5.x): 86 bytes, ver byte = 3 at offset 0.
        // Modern snes9x v12 expects 91 bytes with ver byte = 4 and a trailing
        // internal_macs[5] field that debuted in v10. Three things to fix:
        //   - Bump ver byte 0 from 3 to 4 (modern loader checks this).
        //   - Zero out internal[60] at offset 26..85 — that's the controller
        //     state machine's mid-scan bookkeeping; the legacy layout doesn't
        //     match modern bit-for-bit, and snes9x re-populates these on the
        //     next auto-joypad pass anyway.
        //   - Append 5 zero bytes for internal_macs[5].
        // Without this the SControlSnapshot loader sees a stale version /
        // garbage state and the auto-joypad path locks the CPU immediately.
        raw[0] = 4;
        std::memset(&raw[26], 0, 60);
        raw.insert(raw.end(), {0, 0, 0, 0, 0});
        return raw;
    }
    return raw;
}

// Normalize a section to v12-equivalent layout (from _upgrade_section_to_v12 in .py).
static Bytes upgrade_section_to_v12(int version, const std::string& tag, Bytes raw) {
    if (version >= 12) return raw;

    if (tag == "CPU" && version < 7) {
        if (raw.size() != 56)
            throw ConvertError("v6 CPU section: expected 56 bytes");
        static const int we_remap[13] = {0, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 1};
        Bytes tmp; tmp.reserve(48);
        tmp.insert(tmp.end(), raw.begin(), raw.begin() + 16);    // Cycles..Flags
        tmp.insert(tmp.end(), raw.begin() + 17, raw.begin() + 44); // IRQPending..WaitingForInterrupt
        tmp.insert(tmp.end(), 5, 0);                              // NMIPending..IRQExternal
        int we = tmp[37];
        tmp[37] = (we >= 0 && we < 13) ? we_remap[we] : 1;
        return tmp;
    }
    if (tag == "PPU" && version < 11) {
        if (raw.size() != 2649)
            throw ConvertError("pre-v11 PPU section: expected 2649 bytes");
        Bytes tmp; tmp.reserve(2652);
        tmp.insert(tmp.end(), raw.begin(),       raw.begin() + 63);
        tmp.push_back(0);                                         // CGSavedByte
        tmp.insert(tmp.end(), raw.begin() + 63,  raw.begin() + 2649);
        tmp.push_back(0); tmp.push_back(0);                       // VRAMReadBuffer
        return tmp;
    }
    if (tag == "TIM") {
        if (version < 7) {
            if (raw.size() != 61)
                throw ConvertError("v6 TIM section: expected 61 bytes");
            raw.push_back(0); raw.push_back(0); raw.push_back(0); raw.push_back(0x0e); // IRQTriggerCycles = 14
            raw.push_back(0); // APUAllowTimeOverflow
        }
        if (version < 11) {
            if (raw.size() != 66)
                throw ConvertError("pre-v11 TIM section: expected 66 bytes");
            raw.push_back(0x0f); raw.push_back(0xff); raw.push_back(0xff); raw.push_back(0xff); // NextIRQTimer
        }
        return raw;
    }
    return raw;
}

// Decompress with miniz if gzip-magic-headered. Returns raw payload either way.
static Bytes maybe_gunzip(const Bytes& in) {
    if (in.size() >= 2 && in[0] == 0x1f && in[1] == 0x8b) {
        // gzip stream - peel header and use raw deflate via tinfl, the simplest path
        // that miniz exposes for arbitrary input sizes is mz_uncompress on zlib input,
        // so we use tinfl_decompress_mem_to_heap with no header skip after a manual
        // gzip header strip.
        size_t pos = 10;
        if (in.size() < pos) throw ConvertError("truncated gzip header");
        uint8_t flags = in[3];
        if (flags & 0x04) { // FEXTRA
            if (pos + 2 > in.size()) throw ConvertError("bad gzip FEXTRA");
            uint16_t xlen = uint16_t(in[pos]) | (uint16_t(in[pos+1]) << 8);
            pos += 2 + xlen;
        }
        if (flags & 0x08) { while (pos < in.size() && in[pos]) pos++; pos++; } // FNAME
        if (flags & 0x10) { while (pos < in.size() && in[pos]) pos++; pos++; } // FCOMMENT
        if (flags & 0x02) pos += 2; // FHCRC
        if (pos + 8 > in.size()) throw ConvertError("truncated gzip body");
        size_t comp_end = in.size() - 8; // trailing CRC32+ISIZE
        size_t out_size = 0;
        void* out = tinfl_decompress_mem_to_heap(in.data() + pos, comp_end - pos, &out_size, 0);
        if (!out) throw ConvertError("gzip decompress failed");
        Bytes res(reinterpret_cast<uint8_t*>(out), reinterpret_cast<uint8_t*>(out) + out_size);
        mz_free(out);
        return res;
    }
    return in;
}

S9xState S9xState::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw ConvertError("cannot open: " + path);
    Bytes raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    raw = maybe_gunzip(raw);
    bool legacy = raw.size() >= 11 && std::memcmp(raw.data(), "#!snes9x:", 9) == 0;
    bool modern = raw.size() >= 11 && std::memcmp(raw.data(), "#!s9xsnp:", 9) == 0;
    if (!legacy && !modern)
        throw ConvertError("not a SNES9x save state (missing magic)");

    // Parse version: digits up to '\n'.
    size_t eol = 9;
    while (eol < raw.size() && raw[eol] != '\n') eol++;
    if (eol == raw.size()) throw ConvertError("no version newline");
    int version = 0;
    for (size_t i = 9; i < eol; ++i) {
        if (raw[i] < '0' || raw[i] > '9') throw ConvertError("bad version");
        version = version * 10 + (raw[i] - '0');
    }

    S9xState st;
    st.original_version = version;
    // Internally we always work in the modern v6+ numbering; the legacy
    // "snes9x 1.51 -> 1510" version is meaningless to the rest of the
    // converter. Pretend legacy is v6 and let the section promoters take it
    // to v12.
    st.version = legacy ? 6 : version;

    size_t i = eol + 1;
    while (i < raw.size()) {
        if (i + 11 > raw.size()) break;
        if (raw[i + 3] != ':' || raw[i + 10] != ':') break;
        std::string tag(reinterpret_cast<const char*>(raw.data() + i), 3);
        // size is 6 ASCII digits
        int sz = 0;
        for (int k = 0; k < 6; ++k) {
            char c = char(raw[i + 4 + k]);
            if (c < '0' || c > '9') throw ConvertError("bad section size for " + tag);
            sz = sz * 10 + (c - '0');
        }
        size_t data_off = i + 11;
        if (data_off + sz > raw.size()) throw ConvertError("section " + tag + " runs past EOF");
        st.sections[tag] = Bytes(raw.begin() + data_off, raw.begin() + data_off + sz);
        i = data_off + sz;
    }

    if (legacy) {
        // Normalise sections that have a different byte size from their v6
        // equivalent. After this the v6 -> v12 promoter below handles the rest.
        for (const char* tag : {"CPU", "TIM", "CTL"}) {
            auto it = st.sections.find(tag);
            if (it != st.sections.end())
                it->second = upgrade_legacy_section(tag, std::move(it->second));
        }
        // Pad SRA to 512 KB. Legacy snes9x always wrote 128 KB regardless of
        // the cartridge's actual SRAM size; modern snes9x reads up to
        // Memory.SRAM_SIZE bytes (which is 512 KB for games like Yoshi's
        // Safari) and zero-fills the partial read, which leaves the upper
        // SRAM at zeros and can crash games that rely on data there. Pad to
        // 512 KB so any SRAM_SIZE up to 4 Mbit works; snes9x will read only
        // what its cartridge declares and discard the rest.
        auto sra_it = st.sections.find("SRA");
        if (sra_it != st.sections.end() && sra_it->second.size() < 0x80000)
            sra_it->second.resize(0x80000, 0);
        // Synthesise SND from the legacy ARA (apuram) section. SMP, DSP and
        // CPU-port state are zeroed — snes9x will reboot the SPC from apuram
        // contents on the first frame after load. Audio glitches briefly but
        // the game advances and graphics/state are intact.
        auto ara = st.sections.find("ARA");
        if (ara != st.sections.end() && ara->second.size() == 65536) {
            Bytes snd(66560, 0);
            std::memcpy(snd.data(), ara->second.data(), 65536);
            st.sections["SND"] = std::move(snd);
        }
        // Keep APU / ARE / IAP / SOU around so apply_forward_smp_legacy can
        // pull the SPC700 register state (PC/A/X/Y/SP/PSW), DSP regs, and
        // timer state out of them when synthesising .mss SPC keys. The ARA
        // is no longer needed (apuram lives in SND now); GBJ and SHO have
        // no modern equivalent.
        for (const char* tag : {"ARA", "GBJ", "SHO"})
            st.sections.erase(tag);
    }

    for (const char* tag : {"CPU", "PPU", "TIM"}) {
        auto it = st.sections.find(tag);
        if (it != st.sections.end())
            it->second = upgrade_section_to_v12(st.version, tag, std::move(it->second));
    }
    if (st.version < 12) st.version = 12;
    return st;
}

void S9xState::save(const std::string& path) const {
    // Write uncompressed: snes9x accepts both gzipped and plain .009 streams.
    // The version line is "#!s9xsnp:NNNN\n" exactly (14 bytes) - snes9x's
    // S9xUnfreezeFromStream reads a fixed-width header and atoi's the
    // 4-digit version. See snapshot.cpp:1223.
    std::ostringstream out(std::ios::binary);
    char hdr[16];
    std::snprintf(hdr, sizeof(hdr), "#!s9xsnp:%04d\n", version);
    out.write(hdr, std::strlen(hdr));
    // The canonical write order in snes9x is: CPU REG PPU DMA APU SRA RAM SRT
    // CTL TIM SUB SAR SBG SCH CX4 DP1 ST1 ST2 BSX SRC OBC SFX SA1 SAR XBND...
    // The loader is order-agnostic since each section is self-describing. We
    // write the ones we have in a stable sequence so files diff cleanly.
    // Section order must match snes9x's UnfreezeStateFromCopy in snapshot.cpp:
    // it reads sections in this exact sequence and errors out on any missing
    // section. NAM goes first, then the bulk fixed-section list, then optional
    // coprocessor/extra sections at the end.
    static const char* ORDER[] = {
        "NAM",
        "CPU", "REG", "PPU", "DMA",
        "VRA", "RAM", "SRA", "FIL",
        "SND",
        "CTL", "TIM",
        "SFX", "SA1", "SAR",
        "DP1", "DP2", "DP4",
        "ST0", "ST1", "ST2",
        "OBC", "S71", "SRT",
        "BSX", "MSU", "CX4",
        "SHO", "MOV",
    };
    auto write_section = [&](const std::string& tag, const Bytes& data) {
        char hdr[16];
        std::snprintf(hdr, sizeof(hdr), "%s:%06zu:", tag.c_str(), data.size());
        out.write(hdr, std::strlen(hdr));
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
    };
    for (const char* tag : ORDER) {
        auto it = sections.find(tag);
        if (it != sections.end()) write_section(it->first, it->second);
    }
    // Catch any tags not in the canonical order list.
    for (const auto& [tag, data] : sections) {
        bool listed = false;
        for (const char* t : ORDER) if (tag == t) { listed = true; break; }
        if (!listed) write_section(tag, data);
    }

    std::string s = out.str();

    // Wrap as gzip - snes9x always writes gzipped .009 files (size ~3-4x
    // smaller, and various OS shell extensions identify .009 as such). Both
    // are accepted by S9xUnfreezeFromStream via the gunzip path. Format per
    // RFC 1952: 10-byte header, raw deflate, CRC32, ISIZE.
    const uint8_t* src = reinterpret_cast<const uint8_t*>(s.data());
    size_t        src_len = s.size();
    size_t        comp_len = 0;
    void* comp = tdefl_compress_mem_to_heap(src, src_len, &comp_len, 0);   // flags=0 -> raw deflate
    if (!comp) throw ConvertError("gzip deflate failed");

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { mz_free(comp); throw ConvertError("cannot write: " + path); }

    const uint8_t hdr_bytes[10] = {
        0x1f, 0x8b,   // magic
        0x08,         // CM = deflate
        0x00,         // FLG = no extras
        0x00, 0x00, 0x00, 0x00,  // mtime = 0
        0x00,         // XFL
        0xff,         // OS = unknown
    };
    f.write(reinterpret_cast<const char*>(hdr_bytes), 10);
    f.write(reinterpret_cast<const char*>(comp), comp_len);
    uint32_t crc   = uint32_t(mz_crc32(MZ_CRC32_INIT, src, src_len));
    uint32_t isize = uint32_t(src_len);
    uint8_t  tail[8];
    wr_u32_le(&tail[0], crc);
    wr_u32_le(&tail[4], isize);
    f.write(reinterpret_cast<const char*>(tail), 8);
    mz_free(comp);
}

const Bytes& S9xState::section(const std::string& tag) const {
    auto it = sections.find(tag);
    if (it == sections.end()) throw ConvertError("missing section: " + tag);
    return it->second;
}
Bytes& S9xState::section(const std::string& tag) {
    auto it = sections.find(tag);
    if (it == sections.end()) throw ConvertError("missing section: " + tag);
    return it->second;
}

uint8_t  S9xState::ppu_u8 (int off) const { return section("PPU").at(off); }
uint16_t S9xState::ppu_u16_be(int off) const {
    const auto& p = section("PPU"); return rd_u16_be(&p[off]);
}
int16_t  S9xState::ppu_s16_be(int off) const {
    const auto& p = section("PPU"); return rd_s16_be(&p[off]);
}
Bytes    S9xState::ppu_slice(int off, int n) const {
    const auto& p = section("PPU");
    if (off + n > int(p.size())) throw ConvertError("PPU slice out of range");
    return Bytes(p.begin() + off, p.begin() + off + n);
}

uint8_t S9xState::fil(int addr) const {
    const auto& p = section("FIL");
    if (addr < 0 || addr >= int(p.size())) return 0;
    return p[addr];
}

S9xPPUFields s9x_decode_ppu(const S9xState& s) {
    S9xPPUFields f{};
    f.VMA_High      = s.ppu_u8(0);
    f.VMA_Increment = s.ppu_u8(1);
    f.VMA_Address   = s.ppu_u16_be(2);
    for (int n = 0; n < 4; ++n) {
        int base = 14 + n * 11;
        f.BG_SCBase[n]   = s.ppu_u16_be(base + 0);
        f.BG_HOffset[n]  = s.ppu_u16_be(base + 2);
        f.BG_VOffset[n]  = s.ppu_u16_be(base + 4);
        f.BG_BGSize[n]   = s.ppu_u8    (base + 6);
        f.BG_NameBase[n] = s.ppu_u16_be(base + 7);
        f.BG_SCSize[n]   = s.ppu_u16_be(base + 9);
    }
    f.BGMode         = s.ppu_u8(58);
    f.BG3Priority    = s.ppu_u8(59);
    f.CGADD          = s.ppu_u8(62);
    int post         = PPU_CGDATA_OFF + 512 + PPU_OBJ_LEN;       // 1984
    f.OBJThroughMain = s.ppu_u8    (post + 0);
    f.OBJThroughSub  = s.ppu_u8    (post + 1);
    f.OBJNameBase    = s.ppu_u16_be(post + 3);
    f.OBJNameSelect  = s.ppu_u16_be(post + 5);
    f.OBJSizeSelect  = s.ppu_u8    (post + 7);
    f.OAMAddr        = s.ppu_u16_be(post + 8);
    f.ForcedBlanking = s.ppu_u8(2636);
    f.FixedColourRed   = s.ppu_u8(2637);
    f.FixedColourGreen = s.ppu_u8(2638);
    f.FixedColourBlue  = s.ppu_u8(2639);
    f.Brightness     = s.ppu_u8(2640);
    f.ScreenHeight   = s.ppu_u16_be(2641);
    f.HDMA           = s.ppu_u8(2646);
    f.VRAMReadBuffer = s.ppu_u16_be(2650);
    f.MatrixA = s.ppu_s16_be(2573);
    f.MatrixB = s.ppu_s16_be(2575);
    f.MatrixC = s.ppu_s16_be(2577);
    f.MatrixD = s.ppu_s16_be(2579);
    f.CentreX = s.ppu_s16_be(2581);
    f.CentreY = s.ppu_s16_be(2583);
    f.M7HOFS  = s.ppu_s16_be(2585);
    f.M7VOFS  = s.ppu_s16_be(2587);
    return f;
}

std::vector<S9xDmaChannel> s9x_decode_dma(const S9xState& s) {
    const auto& dma = s.section("DMA");
    std::vector<S9xDmaChannel> out(8);
    for (int ch = 0; ch < 8; ++ch) {
        int b = ch * 19;
        if (b + 19 > int(dma.size())) throw ConvertError("DMA section too short");
        S9xDmaChannel& c = out[ch];
        c.ReverseTransfer        = dma[b + 0];
        c.HDMAIndirectAddressing = dma[b + 1];
        c.UnusedBit43x0          = dma[b + 2];
        c.AAddressFixed          = dma[b + 3];
        c.AAddressDecrement      = dma[b + 4];
        c.TransferMode           = dma[b + 5];
        c.BAddress               = dma[b + 6];
        c.AAddress  = rd_u16_be(&dma[b + 7]);
        c.ABank                  = dma[b + 9];
        c.DMACount  = rd_u16_be(&dma[b + 10]);
        c.IndirectBank           = dma[b + 12];
        c.Address   = rd_u16_be(&dma[b + 13]);
        c.Repeat                 = dma[b + 15];
        c.LineCount              = dma[b + 16];
        c.UnknownByte            = dma[b + 17];
        c.DoTransfer             = dma[b + 18];
    }
    return out;
}

S9xCpuState s9x_decode_cpu(const S9xState& s) {
    const auto& c = s.section("CPU");
    if (c.size() < 48) throw ConvertError("CPU section too small");
    S9xCpuState x{};
    x.Cycles             = rd_s32_be(&c[0]);
    x.PrevCycles         = rd_s32_be(&c[4]);
    x.V_Counter          = rd_s32_be(&c[8]);
    x.Flags              = rd_u32_be(&c[12]);
    x.IRQPending         = rd_s32_be(&c[16]);
    x.MemSpeed           = rd_s32_be(&c[20]);
    x.MemSpeedx2         = rd_s32_be(&c[24]);
    x.FastROMSpeed       = rd_s32_be(&c[28]);
    x.InDMA              = c[32];
    x.InHDMA             = c[33];
    x.InDMAorHDMA        = c[34];
    x.InWRAMDMAorHDMA    = c[35];
    x.HDMARanInDMA       = c[36];
    x.WhichEvent         = c[37];
    x.NextEvent          = rd_s32_be(&c[38]);
    x.WaitingForInterrupt= c[42];
    x.NMIPending         = c[43];
    x.IRQLine            = c[44];
    x.IRQTransition      = c[45];
    x.IRQLastState       = c[46];
    x.IRQExternal        = c[47];
    return x;
}

S9xRegisters s9x_decode_registers(const S9xState& s) {
    const auto& r = s.section("REG");
    if (r.size() < 16) throw ConvertError("REG section too small");
    S9xRegisters x{};
    x.PB = r[0];
    x.DB = r[1];
    x.P  = rd_u16_be(&r[2]);
    x.A  = rd_u16_be(&r[4]);
    x.D  = rd_u16_be(&r[6]);
    x.S  = rd_u16_be(&r[8]);
    x.X  = rd_u16_be(&r[10]);
    x.Y  = rd_u16_be(&r[12]);
    x.PC = rd_u16_be(&r[14]);
    return x;
}

Bytes s9x_ppu_cgdata_be(const S9xState& s) { return s.ppu_slice(PPU_CGDATA_OFF, 512); }
Bytes s9x_ppu_oam      (const S9xState& s) { return s.ppu_slice(PPU_OAMDATA_OFF, 544); }

// Forward declaration; defined just below probe_s9x.
Bytes maybe_gunzip_first14(const Bytes& in);

// ---- header probe: cheap magic+version check without parsing all sections ----
ProbeResult probe_s9x(const std::string& path) {
    ProbeResult r;
    std::ifstream f(path, std::ios::binary);
    if (!f) { r.error = "cannot open file"; return r; }
    // Read enough for gzip detection + 14-byte snes9x header.
    char head[32]{};
    f.read(head, sizeof(head));
    std::streamsize got = f.gcount();
    if (got < 14) { r.error = "file too small to be a SNES9x save state"; return r; }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(head);
    Bytes payload_head;
    // gzipped snes9x save states are common - peek through the gzip header.
    if (p[0] == 0x1f && p[1] == 0x8b) {
        // Read whole file and decompress.
        f.clear(); f.seekg(0);
        Bytes all((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
        try {
            payload_head = maybe_gunzip_first14(all);   // declared below
        } catch (const std::exception& e) {
            r.error = std::string("gzip decode failed: ") + e.what();
            return r;
        }
        if (payload_head.size() < 14) {
            r.error = "decompressed stream too small";
            return r;
        }
        p = payload_head.data();
    }
    // Two snes9x magics exist in the wild:
    //   "#!s9xsnp" — modern, v6+ (snes9x 1.52 onwards). What we convert.
    //   "#!snes9x" — legacy, v1.50/1.51 era. Different section layout (CPU=58
    //                bytes, separate APU/ARE/ARA/SOU/IAP audio sections, GBJ,
    //                etc.) — not supported by the converter.
    bool legacy = (std::memcmp(p, "#!snes9x", 8) == 0);
    bool modern = (std::memcmp(p, "#!s9xsnp", 8) == 0);
    if (!legacy && !modern) {
        r.error = "missing SNES9x magic (#!s9xsnp or #!snes9x)";
        return r;
    }
    if (p[8] != ':') { r.error = "malformed SNES9x header"; return r; }
    int v = 0;
    int i = 9;
    while (i < 14 && p[i] >= '0' && p[i] <= '9') { v = v * 10 + (p[i] - '0'); i++; }
    if (v == 0) { r.error = "could not parse version"; return r; }

    r.ok = true;
    r.version = v;
    if (legacy) {
        r.label = "SNES9x legacy v1.5.x (#!snes9x:" + std::to_string(v)
                + ") — best-effort conversion, audio may glitch briefly";
    } else {
        r.label = "SNES9x v" + std::to_string(v);
        if (v > 12) r.label += " (newer than v12 - not fully supported)";
    }
    return r;
}

// Helper used by probe_s9x for the gzipped case. Defined out-of-line so we
// can reuse the gzip stripping logic from maybe_gunzip without duplicating it.
Bytes maybe_gunzip_first14(const Bytes& in) {
    // Reuse the existing static function by exposing a wrapper. We inline a
    // copy of the strip logic here because maybe_gunzip is file-static above.
    if (in.size() < 2 || in[0] != 0x1f || in[1] != 0x8b)
        return in;
    size_t pos = 10;
    if (in.size() < pos) throw ConvertError("truncated gzip header");
    uint8_t flags = in[3];
    if (flags & 0x04) {
        if (pos + 2 > in.size()) throw ConvertError("bad gzip FEXTRA");
        uint16_t xlen = uint16_t(in[pos]) | (uint16_t(in[pos+1]) << 8);
        pos += 2 + xlen;
    }
    if (flags & 0x08) { while (pos < in.size() && in[pos]) pos++; pos++; }
    if (flags & 0x10) { while (pos < in.size() && in[pos]) pos++; pos++; }
    if (flags & 0x02) pos += 2;
    if (pos + 8 > in.size()) throw ConvertError("truncated gzip body");
    size_t comp_end = in.size() - 8;
    size_t out_size = 0;
    void* out = tinfl_decompress_mem_to_heap(in.data() + pos, comp_end - pos, &out_size, 0);
    if (!out) throw ConvertError("gzip decompress failed");
    Bytes res(reinterpret_cast<uint8_t*>(out), reinterpret_cast<uint8_t*>(out) + out_size);
    mz_free(out);
    return res;
}

S9xSmpState s9x_decode_smp(const S9xState& s) {
    const auto& snd = s.section("SND");
    if (snd.size() < 65536 + S9xSmpState::COUNT_ * 4)
        throw ConvertError("SND section too short for SMP state");
    S9xSmpState st{};
    for (int i = 0; i < S9xSmpState::COUNT_; ++i) {
        // Little-endian int32. Reuse rd_u32_le and reinterpret.
        st.v[i] = int32_t(rd_u32_le(&snd[65536 + i * 4]));
    }
    return st;
}
