#include "psx_cd_loc.hpp"
#include "psx_disc.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
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

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value);
  bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
  bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value);
  bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

std::vector<std::uint8_t> directory_record(std::uint32_t lba, std::uint32_t length, bool directory,
                                           const std::string& name) {
  const std::uint8_t name_length = static_cast<std::uint8_t>(name.size());
  std::uint8_t record_length = static_cast<std::uint8_t>(33 + name_length);
  if ((record_length & 1u) != 0) {
    ++record_length;
  }
  std::vector<std::uint8_t> record(record_length, 0);
  record[0] = record_length;
  put_u32(record, 2, lba);
  put_u32(record, 10, length);
  record[25] = directory ? 0x02 : 0x00;
  record[32] = name_length;
  for (std::size_t i = 0; i < name.size(); ++i) {
    record[33 + i] = static_cast<std::uint8_t>(name[i]);
  }
  return record;
}

void test_iso_lists_one_file() {
  std::map<std::uint32_t, std::vector<std::uint8_t>> sectors;
  sectors[16] = std::vector<std::uint8_t>(2048, 0);
  sectors[16][0] = 1;
  sectors[16][1] = 'C';
  sectors[16][2] = 'D';
  sectors[16][3] = '0';
  sectors[16][4] = '0';
  sectors[16][5] = '1';
  std::fill(sectors[16].begin() + 40, sectors[16].begin() + 72, static_cast<std::uint8_t>(' '));
  const std::string volume = "DEMO";
  for (std::size_t i = 0; i < volume.size(); ++i) {
    sectors[16][40 + i] = static_cast<std::uint8_t>(volume[i]);
  }
  const auto root = directory_record(20, 2048, true, std::string(1, '\0'));
  std::copy(root.begin(), root.end(), sectors[16].begin() + 156);

  sectors[20] = std::vector<std::uint8_t>(2048, 0);
  const auto dot = directory_record(20, 2048, true, std::string(1, '\0'));
  const auto dotdot = directory_record(20, 2048, true, std::string(1, '\1'));
  const auto file = directory_record(21, 5, false, "HELLO.TXT;1");
  std::copy(dot.begin(), dot.end(), sectors[20].begin());
  std::copy(dotdot.begin(), dotdot.end(), sectors[20].begin() + dot.size());
  std::copy(file.begin(), file.end(), sectors[20].begin() + dot.size() + dotdot.size());
  sectors[21] = std::vector<std::uint8_t>(2048, 0);
  sectors[21][0] = 'H';

  psx_cd_loc::IsoError error = psx_cd_loc::IsoError::None;
  const auto listed = psx_cd_loc::read_iso9660(
      [&](std::uint32_t lba, std::vector<std::uint8_t>& user, std::string&) {
        const auto found = sectors.find(lba);
        if (found == sectors.end()) {
          return false;
        }
        user = found->second;
        return true;
      },
      error);
  CHECK(error == psx_cd_loc::IsoError::None);
  CHECK(listed.has_value());
  CHECK(listed->volume_id == "DEMO");
  CHECK(listed->entries.size() == 1);
  CHECK(listed->entries[0].path == "HELLO.TXT");
  CHECK(listed->entries[0].lba == 21);
  CHECK(listed->entries[0].length == 5);
  CHECK(listed->entries[0].directory == false);
}

void test_index_ids_follow_halfword_base() {
  std::vector<std::uint8_t> records(30 * 7, 0);
  auto put = [&](int index, std::uint32_t lba, std::uint32_t length) {
    const std::size_t at = static_cast<std::size_t>(index) * 7;
    records[at] = static_cast<std::uint8_t>(lba);
    records[at + 1] = static_cast<std::uint8_t>(lba >> 8);
    records[at + 2] = static_cast<std::uint8_t>(lba >> 16);
    put_u32(records, at + 3, length);
  };
  put(22, 108606, 100);
  put(23, 108754, 0xFFFFFFFA);
  put(24, 108754, 50);
  put(26, 109000, 8);
  put(27, 1, 1);
  const auto files = psx_cd_loc::parse_indexed_files(records, 24);
  CHECK(files.size() == 4);
  CHECK(files[0].id == 0 && files[0].lba == 108606 && files[0].length == 100 && !files[0].group);
  CHECK(files[1].group && files[1].id == 1 && files[1].length == -6);
  CHECK(files[2].id == 2 && files[2].length == 50);
  CHECK(files[3].id == 4 && files[3].lba == 109000);
}

