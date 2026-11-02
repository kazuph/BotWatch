#include <cstdio>
#include "ir_codes.h"
#include "ir_monitor_decode.h"

int main()
{
    auto frame = ir_codes::nec(ir_codes::tv_code(ir_codes::TvKey::volume_up));
    uint32_t code = 0;
    bool ok = decode_monitor_nec(frame.data(), frame.size(), code) && code == 0x20DF40BF;
    ok &= !decode_monitor_nec(frame.data(), 4, code);
    frame[3] = 1000;
    ok &= !decode_monitor_nec(frame.data(), frame.size(), code);
    std::printf("%s monitor NEC, short, invalid\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
