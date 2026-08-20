#include "gdr_parse.hpp"

#include <gdr/gdr.hpp>
#include <gdr/physics_extension.hpp>

#include <cmath>
#include <span>
#include <utility>

double parseGdr2Bytes(const uint8_t* data, std::size_t size, std::vector<GdrRawInput>& out,
                      std::vector<GdrPos>* posOut) {
    out.clear();
    // Try the physics extension first. A replay recorded with it carries the player's real
    // position on every input, which is the path the recording actually flew - worth more than any
    // simulation of it, and free. A replay without it fails this import and falls back below.
    if (posOut) {
        auto pres = gdr::Replay<void, PhysicsInput>::importData(
                        std::span<uint8_t>(const_cast<uint8_t*>(data), size));
        if (pres.isOk()) {
            auto pr = std::move(pres).unwrap();
            double pfps = pr.framerate;
            if (!(std::isfinite(pfps) && pfps > 1.0 && pfps <= 100000.0)) pfps = 240.0;
            out.reserve(pr.inputs.size());
            posOut->reserve(pr.inputs.size());
            for (auto const& in : pr.inputs) {
                out.push_back({ in.frame, in.down, in.player2, (uint8_t)in.button });
                if (std::isfinite(in.xPosition) && std::isfinite(in.yPosition)
                    && !in.player2 && in.xPosition > 0.f)
                    posOut->push_back({ in.xPosition, in.yPosition });
            }
            if (!out.empty()) return pfps;
            out.clear(); posOut->clear();
        }
    }
    auto res = gdr::Replay<>::importData(std::span<uint8_t>(const_cast<uint8_t*>(data), size));
    if (res.isErr()) return 0.0;
    auto replay = std::move(res).unwrap();
    // Same guard every sibling parser has. A file declaring 0, inf or NaN would otherwise divide
    // through the whole timing pipeline; .slc has been clamping this since 1.0.18.
    double fps = replay.framerate;
    if (!(std::isfinite(fps) && fps > 1.0 && fps <= 100000.0)) fps = 240.0;
    out.reserve(replay.inputs.size());
    for (auto const& in : replay.inputs)
        out.push_back({ in.frame, in.down, in.player2, (uint8_t)in.button });
    return fps;
}
