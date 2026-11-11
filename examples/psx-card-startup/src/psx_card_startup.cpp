#include "psx_card_startup.hpp"

namespace psx_card {
namespace {

CallResult recorded() {
  CallResult result;
  result.hardware_effect_modeled = false;
  result.bios_executed = false;
  return result;
}

}  // namespace

const char* to_string(Error error) {
  switch (error) {
    case Error::None:
      return "none";
    case Error::OutOfOrder:
      return "out_of_order";
    case Error::RepeatedStep:
      return "repeated_step";
    case Error::Unsupported:
      return "unsupported";
    case Error::NoBiosImage:
      return "no_bios_image";
  }
  return "unknown";
}

const char* to_string(UnsupportedOp op) {
  switch (op) {
    case UnsupportedOp::Save:
      return "save";
    case UnsupportedOp::Load:
      return "load";
    case UnsupportedOp::Filesystem:
      return "filesystem";
    case UnsupportedOp::SectorIo:
      return "sector_io";
    case UnsupportedOp::Timing:
      return "timing";
    case UnsupportedOp::HardwareEmulation:
      return "hardware_emulation";
    case UnsupportedOp::BiosImage:
      return "bios_image";
    case UnsupportedOp::PatchRecognition:
      return "patch_recognition";
  }
  return "unknown";
}

const char* to_string(Workaround workaround) {
  switch (workaround) {
    case Workaround::EarlyCardIrqAcknowledge:
      return "early_card_irq_acknowledge";
    case Workaround::CardCommandStatusDelay:
      return "card_command_status_delay";
  }
  return "unknown";
}

Host::Host(StatusPair initial) : status_(initial) {}

CallResult Host::accept(ServiceId service, std::optional<int> argument) {
  trace_.push_back(TraceEvent{service, Error::None, argument});
  return recorded();
}

CallResult Host::reject(ServiceId service, Error error, std::optional<int> argument) {
  trace_.push_back(TraceEvent{service, error, argument});
  CallResult result = recorded();
  result.error = error;
  return result;
}

CallResult Host::enter_critical_section() {
  const bool both_set = status_.bit2 && status_.bit10;
  status_.bit2 = false;
  status_.bit10 = false;
  CallResult result = accept(kEnterCritical, both_set ? 1 : 0);
  result.enter_result = both_set ? 1 : 0;
  return result;
}

CallResult Host::exit_critical_section() {
  status_.bit2 = true;
  status_.bit10 = true;
  return accept(kExitCritical);
}

CallResult Host::change_clear_pad(int argument) {
  last_change_clear_pad_ = argument;
  ++change_clear_pad_count_;
  return accept(kChangeClearPad, argument);
}

CallResult Host::init_card2(int pad_enable) {
  if (init_card2_accepted_) {
    return reject(kInitCard2, Error::RepeatedStep, pad_enable);
  }
  init_card2_accepted_ = true;
  pad_enable_ = pad_enable;
  return accept(kInitCard2, pad_enable);
}

CallResult Host::start_card2() {
  if (!init_card2_accepted_) {
    return reject(kStartCard2, Error::OutOfOrder);
  }
  if (start_card2_accepted_) {
    return reject(kStartCard2, Error::RepeatedStep);
  }
  start_card2_accepted_ = true;
  return accept(kStartCard2);
}

CallResult Host::bu_init() {
  if (!start_card2_accepted_) {
    return reject(kBuInit, Error::OutOfOrder);
  }
  if (bu_init_accepted_) {
    return reject(kBuInit, Error::RepeatedStep);
  }
  bu_init_accepted_ = true;
  return accept(kBuInit);
}

CallResult Host::flush_cache() { return reject(kFlushCache, Error::Unsupported); }

CallResult Host::get_b0_table() { return reject(kGetB0Table, Error::NoBiosImage); }

CallResult Host::get_c0_table() { return reject(kGetC0Table, Error::NoBiosImage); }

CallResult Host::unsupported(UnsupportedOp) {
  ++unsupported_count_;
  CallResult result = recorded();
  result.error = Error::Unsupported;
  return result;
}

WorkaroundReceipt Host::request_workaround(Workaround workaround) {
  if (workaround == Workaround::EarlyCardIrqAcknowledge) {
    ++early_irq_requests_;
  } else {
    ++status_delay_requests_;
  }
  WorkaroundReceipt receipt;
  receipt.request = workaround;
  receipt.applied = false;
  receipt.delay_elapsed = false;
  receipt.bios_bytes_written = false;
  receipt.critical_section_held = interrupts_disabled();
  return receipt;
}

StatusPair Host::status() const { return status_; }

bool Host::interrupts_disabled() const { return !status_.bit2 && !status_.bit10; }

bool Host::init_card2_accepted() const { return init_card2_accepted_; }

bool Host::start_card2_accepted() const { return start_card2_accepted_; }

bool Host::bu_init_accepted() const { return bu_init_accepted_; }

std::optional<int> Host::pad_enable() const { return pad_enable_; }

std::optional<int> Host::last_change_clear_pad() const { return last_change_clear_pad_; }

int Host::change_clear_pad_count() const { return change_clear_pad_count_; }

int Host::workaround_count(Workaround workaround) const {
  return workaround == Workaround::EarlyCardIrqAcknowledge ? early_irq_requests_
                                                           : status_delay_requests_;
}

int Host::unsupported_count() const { return unsupported_count_; }

bool Host::filesystem_available() const { return false; }

bool Host::auto_format_state_known() const { return false; }

const std::vector<TraceEvent>& Host::trace() const { return trace_; }

}  // namespace psx_card
