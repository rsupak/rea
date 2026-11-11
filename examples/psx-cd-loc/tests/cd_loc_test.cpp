#include "psx_cd_loc.hpp"

#include <cstdio>
#include <cstdint>
#include <span>

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

void test_lead_in_and_frame_carry() {
  // 0 logical frames is exactly two seconds into the lead-in: 00:02:00.
  const auto zero = psx_cd_loc::logical_sector_to_bcd_msf(0);
  CHECK(zero.has_value());
  CHECK(*zero == psx_cd_loc::BcdMsf{0x00, 0x02, 0x00});

  const auto one = psx_cd_loc::logical_sector_to_bcd_msf(1);
  CHECK(one.has_value());
  CHECK(*one == psx_cd_loc::BcdMsf{0x00, 0x02, 0x01});

  // 74 is the last frame of that second. 75 carries into the next second.
  const auto last = psx_cd_loc::logical_sector_to_bcd_msf(74);
  CHECK(last.has_value());
  CHECK(*last == psx_cd_loc::BcdMsf{0x00, 0x02, 0x74});

  const auto carry = psx_cd_loc::logical_sector_to_bcd_msf(75);
  CHECK(carry.has_value());
  CHECK(*carry == psx_cd_loc::BcdMsf{0x00, 0x03, 0x00});
}

void test_minute_and_ten_second_nibbles() {
  // 150 logical frames is four absolute seconds: 00:04:00.
  const auto four = psx_cd_loc::logical_sector_to_bcd_msf(150);
  CHECK(four.has_value());
  CHECK(*four == psx_cd_loc::BcdMsf{0x00, 0x04, 0x00});

  // 600 logical frames is ten absolute seconds. The tens digit is the high nibble.
  const auto ten = psx_cd_loc::logical_sector_to_bcd_msf(600);
  CHECK(ten.has_value());
  CHECK(*ten == psx_cd_loc::BcdMsf{0x00, 0x10, 0x00});

  // 4500 + 150 = 4650 frames = 62 seconds = 01:02:00.
  const auto minute = psx_cd_loc::logical_sector_to_bcd_msf(4500);
  CHECK(minute.has_value());
  CHECK(*minute == psx_cd_loc::BcdMsf{0x01, 0x02, 0x00});

  // 44850 logical frames is exactly ten absolute minutes.
  const auto ten_minutes = psx_cd_loc::logical_sector_to_bcd_msf(44850);
  CHECK(ten_minutes.has_value());
  CHECK(*ten_minutes == psx_cd_loc::BcdMsf{0x10, 0x00, 0x00});
}

void test_sector_domain_stops_at_signed_add() {
  const auto too_large =
      psx_cd_loc::logical_sector_to_bcd_msf(0x7FFFFFFF - 149);
  CHECK(!too_large.has_value());
  const auto last_accepted =
      psx_cd_loc::logical_sector_to_bcd_msf(0x7FFFFFFF - 150);
  CHECK(last_accepted.has_value());
}

void test_index_record_is_seven_little_endian_bytes() {
  const std::uint8_t bytes[] = {0x01, 0x02, 0x03, 0x05, 0x00, 0x00, 0x00};
  const auto record = psx_cd_loc::decode_index_record(bytes);
  CHECK(record.has_value());
  CHECK(record->logical_sector == 0x030201);
  CHECK(record->byte_length == 5);

  const std::uint8_t high[] = {0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12};
  const auto wide = psx_cd_loc::decode_index_record(high);
  CHECK(wide.has_value());
  CHECK(wide->logical_sector == 0);
  CHECK(wide->byte_length == 0x12345678);

  const std::uint8_t short_bytes[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  CHECK(!psx_cd_loc::decode_index_record(short_bytes).has_value());
  const std::uint8_t long_bytes[] = {0, 1, 2, 3, 4, 5, 6, 7};
  CHECK(!psx_cd_loc::decode_index_record(long_bytes).has_value());
  CHECK(!psx_cd_loc::decode_index_record(std::span<const std::uint8_t>{}).has_value());
}

void test_byte_length_rounds_up_to_four() {
  const auto zero = psx_cd_loc::aligned_byte_length(0);
  const auto one = psx_cd_loc::aligned_byte_length(1);
  const auto five = psx_cd_loc::aligned_byte_length(5);
  const auto eight = psx_cd_loc::aligned_byte_length(8);
  CHECK(zero.has_value() && *zero == 0);
  CHECK(one.has_value() && *one == 4);
  CHECK(five.has_value() && *five == 8);
  CHECK(eight.has_value() && *eight == 8);
  CHECK(!psx_cd_loc::aligned_byte_length(-1).has_value());
  CHECK(!psx_cd_loc::aligned_byte_length(0x7FFFFFFE).has_value());
  const auto near = psx_cd_loc::aligned_byte_length(0x7FFFFFFC);
  CHECK(near.has_value() && *near == 0x7FFFFFFC);
}

}  // namespace

int main() {
  test_lead_in_and_frame_carry();
  test_minute_and_ten_second_nibbles();
  test_sector_domain_stops_at_signed_add();
  test_index_record_is_seven_little_endian_bytes();
  test_byte_length_rounds_up_to_four();
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("%d checks passed\n", checks);
  return 0;
}
