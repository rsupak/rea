#include "psx_catalog.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace psx_cd_loc {
namespace {

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool starts_with(std::span<const std::uint8_t> bytes, const char* magic, std::size_t n) {
  if (bytes.size() < n) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (bytes[i] != static_cast<std::uint8_t>(magic[i])) {
      return false;
    }
  }
  return true;
}

bool looks_like_text(std::span<const std::uint8_t> bytes) {
  const std::size_t n = std::min<std::size_t>(bytes.size(), 64);
  if (n < 16) {
    return false;
  }
  std::size_t printable = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char ch = bytes[i];
    if (ch == '\n' || ch == '\r' || ch == '\t' || (ch >= 32 && ch < 127)) {
      ++printable;
    }
  }
  return printable * 5 >= n * 4;
}

bool looks_like_mips_prologue(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 8) {
    return false;
  }
  const std::uint32_t first = read_u32(bytes, 0);
  const std::uint32_t second = read_u32(bytes, 4);
  const bool addiu_sp = (first >> 26) == 9 && ((first >> 21) & 31) == 29 && ((first >> 16) & 31) == 29 &&
                        (static_cast<std::int16_t>(first & 0xFFFF) < 0);
  const bool sw_ra = (second >> 26) == 43 && ((second >> 21) & 31) == 29 && ((second >> 16) & 31) == 31;
  return addiu_sp && sw_ra;
}

// The loader adds the block base to each of the count words at offset 4.
// A negative word is still relocated, so only a majority must land inside.
bool is_offset_table(std::span<const std::uint8_t> bytes, std::uint32_t& count, std::uint32_t& inside) {
  if (bytes.size() < 8) {
    return false;
  }
  count = read_u32(bytes, 0);
  if (count < 1 || count > 256) {
    return false;
  }
  const std::uint64_t table_bytes = (static_cast<std::uint64_t>(count) + 1ull) * 4ull;
  if (table_bytes > bytes.size()) {
    return false;
  }
  inside = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto offset = static_cast<std::int32_t>(read_u32(bytes, 4 + static_cast<std::size_t>(i) * 4));
    if (offset >= 0 && static_cast<std::uint32_t>(offset) < bytes.size()) {
      ++inside;
    }
  }
  return inside * 2 >= count;
}

void collect_offset_tims(std::span<const std::uint8_t> bytes, std::vector<FoundTim>& pictures) {
  std::uint32_t count = 0;
  std::uint32_t inside = 0;
  if (!is_offset_table(bytes, count, inside)) {
    return;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto offset = static_cast<std::int32_t>(read_u32(bytes, 4 + static_cast<std::size_t>(i) * 4));
    if (offset < 0 || static_cast<std::uint32_t>(offset) >= bytes.size()) {
      continue;
    }
    auto picture = decode_tim(bytes.subspan(static_cast<std::size_t>(offset)));
    if (picture.has_value()) {
      pictures.push_back(FoundTim{std::move(*picture), "offset:" + std::to_string(i)});
    }
  }
}

}  // namespace

const char* file_kind_name(FileKind kind) {
  switch (kind) {
    case FileKind::Tim:
      return "tim";
    case FileKind::Lz:
      return "lz";
    case FileKind::LzTim:
      return "lz_tim";
    case FileKind::OffsetTable:
      return "offset_table";
    case FileKind::Text:
      return "text";
    case FileKind::PsxExe:
      return "psx_exe";
    case FileKind::Vag:
      return "vag";
    case FileKind::Vab:
      return "vab";
    case FileKind::Seq:
      return "seq";
    case FileKind::Str:
      return "str";
    case FileKind::Mips:
      return "mips";
    case FileKind::Data:
      return "data";
  }
  return "data";
}

