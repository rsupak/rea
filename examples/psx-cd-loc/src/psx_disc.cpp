#include "psx_disc.hpp"

#include <cctype>
#include <string_view>

namespace psx_cd_loc {
namespace {

constexpr std::uint32_t kMaxDirectoryBytes = 1024u * 1024u;
constexpr std::size_t kMaxEntries = 4096;

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] |
                                    (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::string trim_id(std::string_view text) {
  while (!text.empty() && text.back() == ' ') {
    text.remove_suffix(1);
  }
  return std::string(text);
}

bool load_extent(const ReadUserSector& read_user, std::uint32_t lba, std::uint32_t length,
                 std::vector<std::uint8_t>& out, IsoError& error) {
  if (length > kMaxDirectoryBytes) {
    error = IsoError::TooLarge;
    return false;
  }
  const std::uint32_t sectors = (length + kUserBytesPerSector - 1) / kUserBytesPerSector;
  out.clear();
  out.reserve(static_cast<std::size_t>(sectors) * kUserBytesPerSector);
  for (std::uint32_t i = 0; i < sectors; ++i) {
    std::vector<std::uint8_t> user;
    std::string failure;
    if (!read_user(lba + i, user, failure) || user.size() != kUserBytesPerSector) {
      error = IsoError::BadSector;
      return false;
    }
    out.insert(out.end(), user.begin(), user.end());
  }
  if (out.size() < length) {
    error = IsoError::Truncated;
    return false;
  }
  out.resize(length);
  return true;
}

void walk_directory(const ReadUserSector& read_user, std::uint32_t lba, std::uint32_t length,
                    const std::string& prefix, int depth, IsoVolume& volume, IsoError& error) {
  if (depth > 8 || volume.entries.size() >= kMaxEntries) {
    return;
  }
  std::vector<std::uint8_t> data;
  if (!load_extent(read_user, lba, length, data, error)) {
    return;
  }
  std::size_t offset = 0;
  while (offset + 1 < data.size() && volume.entries.size() < kMaxEntries) {
    const std::uint8_t record_length = data[offset];
    if (record_length == 0) {
      offset = (offset + kUserBytesPerSector) & ~(static_cast<std::size_t>(kUserBytesPerSector) - 1);
      continue;
    }
    if (record_length < 34 || offset + record_length > data.size()) {
      error = IsoError::NotIso;
      return;
    }
    const auto record = std::span<const std::uint8_t>(data).subspan(offset, record_length);
    const std::uint32_t extent = read_u32(record, 2);
    const std::uint32_t data_length = read_u32(record, 10);
    const bool directory = (record[25] & 0x02) != 0;
    const std::uint8_t name_length = record[32];
    if (33u + name_length > record.size()) {
      error = IsoError::NotIso;
      return;
    }
    const bool skip_dot = name_length == 1 && (record[33] == 0 || record[33] == 1);
    if (!skip_dot) {
      std::string name(reinterpret_cast<const char*>(record.data() + 33), name_length);
      const auto semi = name.find(';');
      if (semi != std::string::npos) {
        name.resize(semi);
      }
      IsoEntry entry;
      entry.path = prefix.empty() ? name : prefix + "/" + name;
      entry.lba = extent;
      entry.length = data_length;
      entry.directory = directory;
      volume.entries.push_back(entry);
      if (directory) {
        walk_directory(read_user, extent, data_length, entry.path, depth + 1, volume, error);
        if (error != IsoError::None) {
          return;
        }
      }
    }
    offset += record_length;
  }
}

}  // namespace

const char* iso_error_text(IsoError error) {
  switch (error) {
    case IsoError::None:
      return "ok";
    case IsoError::NotIso:
      return "sector 16 is not an ISO 9660 primary volume";
    case IsoError::Truncated:
      return "directory extent is short";
    case IsoError::BadSector:
      return "a directory sector could not be read";
    case IsoError::TooLarge:
      return "directory extent is above 1 MiB";
  }
  return "unknown iso error";
}

std::optional<IsoVolume> read_iso9660(const ReadUserSector& read_user, IsoError& error) {
  std::vector<std::uint8_t> pvd;
  std::string failure;
  if (!read_user(16, pvd, failure) || pvd.size() != kUserBytesPerSector) {
    error = IsoError::BadSector;
    return std::nullopt;
  }
  if (pvd[0] != 1 || std::string_view(reinterpret_cast<const char*>(pvd.data() + 1), 5) != "CD001") {
    error = IsoError::NotIso;
    return std::nullopt;
  }
  IsoVolume volume;
  volume.volume_id = trim_id(std::string_view(reinterpret_cast<const char*>(pvd.data() + 40), 32));
  if (pvd.size() < 156 + 34) {
    error = IsoError::NotIso;
    return std::nullopt;
  }
  const auto root = std::span<const std::uint8_t>(pvd).subspan(156, 34);
  error = IsoError::None;
  walk_directory(read_user, read_u32(root, 2), read_u32(root, 10), "", 0, volume, error);
  if (error != IsoError::None) {
    return std::nullopt;
  }
  return volume;
}

std::optional<std::string> cue_mode2_file(std::string_view cue_text, std::string& error) {
  std::string current_file;
  std::size_t offset = 0;
  while (offset < cue_text.size()) {
    std::size_t end = cue_text.find('\n', offset);
    if (end == std::string_view::npos) {
      end = cue_text.size();
    }
    std::string_view line = cue_text.substr(offset, end - offset);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    offset = end + (end < cue_text.size() ? 1 : 0);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
      line.remove_prefix(1);
    }
    if (line.size() >= 5 && (line.substr(0, 4) == "FILE" || line.substr(0, 4) == "file")) {
      const auto quote = line.find('"');
      const auto end_quote = quote == std::string_view::npos ? std::string_view::npos : line.find('"', quote + 1);
      if (quote == std::string_view::npos || end_quote == std::string_view::npos) {
        error = "CUE FILE line has no quoted name";
        return std::nullopt;
      }
      current_file = std::string(line.substr(quote + 1, end_quote - quote - 1));
    } else if (line.find("MODE2/2352") != std::string_view::npos) {
      if (current_file.empty()) {
        error = "MODE2/2352 track has no FILE";
        return std::nullopt;
      }
      error.clear();
      return current_file;
    }
  }
  error = "CUE has no MODE2/2352 track";
  return std::nullopt;
}

