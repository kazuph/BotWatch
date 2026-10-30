extern const uint8_t ui15_start[] asm("_binary_ui15_vlw_start");
extern const uint8_t ui17_start[] asm("_binary_ui17_vlw_start");
extern const uint8_t ui20_start[] asm("_binary_ui20_vlw_start");
extern const uint8_t ui22_start[] asm("_binary_ui22_vlw_start");
extern const uint8_t ui24_start[] asm("_binary_ui24_vlw_start");
extern const uint8_t ui32_start[] asm("_binary_ui32_vlw_start");
extern const uint8_t ui34_start[] asm("_binary_ui34_vlw_start");
extern const uint8_t ui38_start[] asm("_binary_ui38_vlw_start");
extern const uint8_t ui40_start[] asm("_binary_ui40_vlw_start");
extern const uint8_t ui56_start[] asm("_binary_ui56_vlw_start");
struct UiFont { int size, ascent; const uint8_t *data; };
constexpr UiFont kUiFonts[] = {
    {15, 13, ui15_start},
    {17, 15, ui17_start},
    {20, 17, ui20_start},
    {22, 19, ui22_start},
    {24, 21, ui24_start},
    {32, 28, ui32_start},
    {34, 29, ui34_start},
    {38, 31, ui38_start},
    {40, 33, ui40_start},
    {56, 42, ui56_start},
};
