#pragma once
#include <cstdint>
#include <vector>
namespace vision {
enum class Mode { off, faces, classify };
struct Box { int x1, y1, x2, y2; float score; };
struct Prediction { int category; float probability; };
void set_mode(Mode mode);
void stop();
bool faces(const uint8_t* pixels, uint16_t width, uint16_t height, std::vector<Box>& out);
bool classify(const uint8_t* pixels, uint16_t width, uint16_t height, std::vector<Prediction>& out);
}
