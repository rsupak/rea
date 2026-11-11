#include "psx_audio.hpp"

#include <algorithm>

namespace psx_cd_loc {
namespace {

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool tagged(std::span<const std::uint8_t> bytes, const char* tag) {
  return bytes.size() >= 4 && bytes[0] == static_cast<std::uint8_t>(tag[0]) &&
         bytes[1] == static_cast<std::uint8_t>(tag[1]) &&
         bytes[2] == static_cast<std::uint8_t>(tag[2]) &&
         bytes[3] == static_cast<std::uint8_t>(tag[3]);
}

void write_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void write_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
  out.push_back(static_cast<std::uint8_t>(value >> 16));
  out.push_back(static_cast<std::uint8_t>(value >> 24));
}

int clamp16(int value) {
  if (value > 32767) {
    return 32767;
  }
  if (value < -32768) {
    return -32768;
  }
  return value;
}

}  // namespace

std::optional<WdsBank> parse_wds(std::span<const std::uint8_t> bytes) {
  if (!tagged(bytes, "wds ") || bytes.size() < 44) {
    return std::nullopt;
  }
  WdsBank bank;
  bank.header_bytes = read_u32(bytes, 16);
  bank.sample_bytes = read_u32(bytes, 20);
  bank.sample_offset = read_u32(bytes, 24);
  bank.half_at_30 = read_u16(bytes, 30);
  bank.word_at_40 = read_u32(bytes, 40);
  if (bank.header_bytes < 44 || bank.header_bytes > bytes.size() ||
      bank.sample_offset > bytes.size() ||
      bank.sample_bytes > bytes.size() - bank.sample_offset) {
    return std::nullopt;
  }
  const auto samples = bytes.subspan(bank.sample_offset, bank.sample_bytes);
  const std::uint32_t count = bytes.size() >= 32 ? read_u32(bytes, 28) : 0;
  const bool table = count >= 1 && count <= 4096 &&
                     static_cast<std::uint64_t>(64) + static_cast<std::uint64_t>(count) * 16 <=
                         bank.header_bytes;
  if (table) {
    std::vector<WdsSample> records;
    bool ordered = true;
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::size_t at = 64 + static_cast<std::size_t>(i) * 16;
      WdsSample sample;
      sample.spu_units = read_u32(bytes, at);
      sample.field_4 = read_u16(bytes, at + 4);
      sample.field_6 = read_u16(bytes, at + 6);
      const std::uint64_t offset = static_cast<std::uint64_t>(sample.spu_units) * 8ull;
      if (offset > samples.size() || (i > 0 && offset < records.back().byte_offset)) {
        ordered = false;
        break;
      }
      sample.byte_offset = static_cast<std::uint32_t>(offset);
      records.push_back(sample);
    }
    if (ordered && records.size() == count) {
      for (std::size_t i = 0; i < records.size(); ++i) {
        const std::uint32_t end =
            i + 1 < records.size() ? records[i + 1].byte_offset : static_cast<std::uint32_t>(samples.size());
        records[i].byte_size = end - records[i].byte_offset;
      }
      bank.records = true;
      bank.samples = std::move(records);
      return bank;
    }
  }
  WdsSample whole;
  whole.byte_size = bank.sample_bytes;
  bank.samples.push_back(whole);
  return bank;
}

std::optional<SedsInfo> parse_seds(std::span<const std::uint8_t> bytes) {
  if (!tagged(bytes, "seds") || bytes.size() < 22) {
    return std::nullopt;
  }
  SedsInfo info;
  info.covered_bytes = read_u32(bytes, 8);
  info.version = read_u16(bytes, 12);
  info.id = read_u16(bytes, 20);
  return info;
}

std::vector<std::int16_t> decode_psx_adpcm(std::span<const std::uint8_t> bytes) {
  static constexpr int kF0[] = {0, 60, 115, 98, 122};
  static constexpr int kF1[] = {0, 0, -52, -55, -60};
  std::vector<std::int16_t> pcm;
  int older = 0;
  int old = 0;
  for (std::size_t block = 0; block + 16 <= bytes.size(); block += 16) {
    const unsigned header = bytes[block];
    const unsigned shift = std::min(header & 0x0Fu, 12u);
    const unsigned filter = (header >> 4) & 0x0Fu;
    const int f0 = filter < 5 ? kF0[filter] : 0;
    const int f1 = filter < 5 ? kF1[filter] : 0;
    const unsigned flags = bytes[block + 1];
    for (int i = 0; i < 14; ++i) {
      const unsigned packed = bytes[block + 2 + static_cast<std::size_t>(i)];
      for (int nibble_index = 0; nibble_index < 2; ++nibble_index) {
        int nibble = (packed >> (nibble_index * 4)) & 0x0F;
        if (nibble >= 8) {
          nibble -= 16;
        }
        int sample = (nibble << 12) >> shift;
        sample += ((old * f0) + (older * f1) + 32) >> 6;
        sample = clamp16(sample);
        older = old;
        old = sample;
        pcm.push_back(static_cast<std::int16_t>(sample));
      }
    }
    if ((flags & 0x01u) != 0) {
      break;
    }
  }
  return pcm;
}

std::vector<std::uint8_t> encode_wav(std::span<const std::int16_t> samples, std::uint32_t rate) {
  const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
  std::vector<std::uint8_t> out;
  out.reserve(44 + data_bytes);
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  write_u32(out, 36 + data_bytes);
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  write_u32(out, 16);
  write_u16(out, 1);
  write_u16(out, 1);
  write_u32(out, rate);
  write_u32(out, rate * 2);
  write_u16(out, 2);
  write_u16(out, 16);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  write_u32(out, data_bytes);
  for (const std::int16_t sample : samples) {
    write_u16(out, static_cast<std::uint16_t>(sample));
  }
  return out;
}

std::string wds_detail(const WdsBank& bank) {
  return "header=" + std::to_string(bank.header_bytes) + " bytes=" + std::to_string(bank.sample_bytes) +
         " off=" + std::to_string(bank.sample_offset) + " half30=" + std::to_string(bank.half_at_30) +
         " word40=" + std::to_string(bank.word_at_40) + " n=" + std::to_string(bank.samples.size()) +
         (bank.records ? " table=yes" : " table=no");
}

std::string seds_detail(const SedsInfo& info) {
  return "size=" + std::to_string(info.covered_bytes) + " ver=" + std::to_string(info.version) +
         " id=" + std::to_string(info.id);
}

}  // namespace psx_cd_loc
