#pragma once

// Logical CD sector position and the 7-byte index record that carries one.
// The position and index helpers convert numbers. They do not issue a drive
// command. The Mode 2 reader copies user bytes out of an image buffer the
// caller already has. It does not program DMA or emulate a drive.
//
// A logical sector is counted from the start of the data area. The position
// helper adds 150 frames first (2 seconds * 75 frames/second), which is the
// lead-in used when a drive command wants an absolute minute/second/frame.
// Each field is then stored as two decimal nibbles: high nibble is tens,
// low nibble is units.
//
// The record is seven bytes, little-endian:
//   0..2  logical sector (24-bit; the high byte of the returned word is 0)
//   3..6  byte length
// A nonnegative length used as a transfer size is rounded up to a multiple
// of 4. A length whose +3 does not fit in a signed 32-bit value is rejected.
// The helper also has a separate branch for negative lengths; that branch is
// not a byte-count alignment and is not exposed here.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psx_cd_loc {

inline constexpr std::uint32_t kFramesPerSecond = 75;
inline constexpr std::uint32_t kSecondsPerMinute = 60;
inline constexpr std::uint32_t kLeadInFrames = 2 * kFramesPerSecond;

struct BcdMsf {
  std::uint8_t minute = 0;
  std::uint8_t second = 0;
  std::uint8_t sector = 0;

  friend constexpr bool operator==(BcdMsf lhs, BcdMsf rhs) {
    return lhs.minute == rhs.minute && lhs.second == rhs.second &&
           lhs.sector == rhs.sector;
  }
};

struct IndexRecord {
  std::uint32_t logical_sector = 0;
  std::uint32_t byte_length = 0;
};

// Empty when logical_sector + 150 does not fit in a positive signed 32-bit
// value. The division sequence being matched is signed, so this stays inside
// the range where unsigned arithmetic and that sequence agree.
std::optional<BcdMsf> logical_sector_to_bcd_msf(std::uint32_t logical_sector);

// Two different tests. They are not the same comparison.
//
// The init test takes the CD controller path when the mode argument is 0 or
// -1. Every other value takes the development-host init break. A stepped
// run with the argument equal to -1 fell through into the CD init call.
//
// Before returning, that function writes the cell later tests consult:
// 0 and -1 both store 0; any other argument stores itself. The transfer
// test is only "cell == 0". It does not look at the original argument again.
// A stored 0 takes the indexed CD request (Setloc of the record's position).
// A nonzero cell takes the host read. Mode 0 also schedules one fixed-size
// copy that mode -1 skips.
enum class StartupPath { Cd, Host };

struct StartupDispatch {
  StartupPath init = StartupPath::Cd;
  bool fixed_range_copy = false;
  std::int32_t later_cell = 0;
  StartupPath transfer = StartupPath::Cd;
};

StartupDispatch dispatch_for_mode(std::int32_t mode);

// Halfword 0 stores adjustment 0. Any other halfword stores halfword - 1.
// The record number is slot + adjustment - 1. Empty when that sum is negative.
std::int32_t index_adjustment(std::uint16_t halfword);
std::optional<std::uint32_t> record_index(std::uint32_t slot,
                                          std::int32_t adjustment);

// The CD request formed from one record: absolute position plus the byte
// length that is allocated and retained. Forming the request does not send
// Setloc or read a sector. read_cd_load copies that byte length from an
// image. The captured transfer copied byte_length, not a wider rounded size.
struct CdLoad {
  std::uint32_t logical_sector = 0;
  BcdMsf position{};
  std::uint32_t byte_length = 0;
  std::uint32_t aligned_length = 0;
};

std::optional<CdLoad> cd_load_from_record(const IndexRecord& record);

// Requires exactly seven bytes. A shorter or longer span is not a record.
std::optional<IndexRecord> decode_index_record(
    std::span<const std::uint8_t> bytes);

// Next multiple of 4, including the value itself when it is already aligned.
// Empty when byte_length is negative or byte_length + 3 overflows signed 32-bit.
std::optional<std::uint32_t> aligned_byte_length(std::int32_t byte_length);

// A 2352-byte Mode 2 sector, as transferred: 12-byte sync, then 12 bytes at
// raw offset 12 (BCD minute/second/frame, mode, and the 8-byte subheader),
// then up to 2048 user bytes at raw offset 24. The file index is the ISO
// logical sector. Absolute minute/second/frame is that sector plus 150, and
// is not added to the file offset. A read copies exactly the requested user
// byte count; the last sector contributes only the leading leftover bytes.
inline constexpr std::uint64_t kRawSectorBytes = 2352;
inline constexpr std::uint32_t kUserBytesPerSector = 2048;