Classification classify_file_bytes(std::span<const std::uint8_t> rounded, std::uint32_t exact_length) {
  Classification result;
  const std::size_t exact = std::min(rounded.size(), static_cast<std::size_t>(exact_length));
  const auto file = rounded.first(exact);
  if (starts_with(file, "PS-X EXE", 8)) {
    result.kind = FileKind::PsxExe;
    result.detail = "boot executable";
    return result;
  }
  if (auto picture = decode_tim(file)) {
    result.kind = FileKind::Tim;
    result.detail = std::to_string(picture->width) + "x" + std::to_string(picture->height);
    result.pictures.push_back(FoundTim{std::move(*picture), "raw"});
    return result;
  }
  if (starts_with(file, "VAGp", 4)) {
    result.kind = FileKind::Vag;
    return result;
  }
  if (starts_with(file, "pBAV", 4)) {
    result.kind = FileKind::Vab;
    return result;
  }
  if (starts_with(file, "pQES", 4)) {
    result.kind = FileKind::Seq;
    return result;
  }
  if (file.size() >= 4 && read_u32(file, 0) == 0x80010160u) {
    result.kind = FileKind::Str;
    result.detail = "frame signature";
    return result;
  }
  LzError lz_error = LzError::None;
  auto decoded = lz_decompress(rounded, lz_error);
  if (decoded.has_value() && !decoded->bytes.empty() && decoded->source_used <= rounded.size()) {
    if (auto picture = decode_tim(decoded->bytes)) {
      result.kind = FileKind::LzTim;
      result.detail = std::to_string(picture->width) + "x" + std::to_string(picture->height);
      result.pictures.push_back(FoundTim{std::move(*picture), "lz"});
      return result;
    }
    std::uint32_t count = 0;
    std::uint32_t inside = 0;
    if (is_offset_table(decoded->bytes, count, inside)) {
      result.kind = FileKind::OffsetTable;
      result.detail = "count=" + std::to_string(count) + " inside=" + std::to_string(inside);
      collect_offset_tims(decoded->bytes, result.pictures);
      return result;
    }
    result.kind = FileKind::Lz;
    result.detail = "out=" + std::to_string(decoded->bytes.size());
    return result;
  }
  std::uint32_t count = 0;
  std::uint32_t inside = 0;
  if (is_offset_table(file, count, inside)) {
    result.kind = FileKind::OffsetTable;
    result.detail = "count=" + std::to_string(count) + " inside=" + std::to_string(inside);
    collect_offset_tims(file, result.pictures);
    return result;
  }
  if (looks_like_text(file)) {
    result.kind = FileKind::Text;
    return result;
  }
  if (looks_like_mips_prologue(file)) {
    result.kind = FileKind::Mips;
    result.detail = "prologue";
    return result;
  }
  result.kind = FileKind::Data;
  return result;
}