void test_sector_round_up_supplies_the_trailing_flag() {
  CHECK(psx_cd_loc::user_bytes_covering_sectors(14360) == 16384);
  CHECK(psx_cd_loc::user_bytes_covering_sectors(2048) == 2048);
  CHECK(psx_cd_loc::user_bytes_covering_sectors(0) == 0);
  std::vector<std::uint8_t> sector(psx_cd_loc::kRawSectorBytes, 0);
  static constexpr std::uint8_t kSync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
  std::memcpy(sector.data(), kSync, 12);
  const auto msf = psx_cd_loc::logical_sector_to_bcd_msf(0);
  CHECK(msf.has_value());
  sector[12] = msf->minute;
  sector[13] = msf->second;
  sector[14] = msf->sector;
  sector[15] = psx_cd_loc::kMode2;
  sector[16] = sector[20] = 1;
  sector[17] = sector[21] = 2;
  sector[18] = sector[22] = 8;
  const std::uint8_t stream[] = {8, 0, 0, 0, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 0x00};
  std::memcpy(sector.data() + 24, stream, 13);
  psx_cd_loc::ImageError image_error = psx_cd_loc::ImageError::None;
  const auto exact = psx_cd_loc::read_mode2_user(sector, 0, 13, 0, image_error);
  CHECK(exact.has_value());
  psx_cd_loc::LzError lz_error = psx_cd_loc::LzError::None;
  CHECK(!psx_cd_loc::lz_decompress(exact->user, lz_error));
  CHECK(lz_error == psx_cd_loc::LzError::Truncated);
  const auto rounded = psx_cd_loc::read_mode2_user(
      sector, 0, psx_cd_loc::user_bytes_covering_sectors(13), 0, image_error);
  CHECK(rounded.has_value());
  CHECK(rounded->user.size() == 2048);
  const auto decoded = psx_cd_loc::lz_decompress(rounded->user, lz_error);
  CHECK(lz_error == psx_cd_loc::LzError::None);
  CHECK(decoded.has_value());
  CHECK(decoded->bytes.size() == 8);
  CHECK(decoded->source_used == 14);
}

void test_cue_selects_mode2_file() {
  std::string error;
  const auto name = psx_cd_loc::cue_mode2_file(
      "FILE \"game.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n", error);
  CHECK(name.has_value());
  CHECK(*name == "game.bin");
  CHECK(!psx_cd_loc::cue_mode2_file("FILE \"a.bin\" BINARY\n  TRACK 01 MODE1/2352\n", error));
}

void test_tim_16bit_becomes_ppm() {
  std::vector<std::uint8_t> tim(8 + 12 + 4, 0);
  put_u32(tim, 0, 0x10);
  put_u32(tim, 4, 2);
  put_u32(tim, 8, 12 + 4);
  put_u16(tim, 16, 2);
  put_u16(tim, 18, 1);
  put_u16(tim, 20, 0x001F);
  put_u16(tim, 22, 0x03E0);
  const auto picture = psx_cd_loc::decode_tim(tim);
  CHECK(picture.has_value());
  CHECK(picture->width == 2);
  CHECK(picture->height == 1);
  CHECK(picture->rgb[0] == 0xF8);
  CHECK(picture->rgb[1] == 0);
  CHECK(picture->rgb[3] == 0);
  CHECK(picture->rgb[4] == 0xF8);
  const auto ppm = psx_cd_loc::encode_ppm(*picture);
  CHECK(ppm.size() > 10);
  CHECK(ppm[0] == 'P' && ppm[1] == '6');
  const std::vector<std::uint8_t> text{0x08, 0, 0, 0};
  CHECK(!psx_cd_loc::decode_tim(text));
}

}  // namespace

int main() {
  test_iso_lists_one_file();
  test_index_ids_follow_halfword_base();
  test_sector_round_up_supplies_the_trailing_flag();
  test_cue_selects_mode2_file();
  test_tim_16bit_becomes_ppm();
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("%d checks passed\n", checks);
  return 0;
}
