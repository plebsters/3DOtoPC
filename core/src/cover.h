// cover.h - cover art for converted games (see cover.cpp).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tdo {

struct Image { int w = 0, h = 0; std::vector<uint8_t> rgba; };

// A disc's BannerScreen file decoded (false: not one, the developer-kit
// sample picture, or a single flat colour).
bool decode_banner(const std::vector<uint8_t>& file, Image& img);

// An image the user put beside the disc: <disc name>.png/.jpg/.jpeg, or
// cover / folder / boxart .png/.jpg in its folder. Returns its file name, or
// "" when there is none.
std::string find_user_cover(const std::filesystem::path& disc_path, std::vector<uint8_t>& bytes, Image& img);

// A PNG, JPEG or BMP file decoded.
bool load_image(const std::filesystem::path& file, Image& img);

bool save_cover(const Image& img, const std::filesystem::path& png_path);

// The picture as an .ico file (256, 48, 32 and 16 px, letterboxed to a square).
std::vector<uint8_t> make_ico(const Image& img);

// Writes the same icon into a built .exe (Windows only).
bool set_exe_icon(const std::filesystem::path& exe, const Image& img, std::string& err);

}  // namespace tdo
