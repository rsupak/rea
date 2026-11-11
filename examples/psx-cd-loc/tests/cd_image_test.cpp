#include "psx_cd_loc.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const char* expression, const char* file, int line) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "%s:%d: %s\n", file, line, expression);
    ++failures;
  }
}

#define CHECK(...) \
  check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

std::vector<std::uint8_t> sector_for(std::uint32_t lba,
                                     std::span<const std::uint8_t> user,
                                     std::uint8_t mode, bool good_sync,
                                     bool good_copy, bool override_msf,
                                     psx_cd_loc::BcdMsf msf) {
  std::vector<std::uint8_t> sector(psx_cd_loc::kRawSectorBytes, 0xDD);
  if (good_sync) {
    static constexpr std::uint8_t kSync[12] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    std::memcpy(sector.data(), kSync, 12);
  }
  const auto expected = psx_cd_loc::logical_sector_to_bcd_msf(lba);
  const psx_cd_loc::BcdMsf stored =
      override_msf ? msf : (expected.has_value() ? *expected : psx_cd_loc::BcdMsf{});
  sector[12] = stored.minute;
  sector[13] = stored.second;
  sector[14] = stored.sector;
  sector[15] = mode;
  sector[16] = 0x01;
  sector[17] = 0x02;
  sector[18] = 0x08;
  sector[19] = 0x00;
  if (good_copy) {
    std::memcpy(sector.data() + 20, sector.data() + 16, 4);
  }
  std::fill(sector.begin() + psx_cd_loc::kUserDataOffset,
            sector.begin() + psx_cd_loc::kUserDataOffset + psx_cd_loc::kUserBytesPerSector,
            0);
  const std::size_t count =
      user.size() > psx_cd_loc::kUserBytesPerSector ? psx_cd_loc::kUserBytesPerSector
                                                   : user.size();
  if (count != 0) {
    std::memcpy(sector.data() + psx_cd_loc::kUserDataOffset, user.data(), count);
  }
  return sector;
}

std::vector<std::uint8_t> image_from(std::uint32_t first_lba,
                                     const std::vector<std::vector<std::uint8_t>>& users) {
  std::vector<std::uint8_t> image;
  for (std::size_t i = 0; i < users.size(); ++i) {
    auto sector = sector_for(first_lba + static_cast<std::uint32_t>(i), users[i],
                             psx_cd_loc::kMode2, true, true, false, {});
    image.insert(image.end(), sector.begin(), sector.end());
  }
  return image;
}

void test_file_offset_does_not_add_the_lead_in() {
  psx_cd_loc::ImageError error = psx_cd_loc::ImageError::None;
  const auto zero = psx_cd_loc::mode2_user_range(0, 1, 0, error);
  CHECK(error == psx_cd_loc::ImageError::None);
  CHECK(zero.has_value());
  CHECK(zero->file_offset == 0);
  CHECK(zero->sector_count == 1);
  CHECK(zero->raw_bytes == psx_cd_loc::kRawSectorBytes);

  const auto later = psx_cd_loc::mode2_user_range(150, 2048, 0, error);
  CHECK(later.has_value());
  CHECK(later->file_offset == 150 * psx_cd_loc::kRawSectorBytes);
  CHECK(later->file_offset != 300 * psx_cd_loc::kRawSectorBytes);

  const auto extract = psx_cd_loc::mode2_user_range(150, 10, 150, error);
  CHECK(extract.has_value());
  CHECK(extract->file_offset == 0);
}

void test_lead_in_is_checked_against_the_sector_header() {
  const std::uint8_t user[] = {0x10, 0x20, 0x30, 0x40};
  auto image = image_from(0, {std::vector<std::uint8_t>(user, user + 4)});
  psx_cd_loc::ImageError error = psx_cd_loc::ImageError::None;
  const auto read = psx_cd_loc::read_mode2_user(image, 0, 4, 0, error);
  CHECK(error == psx_cd_loc::ImageError::None);
  CHECK(read.has_value());
  CHECK(read->user.size() == 4);
  CHECK(read->user[0] == 0x10 && read->user[3] == 0x40);
  CHECK(read->sectors.size() == 1);
  CHECK(read->sectors[0].absolute_msf == psx_cd_loc::BcdMsf{0x00, 0x02, 0x00});
  CHECK(read->sectors[0].header[0] == 0x00);
  CHECK(read->sectors[0].header[1] == 0x02);
  CHECK(read->sectors[0].header[2] == 0x00);
  CHECK(read->sectors[0].header[3] == psx_cd_loc::kMode2);

  auto wrong = sector_for(0, user, psx_cd_loc::kMode2, true, true, true,
                          psx_cd_loc::BcdMsf{0x00, 0x00, 0x00});
  const auto rejected = psx_cd_loc::read_mode2_user(wrong, 0, 4, 0, error);
  CHECK(!rejected.has_value());
  CHECK(error == psx_cd_loc::ImageError::MsfMismatch);
}

