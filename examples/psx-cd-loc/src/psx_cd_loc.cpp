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

StartupDispatch dispatch_for_mode(std::int32_t mode) {
  StartupDispatch result;
  const bool retail_cd = mode == 0 || mode == -1;
  result.init = retail_cd ? StartupPath::Cd : StartupPath::Host;
  result.fixed_range_copy = mode == 0;
  result.later_cell = retail_cd ? 0 : mode;
  result.transfer =
      result.later_cell == 0 ? StartupPath::Cd : StartupPath::Host;
  return result;
}

std::int32_t index_adjustment(std::uint16_t halfword) {
  if (halfword == 0) {
    return 0;
  }
  return static_cast<std::int32_t>(halfword) - 1;
}

std::optional<std::uint32_t> record_index(std::uint32_t slot,
                                          std::int32_t adjustment) {
  const auto index = static_cast<std::int64_t>(slot) + adjustment - 1;
  if (index < 0 || index > static_cast<std::int64_t>(kSignedMax)) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(index);
}

std::optional<CdLoad> cd_load_from_record(const IndexRecord& record) {
  const auto position = logical_sector_to_bcd_msf(record.logical_sector);
  if (!position.has_value()) {
    return std::nullopt;
  }
  if (record.byte_length > kSignedMax) {
    return std::nullopt;
  }
  const auto aligned =
      aligned_byte_length(static_cast<std::int32_t>(record.byte_length));
  if (!aligned.has_value()) {
    return std::nullopt;
  }
  CdLoad load;
  load.logical_sector = record.logical_sector;
  load.position = *position;
  load.byte_length = record.byte_length;
  load.aligned_length = *aligned;
  return load;
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
