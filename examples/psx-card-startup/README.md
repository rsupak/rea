# PS1 card-startup host contract

Small portable C++ model of the memory-card services used during PlayStation
startup. It records calls and enforces the documented order. It does not run
a BIOS, copy patches, emulate a card, or play a game.

The checks are synthetic tests of this model. They are not a run of an
original executable, and a passing test is not game compatibility.

## Host policy, not patch recognition

Public BIOS notes describe two startup patches. One waits before acknowledging
a memory-card IRQ. The other inserts a delay around a command/status byte.
Both locate internal BIOS code through `GetC0Table` / `GetB0Table` and then
call `FlushCache`.

This model does not recognize those instruction patterns and does not invent a
BIOS image for them to patch. A host that has no card IRQ line records the
request and reports that nothing was applied: no delay is claimed to have
elapsed, and no BIOS bytes are claimed to have been written. That refusal is
intentional. Treating it as a successful hardware delay would mix a PC policy
with PlayStation IRQ behavior.

`EnterCriticalSection` and `ExitCriticalSection` follow the published Status
bit pair (bits 2 and 10). Enter clears both and returns 1 only when both were
set. Exit always sets both. There is no nesting counter. A nested exit
re-enables the pair even if an earlier enter had disabled it. The model does
not include the exception-return copy of bit 2 into bit 0, and it does not
claim a measured boot-time Status value. The caller supplies the initial bits.

`InitCARD2`, `StartCARD2`, and `_bu_init` are accepted only in that order, and
only once each. Acceptance means the sequence step was recorded. It does not
initialize a filesystem, disable auto-format, or touch a memory card.
`InitCARD2`'s destruction of `k0`/`k1` is not modeled. `ChangeClearPAD` is
recorded and does not change interrupt hardware. `FlushCache` and jump-table
queries fail because this host has no instruction cache image and no BIOS
tables. Save, load, sector I/O, timing, and hardware emulation are explicit
unsupported operations.

## Public references

- BIOS function numbers and critical-section behavior:
  <https://www.problemkaputt.de/psxspx-bios-function-summary.htm>
- The same kernel chapter, including the card IRQ patch descriptions:
  <https://psx-spx.consoledev.net/kernelbios/>
- Card startup order (`InitCARD2`, then `StartCARD2`, then `_bu_init`):
  <https://www.problemkaputt.de/psxspx-bios-memory-card-functions.htm>

Service numbers in the header are those public indexes.

## Build

From this directory, with a C++20 compiler:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Another compiler:

```bash
cmake -S . -B build-clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-clang
ctest --test-dir build-clang --output-on-failure
```

The CMake file enables `-Wall -Wextra -Wpedantic -Werror` for GCC and Clang,
and `/W4 /WX` for MSVC. Compiling with MSVC or MinGW does not by itself show
that the tests were run on Windows.

## License

This example is part of the repository and uses its MIT license.
