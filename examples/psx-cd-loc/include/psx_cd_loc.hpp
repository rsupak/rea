#pragma once

// Logical CD sector position and the 7-byte index record that carries one.
// This converts numbers. It does not issue a drive command, open a file,
// allocate a buffer, or read a sector.
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

#include <cstdint>
#include <optional>
#include <span>

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

// Requires exactly seven bytes. A shorter or longer span is not a record.
std::optional<IndexRecord> decode_index_record(
    std::span<const std::uint8_t> bytes);

// Next multiple of 4, including the value itself when it is already aligned.
// Empty when byte_length is negative or byte_length + 3 overflows signed 32-bit.
std::optional<std::uint32_t> aligned_byte_length(std::int32_t byte_length);

}  // namespace psx_cd_loc
