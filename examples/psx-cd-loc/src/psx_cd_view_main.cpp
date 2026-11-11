#include "psx_cd_loc.hpp"
#include "psx_disc.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {

constexpr std::uint32_t kResourceLba = 108754;
constexpr std::uint32_t kResourceLength = 61680;

struct InferredStream {
  const char* name;
  std::uint32_t lba;
  std::uint32_t length;
  std::uint32_t expected_output;
};

// Loader-table mapping from the traced structure pointer. Not a captured Setloc.
constexpr InferredStream kInferredLz[] = {
    {"slot18", 109158, 14360, 29779},
    {"slot14", 108933, 125304, 260862},
};

void usage() {
  std::cerr
      << "psx_cd_view (--image BIN | --cue CUE) [--export-dir DIR] [--export-all]\n"
      << "\n"
      << "Read-only. Lists the ISO 9660 files in a MODE2/2352 image, prints\n"
      << "the verified resource at LBA 108754, and unpacks two loader-table\n"
      << "streams. A PPM is written only when unpacked bytes are a TIM.\n";
}

std::string hex_byte(std::uint8_t value) {
  static constexpr char kHex[] = "0123456789abcdef";
  return std::string{kHex[value >> 4], kHex[value & 0xF]};
}

void print_hex_ascii(const std::vector<std::uint8_t>& bytes, std::size_t count) {
  const std::size_t n = std::min(count, bytes.size());
  std::cout << "hex";
  for (std::size_t i = 0; i < n; ++i) {
    std::cout << ' ' << hex_byte(bytes[i]);
  }
  std::cout << "\nascii ";
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char ch = bytes[i];
    std::cout << (ch >= 32 && ch < 127 ? static_cast<char>(ch) : '.');
  }
  std::cout << '\n';
}

bool read_user_sector(std::ifstream& input, std::uint64_t file_size, std::uint32_t lba,
                      std::vector<std::uint8_t>& user, std::string& error) {
  psx_cd_loc::ImageError image_error = psx_cd_loc::ImageError::None;
  const auto range = psx_cd_loc::mode2_user_range(lba, psx_cd_loc::kUserBytesPerSector, 0, image_error);
  if (!range.has_value()) {
    error = psx_cd_loc::image_error_text(image_error);
    return false;
  }
  if (range->file_offset > file_size || range->raw_bytes > file_size - range->file_offset) {
    error = psx_cd_loc::image_error_text(psx_cd_loc::ImageError::Truncated);
    return false;
  }
  input.clear();
  input.seekg(static_cast<std::streamoff>(range->file_offset), std::ios::beg);
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(range->raw_bytes));
  input.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
  if (input.gcount() != static_cast<std::streamsize>(raw.size())) {
    error = psx_cd_loc::image_error_text(psx_cd_loc::ImageError::Truncated);
    return false;
  }
  const auto decoded = psx_cd_loc::read_mode2_user(raw, lba, psx_cd_loc::kUserBytesPerSector, lba, image_error);
  if (!decoded.has_value()) {
    error = psx_cd_loc::image_error_text(image_error);
    return false;
  }
  user = decoded->user;
  return true;
}

