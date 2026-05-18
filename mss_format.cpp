// mss_format.cpp - Mesen2 .mss parsing/writing + zlib (miniz) helpers.
#include "s9x_mss.h"
#include "miniz.h"
#include <cstring>
#include <fstream>

Bytes zlib_inflate(const uint8_t* p, size_t n, size_t expected_size) {
    Bytes out(expected_size);
    mz_ulong sz = expected_size;
    int rc = mz_uncompress(out.data(), &sz, p, n);
    if (rc != MZ_OK) throw ConvertError("zlib inflate failed");
    out.resize(sz);
    return out;
}

Bytes zlib_deflate(const uint8_t* p, size_t n, int level) {
    mz_ulong bound = mz_compressBound(n);
    Bytes out(bound);
    int rc = mz_compress2(out.data(), &bound, p, n, level);
    if (rc != MZ_OK) throw ConvertError("zlib deflate failed");
    out.resize(bound);
    return out;
}

static void parse_entries(MssFile& m) {
    m.entries.clear();
    m.index.clear();
    size_t i = 0;
    while (i < m.state.size()) {
        size_t end = i;
        while (end < m.state.size() && m.state[end] != 0) end++;
        if (end >= m.state.size()) break;
        std::string key(reinterpret_cast<const char*>(&m.state[i]), end - i);
        i = end + 1;
        if (i + 4 > m.state.size()) break;
        uint32_t sz = rd_u32_le(&m.state[i]);
        i += 4;
        if (i + sz > m.state.size()) break;
        MssEntry e{ key, i, sz };
        m.entries.push_back(e);
        m.index[key] = e;
        i += sz;
    }
}

MssFile MssFile::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw ConvertError("cannot open: " + path);
    Bytes data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    if (data.size() < 35 || std::memcmp(data.data(), "MSS", 3) != 0)
        throw ConvertError("not a Mesen2 .mss (missing magic)");

    MssFile m;
    m.emu_version  = rd_u32_le(&data[3]);
    m.fmt_version  = rd_u32_le(&data[7]);
    m.console_type = rd_u32_le(&data[11]);
    m.fb_size      = rd_u32_le(&data[15]);
    m.w            = rd_u32_le(&data[19]);
    m.h            = rd_u32_le(&data[23]);
    m.scale        = rd_u32_le(&data[27]);
    m.fb_comp_size = rd_u32_le(&data[31]);

    size_t off = 35;
    if (off + m.fb_comp_size > data.size()) throw ConvertError("fb past EOF");
    m.fb_compressed.assign(data.begin() + off, data.begin() + off + m.fb_comp_size);
    off += m.fb_comp_size;

    if (off + 4 > data.size()) throw ConvertError("rom name length past EOF");
    uint32_t rom_name_len = rd_u32_le(&data[off]); off += 4;
    if (off + rom_name_len > data.size()) throw ConvertError("rom name past EOF");
    m.rom_name.assign(reinterpret_cast<const char*>(&data[off]), rom_name_len);
    off += rom_name_len;

    if (off + 9 > data.size()) throw ConvertError("state envelope past EOF");
    uint8_t is_compressed = data[off]; off += 1;
    if (is_compressed != 1) throw ConvertError("uncompressed state envelope not supported");
    uint32_t decomp_size = rd_u32_le(&data[off]); off += 4;
    uint32_t comp_size   = rd_u32_le(&data[off]); off += 4;
    if (off + comp_size > data.size()) throw ConvertError("state body past EOF");

    m.state = zlib_inflate(&data[off], comp_size, decomp_size);
    if (m.state.size() != decomp_size) throw ConvertError("state size mismatch");

    parse_entries(m);
    return m;
}

void MssFile::save(const std::string& path) const {
    Bytes comp = zlib_deflate(state.data(), state.size(), 1);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw ConvertError("cannot write: " + path);
    f.write("MSS", 3);
    auto write_u32 = [&](uint32_t v) {
        uint8_t b[4]; wr_u32_le(b, v); f.write(reinterpret_cast<const char*>(b), 4);
    };
    write_u32(emu_version);
    write_u32(fmt_version);
    write_u32(console_type);
    write_u32(fb_size);
    write_u32(w);
    write_u32(h);
    write_u32(scale);
    write_u32(fb_comp_size);
    f.write(reinterpret_cast<const char*>(fb_compressed.data()), fb_compressed.size());
    write_u32(uint32_t(rom_name.size()));
    f.write(rom_name.data(), rom_name.size());
    f.put(1);                          // isCompressed
    write_u32(uint32_t(state.size())); // decomp size
    write_u32(uint32_t(comp.size()));  // comp size
    f.write(reinterpret_cast<const char*>(comp.data()), comp.size());
}

bool MssFile::has(const std::string& key) const {
    return index.find(key) != index.end();
}

Bytes MssFile::get(const std::string& key) const {
    auto it = index.find(key);
    if (it == index.end()) throw ConvertError("missing key: " + key);
    const auto& e = it->second;
    return Bytes(state.begin() + e.off, state.begin() + e.off + e.size);
}

void MssFile::patch(const std::string& key, const uint8_t* data, size_t n) {
    auto it = index.find(key);
    if (it == index.end()) throw ConvertError("key not present in template: " + key);
    const auto& e = it->second;
    if (n != e.size)
        throw ConvertError("size mismatch for " + key + " (template " +
                           std::to_string(e.size) + ", got " + std::to_string(n) + ")");
    std::memcpy(&state[e.off], data, n);
}

