#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// button is carried through because libGDR records platformer steering (2 = left, 3 = right) in
// the same stream as jumps. Dropping it turned every sideways step into a click cue.
struct GdrRawInput { uint64_t frame; bool down; bool player2; uint8_t button; };
// Positions recorded alongside the inputs, when the replay carries them.
struct GdrPos { float x, y; };

// isolated TU - libGDR NTTP-heavy templates ICE MSVC when mixed with async/web/matjson
double parseGdr2Bytes(const uint8_t* data, std::size_t size, std::vector<GdrRawInput>& out,
                      std::vector<GdrPos>* posOut = nullptr);
