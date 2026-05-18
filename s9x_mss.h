// s9x_mss.h - shared declarations for the SNES9x <-> Mesen2 save state converter.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>

using Bytes = std::vector<uint8_t>;

struct ConvertError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- byte helpers --------------------------------------------------------
inline uint16_t rd_u16_be(const uint8_t* p) { return (uint16_t(p[0]) << 8) | p[1]; }
inline uint16_t rd_u16_le(const uint8_t* p) { return (uint16_t(p[1]) << 8) | p[0]; }
inline uint32_t rd_u32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint32_t rd_u32_le(const uint8_t* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | p[0];
}
inline int16_t  rd_s16_be(const uint8_t* p) { return int16_t(rd_u16_be(p)); }
inline int32_t  rd_s32_be(const uint8_t* p) { return int32_t(rd_u32_be(p)); }
inline uint64_t rd_u64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

inline void wr_u16_le(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
inline void wr_u32_le(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
inline void wr_u64_le(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) { p[i] = v & 0xFF; v >>= 8; }
}
inline void wr_u16_be(uint8_t* p, uint16_t v) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
inline void wr_u32_be(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
}

// ---- SNES9x .009 snapshot ------------------------------------------------
struct S9xState {
    int                                version = 0;      // post-normalization (>= 12)
    int                                original_version = 0;
    std::map<std::string, Bytes>       sections;          // tag (3 chars) -> raw bytes

    static S9xState load(const std::string& path);
    void            save(const std::string& path) const; // writes uncompressed .009

    // Section accessors. Throws ConvertError if missing/too short.
    const Bytes& section(const std::string& tag) const;
    Bytes&       section(const std::string& tag);

    // PPU struct field accessors (all big-endian).
    uint8_t  ppu_u8 (int off) const;
    uint16_t ppu_u16_be(int off) const;
    int16_t  ppu_s16_be(int off) const;
    Bytes    ppu_slice(int off, int n) const;

    // FillRAM I/O register shadow (by CPU address).
    uint8_t fil(int addr) const;
};

struct S9xPPUFields {
    int     VMA_High, VMA_Increment, VMA_Address;
    int     BG_SCBase[4], BG_HOffset[4], BG_VOffset[4];
    int     BG_BGSize[4], BG_NameBase[4], BG_SCSize[4];
    int     BGMode, BG3Priority, CGADD;
    int     OBJThroughMain, OBJThroughSub, OBJNameBase, OBJNameSelect, OBJSizeSelect;
    int     OAMAddr;
    int     ForcedBlanking, FixedColourRed, FixedColourGreen, FixedColourBlue;
    int     Brightness, ScreenHeight, HDMA, VRAMReadBuffer;
    int     MatrixA, MatrixB, MatrixC, MatrixD;
    int     CentreX, CentreY, M7HOFS, M7VOFS;
};
S9xPPUFields s9x_decode_ppu(const S9xState& s);

struct S9xDmaChannel {
    int ReverseTransfer, HDMAIndirectAddressing, UnusedBit43x0;
    int AAddressFixed, AAddressDecrement, TransferMode, BAddress;
    int AAddress, ABank, DMACount, IndirectBank, Address;
    int Repeat, LineCount, UnknownByte, DoTransfer;
};
std::vector<S9xDmaChannel> s9x_decode_dma(const S9xState& s);

struct S9xCpuState {
    int32_t  Cycles, PrevCycles, V_Counter;
    uint32_t Flags;
    int32_t  IRQPending, MemSpeed, MemSpeedx2, FastROMSpeed;
    uint8_t  InDMA, InHDMA, InDMAorHDMA, InWRAMDMAorHDMA, HDMARanInDMA;
    uint8_t  WhichEvent;
    int32_t  NextEvent;
    uint8_t  WaitingForInterrupt, NMIPending, IRQLine, IRQTransition, IRQLastState, IRQExternal;
};
S9xCpuState s9x_decode_cpu(const S9xState& s);

struct S9xRegisters {
    int PB, DB, P, A, D, S, X, Y, PC;
};
S9xRegisters s9x_decode_registers(const S9xState& s);

struct S9xSmpState {
    // SMP::save_state writes 41 LE int32s (164 bytes) right after apuram.
    // Order verified against snes9x apu/bapu/smp/smp_state.cpp.
    int32_t v[41];
    enum {
        CLOCK = 0, OPCODE_NUMBER, OPCODE_CYCLE,
        REG_PC, REG_SP, REG_A, REG_X, REG_Y,
        P_N, P_V, P_P, P_B, P_H, P_I, P_Z, P_C,
        STATUS_IPLROM_ENABLE, STATUS_DSP_ADDR,
        STATUS_RAM00F8, STATUS_RAM00F9,
        T0_ENABLE, T0_TARGET, T0_STAGE1, T0_STAGE2, T0_STAGE3,
        T1_ENABLE, T1_TARGET, T1_STAGE1, T1_STAGE2, T1_STAGE3,
        T2_ENABLE, T2_TARGET, T2_STAGE1, T2_STAGE2, T2_STAGE3,
        RD, WR, DP, SP, YA, BIT,
        COUNT_   // = 41
    };
};
S9xSmpState s9x_decode_smp(const S9xState& s);

// CGDATA: 256 uint16 big-endian (= 512 bytes). OAM: 544 bytes uint8.
Bytes s9x_ppu_cgdata_be(const S9xState& s);
Bytes s9x_ppu_oam(const S9xState& s);

// Byte offsets within the PPU section (precomputed from snapshot.cpp).
extern const int PPU_CGDATA_OFF;   // 64
extern const int PPU_OBJ_LEN;      // 1408
extern const int PPU_OAMDATA_OFF;  // 2003

// ---- Mesen2 .mss save state ---------------------------------------------
struct MssEntry {
    std::string key;
    size_t      off;
    uint32_t    size;
};

struct MssFile {
    uint32_t emu_version = 0;
    uint32_t fmt_version = 0;
    uint32_t console_type = 0;
    uint32_t fb_size = 0, w = 0, h = 0, scale = 0, fb_comp_size = 0;
    Bytes    fb_compressed;
    std::string rom_name;

    Bytes                              state;     // decompressed state blob
    std::vector<MssEntry>              entries;
    std::map<std::string, MssEntry>    index;

    static MssFile load(const std::string& path);
    void           save(const std::string& path) const;

    // Synthesize an empty .mss with no state entries and Mesen2-correct
    // header constants (SNES console type, FileFormatVersion=4). The
    // deserializer tolerates missing keys, so callers can then add only the
    // (key,value) pairs they have data for via add_entry().
    static MssFile create_empty(const std::string& rom_name);

    bool   has(const std::string& key) const;
    Bytes  get(const std::string& key) const;
    // Replace value bytes for an existing key. Sizes must match.
    void   patch(const std::string& key, const uint8_t* data, size_t n);
    void   patch(const std::string& key, const Bytes& v) { patch(key, v.data(), v.size()); }
    // Append a new (key,value) entry. If the key already exists, behaves
    // like patch() (in-place replace). Used by from-scratch synthesis.
    void   add_entry(const std::string& key, const uint8_t* data, size_t n);
    void   add_entry(const std::string& key, const Bytes& v) { add_entry(key, v.data(), v.size()); }
    void   add_u8 (const std::string& key, uint32_t v);
    void   add_u16(const std::string& key, uint32_t v);
    void   add_u32(const std::string& key, uint32_t v);
    void   add_s16(const std::string& key, int32_t  v);
    // Patch only if key exists; pad with zeros or truncate to fit.
    // Used by template-based code paths (which we no longer need but keep
    // for API parity in case a template-based call site comes back later).
    void   try_patch(const std::string& key, const uint8_t* data, size_t n);
    void   try_patch(const std::string& key, const Bytes& v) { try_patch(key, v.data(), v.size()); }
    void   try_patch_u8 (const std::string& key, uint32_t v);
    void   try_patch_u16(const std::string& key, uint32_t v);
    void   try_patch_u32(const std::string& key, uint32_t v);
    void   try_patch_s16(const std::string& key, int32_t  v);
};

// ---- Conversion entry points --------------------------------------------
// Convert a SNES9x .009 to a Mesen2 .mss. No template required - the state blob is
// synthesized from scratch using a hardcoded schema of (key, default value) tuples.
// Throws ConvertError on failure.
void convert_s9x_to_mss(const std::string& in_path, const std::string& out_path);

// Video region override for mss -> s9x. Mesen2 doesn't store ROM region in
// its state blob, so the TIM section we synthesize defaults to NTSC. Auto
// applies a heuristic on V counter values; ForceNTSC / ForcePAL bypass it.
enum class RegionOverride { Auto = 0, ForceNTSC = 1, ForcePAL = 2 };

// Reverse: convert a Mesen2 .mss to a SNES9x .009. Also from scratch.
// `rom_path` is optional; if non-empty and `region == Auto`, the ROM header
// region byte ($FFD9 / $7FD9) is read and overrides the V-counter heuristic.
void convert_mss_to_s9x(const std::string& in_path, const std::string& out_path,
                        RegionOverride region = RegionOverride::Auto,
                        const std::string& rom_path = "");

// Detect ROM region from an SNES ROM file (.sfc / .smc / etc.). Tries LoROM
// then HiROM header. Returns 0 = NTSC, 1 = PAL, -1 = unknown / not a ROM.
int detect_rom_region_pal(const std::string& rom_path);

// Upgrade a legacy snes9x 1.5.x save state (#!snes9x:NNNN with separate
// APU/ARE/ARA/SOU/IAP/GBJ/SHO sections) to the modern v12 format
// (#!s9xsnp:0012 with a unified SND section) that current snes9x builds can
// load. Best-effort: SPC700 registers, DSP regs, timer state, and the
// CPU<->SPC ports are extracted; mid-instruction state and DSP voice
// internals are zeroed. Throws ConvertError if the input isn't a legacy
// state. Output is gzipped just like a real snes9x .009.
void upgrade_legacy_s9x_state(const std::string& in_path,
                              const std::string& out_path);

// Auto-name the output: insert _from_snes9x or _from_mesen2 before the extension.
std::string tag_output_path(const std::string& path, const std::string& tag);

// zlib (miniz) thin wrappers.
Bytes zlib_inflate(const uint8_t* p, size_t n, size_t expected_size);
Bytes zlib_deflate(const uint8_t* p, size_t n, int level);

// ---- header probe ----------------------------------------------------------
// Read just enough bytes to validate the magic + extract the version of a
// save state, without parsing the full file. Used by the GUI to give feedback
// on drag/drop. Returns true on success and fills *out_version. Returns false
// (with *out_reason populated) on invalid magic / IO error.
struct ProbeResult {
    bool        ok = false;
    int         version = 0;        // SNES9x: 6..12.  Mesen2: emuVersion field.
    int         fmt_version = 0;    // Mesen2 only - state-blob format version.
    std::string label;              // human-readable like "SNES9x v12" or
                                    // "Mesen2 (state fmt v4)"
    std::string error;              // populated when ok == false
};

ProbeResult probe_s9x(const std::string& path);
ProbeResult probe_mss(const std::string& path);