void MssFile::try_patch(const std::string& key, const uint8_t* data, size_t n) {
    auto it = index.find(key);
    if (it == index.end()) return;
    const auto& e = it->second;
    if (n == e.size) {
        std::memcpy(&state[e.off], data, n);
        return;
    }
    if (n < e.size) {
        std::memcpy(&state[e.off], data, n);
        std::memset(&state[e.off + n], 0, e.size - n);
    } else {
        std::memcpy(&state[e.off], data, e.size);
    }
}

void MssFile::try_patch_u8 (const std::string& key, uint32_t v) {
    uint8_t b = uint8_t(v & 0xFF);
    try_patch(key, &b, 1);
}
void MssFile::try_patch_u16(const std::string& key, uint32_t v) {
    uint8_t b[2]; wr_u16_le(b, uint16_t(v & 0xFFFF)); try_patch(key, b, 2);
}
void MssFile::try_patch_u32(const std::string& key, uint32_t v) {
    uint8_t b[4]; wr_u32_le(b, v); try_patch(key, b, 4);
}
void MssFile::try_patch_s16(const std::string& key, int32_t v) {
    uint8_t b[2]; wr_u16_le(b, uint16_t(int16_t(v))); try_patch(key, b, 2);
}

// ---- from-scratch synthesis -----------------------------------------------
MssFile MssFile::create_empty(const std::string& rom_name) {
    MssFile m;
    // Header constants from Mesen2 source (Core/Shared/SaveStateManager.h):
    //   FileFormatVersion = 4 (current), MinimumSupportedVersion = 3
    //   SettingTypes.h: ConsoleType::Snes = 0
    // emu_version is the Mesen2 binary version; if it exceeds the runtime
    // Mesen2's version the load is refused. 0 is always <= any version so
    // this is safe for any Mesen2 build.
    m.emu_version  = 0;
    m.fmt_version  = 4;
    m.console_type = 0;  // Snes

    // Preview frame buffer. Mesen2's SaveStateManager::GetVideoData() calls
    // miniz uncompress() on this region during load; if either the
    // decompressed size or compressed size is zero, uncompress() returns
    // an error and Mesen2 prints "invalid save state". So we emit a minimal
    // valid 1x1 RGBA framebuffer compressed with zlib - it produces a tiny
    // blank thumbnail in the save state slot.
    const uint32_t W = 1, H = 1;
    m.fb_size = W * H * 4;          // 4 bytes (RGBA, single pixel)
    m.w       = W;
    m.h       = H;
    m.scale   = 100;                // GetVideoData stores scale * 100; 100 = 1.0x
    Bytes fb(m.fb_size, 0);
    m.fb_compressed = zlib_deflate(fb.data(), fb.size(), 1);
    m.fb_comp_size  = uint32_t(m.fb_compressed.size());

    m.rom_name = rom_name;
    return m;
}

void MssFile::add_entry(const std::string& key, const uint8_t* data, size_t n) {
    auto it = index.find(key);
    if (it != index.end()) {
        // Already present - act like patch() to keep sizes consistent.
        const auto& e = it->second;
        if (n != e.size)
            throw ConvertError("add_entry size mismatch for " + key);
        std::memcpy(&state[e.off], data, n);
        return;
    }
    size_t key_off = state.size();
    state.insert(state.end(), key.begin(), key.end());
    state.push_back(0);
    uint8_t sz_le[4]; wr_u32_le(sz_le, uint32_t(n));
    state.insert(state.end(), sz_le, sz_le + 4);
    size_t value_off = state.size();
    state.insert(state.end(), data, data + n);
    MssEntry e{ key, value_off, uint32_t(n) };
    entries.push_back(e);
    index[key] = e;
    (void)key_off;
}

void MssFile::add_u8 (const std::string& key, uint32_t v) {
    uint8_t b = uint8_t(v); add_entry(key, &b, 1);
}
void MssFile::add_u16(const std::string& key, uint32_t v) {
    uint8_t b[2]; wr_u16_le(b, uint16_t(v)); add_entry(key, b, 2);
}
void MssFile::add_u32(const std::string& key, uint32_t v) {
    uint8_t b[4]; wr_u32_le(b, v); add_entry(key, b, 4);
}
void MssFile::add_s16(const std::string& key, int32_t v) {
    uint8_t b[2]; wr_u16_le(b, uint16_t(int16_t(v))); add_entry(key, b, 2);
}

// ---- header probe ---------------------------------------------------------
ProbeResult probe_mss(const std::string& path) {
    ProbeResult r;
    std::ifstream f(path, std::ios::binary);
    if (!f) { r.error = "cannot open file"; return r; }
    char head[15]{};
    f.read(head, sizeof(head));
    if (f.gcount() < 15) { r.error = "file too small to be a Mesen2 .mss"; return r; }
    if (std::memcmp(head, "MSS", 3) != 0) {
        r.error = "missing Mesen2 magic (MSS)";
        return r;
    }
    uint32_t emu_v = rd_u32_le(reinterpret_cast<const uint8_t*>(&head[3]));
    uint32_t fmt_v = rd_u32_le(reinterpret_cast<const uint8_t*>(&head[7]));
    uint32_t ctype = rd_u32_le(reinterpret_cast<const uint8_t*>(&head[11]));
    r.ok = true;
    r.version = int(emu_v);
    r.fmt_version = int(fmt_v);
    r.label = "Mesen2 .mss (state fmt v" + std::to_string(fmt_v) + ")";
    if (ctype != 0)
        r.label += " - non-SNES console (type=" + std::to_string(ctype) + ")";
    if (fmt_v < 3)
        r.label += " - older than Mesen2's MinimumSupportedVersion";
    return r;
}