void test_multi_sector_read_keeps_only_the_tail_prefix() {
  std::vector<std::uint8_t> first(psx_cd_loc::kUserBytesPerSector, 0x11);
  std::vector<std::uint8_t> second(psx_cd_loc::kUserBytesPerSector, 0x22);
  auto image = image_from(5, {first, second});
  psx_cd_loc::ImageError error = psx_cd_loc::ImageError::None;
  const auto read = psx_cd_loc::read_mode2_user(image, 5, 2048 + 5, 5, error);
  CHECK(error == psx_cd_loc::ImageError::None);
  CHECK(read.has_value());
  CHECK(read->user.size() == 2053);
  CHECK(read->sectors.size() == 2);
  CHECK(read->sectors[0].user_bytes_taken == 2048);
  CHECK(read->sectors[1].user_bytes_taken == 5);
  CHECK(read->sectors[1].iso_lba == 6);
  CHECK(read->user[0] == 0x11);
  CHECK(read->user[2047] == 0x11);
  CHECK(read->user[2048] == 0x22);
  CHECK(read->user[2052] == 0x22);
  CHECK(read->sectors[0].submode == 0x08);
}

void test_exact_sector_does_not_pull_the_next_one() {
  std::vector<std::uint8_t> first(16, 0x41);
  std::vector<std::uint8_t> second(16, 0x42);
  auto image = image_from(1, {first, second});
  psx_cd_loc::ImageError error = psx_cd_loc::ImageError::None;
  const auto read =
      psx_cd_loc::read_mode2_user(image, 1, psx_cd_loc::kUserBytesPerSector, 1, error);
  CHECK(read.has_value());
  CHECK(read->sectors.size() == 1);
  CHECK(read->user.size() == psx_cd_loc::kUserBytesPerSector);
  CHECK(read->user[0] == 0x41);
  CHECK(read->user[16] == 0);
}

void test_layout_and_range_failures() {
  const std::uint8_t user[] = {1, 2, 3, 4};
  psx_cd_loc::ImageError error = psx_cd_loc::ImageError::None;

  auto bad_sync = sector_for(3, user, psx_cd_loc::kMode2, false, true, false, {});
  CHECK(!psx_cd_loc::read_mode2_user(bad_sync, 3, 4, 3, error));
  CHECK(error == psx_cd_loc::ImageError::BadSync);

  auto bad_mode = sector_for(3, user, 1, true, true, false, {});
  CHECK(!psx_cd_loc::read_mode2_user(bad_mode, 3, 4, 3, error));
  CHECK(error == psx_cd_loc::ImageError::UnsupportedMode);

  auto bad_copy = sector_for(3, user, psx_cd_loc::kMode2, true, false, false, {});
  CHECK(!psx_cd_loc::read_mode2_user(bad_copy, 3, 4, 3, error));
  CHECK(error == psx_cd_loc::ImageError::BadSubheader);

  auto image = image_from(3, {std::vector<std::uint8_t>(user, user + 4)});
  image.resize(image.size() - 1);
  CHECK(!psx_cd_loc::read_mode2_user(image, 3, 4, 3, error));
  CHECK(error == psx_cd_loc::ImageError::Truncated);

  CHECK(!psx_cd_loc::read_mode2_user(image, 2, 4, 3, error));
  CHECK(error == psx_cd_loc::ImageError::RangeOverflow);

  CHECK(!psx_cd_loc::read_mode2_user(image, 3, 0, 3, error));
  CHECK(error == psx_cd_loc::ImageError::EmptyLength);

  CHECK(!psx_cd_loc::mode2_user_range(0xFFFFFFFFU, 0xFFFFFFFFU, 0, error));
  CHECK(error == psx_cd_loc::ImageError::RangeOverflow);
}

