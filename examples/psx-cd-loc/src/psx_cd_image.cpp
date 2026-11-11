#include "psx_cd_loc.hpp"

#include <algorithm>
#include <cstddef>

namespace psx_cd_loc {
namespace {

constexpr std::uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) {
  return (value >> bits) | (value << (32 - bits));
}

bool multiply_fits(std::uint64_t left, std::uint64_t right, std::uint64_t& out) {
  if (left != 0 && right > ~std::uint64_t{0} / left) {
    return false;
  }
  out = left * right;
  return true;
}

}  // namespace

const char* image_error_text(ImageError error) {
  switch (error) {
    case ImageError::None:
      return "ok";
    case ImageError::EmptyLength:
      return "length is zero";
    case ImageError::RangeOverflow:
      return "sector range overflows the file index";
    case ImageError::Truncated:
      return "image does not contain the full sector range";
    case ImageError::BadSync:
      return "sector sync is not the 12-byte Mode 2 sync";
    case ImageError::UnsupportedMode:
      return "sector mode is not 2";
    case ImageError::BadSubheader:
      return "subheader copy does not match";
    case ImageError::MsfMismatch:
      return "sector minute/second/frame does not match the logical sector";
  }
  return "unknown image error";
}

std::optional<Mode2Range> mode2_user_range(std::uint32_t iso_lba,
                                           std::uint32_t user_bytes,
                                           std::uint32_t base_lba,
                                           ImageError& error) {
  if (user_bytes == 0) {
    error = ImageError::EmptyLength;
    return std::nullopt;
  }
  if (iso_lba < base_lba) {
    error = ImageError::RangeOverflow;
    return std::nullopt;
  }
  const std::uint64_t index =
      static_cast<std::uint64_t>(iso_lba) - static_cast<std::uint64_t>(base_lba);
  const std::uint64_t sector_count =
      (static_cast<std::uint64_t>(user_bytes) + kUserBytesPerSector - 1) /
      kUserBytesPerSector;
  if (sector_count > 0xFFFFFFFFULL) {
    error = ImageError::RangeOverflow;
    return std::nullopt;
  }
  const std::uint64_t last_lba =
      static_cast<std::uint64_t>(iso_lba) + sector_count - 1;
  if (last_lba > 0xFFFFFFFFULL ||
      !logical_sector_to_bcd_msf(static_cast<std::uint32_t>(last_lba)).has_value()) {
    error = ImageError::RangeOverflow;
    return std::nullopt;
  }
  std::uint64_t file_offset = 0;
  std::uint64_t raw_bytes = 0;
  if (!multiply_fits(index, kRawSectorBytes, file_offset) ||
      !multiply_fits(sector_count, kRawSectorBytes, raw_bytes) ||
      file_offset > ~std::uint64_t{0} - raw_bytes) {
    error = ImageError::RangeOverflow;
    return std::nullopt;
  }
  Mode2Range range;
  range.file_offset = file_offset;
  range.raw_bytes = raw_bytes;
  range.sector_count = static_cast<std::uint32_t>(sector_count);
  error = ImageError::None;
  return range;
}

std::optional<Mode2Read> read_mode2_user(std::span<const std::uint8_t> image,
                                         std::uint32_t iso_lba,
                                         std::uint32_t user_bytes,
                                         std::uint32_t base_lba,
                                         ImageError& error) {
  const auto range = mode2_user_range(iso_lba, user_bytes, base_lba, error);
  if (!range.has_value()) {
    return std::nullopt;
  }
  if (range->file_offset > image.size() ||
      range->raw_bytes > image.size() - range->file_offset) {
    error = ImageError::Truncated;
    return std::nullopt;
  }

  Mode2Read result;
  result.user.reserve(user_bytes);
  result.sectors.reserve(range->sector_count);
  std::uint32_t remaining = user_bytes;
  for (std::uint32_t sector_index = 0; sector_index < range->sector_count;
       ++sector_index) {
    const std::uint64_t at =
        range->file_offset + static_cast<std::uint64_t>(sector_index) * kRawSectorBytes;
    const auto sector = image.subspan(static_cast<std::size_t>(at),
                                      static_cast<std::size_t>(kRawSectorBytes));
    if (!std::equal(std::begin(kSync), std::end(kSync), sector.begin())) {
      error = ImageError::BadSync;
      return std::nullopt;
    }
    const std::uint32_t logical = iso_lba + sector_index;
    const auto expected = logical_sector_to_bcd_msf(logical);
    if (!expected.has_value()) {
      error = ImageError::RangeOverflow;
      return std::nullopt;
    }
    const BcdMsf stored{sector[12], sector[13], sector[14]};
    if (!(stored == *expected)) {
      error = ImageError::MsfMismatch;
      return std::nullopt;
    }
    if (sector[15] != kMode2) {
      error = ImageError::UnsupportedMode;
      return std::nullopt;
    }
    if (!std::equal(sector.begin() + 16, sector.begin() + 20, sector.begin() + 20)) {
      error = ImageError::BadSubheader;
      return std::nullopt;
    }
    const std::uint32_t take =
        remaining > kUserBytesPerSector ? kUserBytesPerSector : remaining;
    result.user.insert(result.user.end(), sector.begin() + kUserDataOffset,
                       sector.begin() + kUserDataOffset + take);
    Mode2Sector view;
    view.iso_lba = logical;
    view.absolute_msf = *expected;
    view.mode = sector[15];
    view.submode = sector[18];
    std::copy_n(sector.begin() + 12, kSectorHeaderBytes, view.header.begin());
    view.user_bytes_taken = take;
    result.sectors.push_back(view);
    remaining -= take;
  }
  error = ImageError::None;
  return result;
}

