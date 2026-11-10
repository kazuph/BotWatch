#include "ruby_bytecode.h"
#include <cstring>

namespace {
constexpr size_t kHeaderSize = 20;
constexpr unsigned kMaxIrepDepth = 64;

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t pos;
    bool take(size_t count) {
        if (count > size - pos) return false;
        pos += count;
        return true;
    }
    bool u8(uint8_t& value) {
        if (!take(1)) return false;
        value = data[pos - 1]; return true;
    }
    bool u16(uint16_t& value) {
        if (!take(2)) return false;
        value = uint16_t(data[pos - 2]) << 8 | data[pos - 1]; return true;
    }
    bool u32(uint32_t& value) {
        if (!take(4)) return false;
        value = uint32_t(data[pos - 4]) << 24 | uint32_t(data[pos - 3]) << 16 |
                uint32_t(data[pos - 2]) << 8 | data[pos - 1]; return true;
    }
};

bool irep(Reader& section, unsigned depth) {
    if (depth >= kMaxIrepDepth) return false;
    const size_t start = section.pos;
    uint32_t recordSize;
    if (!section.u32(recordSize) || recordSize < 16 || recordSize > section.size - start) return false;
    Reader record{section.data, start + recordSize, section.pos};
    uint16_t nlocals, nregs, children, catches, pools, symbols;
    uint32_t instructions;
    if (!record.u16(nlocals) || !record.u16(nregs) || !record.u16(children) ||
        !record.u16(catches) || !record.u32(instructions) || instructions > 0xffff ||
        !record.take(instructions) || !record.take(size_t(catches) * 13) || !record.u16(pools)) return false;
    (void)nlocals; (void)nregs;
    for (uint16_t i = 0; i < pools; ++i) {
        uint8_t type; uint16_t length;
        if (!record.u8(type)) return false;
        switch (type) {
        case 0: case 2:
            if (!record.u16(length) || !record.take(size_t(length) + 1) || record.data[record.pos-1] != 0) return false;
            break;
        case 1: if (!record.take(4)) return false; break;
        case 5: if (!record.take(8)) return false; break;
        default: return false; // INT64 and BIGINT are disabled in this VM.
        }
    }
    if (!record.u16(symbols)) return false;
    for (uint16_t i = 0; i < symbols; ++i) {
        uint16_t length;
        if (!record.u16(length) || !record.take(size_t(length) + 1) || record.data[record.pos-1] != 0 ||
            memchr(record.data + record.pos - length - 1, 0, length)) return false;
    }
    if (record.pos != record.size) return false;
    section.pos = record.size;
    for (uint16_t i = 0; i < children; ++i) if (!irep(section, depth + 1)) return false;
    return true;
}
}

bool rubyValidBytecode(const uint8_t* bytes, size_t size) {
    if (!bytes || size < kHeaderSize || memcmp(bytes, "RITE0400", 8)) return false;
    Reader file{bytes, size, 8};
    uint32_t declared;
    if (!file.u32(declared) || declared != size || !file.take(8)) return false;
    bool haveIrep = false, haveEnd = false;
    while (file.pos < size) {
        const size_t start = file.pos;
        if (!file.take(4)) return false;
        uint32_t length;
        if (!file.u32(length) || length < 8 || length > size - start) return false;
        Reader section{bytes, start + length, file.pos};
        if (!memcmp(bytes + start, "IREP", 4)) {
            if (haveIrep || !section.take(4) || !irep(section, 0) || section.pos != section.size) return false;
            haveIrep = true;
        } else if (!memcmp(bytes + start, "END\0", 4)) {
            if (!haveIrep || length != 8 || start + length != size) return false;
            haveEnd = true;
        }
        file.pos = start + length;
        if (haveEnd) break;
    }
    return haveIrep && haveEnd && file.pos == size;
}