std::optional<TimRgb> decode_tim(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 8 || read_u32(bytes, 0) != 0x10u) {
    return std::nullopt;
  }
  const std::uint32_t flags = read_u32(bytes, 4);
  const std::uint32_t mode = flags & 7u;
  const bool has_clut = (flags & 8u) != 0;
  std::size_t cursor = 8;
  std::vector<std::uint16_t> clut;
  int clut_width = 0;
  if (has_clut) {
    if (cursor + 12 > bytes.size()) {
      return std::nullopt;
    }
    const std::uint32_t block = read_u32(bytes, cursor);
    clut_width = read_u16(bytes, cursor + 8);
    const int clut_height = read_u16(bytes, cursor + 10);
    if (block < 12 || cursor + block > bytes.size() || clut_width <= 0 || clut_height <= 0) {
      return std::nullopt;
    }
    const std::size_t colors = static_cast<std::size_t>(clut_width) * static_cast<std::size_t>(clut_height);
    if (12 + colors * 2 > block) {
      return std::nullopt;
    }
    clut.resize(colors);
    for (std::size_t i = 0; i < colors; ++i) {
      clut[i] = read_u16(bytes, cursor + 12 + i * 2);
    }
    cursor += block;
  }
  if (cursor + 12 > bytes.size()) {
    return std::nullopt;
  }
  const std::uint32_t pixel_block = read_u32(bytes, cursor);
  const int width = read_u16(bytes, cursor + 8);
  const int height = read_u16(bytes, cursor + 10);
  if (pixel_block < 12 || cursor + pixel_block > bytes.size() || width <= 0 || height <= 0) {
    return std::nullopt;
  }
  const auto pixels = bytes.subspan(cursor + 12, pixel_block - 12);
  TimRgb picture;
  picture.width = width;
  picture.height = height;
  picture.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);
  auto expand = [](std::uint16_t color, std::uint8_t* rgb) {
    rgb[0] = static_cast<std::uint8_t>((color & 0x1Fu) << 3);
    rgb[1] = static_cast<std::uint8_t>(((color >> 5) & 0x1Fu) << 3);
    rgb[2] = static_cast<std::uint8_t>(((color >> 10) & 0x1Fu) << 3);
  };
  if (mode == 2 && !has_clut) {
    if (pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2) {
      return std::nullopt;
    }
    for (int i = 0; i < width * height; ++i) {
      expand(read_u16(pixels, static_cast<std::size_t>(i) * 2), picture.rgb.data() + static_cast<std::size_t>(i) * 3);
    }
    return picture;
  }
  if (mode == 1 && has_clut && clut_width >= 256) {
    if (pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
      return std::nullopt;
    }
    for (int i = 0; i < width * height; ++i) {
      expand(clut[pixels[static_cast<std::size_t>(i)]], picture.rgb.data() + static_cast<std::size_t>(i) * 3);
    }
    return picture;
  }
  return std::nullopt;
}

std::vector<std::uint8_t> encode_ppm(const TimRgb& picture) {
  if (picture.width <= 0 || picture.height <= 0 ||
      picture.rgb.size() != static_cast<std::size_t>(picture.width) * static_cast<std::size_t>(picture.height) * 3) {
    return {};
  }
  const std::string header = "P6\n" + std::to_string(picture.width) + " " + std::to_string(picture.height) + "\n255\n";
  std::vector<std::uint8_t> out(header.begin(), header.end());
  out.insert(out.end(), picture.rgb.begin(), picture.rgb.end());
  return out;
}

}  // namespace psx_cd_loc