// The index length is the exact file size. A disc read that must feed the
// bitstream also needs the unused bytes at the end of the last sector,
// because a finished unpack consumes one more flag byte. This rounds the
// read up to whole user sectors. The decoder itself stays exact.
inline constexpr std::uint32_t user_bytes_covering_sectors(std::uint32_t length) {
  if (length == 0) {
    return 0;
  }
  const std::uint64_t sectors =
      (static_cast<std::uint64_t>(length) + kUserBytesPerSector - 1) / kUserBytesPerSector;
  const std::uint64_t bytes = sectors * kUserBytesPerSector;
  if (bytes > 0xFFFFFFFFull) {
    return 0xFFFFFFFFu;
  }
  return static_cast<std::uint32_t>(bytes);
}
inline constexpr std::uint32_t kSectorHeaderBytes = 12;
inline constexpr std::uint32_t kUserDataOffset = 24;
inline constexpr std::uint8_t kMode2 = 2;

enum class ImageError {
  None,
  EmptyLength,
  RangeOverflow,
  Truncated,
  BadSync,
  UnsupportedMode,
  BadSubheader,
  MsfMismatch,
};

struct Mode2Range {
  std::uint64_t file_offset = 0;
  std::uint64_t raw_bytes = 0;
  std::uint32_t sector_count = 0;
};

struct Mode2Sector {
  std::uint32_t iso_lba = 0;
  BcdMsf absolute_msf{};
  std::uint8_t mode = 0;
  std::uint8_t submode = 0;
  std::array<std::uint8_t, kSectorHeaderBytes> header{};
  std::uint32_t user_bytes_taken = 0;
};

struct Mode2Read {
  std::vector<std::uint8_t> user;
  std::vector<Mode2Sector> sectors;
};

const char* image_error_text(ImageError error);

// File offset of the ISO sector, relative to base_lba. base_lba 0 is a disc
// image whose first sector is logical sector 0. A bounded extract of a later
// range uses that range's first logical sector as base_lba.
std::optional<Mode2Range> mode2_user_range(std::uint32_t iso_lba,
                                           std::uint32_t user_bytes,
                                           std::uint32_t base_lba,
                                           ImageError& error);

std::optional<Mode2Read> read_mode2_user(std::span<const std::uint8_t> image,
                                         std::uint32_t iso_lba,
                                         std::uint32_t user_bytes,
                                         std::uint32_t base_lba,
                                         ImageError& error);

std::optional<Mode2Read> read_cd_load(std::span<const std::uint8_t> image,
                                      const CdLoad& load,
                                      std::uint32_t base_lba,
                                      ImageError& error);

std::string sha256_hex(std::span<const std::uint8_t> data);

// Fields the first consumer loads from the resource. The word at offset 16 is
// the prefix size passed to allocation and to the 16-byte copy. That copy
// keeps whole words only, so the copied length is prefix_bytes with the low
// two bits cleared. The word at offset 24 is added to the resource base and
// passed on as a pointer. The word at offset 20 and the halfword at offset 30
// are passed on with it. On the zero second-argument path, the word at offset
// 40 is loaded and can be returned unchanged. No tag is compared.
enum class ResourceError {
  None,
  TooSmall,
  PrefixPastEnd,
  TailPastEnd,
};

struct ResourceFields {
  std::uint32_t prefix_bytes = 0;
  std::uint32_t copied_bytes = 0;
  std::uint32_t companion = 0;
  std::uint32_t tail_offset = 0;
  std::uint16_t half_at_30 = 0;
  std::uint32_t word_at_40 = 0;
};

const char* resource_error_text(ResourceError error);

std::optional<ResourceFields> resource_fields(std::span<const std::uint8_t> bytes,
                                             ResourceError& error);

// Bitstream unpacker. The first little-endian word is the output size.
// Each flag byte is eight bits, lowest bit first. 0 copies the next byte.
// 1 reads two bytes: length is the high nibble plus 3 (3..18), and the
// distance is the low nibble shifted 8 bits combined with the first byte.
// Bytes are copied one at a time from output_end - distance, so a distance
// of 1 repeats the previous byte. A finished output stops at the next flag
// byte, which is consumed. Declared sizes above 16 MiB are rejected.
inline constexpr std::uint32_t kLzMaxOutput = 16u * 1024u * 1024u;

enum class LzError {
  None,
  Truncated,
  BadSize,
  BackrefRange,
  OutputOverflow,
};

struct LzOutput {
  std::vector<std::uint8_t> bytes;
  std::size_t source_used = 0;
};

const char* lz_error_text(LzError error);

std::optional<LzOutput> lz_decompress(std::span<const std::uint8_t> source,
                                     LzError& error);

}  // namespace psx_cd_loc
