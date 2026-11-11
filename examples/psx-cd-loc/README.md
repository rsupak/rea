# PS1 logical-sector position and index record

Small portable C++ conversion of two data formats used beside a CD load:

- a logical sector number becomes three BCD bytes (minute, second, frame)
- a 7-byte little-endian record holds that sector number and a byte length

The position conversion adds 150 frames before dividing. That is two seconds
at 75 frames per second, the lead-in between a logical sector and the absolute
minute/second/frame a `Setloc` parameter uses. The byte length, when it is a
nonnegative transfer size, is rounded up to a multiple of 4.

The position helpers do not send a command. `psx_cd_read` copies user bytes
from a 2352-byte Mode 2 image the caller points at. It does not program DMA
or emulate a drive. Passing the synthetic checks does not show that any
original program took a particular branch.

## Which startup value selects the CD request

An earlier note in this file said a nonzero mode word selects the
development-host file server and skips CD init. That was wrong. The init
test is two compares: the value 0 takes the CD call, and the value -1 is
compared with a register just set to -1 and falls through into the same CD
call. Only a value that is neither 0 nor -1 takes the host init break.
A stepped run with the file value -1 reached that CD call. It stopped at
the BIOS console write of the init banner, before the call returned.

The transfer test is not the same compare. After a nonzero return from
init, both 0 and -1 store 0 into the cell the later test reads. That later
test is only "cell == 0". Zero takes the indexed CD request: `Setloc` of
the record's BCD position, and the record's byte length is what gets
allocated. A nonzero cell is the host read. Mode 0 also schedules a
fixed-size copy that mode -1 skips. The low-address host filenames are
computed from a nonzero cell; they are not the path taken once -1 has been
stored as 0.

`dispatch_for_mode` and `cd_load_from_record` are those two facts. They do
not send a command or claim a sector was transferred. The DMA programming
for a later read is not a direct call from the straight-line prefix that
issues `Setloc`. The checks below are synthetic values of this contract.
They are not a trace of an original executable.

## Mode 2 user bytes

A disc image is indexed by the ISO logical sector. Sector 0 is the first
2352 bytes of the file. The absolute minute/second/frame stored in the
sector header is that logical sector plus 150, and the reader checks it.
The 150-frame lead-in is not added to the file offset.

Each sector contributes at most 2048 bytes from raw offset 24. The requested
length is exact: thirty full blocks and a 240-byte tail are 61440 + 240
bytes, and the tail is the leading user bytes of the last sector, not a
2340-byte whole-sector block. `read_cd_load` uses the record's byte length,
not the 4-byte-rounded allocation size.

`--base-lba` is for a file that begins at a later logical sector. A full
disc image uses the default, 0.

```bash
psx_cd_read --image DISC.BIN --lba LOGICAL --length BYTES --output user.bin
psx_cd_read --image EXTRACT.BIN --base-lba FIRST --lba LOGICAL --length BYTES
```

`--record` takes 14 hex digits, one 7-byte index record, and reads that
record's logical sector and byte length. The tool prints the user-byte
count, the sector count, and the SHA-256 of the user bytes.

`--payload FILE` reads a resource that was already copied out of the image.
The first consumer loads five little-endian fields and does not compare a
tag: the word at offset 16 is the prefix size, the word at offset 20 and the
halfword at offset 30 travel with it, the word at offset 24 is an offset
from the start of the resource, and the word at offset 40 is loaded on the
zero second-argument path. The copy keeps whole words, so a size of 6 copies
4 bytes. `--prefix-out` writes that word-aligned prefix. It does not decode
the bytes inside the prefix.

`--lz FILE` unpacks a bitstream. The first little-endian word is the
output size. Flag bytes are read lowest bit first: 0 copies one literal
byte, and 1 is a backreference of length `(high nibble + 3)` at a distance
of `(low nibble << 8) | first byte`, copied one byte at a time so a
distance of 1 can repeat the previous byte. The unpack stops when the next
flag byte is reached and the output already has the declared length; that
flag byte is consumed. `--lz-offset` starts later in the file.
`--lz-output` writes the bytes. A short input, a backreference before the
bytes it names, a token past the declared end, and a declared size above
16 MiB fail.

Shorter than a full sector, a mode other than 2, a bad sync, a subheader
whose two copies differ, a header that does not match the logical sector,
a zero length, and a range that overflows the file index all fail.

Direct build, from this directory:

```bash
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -I include src/psx_cd_loc.cpp src/psx_cd_image.cpp src/psx_cd_read_main.cpp \
  -o psx_cd_read
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -I include src/psx_cd_loc.cpp src/psx_cd_image.cpp tests/cd_loc_test.cpp \
  -o psx_cd_loc_tests
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -I include src/psx_cd_loc.cpp src/psx_cd_image.cpp tests/cd_image_test.cpp \
  -o psx_cd_image_tests
```

## Public references

- `Setloc` (command `02h`) takes three BCD bytes: minute, second, sector:
  <https://www.problemkaputt.de/psxspx-cdromdrive.htm>
- The same chapter, including 75 frames per second:
  <https://psx-spx.consoledev.net/cdromdrive/>
- Mode 2 sector layout (12-byte sync, 4-byte header, 8-byte subheader,
  2048-byte user data):
  <https://problemkaputt.de/psx-spx.htm#cdromsectorencoding>
- The host file calls on the other startup branch (`PCInit`, `PCOpen`,
  `PCRead`, `PClSeek`) are break functions `101h`, `103h`, `105h`, and `107h`:
  <https://problemkaputt.de/psxspx-bios-pc-file-server.htm>

## Standalone disc view

`psx_cd_view` is a read-only program. It does not link to an emulator.
Pass a MODE2/2352 BIN, or a CUE whose first data track is MODE2/2352.
It lists the ISO 9660 volume, prints the resource at logical sector
108754 (61680 user bytes) using the field offsets the consumer loads,
and unpacks two streams whose sectors come from the loader table. Those
two sector numbers were not a captured `Setloc`. A PPM file is written
only when unpacked bytes start with TIM id `0x10` and the block sizes fit.
The captured heads of those streams do not.

From this directory, one command on Apple clang:

```bash
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -I include src/psx_cd_loc.cpp src/psx_cd_image.cpp src/psx_disc.cpp \
  src/psx_cd_view_main.cpp -o psx_cd_view
```

The shared headers use `std::span`, so this is C++20. Apple clang 21 accepts
that standard.

```bash
psx_cd_view --cue "/path/to/SLUS-00664.CUE"
psx_cd_view --image "/path/to/SLUS-00664.BIN"
```

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
