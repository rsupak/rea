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

Which branch a program takes is a separate fact. One examined startup word
selects a development-host file read and skips both the controller init and
the `Setloc` that would consume these BCD bytes. The conversion is still the
position encoding on the other side of that word. The tests below are
synthetic values of this contract. They are not measurements from a disc and
not a run of an original executable.

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