void test_cd_load_reads_byte_length_not_the_rounded_size() {
  const std::uint8_t bytes[] = {0x02, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00};
  const auto record = psx_cd_loc::decode_index_record(bytes);
  CHECK(record.has_value());
  const auto load = psx_cd_loc::cd_load_from_record(*record);
  CHECK(load.has_value());
  CHECK(load->logical_sector == 2);
  CHECK(load->byte_length == 5);
  CHECK(load->aligned_length == 8);

  std::vector<std::uint8_t> user = {9, 8, 7, 6, 5, 4, 3, 2};
  auto image = image_from(2, {user});
  psx_cd_loc::ImageError error = psx_cd_loc::ImageError::None;
  const auto read = psx_cd_loc::read_cd_load(image, *load, 2, error);
  CHECK(error == psx_cd_loc::ImageError::None);
  CHECK(read.has_value());
  CHECK(read->user.size() == 5);
  CHECK(read->user[4] == 5);
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value);
  bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
  bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void test_resource_fields_follow_the_loaded_words() {
  std::vector<std::uint8_t> bytes(48, 0);
  put_u32(bytes, 16, 8);
  put_u32(bytes, 20, 3);
  put_u32(bytes, 24, 8);
  bytes[30] = 0x34;
  bytes[31] = 0x12;
  put_u32(bytes, 40, 9);
  bytes[0] = 0xAA;
  bytes[7] = 0xBB;
  psx_cd_loc::ResourceError error = psx_cd_loc::ResourceError::None;
  const auto fields = psx_cd_loc::resource_fields(bytes, error);
  CHECK(error == psx_cd_loc::ResourceError::None);
  CHECK(fields.has_value());
  CHECK(fields->prefix_bytes == 8);
  CHECK(fields->copied_bytes == 8);
  CHECK(fields->companion == 3);
  CHECK(fields->tail_offset == 8);
  CHECK(fields->half_at_30 == 0x1234);
  CHECK(fields->word_at_40 == 9);

  put_u32(bytes, 16, 6);
  const auto clipped = psx_cd_loc::resource_fields(bytes, error);
  CHECK(clipped.has_value());
  CHECK(clipped->prefix_bytes == 6);
  CHECK(clipped->copied_bytes == 4);

  CHECK(!psx_cd_loc::resource_fields(std::span<const std::uint8_t>(bytes.data(), 43), error));
  CHECK(error == psx_cd_loc::ResourceError::TooSmall);
  put_u32(bytes, 16, 49);
  CHECK(!psx_cd_loc::resource_fields(bytes, error));
  CHECK(error == psx_cd_loc::ResourceError::PrefixPastEnd);
  put_u32(bytes, 16, 8);
  put_u32(bytes, 24, 49);
  CHECK(!psx_cd_loc::resource_fields(bytes, error));
  CHECK(error == psx_cd_loc::ResourceError::TailPastEnd);
}

void test_cd_load_feeds_the_resource_fields() {
  std::vector<std::uint8_t> user(48, 0);
  put_u32(user, 16, 16);
  put_u32(user, 20, 4);
  put_u32(user, 24, 16);
  put_u32(user, 40, 7);
  const std::uint8_t record_bytes[] = {0x04, 0x00, 0x00, 48, 0x00, 0x00, 0x00};
  const auto record = psx_cd_loc::decode_index_record(record_bytes);
  const auto load = psx_cd_loc::cd_load_from_record(*record);
  CHECK(load.has_value());
  auto image = image_from(load->logical_sector, {user});
  psx_cd_loc::ImageError image_error = psx_cd_loc::ImageError::None;
  const auto read =
      psx_cd_loc::read_cd_load(image, *load, load->logical_sector, image_error);
  CHECK(image_error == psx_cd_loc::ImageError::None);
  CHECK(read.has_value());
  psx_cd_loc::ResourceError error = psx_cd_loc::ResourceError::None;
  const auto fields = psx_cd_loc::resource_fields(read->user, error);
  CHECK(fields.has_value());
  CHECK(fields->prefix_bytes == 16);
  CHECK(fields->companion == 4);
  CHECK(fields->tail_offset == 16);
  CHECK(fields->word_at_40 == 7);
}

std::vector<std::uint8_t> lz_source(std::uint32_t size, std::uint8_t flag,
                                     const std::vector<std::uint8_t>& body,
                                     std::uint8_t trailing_flag) {
  std::vector<std::uint8_t> source(4 + 1 + body.size() + 1);
  put_u32(source, 0, size);
  source[4] = flag;
  std::copy(body.begin(), body.end(), source.begin() + 5);
  source.back() = trailing_flag;
  return source;
}

