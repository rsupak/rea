#include "psx_cd_loc.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

void usage() {
  std::cerr
      << "psx_cd_read --image PATH (--lba N --length BYTES | --record HEX14)\n"
      << "            [--base-lba N] [--output PATH]\n"
      << "\n"
      << "Read Mode 2 user bytes from a 2352-byte sector image.\n"
      << "LBA is the ISO logical sector. The file offset is\n"
      << "(lba - base-lba) * 2352. base-lba defaults to 0, which is a disc\n"
      << "image whose first sector is logical sector 0. Absolute\n"
      << "minute/second/frame is lba + 150 and is checked against the sector\n"
      << "header; it is not added to the file offset.\n"
      << "The byte count is exact. A final partial sector contributes only\n"
      << "its leading user bytes, from raw offset 24.\n";
}

std::optional<std::uint32_t> parse_u32(const std::string& text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    value = value * 10 + static_cast<std::uint32_t>(ch - '0');
    if (value > 0xFFFFFFFFULL) {
      return std::nullopt;
    }
  }
  return static_cast<std::uint32_t>(value);
}

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

std::optional<psx_cd_loc::CdLoad> load_from_record(const std::string& hex) {
  if (hex.size() != 14) {
    return std::nullopt;
  }
  std::uint8_t bytes[7];
  for (int i = 0; i < 7; ++i) {
    const int high = hex_value(hex[static_cast<std::size_t>(i * 2)]);
    const int low = hex_value(hex[static_cast<std::size_t>(i * 2 + 1)]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
  }
  const auto record = psx_cd_loc::decode_index_record(bytes);
  if (!record.has_value()) {
    return std::nullopt;
  }
  return psx_cd_loc::cd_load_from_record(*record);
}

}  // namespace

int main(int argc, char** argv) {
  std::string image_path;
  std::string output_path;
  std::string record_hex;
  std::optional<std::uint32_t> lba;
  std::optional<std::uint32_t> length;
  std::uint32_t base_lba = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << '\n';
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--image") {
      const char* value = need("--image");
      if (value == nullptr) {
        return 2;
      }
      image_path = value;
    } else if (arg == "--output") {
      const char* value = need("--output");
      if (value == nullptr) {
        return 2;
      }
      output_path = value;
    } else if (arg == "--lba") {
      const char* value = need("--lba");
      if (value == nullptr) {
        return 2;
      }
      lba = parse_u32(value);
      if (!lba.has_value()) {
        std::cerr << "lba is not an unsigned 32-bit integer\n";
        return 2;
      }
    } else if (arg == "--length") {
      const char* value = need("--length");
      if (value == nullptr) {
        return 2;
      }
      length = parse_u32(value);
      if (!length.has_value()) {
        std::cerr << "length is not an unsigned 32-bit integer\n";
        return 2;
      }
    } else if (arg == "--base-lba") {
      const char* value = need("--base-lba");
      if (value == nullptr) {
        return 2;
      }
      const auto parsed = parse_u32(value);
      if (!parsed.has_value()) {
        std::cerr << "base-lba is not an unsigned 32-bit integer\n";
        return 2;
      }
      base_lba = *parsed;
    } else if (arg == "--record") {
      const char* value = need("--record");
      if (value == nullptr) {
        return 2;
      }
      record_hex = value;
    } else if (arg == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "unknown argument " << arg << '\n';
      usage();
      return 2;
    }
  }

  if (image_path.empty()) {
    usage();
    return 2;
  }
  const bool have_record = !record_hex.empty();
  const bool have_explicit = lba.has_value() || length.has_value();
  if (have_record == have_explicit || (have_explicit && (!lba || !length))) {
    std::cerr << "pass either --lba and --length, or --record\n";
    return 2;
  }
  if (have_record) {
    const auto load = load_from_record(record_hex);
    if (!load.has_value()) {
      std::cerr << "record must be 14 hex digits and a usable index record\n";
      return 2;
    }
    lba = load->logical_sector;
    length = load->byte_length;
  }

  psx_cd_loc::ImageError error = psx_cd_loc::ImageError::None;
  const auto range =
      psx_cd_loc::mode2_user_range(*lba, *length, base_lba, error);
  if (!range.has_value()) {
    std::cerr << psx_cd_loc::image_error_text(error) << '\n';
    return 1;
  }

  std::ifstream input(image_path, std::ios::binary);
  if (!input) {
    std::cerr << "cannot open image\n";
    return 1;
  }
  input.seekg(0, std::ios::end);
  const std::streamoff file_end = input.tellg();
  if (file_end < 0) {
    std::cerr << "cannot read image size\n";
    return 1;
  }
  const auto file_size = static_cast<std::uint64_t>(file_end);
  if (range->file_offset > file_size ||
      range->raw_bytes > file_size - range->file_offset) {
    std::cerr << psx_cd_loc::image_error_text(psx_cd_loc::ImageError::Truncated)
              << '\n';
    return 1;
  }
  input.seekg(static_cast<std::streamoff>(range->file_offset), std::ios::beg);
  std::vector<std::uint8_t> slice(static_cast<std::size_t>(range->raw_bytes));
  input.read(reinterpret_cast<char*>(slice.data()),
             static_cast<std::streamsize>(slice.size()));
  if (input.gcount() != static_cast<std::streamsize>(slice.size())) {
    std::cerr << psx_cd_loc::image_error_text(psx_cd_loc::ImageError::Truncated)
              << '\n';
    return 1;
  }

  const auto read = psx_cd_loc::read_mode2_user(slice, *lba, *length, *lba, error);
  if (!read.has_value()) {
    std::cerr << psx_cd_loc::image_error_text(error) << '\n';
    return 1;
  }
  if (!output_path.empty()) {
    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
      std::cerr << "cannot open output\n";
      return 1;
    }
    output.write(reinterpret_cast<const char*>(read->user.data()),
                 static_cast<std::streamsize>(read->user.size()));
    if (!output) {
      std::cerr << "cannot write output\n";
      return 1;
    }
  }
  std::cout << "bytes " << read->user.size() << '\n'
            << "sectors " << read->sectors.size() << '\n'
            << "sha256 " << psx_cd_loc::sha256_hex(read->user) << '\n';
  return 0;
}