std::optional<Mode2Read> read_cd_load(std::span<const std::uint8_t> image,
                                      const CdLoad& load,
                                      std::uint32_t base_lba,
                                      ImageError& error) {
  return read_mode2_user(image, load.logical_sector, load.byte_length, base_lba,
                         error);
}

const char* resource_error_text(ResourceError error) {
  switch (error) {
    case ResourceError::None:
      return "ok";
    case ResourceError::TooSmall:
      return "resource is shorter than the loaded header";
    case ResourceError::PrefixPastEnd:
      return "prefix length passes the end of the resource";
    case ResourceError::TailPastEnd:
      return "tail offset passes the end of the resource";
  }
  return "unknown resource error";
}

static std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::optional<ResourceFields> resource_fields(std::span<const std::uint8_t> bytes,
                                             ResourceError& error) {
  constexpr std::size_t kHeaderBytes = 44;
  if (bytes.size() < kHeaderBytes) {
    error = ResourceError::TooSmall;
    return std::nullopt;
  }
  ResourceFields fields;
  fields.prefix_bytes = read_u32(bytes, 16);
  fields.copied_bytes = fields.prefix_bytes & ~std::uint32_t{3};
  fields.companion = read_u32(bytes, 20);
  fields.tail_offset = read_u32(bytes, 24);
  fields.half_at_30 = static_cast<std::uint16_t>(bytes[30] |
                                                 (static_cast<std::uint16_t>(bytes[31]) << 8));
  fields.word_at_40 = read_u32(bytes, 40);
  if (static_cast<std::uint64_t>(fields.prefix_bytes) > bytes.size() ||
      static_cast<std::uint64_t>(fields.copied_bytes) > bytes.size()) {
    error = ResourceError::PrefixPastEnd;
    return std::nullopt;
  }
  if (static_cast<std::uint64_t>(fields.tail_offset) > bytes.size()) {
    error = ResourceError::TailPastEnd;
    return std::nullopt;
  }
  error = ResourceError::None;
  return fields;
}

std::string sha256_hex(std::span<const std::uint8_t> data) {
  constexpr std::uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::uint32_t hash[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::uint8_t block[64] = {};
  std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8;
  std::size_t offset = 0;
  auto compress = [&](const std::uint8_t* chunk) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (std::uint32_t{chunk[i * 4]} << 24) |
             (std::uint32_t{chunk[i * 4 + 1]} << 16) |
             (std::uint32_t{chunk[i * 4 + 2]} << 8) |
             std::uint32_t{chunk[i * 4 + 3]};
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 =
          rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 =
          rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3];
    std::uint32_t e = hash[4], f = hash[5], g = hash[6], h = hash[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t temp1 = h + S1 + ch + k[i] + w[i];
      const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = S0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  };

  while (offset + 64 <= data.size()) {
    compress(data.data() + offset);
    offset += 64;
  }
  const std::size_t tail = data.size() - offset;
  for (std::size_t i = 0; i < tail; ++i) {
    block[i] = data[offset + i];
  }
  block[tail] = 0x80;
  if (tail >= 56) {
    compress(block);
    std::fill(std::begin(block), std::end(block), 0);
  }
  for (int i = 0; i < 8; ++i) {
    block[63 - i] = static_cast<std::uint8_t>(bit_length >> (8 * i));
  }
  compress(block);

  static constexpr char kHex[] = "0123456789abcdef";
  std::string text(64, '0');
  for (int i = 0; i < 8; ++i) {
    for (int shift = 28; shift >= 0; shift -= 4) {
      text[static_cast<std::size_t>(i * 8 + (28 - shift) / 4)] =
          kHex[(hash[i] >> shift) & 0xF];
    }
  }
  return text;
}

}  // namespace psx_cd_loc
