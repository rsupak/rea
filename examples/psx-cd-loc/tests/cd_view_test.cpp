#include "psx_audio.hpp"
#include "psx_catalog.hpp"
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

void test_tim_4bpp_low_nibble_and_first_clut_row() {
  std::vector<std::uint8_t> tim(8 + 12 + 16 * 2 * 2 + 12 + 2, 0);
  put_u32(tim, 0, 0x10);
  put_u32(tim, 4, 8);
  put_u32(tim, 8, 12 + 64);
  put_u16(tim, 16, 16);
  put_u16(tim, 18, 2);
  put_u16(tim, 20 + 2, 0x001F);
  put_u16(tim, 20 + 4, 0x7C00);
  put_u16(tim, 20 + 34, 0x03E0);
  const std::size_t pixels = 8 + 12 + 64;
  put_u32(tim, pixels, 14);
  put_u16(tim, pixels + 8, 1);
  put_u16(tim, pixels + 10, 1);
  tim[pixels + 12] = 0x21;
  const auto picture = psx_cd_loc::decode_tim(tim);
  CHECK(picture.has_value());
  CHECK(picture->width == 4);
  CHECK(picture->height == 1);
  CHECK(picture->rgb[0] == 0xF8);
  CHECK(picture->rgb[1] == 0);
  CHECK(picture->rgb[3] == 0);
  CHECK(picture->rgb[5] == 0xF8);
}

void test_tim_8bpp_width_is_twice_the_halfword_field() {
  std::vector<std::uint8_t> tim(8 + 12 + 256 * 2 + 12 + 4, 0);
  put_u32(tim, 0, 0x10);
  put_u32(tim, 4, 1 | 8);
  put_u32(tim, 8, 12 + 512);
  put_u16(tim, 16, 256);
  put_u16(tim, 18, 1);
  put_u16(tim, 20 + 2, 0x001F);
  put_u16(tim, 20 + 4, 0x03E0);
  const std::size_t pixels = 8 + 12 + 512;
  put_u32(tim, pixels, 12 + 4);
  put_u16(tim, pixels + 8, 1);
  put_u16(tim, pixels + 10, 2);
  tim[pixels + 12] = 1;
  tim[pixels + 13] = 2;
  tim[pixels + 14] = 2;
  tim[pixels + 15] = 1;
  const auto picture = psx_cd_loc::decode_tim(tim);
  CHECK(picture.has_value());
  CHECK(picture->width == 2);
  CHECK(picture->height == 2);
  CHECK(picture->rgb[0] == 0xF8);
  CHECK(picture->rgb[4] == 0xF8);
  CHECK(picture->rgb[7] == 0xF8);
  CHECK(picture->rgb[9] == 0xF8);
}

std::vector<std::uint8_t> literal_lz(std::uint32_t output_size, const std::vector<std::uint8_t>& literals) {
  std::vector<std::uint8_t> source(4);
  put_u32(source, 0, output_size);
  for (std::size_t i = 0; i < literals.size(); i += 8) {
    source.push_back(0x00);
    for (std::size_t bit = 0; bit < 8 && i + bit < literals.size(); ++bit) {
      source.push_back(literals[i + bit]);
    }
  }
  source.push_back(0x00);
  return source;
}

std::vector<std::uint8_t> tiny_16bpp_tim() {
  std::vector<std::uint8_t> tim(24, 0);
  put_u32(tim, 0, 0x10);
  put_u32(tim, 4, 2);
  put_u32(tim, 8, 16);
  put_u16(tim, 16, 2);
  put_u16(tim, 18, 1);
  put_u16(tim, 20, 0x001F);
  put_u16(tim, 22, 0x03E0);
  return tim;
}