void test_lz_literals_backrefs_and_overlap() {
  psx_cd_loc::LzError error = psx_cd_loc::LzError::None;
  const auto literals = lz_source(8, 0x00, {1, 2, 3, 4, 5, 6, 7, 8}, 0xFF);
  const auto plain = psx_cd_loc::lz_decompress(literals, error);
  CHECK(error == psx_cd_loc::LzError::None);
  CHECK(plain.has_value());
  CHECK(plain->bytes.size() == 8);
  CHECK(plain->bytes[0] == 1 && plain->bytes[7] == 8);
  CHECK(plain->source_used == literals.size());

  // Bit 0 literal, bits 1..7 overlapping distance-1 copies of length 3.
  std::vector<std::uint8_t> overlap_body{'a'};
  for (int i = 0; i < 7; ++i) {
    overlap_body.push_back(0x01);
    overlap_body.push_back(0x00);
  }
  const auto overlap = lz_source(22, 0xFE, overlap_body, 0);
  const auto run = psx_cd_loc::lz_decompress(overlap, error);
  CHECK(error == psx_cd_loc::LzError::None);
  CHECK(run.has_value());
  CHECK(run->bytes.size() == 22);
  CHECK(run->bytes == std::vector<std::uint8_t>(22, 'a'));

  // Two literal flags, then a flag whose top bit copies distance 16.
  std::vector<std::uint8_t> wide{0, 0, 0, 0, 0x00};
  for (int i = 0; i < 8; ++i) {
    const std::uint8_t value = i == 0 ? 0x5A : (i == 7 ? 0x22 : 0x11);
    wide.push_back(value);
  }
  wide.push_back(0x00);
  for (int i = 0; i < 8; ++i) {
    wide.push_back(0x11);
  }
  wide.push_back(0x80);
  for (int i = 0; i < 7; ++i) {
    wide.push_back(0x11);
  }
  wide.push_back(0x10);
  wide.push_back(0x00);
  wide.push_back(0x00);
  put_u32(wide, 0, 26);
  const auto far_out = psx_cd_loc::lz_decompress(wide, error);
  CHECK(error == psx_cd_loc::LzError::None);
  CHECK(far_out.has_value());
  CHECK(far_out->bytes.size() == 26);
  CHECK(far_out->bytes[0] == 0x5A);
  CHECK(far_out->bytes[7] == 0x22);
  CHECK(far_out->bytes[23] == 0x22);
  CHECK(far_out->bytes[24] == 0x11);
  CHECK(far_out->bytes[25] == 0x11);
}

void test_lz_rejects_malformed_input() {
  psx_cd_loc::LzError error = psx_cd_loc::LzError::None;
  const std::uint8_t short_src[] = {1, 0, 0};
  CHECK(!psx_cd_loc::lz_decompress(short_src, error));
  CHECK(error == psx_cd_loc::LzError::Truncated);

  const auto missing_flag = std::vector<std::uint8_t>{4, 0, 0, 0, 0x00, 1, 2, 3, 4};
  CHECK(!psx_cd_loc::lz_decompress(missing_flag, error));
  CHECK(error == psx_cd_loc::LzError::Truncated);

  const auto past_end = lz_source(1, 0x00, {1, 2, 3, 4, 5, 6, 7, 8}, 0);
  CHECK(!psx_cd_loc::lz_decompress(past_end, error));
  CHECK(error == psx_cd_loc::LzError::OutputOverflow);

  const auto zero_distance = lz_source(4, 0x02, {'a', 0x00, 0x00}, 0);
  CHECK(!psx_cd_loc::lz_decompress(zero_distance, error));
  CHECK(error == psx_cd_loc::LzError::BackrefRange);

  std::vector<std::uint8_t> huge(4);
  put_u32(huge, 0, psx_cd_loc::kLzMaxOutput + 1);
  CHECK(!psx_cd_loc::lz_decompress(huge, error));
  CHECK(error == psx_cd_loc::LzError::BadSize);

  const auto empty = std::vector<std::uint8_t>{0, 0, 0, 0, 0x00};
  const auto none = psx_cd_loc::lz_decompress(empty, error);
  CHECK(none.has_value());
  CHECK(none->bytes.empty());
  CHECK(none->source_used == 5);
}

void test_sha256_known_answers() {
  CHECK(psx_cd_loc::sha256_hex({}) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const std::uint8_t abc[] = {'a', 'b', 'c'};
  CHECK(psx_cd_loc::sha256_hex(abc) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

}  // namespace

int main() {
  test_file_offset_does_not_add_the_lead_in();
  test_lead_in_is_checked_against_the_sector_header();
  test_multi_sector_read_keeps_only_the_tail_prefix();
  test_exact_sector_does_not_pull_the_next_one();
  test_layout_and_range_failures();
  test_cd_load_reads_byte_length_not_the_rounded_size();
  test_resource_fields_follow_the_loaded_words();
  test_cd_load_feeds_the_resource_fields();
  test_lz_literals_backrefs_and_overlap();
  test_lz_rejects_malformed_input();
  test_sha256_known_answers();
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("%d checks passed\n", checks);
  return 0;
}
