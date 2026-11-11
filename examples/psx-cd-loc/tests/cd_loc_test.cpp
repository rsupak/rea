#include "psx_cd_loc.hpp"

#include <array>
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

void test_mode_minus_one_shares_cd_init_and_stores_zero() {
  const psx_cd_loc::StartupDispatch minus_one =
      psx_cd_loc::dispatch_for_mode(-1);
  CHECK(minus_one.init == psx_cd_loc::StartupPath::Cd);
  CHECK(minus_one.fixed_range_copy == false);
  CHECK(minus_one.later_cell == 0);
  CHECK(minus_one.transfer == psx_cd_loc::StartupPath::Cd);

  const psx_cd_loc::StartupDispatch zero = psx_cd_loc::dispatch_for_mode(0);
  CHECK(zero.init == psx_cd_loc::StartupPath::Cd);
  CHECK(zero.fixed_range_copy == true);
  CHECK(zero.later_cell == 0);
  CHECK(zero.transfer == psx_cd_loc::StartupPath::Cd);
}

void test_other_modes_stay_on_the_host_path() {
  const psx_cd_loc::StartupDispatch one = psx_cd_loc::dispatch_for_mode(1);
  CHECK(one.init == psx_cd_loc::StartupPath::Host);
  CHECK(one.fixed_range_copy == false);
  CHECK(one.later_cell == 1);
  CHECK(one.transfer == psx_cd_loc::StartupPath::Host);

  const psx_cd_loc::StartupDispatch minus_two =
      psx_cd_loc::dispatch_for_mode(-2);
  CHECK(minus_two.init == psx_cd_loc::StartupPath::Host);
  CHECK(minus_two.later_cell == -2);
  CHECK(minus_two.transfer == psx_cd_loc::StartupPath::Host);
}

void test_record_index_uses_halfword_adjustment() {
  CHECK(psx_cd_loc::index_adjustment(0) == 0);
  CHECK(psx_cd_loc::index_adjustment(10) == 9);
  const auto index = psx_cd_loc::record_index(3, 9);
  CHECK(index.has_value());
  CHECK(*index == 11);
  CHECK(!psx_cd_loc::record_index(0, 0).has_value());
}

void test_cd_load_carries_position_and_aligned_length() {
  const auto record =
      psx_cd_loc::decode_index_record(
          std::array<std::uint8_t, 7>{0x96, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00});
  CHECK(record.has_value());
  const auto load = psx_cd_loc::cd_load_from_record(*record);
  CHECK(load.has_value());
  CHECK(load->logical_sector == 150);
  CHECK(load->position == psx_cd_loc::BcdMsf{0x00, 0x04, 0x00});
  CHECK(load->byte_length == 5);
  CHECK(load->aligned_length == 8);

  psx_cd_loc::IndexRecord huge;
  huge.logical_sector = 0;
  huge.byte_length = 0x80000000u;
  CHECK(!psx_cd_loc::cd_load_from_record(huge).has_value());
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
  test_mode_minus_one_shares_cd_init_and_stores_zero();
  test_other_modes_stay_on_the_host_path();
  test_record_index_uses_halfword_adjustment();
  test_cd_load_carries_position_and_aligned_length();
  test_byte_length_rounds_up_to_four();
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("%d checks passed\n", checks);
  return 0;
}