void test_classification_of_lz_offset_table_and_signatures() {
  const std::vector<std::uint8_t> table_bytes = {2, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0};
  const auto wrapped = literal_lz(16, table_bytes);
  const auto table = psx_cd_loc::classify_file_bytes(wrapped, static_cast<std::uint32_t>(wrapped.size()));
  CHECK(table.kind == psx_cd_loc::FileKind::OffsetTable);
  CHECK(table.detail.find("count=2") != std::string::npos);

  const std::vector<std::uint8_t> not_table = {8, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0x00};
  const auto plain = literal_lz(8, not_table);
  const auto lz = psx_cd_loc::classify_file_bytes(plain, static_cast<std::uint32_t>(plain.size()));
  CHECK(lz.kind == psx_cd_loc::FileKind::Lz);

  std::vector<std::uint8_t> nested(8);
  put_u32(nested, 0, 1);
  put_u32(nested, 4, 8);
  const auto picture = tiny_16bpp_tim();
  nested.insert(nested.end(), picture.begin(), picture.end());
  const auto nested_lz = literal_lz(static_cast<std::uint32_t>(nested.size()), nested);
  const auto found = psx_cd_loc::classify_file_bytes(nested_lz, static_cast<std::uint32_t>(nested_lz.size()));
  CHECK(found.kind == psx_cd_loc::FileKind::OffsetTable);
  CHECK(found.pictures.size() == 1);
  CHECK(found.pictures[0].label == "offset:0");
  CHECK(found.pictures[0].image.width == 2);

  const auto raw_tim = psx_cd_loc::classify_file_bytes(picture, static_cast<std::uint32_t>(picture.size()));
  CHECK(raw_tim.kind == psx_cd_loc::FileKind::Tim);
  CHECK(raw_tim.pictures.size() == 1);

  const std::vector<std::uint8_t> vag = {'V', 'A', 'G', 'p', 0, 0, 0, 0};
  CHECK(psx_cd_loc::classify_file_bytes(vag, 8).kind == psx_cd_loc::FileKind::Vag);
  const std::vector<std::uint8_t> vab = {'p', 'B', 'A', 'V', 0, 0, 0, 0};
  CHECK(psx_cd_loc::classify_file_bytes(vab, 8).kind == psx_cd_loc::FileKind::Vab);
  const std::vector<std::uint8_t> seq = {'p', 'Q', 'E', 'S', 0, 0, 0, 0};
  CHECK(psx_cd_loc::classify_file_bytes(seq, 8).kind == psx_cd_loc::FileKind::Seq);
  std::vector<std::uint8_t> str(4);
  put_u32(str, 0, 0x80010160u);
  CHECK(psx_cd_loc::classify_file_bytes(str, 4).kind == psx_cd_loc::FileKind::Str);
  const std::vector<std::uint8_t> exe = {'P', 'S', '-', 'X', ' ', 'E', 'X', 'E'};
  CHECK(psx_cd_loc::classify_file_bytes(exe, 8).kind == psx_cd_loc::FileKind::PsxExe);
  std::vector<std::uint8_t> text(32, 'A');
  CHECK(psx_cd_loc::classify_file_bytes(text, 32).kind == psx_cd_loc::FileKind::Text);

  const std::uint32_t addiu_sp = (9u << 26) | (29u << 21) | (29u << 16) | 0xFFF0u;
  const std::uint32_t sw_ra = (43u << 26) | (29u << 21) | (31u << 16) | 12u;
  std::vector<std::uint8_t> mips(8);
  put_u32(mips, 0, addiu_sp);
  put_u32(mips, 4, sw_ra);
  CHECK(psx_cd_loc::classify_file_bytes(mips, 8).kind == psx_cd_loc::FileKind::Mips);

  std::vector<std::uint8_t> tagged(16, 0);
  put_u32(tagged, 0, 1);
  put_u32(tagged, 8, 0x1100u);
  const auto tagged_kind = psx_cd_loc::classify_file_bytes(tagged, 16);
  CHECK(tagged_kind.kind == psx_cd_loc::FileKind::OffsetTable);
  CHECK(tagged_kind.detail.find("tag=0x1100") != std::string::npos);

  std::vector<std::uint8_t> wide(1204, 0);
  put_u32(wide, 0, 300);
  CHECK(psx_cd_loc::classify_file_bytes(wide, 1204).kind == psx_cd_loc::FileKind::OffsetTable);

  std::vector<std::uint8_t> buried(56, 0);
  put_u32(buried, 16, 2);
  put_u32(buried, 20, 12);
  put_u32(buried, 24, 12);
  put_u32(buried, 28, 0x10);
  put_u32(buried, 32, 2);
  put_u32(buried, 36, 20);
  put_u16(buried, 44, 2);
  put_u16(buried, 46, 2);
  const auto buried_lz = literal_lz(56, buried);
  const auto buried_kind =
      psx_cd_loc::classify_file_bytes(buried_lz, static_cast<std::uint32_t>(buried_lz.size()));
  CHECK(buried_kind.kind == psx_cd_loc::FileKind::OffsetTable);
  CHECK(buried_kind.detail.find("at=16") != std::string::npos);
  CHECK(buried_kind.pictures.size() == 2);
  CHECK(buried_kind.pictures[0].image.width == 2);
  CHECK(buried_kind.pictures[0].image.height == 2);

  std::vector<std::uint8_t> opaque(32, 0x11);
  const auto data = psx_cd_loc::classify_file_bytes(opaque, 32);
  CHECK(data.kind == psx_cd_loc::FileKind::Data);
  CHECK(data.detail.find("w0=11111111") != std::string::npos);
  CHECK(lz.detail.find("w0=00000008") != std::string::npos);

  std::vector<std::uint8_t> block(16, 0);
  block[0] = 0x10;
  block[2] = 0x01;
  const auto pcm = psx_cd_loc::decode_psx_adpcm(block);
  CHECK(pcm.size() == 28);
  CHECK(pcm[0] == 4096);
  CHECK(pcm[1] == 3840);
  CHECK(pcm[2] == 3600);
  std::vector<std::uint8_t> ended = block;
  ended[1] = 0x01;
  ended.insert(ended.end(), block.begin(), block.end());
  CHECK(psx_cd_loc::decode_psx_adpcm(ended).size() == 28);

  std::vector<std::uint8_t> wds(96, 0);
  wds[0] = 'w';
  wds[1] = 'd';
  wds[2] = 's';
  wds[3] = ' ';
  put_u32(wds, 16, 80);
  put_u32(wds, 20, 16);
  put_u32(wds, 24, 80);
  put_u32(wds, 28, 1);
  put_u32(wds, 40, 0x1010);
  std::copy(block.begin(), block.end(), wds.begin() + 80);
  const auto bank = psx_cd_loc::parse_wds(wds);
  CHECK(bank.has_value());
  CHECK(bank->records);
  CHECK(bank->samples.size() == 1);
  CHECK(bank->samples[0].byte_size == 16);
  CHECK(bank->half_at_30 == 0);
  CHECK(bank->word_at_40 == 0x1010);
  const auto decoded = psx_cd_loc::decode_psx_adpcm(
      std::span<const std::uint8_t>(wds).subspan(80, 16));
  CHECK(decoded[0] == 4096);
  const auto wav = psx_cd_loc::encode_wav(decoded, psx_cd_loc::kSpuNominalRate);
  CHECK(wav.size() > 44);
  CHECK(wav[0] == 'R' && wav[1] == 'I' && wav[2] == 'F' && wav[3] == 'F');
  CHECK(wav[24] == 0x44 && wav[25] == 0xAC);
  CHECK(psx_cd_loc::classify_file_bytes(wds, static_cast<std::uint32_t>(wds.size())).kind ==
        psx_cd_loc::FileKind::Wds);

  std::vector<std::uint8_t> seds(24, 0);
  seds[0] = 's';
  seds[1] = 'e';
  seds[2] = 'd';
  seds[3] = 's';
  put_u32(seds, 8, 24);
  put_u16(seds, 12, 0x0101);
  put_u16(seds, 20, 9);
  const auto effect = psx_cd_loc::parse_seds(seds);
  CHECK(effect.has_value());
  CHECK(effect->covered_bytes == 24);
  CHECK(effect->version == 0x0101);
  CHECK(effect->id == 9);
  CHECK(psx_cd_loc::classify_file_bytes(seds, 24).kind == psx_cd_loc::FileKind::Seds);

  std::vector<std::uint8_t> smds = {'s', 'm', 'd', 's', 1, 0, 0, 0};
  CHECK(psx_cd_loc::classify_file_bytes(smds, 8).kind == psx_cd_loc::FileKind::Smds);

  std::vector<std::uint8_t> overlay(48, 0);
  put_u32(overlay, 0, 2);
  put_u32(overlay, 4, 0x80070000u);
  put_u32(overlay, 8, 0x80070010u);
  put_u32(overlay, 12, 0x27BDFFE0u);
  put_u32(overlay, 16, 0xAFBF001Cu);
  put_u32(overlay, 20, 0xAFB00018u);
  put_u32(overlay, 24, 0x00808021u);
  put_u32(overlay, 28, 0x8FBF001Cu);
  put_u32(overlay, 32, 0x03E00008u);
  put_u32(overlay, 40, 0x24020001u);
  const auto overlay_lz = literal_lz(48, overlay);
  const auto overlay_kind =
      psx_cd_loc::classify_file_bytes(overlay_lz, static_cast<std::uint32_t>(overlay_lz.size()));
  CHECK(overlay_kind.kind == psx_cd_loc::FileKind::Overlay);
  CHECK(overlay_kind.detail.find("entries=2") != std::string::npos);
  CHECK(overlay_kind.detail.find("low=0x80070000") != std::string::npos);
  CHECK(overlay_kind.detail.find("load=0x8006fff4") != std::string::npos);
  CHECK(!psx_cd_loc::open_indexed_disc("/no/such/image.bin").error.empty());
  CHECK(!psx_cd_loc::preview_indexed_file("/no/such/image.bin", 0, 16).error.empty());
}

}  // namespace

int main() {
  test_iso_lists_one_file();
  test_index_ids_follow_halfword_base();
  test_sector_round_up_supplies_the_trailing_flag();
  test_cue_selects_mode2_file();
  test_tim_16bit_becomes_ppm();
  test_tim_4bpp_low_nibble_and_first_clut_row();
  test_tim_8bpp_width_is_twice_the_halfword_field();
  test_classification_of_lz_offset_table_and_signatures();
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("%d checks passed\n", checks);
  return 0;
}
