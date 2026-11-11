#pragma once

// File classification for one indexed blob. TIM, the bitstream unpacker, and
// the offset-word relocation are taken from the executable. Audio and STR
// checks are standard signatures. A MIPS prologue is only a shape check.

#include "psx_disc.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace psx_cd_loc {

enum class FileKind {
  Tim,
  Lz,
  LzTim,
  OffsetTable,
  Text,
  PsxExe,
  Vag,
  Vab,
  Seq,
  Str,
  Mips,
  Data,
  Wds,
  Smds,
  Seds,
};

struct FoundTim {
  TimRgb image;
  std::string label;
};

struct Classification {
  FileKind kind = FileKind::Data;
  std::string detail;
  std::vector<FoundTim> pictures;
};

const char* file_kind_name(FileKind kind);

// `rounded` is the sector-rounded image used as bitstream input. `exact` is
// the index length and is the prefix of `rounded` when the read was rounded.
Classification classify_file_bytes(std::span<const std::uint8_t> rounded,
                                   std::uint32_t exact_length);

struct IndexedBlob {
  IndexedFile file;
  Classification classification;
  bool read_error = false;
};

struct IndexedDisc {
  std::string image_path;
  std::string volume_id;
  std::uint16_t index_base = 0;
  std::vector<IndexedBlob> entries;
  std::string error;
};

// `path` is a MODE2/2352 BIN, or a CUE whose first data track is MODE2/2352.
// A relative FILE name is resolved next to the CUE. The image is not written.
IndexedDisc open_indexed_disc(const std::string& path);

struct FilePreview {
  Classification classification;
  std::string dump;
  std::string error;
  // First wds sample, when the file is one. Empty otherwise.
  std::vector<std::int16_t> playable;
};

// Re-reads one indexed file and formats the first 256 bytes.
FilePreview preview_indexed_file(const std::string& image_path, std::uint32_t lba,
                                 std::uint32_t length);

}  // namespace psx_cd_loc
