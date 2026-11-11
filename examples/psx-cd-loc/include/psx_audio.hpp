#pragma once

// Sound files named by their first four bytes. The wds and seds field offsets
// are the ones the executable loads. Sample records inside a wds header are
// accepted only when every address, in the SPU's 8-byte units, lands in the
// sample blob. PS-ADPCM uses the standard 16-byte block. The WAV rate is the
// SPU nominal rate at pitch 0x1000; these headers do not supply a pitch.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psx_cd_loc {

inline constexpr std::uint32_t kSpuNominalRate = 44100;

struct WdsSample {
  std::uint32_t spu_units = 0;
  std::uint32_t byte_offset = 0;
  std::uint32_t byte_size = 0;
  std::uint16_t field_4 = 0;
  std::uint16_t field_6 = 0;
};

struct WdsBank {
  std::uint32_t header_bytes = 0;
  std::uint32_t sample_bytes = 0;
  std::uint32_t sample_offset = 0;
  std::uint16_t half_at_30 = 0;
  std::uint32_t word_at_40 = 0;
  bool records = false;
  std::vector<WdsSample> samples;
};

struct SedsInfo {
  std::uint32_t covered_bytes = 0;
  std::uint16_t version = 0;
  std::uint16_t id = 0;
};

std::optional<WdsBank> parse_wds(std::span<const std::uint8_t> bytes);
std::optional<SedsInfo> parse_seds(std::span<const std::uint8_t> bytes);

// One pass over 16-byte blocks. A block whose flag byte has bit 0 set is the
// last block. Loop flags do not repeat samples.
std::vector<std::int16_t> decode_psx_adpcm(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> encode_wav(std::span<const std::int16_t> samples, std::uint32_t rate);

std::string wds_detail(const WdsBank& bank);
std::string seds_detail(const SedsInfo& info);

}  // namespace psx_cd_loc
