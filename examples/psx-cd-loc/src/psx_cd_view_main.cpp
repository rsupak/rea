#include "psx_catalog.hpp"
#include "psx_cd_loc.hpp"
#include "psx_disc.hpp"

#include <algorithm>
#include <cstdint>
#include <span>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
      << "psx_cd_view (--image BIN | --cue CUE) [--export-dir DIR] [--export-all] [--classify]\n"
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
  bool classify = false;
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
    } else if (arg == "--classify") {
      classify = true;
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
  const auto disc = psx_cd_loc::open_indexed_disc(image_path);
  if (!disc.error.empty()) {
    std::cout << "index error=" << disc.error << '\n';
    return 0;
  }
  std::cout << "index_base " << disc.index_base << '\n';
  std::size_t file_count = 0;
  for (const auto& blob : disc.entries) {
    if (blob.file.group) {
      std::cout << "group " << blob.file.id << ' ' << blob.file.lba << ' ' << -blob.file.length << '\n';
    } else {
      std::cout << "index " << blob.file.id << ' ' << blob.file.lba << ' ' << blob.file.length << '\n';
      ++file_count;
    }
  }
  std::cout << "index_files " << file_count << '\n';

  bool found_tim = false;
  bool wrote_tim = false;
  std::map<std::string, int> counts;
  for (const auto& blob : disc.entries) {
    if (blob.file.group || blob.file.length <= 0) {
      continue;
    }
    if (blob.read_error) {
      if (classify) {
        std::cout << "classify " << blob.file.id << ' ' << blob.file.lba << ' ' << blob.file.length
                  << " error\n";
      }
      continue;
    }
    const auto& classified = blob.classification;
    const char* name = psx_cd_loc::file_kind_name(classified.kind);
    ++counts[name];
    if (classify) {
      std::cout << "classify " << blob.file.id << ' ' << blob.file.lba << ' ' << blob.file.length << ' '
                << name;
      if (!classified.detail.empty()) {
        std::cout << ' ' << classified.detail;
      }
      std::cout << '\n';
    }
    int picture_index = 0;
    for (const auto& picture : classified.pictures) {
      found_tim = true;
      std::cout << "index_tim " << blob.file.id << ' ' << blob.file.lba << ' ' << picture.label << ' '
                << picture.image.width << 'x' << picture.image.height << '\n';
      if (!export_dir.empty() && (export_all || !wrote_tim)) {
        const auto ppm = psx_cd_loc::encode_ppm(picture.image);
        std::string label = picture.label;
        for (char& ch : label) {
          if (ch == ':') {
            ch = '-';
          }
        }
        const auto filename = picture_index == 0 && picture.label == "raw"
                                  ? "index-" + std::to_string(blob.file.id) + ".ppm"
                                  : "index-" + std::to_string(blob.file.id) + "-" + label + ".ppm";
        const auto path = std::filesystem::path(export_dir) / filename;
        std::ofstream output(path, std::ios::binary);
        if (output) {
          output.write(reinterpret_cast<const char*>(ppm.data()),
                       static_cast<std::streamsize>(ppm.size()));
          std::cout << "wrote " << path.string() << '\n';
          wrote_tim = true;
        }
      }
      ++picture_index;
    }
  }
  if (classify) {
    std::cout << "summary";
    for (const auto& count : counts) {
      std::cout << ' ' << count.first << '=' << count.second;
    }
    std::cout << '\n';
  }
  if (!found_tim) {
    std::cout << "index_tim none\n";
  }
  return 0;
}