bool read_user_span(std::ifstream& input, std::uint64_t file_size, std::uint32_t lba,
                    std::uint32_t length, std::vector<std::uint8_t>& out, std::string& error) {
  psx_cd_loc::ImageError image_error = psx_cd_loc::ImageError::None;
  const auto range = psx_cd_loc::mode2_user_range(lba, length, 0, image_error);
  if (!range.has_value()) {
    error = psx_cd_loc::image_error_text(image_error);
    return false;
  }
  if (range->file_offset > file_size || range->raw_bytes > file_size - range->file_offset) {
    error = psx_cd_loc::image_error_text(psx_cd_loc::ImageError::Truncated);
    return false;
  }
  input.clear();
  input.seekg(static_cast<std::streamoff>(range->file_offset), std::ios::beg);
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(range->raw_bytes));
  input.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
  if (input.gcount() != static_cast<std::streamsize>(raw.size())) {
    error = psx_cd_loc::image_error_text(psx_cd_loc::ImageError::Truncated);
    return false;
  }
  const auto decoded = psx_cd_loc::read_mode2_user(raw, lba, length, lba, image_error);
  if (!decoded.has_value()) {
    error = psx_cd_loc::image_error_text(image_error);
    return false;
  }
  out = decoded->user;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string image_path;
  std::string cue_path;
  std::string export_dir;
  bool export_all = false;
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
    } else if (arg == "--cue") {
      const char* value = need("--cue");
      if (value == nullptr) {
        return 2;
      }
      cue_path = value;
    } else if (arg == "--export-dir") {
      const char* value = need("--export-dir");
      if (value == nullptr) {
        return 2;
      }
      export_dir = value;
    } else if (arg == "--export-all") {
      export_all = true;
    } else if (arg == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "unknown argument " << arg << '\n';
      usage();
      return 2;
    }
  }
  if (image_path.empty() == cue_path.empty()) {
    usage();
    return 2;
  }
  if (!cue_path.empty()) {
    std::ifstream cue(cue_path);
    if (!cue) {
      std::cerr << "cannot open cue\n";
      return 1;
    }
    const std::string text((std::istreambuf_iterator<char>(cue)), std::istreambuf_iterator<char>());
    std::string cue_error;
    const auto file_name = psx_cd_loc::cue_mode2_file(text, cue_error);
    if (!file_name.has_value()) {
      std::cerr << cue_error << '\n';
      return 1;
    }
    std::filesystem::path resolved(*file_name);
    if (resolved.is_relative()) {
      resolved = std::filesystem::path(cue_path).parent_path() / resolved;
    }
    image_path = resolved.string();
  }

  std::ifstream input(image_path, std::ios::binary);
  if (!input) {
    std::cerr << "cannot open image\n";
    return 1;
  }
  input.seekg(0, std::ios::end);
  const auto file_end = input.tellg();
  if (file_end < 0) {
    std::cerr << "cannot read image size\n";
    return 1;
  }
  const auto file_size = static_cast<std::uint64_t>(file_end);
  std::cout << "image " << image_path << '\n';
  std::cout << "bytes " << file_size << '\n';

  psx_cd_loc::IsoError iso_error = psx_cd_loc::IsoError::None;
  const auto volume = psx_cd_loc::read_iso9660(
      [&](std::uint32_t lba, std::vector<std::uint8_t>& user, std::string& error) {
        return read_user_sector(input, file_size, lba, user, error);
      },
      iso_error);
  if (!volume.has_value()) {
    std::cerr << psx_cd_loc::iso_error_text(iso_error) << '\n';
    return 1;
  }
  std::cout << "volume " << volume->volume_id << '\n';
  for (const auto& entry : volume->entries) {
    std::cout << (entry.directory ? "dir " : "file ") << entry.lba << ' ' << entry.length << ' '
              << entry.path << '\n';
  }

  std::vector<std::uint8_t> resource;
  std::string error;
  if (!read_user_span(input, file_size, kResourceLba, kResourceLength, resource, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  psx_cd_loc::ResourceError resource_error = psx_cd_loc::ResourceError::None;
  const auto fields = psx_cd_loc::resource_fields(resource, resource_error);
  std::cout << "resource lba=" << kResourceLba << " length=" << resource.size()
            << " sha256=" << psx_cd_loc::sha256_hex(resource) << '\n';
  if (!fields.has_value()) {
    std::cout << "resource_fields " << psx_cd_loc::resource_error_text(resource_error) << '\n';
  } else {
    std::cout << "prefix_bytes " << fields->prefix_bytes << '\n'
              << "copied_bytes " << fields->copied_bytes << '\n'
              << "companion " << fields->companion << '\n'
              << "tail_offset " << fields->tail_offset << '\n'
              << "half_at_30 " << fields->half_at_30 << '\n'
              << "word_at_40 " << fields->word_at_40 << '\n';
  }
  print_hex_ascii(resource, 64);
  const auto resource_tim = psx_cd_loc::decode_tim(resource);
  std::cout << "resource_tim " << (resource_tim.has_value() ? "yes" : "no") << '\n';

  for (const auto& stream : kInferredLz) {
    std::vector<std::uint8_t> compressed;
    const std::uint32_t rounded = psx_cd_loc::user_bytes_covering_sectors(stream.length);
    if (!read_user_span(input, file_size, stream.lba, rounded, compressed, error)) {
      std::cout << "lz name=" << stream.name << " lba=" << stream.lba << " error=" << error << '\n';
      continue;
    }
    psx_cd_loc::LzError lz_error = psx_cd_loc::LzError::None;
    const auto decoded = psx_cd_loc::lz_decompress(compressed, lz_error);
    if (!decoded.has_value()) {
      std::cout << "lz name=" << stream.name << " lba=" << stream.lba
                << " length=" << stream.length << " error=" << psx_cd_loc::lz_error_text(lz_error)
                << '\n';
      continue;
    }
    const auto tim = psx_cd_loc::decode_tim(decoded->bytes);
    std::cout << "lz name=" << stream.name << " lba=" << stream.lba << " length=" << stream.length
              << " read=" << rounded << " bytes=" << decoded->bytes.size()
              << " expected=" << stream.expected_output
              << " sha256=" << psx_cd_loc::sha256_hex(decoded->bytes)
              << " tim=" << (tim.has_value() ? "yes" : "no") << '\n';
    print_hex_ascii(decoded->bytes, 48);
    if (tim.has_value() && !export_dir.empty()) {
      const auto ppm = psx_cd_loc::encode_ppm(*tim);
      const auto path = std::filesystem::path(export_dir) / (std::string(stream.name) + ".ppm");
      std::ofstream output(path, std::ios::binary);
      if (!output) {
        std::cerr << "cannot write " << path << '\n';
        return 1;
      }
      output.write(reinterpret_cast<const char*>(ppm.data()), static_cast<std::streamsize>(ppm.size()));
      std::cout << "wrote " << path.string() << ' ' << tim->width << 'x' << tim->height << '\n';
    }
  }
  const psx_cd_loc::IsoEntry* executable = nullptr;
  for (const auto& entry : volume->entries) {
    if (!entry.directory && entry.path.find("SLUS") != std::string::npos) {
      executable = &entry;
      break;
    }
  }
  if (executable == nullptr) {
    std::cout << "index error=no executable in the ISO directory\n";
    return 0;
  }
  std::vector<std::uint8_t> exe;
  if (!read_user_span(input, file_size, executable->lba, executable->length, exe, error)) {
    std::cout << "index error=" << error << '\n';
    return 0;
  }
  if (exe.size() < 2048 + 0x8008 || std::string(reinterpret_cast<const char*>(exe.data()), 8) != "PS-X EXE") {
    std::cout << "index error=executable is not a PS-X EXE\n";
    return 0;
  }
  const std::uint16_t base_halfword = static_cast<std::uint16_t>(exe[2048 + 0x8006] |
                                                                (exe[2048 + 0x8007] << 8));
  const auto records = std::span<const std::uint8_t>(exe).subspan(2048 + 4);
  const auto indexed = psx_cd_loc::parse_indexed_files(records, base_halfword);
  std::cout << "index_base " << base_halfword << '\n';
  std::size_t file_count = 0;
  for (const auto& item : indexed) {
    if (item.group) {
      std::cout << "group " << item.id << ' ' << item.lba << ' ' << -item.length << '\n';
    } else {
      std::cout << "index " << item.id << ' ' << item.lba << ' ' << item.length << '\n';
      ++file_count;
    }
  }
  std::cout << "index_files " << file_count << '\n';

  bool found_tim = false;
  bool wrote_tim = false;
  for (const auto& item : indexed) {
    if (item.group || item.length <= 0) {
      continue;
    }
    const auto length = static_cast<std::uint32_t>(item.length);
    const std::uint32_t rounded = psx_cd_loc::user_bytes_covering_sectors(length);
    std::vector<std::uint8_t> body;
    if (!read_user_span(input, file_size, item.lba, rounded, body, error)) {
      continue;
    }
    const std::size_t exact = std::min(body.size(), static_cast<std::size_t>(length));
    auto raw_tim = psx_cd_loc::decode_tim(std::span<const std::uint8_t>(body).first(exact));
    std::optional<psx_cd_loc::TimRgb> lz_tim;
    const char* source = "raw";
    const psx_cd_loc::TimRgb* picture = nullptr;
    if (raw_tim.has_value()) {
      picture = &*raw_tim;
    } else {
      psx_cd_loc::LzError lz_error = psx_cd_loc::LzError::None;
      auto decoded = psx_cd_loc::lz_decompress(body, lz_error);
      if (decoded.has_value()) {
        lz_tim = psx_cd_loc::decode_tim(decoded->bytes);
        if (lz_tim.has_value()) {
          picture = &*lz_tim;
          source = "lz";
        }
      }
    }
    if (picture == nullptr) {
      continue;
    }
    found_tim = true;
    std::cout << "index_tim " << item.id << ' ' << item.lba << ' ' << source << ' '
              << picture->width << 'x' << picture->height << '\n';
    if (!export_dir.empty() && (export_all || !wrote_tim)) {
      const auto ppm = psx_cd_loc::encode_ppm(*picture);
      const auto path =
          std::filesystem::path(export_dir) / ("index-" + std::to_string(item.id) + ".ppm");
      std::ofstream output(path, std::ios::binary);
      if (output) {
        output.write(reinterpret_cast<const char*>(ppm.data()),
                     static_cast<std::streamsize>(ppm.size()));
        std::cout << "wrote " << path.string() << '\n';
        wrote_tim = true;
      }
    }
  }
  if (!found_tim) {
    std::cout << "index_tim none\n";
  }
  return 0;
}
