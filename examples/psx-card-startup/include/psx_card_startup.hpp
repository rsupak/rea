#pragma once

// Host-side record of the PS1 memory-card startup services named by public
// BIOS documentation. This is not a BIOS, a patcher, or a card image.
//
// COP0 Status bits 2 and 10 are the pair EnterCriticalSection clears and
// ExitCriticalSection sets (nocash PSX-SPX, SYS(01h) and SYS(02h)). Bit 0,
// which the exception return copies from bit 2, is not modeled. The pair is
// not a nesting counter: Exit always sets both bits.

#include <cstdint>
#include <optional>
#include <vector>

namespace psx_card {

enum class Table : std::uint8_t { A, B, Sys };

struct ServiceId {
  Table table;
  unsigned number;

  friend constexpr bool operator==(ServiceId lhs, ServiceId rhs) {
    return lhs.table == rhs.table && lhs.number == rhs.number;
  }
};

// Numbers are the public BIOS indexes, not addresses in any executable.
inline constexpr ServiceId kChangeClearPad{Table::B, 0x5B};
inline constexpr ServiceId kInitCard2{Table::B, 0x4A};
inline constexpr ServiceId kStartCard2{Table::B, 0x4B};
inline constexpr ServiceId kGetC0Table{Table::B, 0x56};
inline constexpr ServiceId kGetB0Table{Table::B, 0x57};
inline constexpr ServiceId kBuInit{Table::A, 0x70};
inline constexpr ServiceId kFlushCache{Table::A, 0x44};
inline constexpr ServiceId kEnterCritical{Table::Sys, 0x01};
inline constexpr ServiceId kExitCritical{Table::Sys, 0x02};

static_assert(kBuInit.number == 0x70 && kBuInit.table == Table::A);
static_assert(kInitCard2.number == 0x4A && kStartCard2.number == 0x4B);
static_assert(kGetC0Table.number == 0x56 && kGetB0Table.number == 0x57);
static_assert(kChangeClearPad.number == 0x5B);
static_assert(kFlushCache.number == 0x44);
static_assert(kEnterCritical.number == 0x01 && kExitCritical.number == 0x02);

enum class Error {
  None,
  // StartCARD2 before InitCARD2, or _bu_init before StartCARD2.
  OutOfOrder,
  // InitCARD2, StartCARD2, or _bu_init was already accepted.
  RepeatedStep,
  // The operation is named and intentionally absent.
  Unsupported,
  // GetC0Table / GetB0Table need a BIOS image this host does not have.
  NoBiosImage,
};

enum class UnsupportedOp {
  Save,
  Load,
  Filesystem,
  SectorIo,
  Timing,
  HardwareEmulation,
  BiosImage,
  PatchRecognition,
};

// The two statically observed workaround requests. Asking for one records the
// intent. It does not copy instructions or wait.
enum class Workaround {
  EarlyCardIrqAcknowledge,
  CardCommandStatusDelay,
};

struct StatusPair {
  bool bit2 = false;
  bool bit10 = false;
};

struct CallResult {
  Error error = Error::None;
  // Documented EnterCriticalSection result: 1 if both bits were set, else 0.
  // -1 when this call is not EnterCriticalSection.
  int enter_result = -1;
  // Always false. No call in this model performs a hardware or BIOS effect.
  bool hardware_effect_modeled = false;
  bool bios_executed = false;
};

struct WorkaroundReceipt {
  Workaround request{};
  bool applied = false;
  bool delay_elapsed = false;
  bool bios_bytes_written = false;
  // True when both modeled Status bits were clear at the request.
  bool critical_section_held = false;
  const char* reason =
      "Recorded only. This host has no BIOS image and no card IRQ line.";
};

struct TraceEvent {
  ServiceId service;
  Error error = Error::None;
  std::optional<int> argument;
};

const char* to_string(Error error);
const char* to_string(UnsupportedOp op);
const char* to_string(Workaround workaround);

// Initial Status bits are chosen by the caller. This model does not claim a
// measured boot-time COP0 Status value.
class Host {
 public:
  explicit Host(StatusPair initial);

  [[nodiscard]] CallResult enter_critical_section();
  [[nodiscard]] CallResult exit_critical_section();
  [[nodiscard]] CallResult change_clear_pad(int argument);
  [[nodiscard]] CallResult init_card2(int pad_enable);
  [[nodiscard]] CallResult start_card2();
  [[nodiscard]] CallResult bu_init();
  [[nodiscard]] CallResult flush_cache();
  [[nodiscard]] CallResult get_b0_table();
  [[nodiscard]] CallResult get_c0_table();
  [[nodiscard]] CallResult unsupported(UnsupportedOp op);
  [[nodiscard]] WorkaroundReceipt request_workaround(Workaround workaround);

  [[nodiscard]] StatusPair status() const;
  [[nodiscard]] bool interrupts_disabled() const;
  [[nodiscard]] bool init_card2_accepted() const;
  [[nodiscard]] bool start_card2_accepted() const;
  [[nodiscard]] bool bu_init_accepted() const;
  [[nodiscard]] std::optional<int> pad_enable() const;
  [[nodiscard]] std::optional<int> last_change_clear_pad() const;
  [[nodiscard]] int change_clear_pad_count() const;
  [[nodiscard]] int workaround_count(Workaround workaround) const;
  [[nodiscard]] int unsupported_count() const;
  // _bu_init is documented to initialize the card filesystem and disable auto
  // format. This host does not do either, including after an accepted call.
  [[nodiscard]] bool filesystem_available() const;
  [[nodiscard]] bool auto_format_state_known() const;
  [[nodiscard]] const std::vector<TraceEvent>& trace() const;

 private:
  CallResult accept(ServiceId service, std::optional<int> argument = std::nullopt);
  CallResult reject(ServiceId service, Error error, std::optional<int> argument = std::nullopt);

  StatusPair status_;
  bool init_card2_accepted_ = false;
  bool start_card2_accepted_ = false;
  bool bu_init_accepted_ = false;
  std::optional<int> pad_enable_;
  std::optional<int> last_change_clear_pad_;
  int change_clear_pad_count_ = 0;
  int early_irq_requests_ = 0;
  int status_delay_requests_ = 0;
  int unsupported_count_ = 0;
  std::vector<TraceEvent> trace_;
};

}  // namespace psx_card
