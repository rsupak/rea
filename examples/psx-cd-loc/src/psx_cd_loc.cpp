#include "psx_cd_loc.hpp"

namespace psx_cd_loc {
namespace {

constexpr std::uint32_t kSignedMax = 0x7FFFFFFF;

std::uint8_t decimal_to_bcd(std::uint32_t value) {
  return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
}

}  // namespace

std::optional<BcdMsf> logical_sector_to_bcd_msf(std::uint32_t logical_sector) {
  if (logical_sector > kSignedMax - kLeadInFrames) {
    return std::nullopt;
  }
  const std::uint32_t absolute = logical_sector + kLeadInFrames;
  const std::uint32_t frames = absolute % kFramesPerSecond;
  const std::uint32_t total_seconds = absolute / kFramesPerSecond;
  BcdMsf result;
  result.minute = decimal_to_bcd(total_seconds / kSecondsPerMinute);
  result.second = decimal_to_bcd(total_seconds % kSecondsPerMinute);
  result.sector = decimal_to_bcd(frames);
  return result;
}

std::optional<IndexRecord> decode_index_record(
    std::span<const std::uint8_t> bytes) {
  if (bytes.size() != 7) {
    return std::nullopt;
  }
  IndexRecord record;
  record.logical_sector = static_cast<std::uint32_t>(bytes[0]) |
                           (static_cast<std::uint32_t>(bytes[1]) << 8) |
                           (static_cast<std::uint32_t>(bytes[2]) << 16);
  record.byte_length = static_cast<std::uint32_t>(bytes[3]) |
                       (static_cast<std::uint32_t>(bytes[4]) << 8) |
                       (static_cast<std::uint32_t>(bytes[5]) << 16) |
                       (static_cast<std::uint32_t>(bytes[6]) << 24);
  return record;
}

std::optional<std::uint32_t> aligned_byte_length(std::int32_t byte_length) {
  if (byte_length < 0) {
    return std::nullopt;
  }
  const auto length = static_cast<std::uint32_t>(byte_length);
  if (length > kSignedMax - 3) {
    return std::nullopt;
  }
  return (length + 3) & ~std::uint32_t{3};
}

}  // namespace psx_cd_loc
