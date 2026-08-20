# Regression tests

Standalone, no test framework - each is a `main()` that exits 0 on pass. They are built by
hand because they exercise the vendored parsers directly, without Geode or the game.

    cl /nologo /std:c++20 /EHsc /I ..\external gdr_truncated_regression.cpp

## gdr_truncated_regression.cpp

A macro file is untrusted input: it is downloaded over the network and loaded from disk.

`binary_reader::operator>>` consumes **nothing** when the stream is too short to hold the
value - `detail::consume` returns 0 and `subspan(0)` is a no-op - so `empty()` never becomes
true. The input loop in `gdr.hpp` is `while (!stream.empty())`, so a replay truncated inside
its input stream made that loop append an input forever.

Measured before the fix: a **220-byte** file drove the process to **2,788 MB** resident in 12
seconds, still climbing. In the game that is a freeze plus memory exhaustion, triggered by
downloading a macro. Afterwards the same file parses to the 180 inputs it genuinely contains.

The test builds a valid replay with the library's own writer, cuts it in half, and sets the
continuation bit on the last byte so the reader is left mid-varint. It must terminate.
