#include <gdr/gdr.hpp>
#include <cstdio>
#include <vector>
#include <span>

// Build a VALID replay with the library's own writer, then truncate it inside the input
// stream and leave a dangling varint continuation byte. That is a plausible corrupt or
// maliciously trimmed download: the header parses, the loop is entered, and the reader then
// cannot consume the next value.
int main(int argc, char** argv) {
    gdr::Replay<> rep;
    rep.framerate = 240.0;
    rep.author = "t"; rep.description = "t";
    rep.levelInfo.id = 1; rep.levelInfo.name = "lvl";
    rep.botInfo.name = "t"; rep.botInfo.version = 1;
    for (uint64_t i = 0; i < 400; i++) rep.inputs.emplace_back(i * 7, 1, false, (i & 1) == 0);
    auto ex = rep.exportData();
    if (ex.isErr()) { printf("export failed\n"); return 2; }
    auto buf = std::move(ex).unwrap();
    printf("valid replay: %zu bytes, %zu inputs\n", buf.size(), rep.inputs.size());

    size_t cut = buf.size() / 2;
    buf.resize(cut);
    buf.back() |= 0x80;             // dangling continuation byte
    printf("truncated to %zu bytes, importing...\n", buf.size());
    fflush(stdout);

    auto r = gdr::Replay<>::importData(std::span<uint8_t>(buf.data(), buf.size()));
    printf("returned: %s, inputs=%zu\n", r.isErr() ? "Err" : "Ok",
           r.isErr() ? (size_t)0 : r.unwrap().inputs.size());
    return 0;
}