namespace {

bool read_user_span(std::ifstream& input, std::uint64_t file_size, std::uint32_t lba, std::uint32_t length,
                    std::vector<std::uint8_t>& out, std::string& error) {
  ImageError image_error = ImageError::None;
  const auto range = mode2_user_range(lba, length, 0, image_error);
  if (!range.has_value()) {
    error = image_error_text(image_error);
    return false;
  }
  if (range->file_offset > file_size || range->raw_bytes > file_size - range->file_offset) {
    error = image_error_text(ImageError::Truncated);
    return false;
  }
  input.clear();
  input.seekg(static_cast<std::streamoff>(range->file_offset), std::ios::beg);
  std::vector<std::uint8_t> raw(static_cast<std::size_t>(range->raw_bytes));
  input.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
  if (input.gcount() != static_cast<std::streamsize>(raw.size())) {
    error = image_error_text(ImageError::Truncated);
    return false;
  }
  const auto decoded = read_mode2_user(raw, lba, length, lba, image_error);
  if (!decoded.has_value()) {
    error = image_error_text(image_error);
    return false;
  }
  out = decoded->user;
  return true;
}

std::string resolve_image(const std::string& path, std::string& error) {
  const auto extension = std::filesystem::path(path).extension().string();
  std::string lowered = extension;
  for (char& ch : lowered) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  if (lowered != ".cue") {
    return path;
  }
  std::ifstream cue(path);
  if (!cue) {
    error = "cannot open cue";
    return {};
  }
  const std::string text((std::istreambuf_iterator<char>(cue)), std::istreambuf_iterator<char>());
  const auto file_name = cue_mode2_file(text, error);
  if (!file_name.has_value()) {
    return {};
  }
  std::filesystem::path resolved(*file_name);
  if (resolved.is_relative()) {
    resolved = std::filesystem::path(path).parent_path() / resolved;
  }
  return resolved.string();
}

char hex_digit(unsigned value) {
  return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

std::string hex_dump(std::span<const std::uint8_t> bytes) {
  std::ostringstream out;
  const std::size_t n = std::min<std::size_t>(bytes.size(), 256);
  for (std::size_t i = 0; i < n; i += 16) {
    out << hex_digit((i >> 12) & 0xF) << hex_digit((i >> 8) & 0xF) << hex_digit((i >> 4) & 0xF)
        << hex_digit(i & 0xF) << "  ";
    for (std::size_t j = 0; j < 16; ++j) {
      if (i + j < n) {
        const unsigned char value = bytes[i + j];
        out << hex_digit(value >> 4) << hex_digit(value & 0xF) << ' ';
      }
    }
    out << ' ';
    for (std::size_t j = 0; j < 16 && i + j < n; ++j) {
      const unsigned char ch = bytes[i + j];
      out << (ch >= 32 && ch < 127 ? static_cast<char>(ch) : '.');
    }
    out << '\n';
  }
  return out.str();
}

}  // namespace

IndexedDisc open_indexed_disc(const std::string& path) {
  IndexedDisc disc;
  disc.image_path = resolve_image(path, disc.error);
  if (!disc.error.empty() || disc.image_path.empty()) {
    if (disc.error.empty()) {
      disc.error = "cannot open image";
    }
    return disc;
  }
  std::ifstream input(disc.image_path, std::ios::binary);
  if (!input) {
    disc.error = "cannot open image";
    return disc;
  }
  input.seekg(0, std::ios::end);
  const auto file_end = input.tellg();
  if (file_end < 0) {
    disc.error = "cannot read image size";
    return disc;
  }
  const auto file_size = static_cast<std::uint64_t>(file_end);
  IsoError iso_error = IsoError::None;
  const auto volume = read_iso9660(
      [&](std::uint32_t lba, std::vector<std::uint8_t>& user, std::string& error) {
        return read_user_span(input, file_size, lba, kUserBytesPerSector, user, error) &&
               user.size() >= kUserBytesPerSector;
      },
      iso_error);
  if (!volume.has_value()) {
    disc.error = iso_error_text(iso_error);
    return disc;
  }
  disc.volume_id = volume->volume_id;
  const IsoEntry* executable = nullptr;
  for (const auto& entry : volume->entries) {
    if (!entry.directory && entry.path.find("SLUS") != std::string::npos) {
      executable = &entry;
      break;
    }
  }
  if (executable == nullptr) {
    disc.error = "no executable in the ISO directory";
    return disc;
  }
  std::vector<std::uint8_t> exe;
  if (!read_user_span(input, file_size, executable->lba, executable->length, exe, disc.error)) {
    return disc;
  }
  if (exe.size() < 2048 + 0x8008 || std::string(reinterpret_cast<const char*>(exe.data()), 8) != "PS-X EXE") {
    disc.error = "executable is not a PS-X EXE";
    return disc;
  }
  disc.index_base = static_cast<std::uint16_t>(exe[2048 + 0x8006] | (exe[2048 + 0x8007] << 8));
  const auto indexed = parse_indexed_files(std::span<const std::uint8_t>(exe).subspan(2048 + 4), disc.index_base);
  for (const auto& item : indexed) {
    IndexedBlob blob;
    blob.file = item;
    if (!item.group && item.length > 0) {
      const auto length = static_cast<std::uint32_t>(item.length);
      std::vector<std::uint8_t> body;
      std::string error;
      const std::uint32_t rounded = user_bytes_covering_sectors(length);
      if (!read_user_span(input, file_size, item.lba, rounded, body, error)) {
        blob.read_error = true;
        blob.classification.detail = error;
      } else {
        blob.classification = classify_file_bytes(body, length);
      }
    }
    disc.entries.push_back(std::move(blob));
  }
  return disc;
}

FilePreview preview_indexed_file(const std::string& image_path, std::uint32_t lba, std::uint32_t length) {
  FilePreview preview;
  std::ifstream input(image_path, std::ios::binary);
  if (!input) {
    preview.error = "cannot open image";
    return preview;
  }
  input.seekg(0, std::ios::end);
  const auto file_end = input.tellg();
  if (file_end < 0) {
    preview.error = "cannot read image size";
    return preview;
  }
  std::vector<std::uint8_t> body;
  const std::uint32_t rounded = user_bytes_covering_sectors(length);
  if (!read_user_span(input, static_cast<std::uint64_t>(file_end), lba, rounded, body, preview.error)) {
    return preview;
  }
  preview.classification = classify_file_bytes(body, length);
  preview.dump = hex_dump(std::span<const std::uint8_t>(body).first(std::min(body.size(), static_cast<std::size_t>(length))));
  return preview;
}

}  // namespace psx_cd_loc
