#pragma once

// ISO 9660 directory listing for a 2352-byte Mode 2 image, plus the TIM
// picture layout. The ISO parser reads 2048-byte user sectors. A TIM is
// accepted only when the buffer starts with id word 0x10 and its block sizes
// fit. Neither check is a guess about a particular disc file.

#include "psx_cd_loc.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace psx_cd_loc {

struct IsoEntry {
  std::string path;
  std::uint32_t lba = 0;
  std::uint32_t length = 0;
  bool directory = false;
};

struct IsoVolume {
  std::string volume_id;
  std::vector<IsoEntry> entries;
};

enum class IsoError {
  None,
  NotIso,
  Truncated,
  BadSector,
  TooLarge,
};

const char* iso_error_text(IsoError error);

// read_user returns the 2048 user bytes of one ISO logical sector.
using ReadUserSector = std::function<bool(std::uint32_t lba, std::vector<std::uint8_t>& user,
                                          std::string& error)>;

std::optional<IsoVolume> read_iso9660(const ReadUserSector& read_user, IsoError& error);

// First FILE that has a MODE2/2352 track. The returned name is the cue text,
// not a resolved path.
std::optional<std::string> cue_mode2_file(std::string_view cue_text, std::string& error);

// Records are 7 bytes: 24-bit little-endian logical sector, then 32-bit
// little-endian length. Halfword base 24 selects record (id + base - 2).
// A negative length whose magnitude is small is a group count, not a file.
// The length is the exact file size, not a sector-rounded read size.
struct IndexedFile {
  std::uint32_t id = 0;
  std::uint32_t lba = 0;
  std::int32_t length = 0;
  bool group = false;
};

std::vector<IndexedFile> parse_indexed_files(std::span<const std::uint8_t> records,
                                            std::uint16_t base_halfword);

struct TimRgb {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgb;
};

// Standard TIM. The pixel width field is in 16-bit units: 4bpp pixels are
// width*4, 8bpp width*2, and 16bpp width*1. Bytes per row are width*2.
// 4bpp uses the low nibble as the first pixel. Multiple CLUT rows are
// alternate palettes; row 0 is used. 24bpp is refused.
std::optional<TimRgb> decode_tim(std::span<const std::uint8_t> bytes);

// Binary PPM (P6). Empty when the picture is empty.
std::vector<std::uint8_t> encode_ppm(const TimRgb& picture);

}  // namespace psx_cd_loc
