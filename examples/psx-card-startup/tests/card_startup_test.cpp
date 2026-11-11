#include "psx_card_startup.hpp"

#include <cstdio>
#include <cstdlib>

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

#define CHECK(...) check(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

psx_card::StatusPair enabled_pair() { return psx_card::StatusPair{true, true}; }

void test_service_ids_match_public_bios_numbers() {
  CHECK(psx_card::kInitCard2 == psx_card::ServiceId{psx_card::Table::B, 0x4A});
  CHECK(psx_card::kStartCard2 == psx_card::ServiceId{psx_card::Table::B, 0x4B});
  CHECK(psx_card::kBuInit == psx_card::ServiceId{psx_card::Table::A, 0x70});
  CHECK(psx_card::kChangeClearPad == psx_card::ServiceId{psx_card::Table::B, 0x5B});
  CHECK(psx_card::kEnterCritical == psx_card::ServiceId{psx_card::Table::Sys, 0x01});
  CHECK(psx_card::kExitCritical == psx_card::ServiceId{psx_card::Table::Sys, 0x02});
}

void test_enter_return_follows_both_status_bits() {
  psx_card::Host enabled(enabled_pair());
  const psx_card::CallResult first = enabled.enter_critical_section();
  CHECK(first.error == psx_card::Error::None);
  CHECK(first.enter_result == 1);
  CHECK(first.bios_executed == false);
  CHECK(enabled.interrupts_disabled());

  const psx_card::CallResult nested = enabled.enter_critical_section();
  CHECK(nested.enter_result == 0);
  CHECK(enabled.interrupts_disabled());

  psx_card::Host mismatched(psx_card::StatusPair{true, false});
  CHECK(mismatched.enter_critical_section().enter_result == 0);
  CHECK(mismatched.status().bit2 == false);
  CHECK(mismatched.status().bit10 == false);
}

void test_exit_sets_both_bits_without_a_nesting_count() {
  psx_card::Host host(enabled_pair());
  CHECK(host.enter_critical_section().enter_result == 1);
  CHECK(host.enter_critical_section().enter_result == 0);
  const psx_card::CallResult exit_result = host.exit_critical_section();
  CHECK(exit_result.error == psx_card::Error::None);
  CHECK(exit_result.enter_result == -1);
  CHECK(host.status().bit2);
  CHECK(host.status().bit10);

  // A second exit still sets the bits. The documented operation has no depth.
  CHECK(host.exit_critical_section().error == psx_card::Error::None);
  CHECK(host.status().bit2 && host.status().bit10);
}

void test_card_steps_reject_bad_order_and_repetition() {
  psx_card::Host early(enabled_pair());
  CHECK(early.start_card2().error == psx_card::Error::OutOfOrder);
  CHECK(early.bu_init().error == psx_card::Error::OutOfOrder);
  CHECK(!early.init_card2_accepted());
  CHECK(!early.start_card2_accepted());
  CHECK(!early.bu_init_accepted());

  CHECK(early.init_card2(1).error == psx_card::Error::None);
  CHECK(early.bu_init().error == psx_card::Error::OutOfOrder);
  CHECK(!early.bu_init_accepted());
  CHECK(early.start_card2().error == psx_card::Error::None);
  CHECK(early.bu_init().error == psx_card::Error::None);

  CHECK(early.init_card2(0).error == psx_card::Error::RepeatedStep);
  CHECK(early.pad_enable() == 1);
  CHECK(early.start_card2().error == psx_card::Error::RepeatedStep);
  CHECK(early.bu_init().error == psx_card::Error::RepeatedStep);
  CHECK(early.bu_init_accepted());
  CHECK(early.filesystem_available() == false);
  CHECK(early.auto_format_state_known() == false);
}

void test_init_card2_does_not_require_the_critical_section() {
  psx_card::Host host(enabled_pair());
  CHECK(host.interrupts_disabled() == false);
  CHECK(host.init_card2(1).error == psx_card::Error::None);
  CHECK(host.pad_enable() == 1);
  CHECK(host.status().bit2 && host.status().bit10);
}

void test_failed_calls_do_not_mutate_status_or_steps() {
  psx_card::Host host(psx_card::StatusPair{true, false});
  CHECK(host.get_b0_table().error == psx_card::Error::NoBiosImage);
  CHECK(host.get_c0_table().error == psx_card::Error::NoBiosImage);
  CHECK(host.flush_cache().error == psx_card::Error::Unsupported);
  CHECK(host.unsupported(psx_card::UnsupportedOp::Save).error == psx_card::Error::Unsupported);
  CHECK(host.unsupported_count() == 1);
  CHECK(host.status().bit2);
  CHECK(host.status().bit10 == false);
  CHECK(!host.init_card2_accepted());
  CHECK(host.trace().size() == 3);
  CHECK(host.trace()[0].error == psx_card::Error::NoBiosImage);
  CHECK(host.trace()[0].service == psx_card::kGetB0Table);
  CHECK(host.trace()[1].service == psx_card::kGetC0Table);
  CHECK(host.trace()[2].service == psx_card::kFlushCache);
}

