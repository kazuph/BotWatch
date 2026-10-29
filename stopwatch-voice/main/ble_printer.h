#pragma once
#include <cstddef>
#include <cstdint>

namespace ble_printer {
using MemoryLog = void (*)(const char *stage, bool keep);
bool probe(MemoryLog memory);
bool query(MemoryLog memory);
struct PrintResult { bool success; size_t sent; const char *reason; int status; int battery_percent; };
PrintResult print(MemoryLog memory, const uint8_t *job, size_t bytes,
                  bool (*cancelled)() = nullptr, void (*started)() = nullptr);
}
