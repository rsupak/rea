# PS1 logical-sector position and index record

Small portable C++ conversion of two data formats used beside a CD load:

- a logical sector number becomes three BCD bytes (minute, second, frame)
- a 7-byte little-endian record holds that sector number and a byte length

The position conversion adds 150 frames before dividing. That is two seconds
at 75 frames per second, the lead-in between a logical sector and the absolute
minute/second/frame a `Setloc` parameter uses. The byte length, when it is a
nonnegative transfer size, is rounded up to a multiple of 4.

This library does not send a command, open a file, allocate a destination, or
read a disc. Passing the checks does not show that any original program took
a particular branch, and it does not show that a drive returned data.

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

## Public references

- `Setloc` (command `02h`) takes three BCD bytes: minute, second, sector:
  <https://www.problemkaputt.de/psxspx-cdromdrive.htm>
- The same chapter, including 75 frames per second:
  <https://psx-spx.consoledev.net/cdromdrive/>
- The host file calls on the other startup branch (`PCInit`, `PCOpen`,
  `PCRead`, `PClSeek`) are break functions `101h`, `103h`, `105h`, and `107h`:
  <https://problemkaputt.de/psxspx-bios-pc-file-server.htm>

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