void test_workaround_requests_are_not_applied() {
  psx_card::Host host(enabled_pair());
  CHECK(host.enter_critical_section().enter_result == 1);
  const psx_card::WorkaroundReceipt early =
      host.request_workaround(psx_card::Workaround::EarlyCardIrqAcknowledge);
  const psx_card::WorkaroundReceipt delay =
      host.request_workaround(psx_card::Workaround::CardCommandStatusDelay);
  CHECK(early.critical_section_held);
  CHECK(delay.critical_section_held);
  CHECK(early.applied == false && early.delay_elapsed == false && early.bios_bytes_written == false);
  CHECK(delay.applied == false && delay.delay_elapsed == false && delay.bios_bytes_written == false);
  CHECK(host.workaround_count(psx_card::Workaround::EarlyCardIrqAcknowledge) == 1);
  CHECK(host.workaround_count(psx_card::Workaround::CardCommandStatusDelay) == 1);
  CHECK(host.unsupported(psx_card::UnsupportedOp::PatchRecognition).error ==
        psx_card::Error::Unsupported);
  CHECK(host.unsupported(psx_card::UnsupportedOp::Timing).error == psx_card::Error::Unsupported);
  CHECK(host.unsupported(psx_card::UnsupportedOp::HardwareEmulation).error ==
        psx_card::Error::Unsupported);
}

// Synthetic script of the service order found by static reading. It does not
// execute an original program, and a passing result is not game compatibility.
void test_synthetic_startup_script_stays_short_of_a_card_system() {
  psx_card::Host host(enabled_pair());

  CHECK(host.change_clear_pad(0).error == psx_card::Error::None);
  CHECK(host.enter_critical_section().enter_result == 1);
  CHECK(host.init_card2(1).error == psx_card::Error::None);
  CHECK(host.enter_critical_section().enter_result == 0);
  const psx_card::WorkaroundReceipt early =
      host.request_workaround(psx_card::Workaround::EarlyCardIrqAcknowledge);
  CHECK(early.applied == false);
  CHECK(host.get_c0_table().error == psx_card::Error::NoBiosImage);
  CHECK(host.flush_cache().error == psx_card::Error::Unsupported);
  CHECK(host.init_card2_accepted());

  CHECK(host.enter_critical_section().enter_result == 0);
  const psx_card::WorkaroundReceipt delay =
      host.request_workaround(psx_card::Workaround::CardCommandStatusDelay);
  CHECK(delay.applied == false && delay.critical_section_held);
  CHECK(host.get_b0_table().error == psx_card::Error::NoBiosImage);
  CHECK(host.flush_cache().error == psx_card::Error::Unsupported);
  CHECK(host.exit_critical_section().error == psx_card::Error::None);
  CHECK(host.interrupts_disabled() == false);

  CHECK(host.enter_critical_section().enter_result == 1);
  CHECK(host.start_card2().error == psx_card::Error::None);
  CHECK(host.change_clear_pad(0).error == psx_card::Error::None);
  CHECK(host.exit_critical_section().error == psx_card::Error::None);
  CHECK(host.interrupts_disabled() == false);
  CHECK(host.bu_init().error == psx_card::Error::None);

  CHECK(host.change_clear_pad_count() == 2);
  CHECK(host.last_change_clear_pad() == 0);
  CHECK(host.pad_enable() == 1);
  CHECK(host.filesystem_available() == false);
  CHECK(host.auto_format_state_known() == false);
  CHECK(host.unsupported(psx_card::UnsupportedOp::Save).error == psx_card::Error::Unsupported);
  CHECK(host.unsupported(psx_card::UnsupportedOp::Load).error == psx_card::Error::Unsupported);
  CHECK(host.unsupported(psx_card::UnsupportedOp::Filesystem).error == psx_card::Error::Unsupported);
  CHECK(host.unsupported(psx_card::UnsupportedOp::SectorIo).error == psx_card::Error::Unsupported);
  CHECK(host.bu_init_accepted());
}

}  // namespace

int main() {
  test_service_ids_match_public_bios_numbers();
  test_enter_return_follows_both_status_bits();
  test_exit_sets_both_bits_without_a_nesting_count();
  test_card_steps_reject_bad_order_and_repetition();
  test_init_card2_does_not_require_the_critical_section();
  test_failed_calls_do_not_mutate_status_or_steps();
  test_workaround_requests_are_not_applied();
  test_synthetic_startup_script_stays_short_of_a_card_system();

  if (failures != 0) {
    std::fprintf(stderr, "%d checks, %d failed\n", checks, failures);
    return EXIT_FAILURE;
  }
  std::printf("%d checks passed\n", checks);
  return EXIT_SUCCESS;
}
