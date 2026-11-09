mruby/c release4.0.0 (2026-06-12)

Source: https://github.com/mrubyc/mrubyc/releases/tag/release4.0.0
Commit: 434bccd609f2a3e90d6bb95c458621898bc6a508
The `_autogen*.h` files were generated from that release with `make autogen`.
The ESP32 HAL header is from the same release; integration changes will be kept separate.
`src/vm_config.h` overrides `MRBC_OUT_OF_MEMORY` so the VM task can discard its arena without rebooting the device.
`src/alloc.c` and `src/alloc.h` expose `mrbc_minimum_alloc_size()` derived from the allocator's pool header, sentinel, minimum free block and four-byte alignment, so callers can reject undersized arenas before initialization.
