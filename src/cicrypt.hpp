#pragma once
//
// The licence used to be checkable entirely offline, by the binary, against a constant the binary
// carried. licHash() is FNV-1a over (key, instance, timestamp) mixed with a seed that sits in the
// .text section, and licGate() accepts any key at all as long as lic-s matches it. So a crack never
// needed to patch anything: write four values into the mod's save file and both gates open.
//
// That is why cracks survived every release. Obfuscated strings, a hidden upstream address and
// split checks all defend against someone editing the BINARY, and the binary was never the thing
// being edited. A data crack costs nothing to re-apply to a new build.
//
// The fix is to make the licence something the machine cannot compute for itself: a grant signed by
// the server with a private key, verified here with a public one. Verification needs real
// cryptography, and this is it - Ed25519 verification and SHA-512, plus XSalsa20-Poly1305 for
// encrypting content at rest so the licence becomes a key rather than a branch.
//
// Derived from TweetNaCl (Bernstein, van Gastel, Janssen, Lange, Schwabe, Smetsers), public domain.
// Correctness is not taken on trust: tools/crypttest.cpp runs the RFC 8032 vectors and the
// RFC 6234 SHA-512 vectors, and the build script refuses to package if any of them fail.
//
#include <cstddef>
#include <cstdint>
#include <string>

namespace cicrypt {

// SHA-512. `out` is 64 bytes.
void sha512(const unsigned char* in, size_t n, unsigned char out[64]);

// Ed25519 detached verification. Returns true only for a signature this exact public key
// produced over this exact message.
bool ed25519Verify(const unsigned char* msg, size_t n,
                   const unsigned char sig[64], const unsigned char pk[32]);

// XSalsa20-Poly1305, the NaCl secretbox. Authenticated: decrypt returns false if a single bit of
// the ciphertext, the nonce or the key is wrong, so a tampered cache is rejected rather than
// parsed. `out` needs `n` bytes for encrypt and `n - 16` for decrypt.
bool secretboxSeal(const unsigned char* in, size_t n, const unsigned char nonce[24],
                   const unsigned char key[32], unsigned char* out);
bool secretboxOpen(const unsigned char* in, size_t n, const unsigned char nonce[24],
                   const unsigned char key[32], unsigned char* out);

// Constant time. Used wherever a comparison result could otherwise be timed.
bool equalCT(const unsigned char* a, const unsigned char* b, size_t n);

// A licence GRANT: 56 bytes the server signed, binding an expiry to one device token.
//
//   0..3   "CIG1"
//   4..11  expires, int64 little endian
//   12..43 SHA-512(token)[0..32] - what makes a grant useless without its token
//   44..51 issued, int64 little endian
//   52..55 version, uint32 little endian
//
// grantVerify checks the signature and the token binding and nothing else: whether an expiry has
// passed is a question about clocks, and the clock rules live with the rest of the licence code
// where dual boots and dead CMOS batteries are already accounted for.
struct Grant { int64_t issued = 0; int64_t expires = 0; uint32_t version = 0; };
bool grantVerify(std::string const& grant, std::string const& token,
                 const unsigned char pub[32], Grant& out);

// THE VAULT. Macro files the mod writes are sealed with a key derived from the account, so the
// licence stops being a branch and becomes a dependency.
//
// The gate is now a code patch rather than a save-file edit, but a patched build still had a whole
// product waiting for it: macros landed on disk in plain bytes, so a cracked binary plus a copied
// macros/ folder was the real thing. The library IS the product - 1,510 files - and a leaked pack
// of it was worth more than the DLL.
//
// Sealed, that pack is noise. Reading it needs a key the server only hands to an account, and an
// account only plays on one device at a time and can be revoked.
//
//   0..7    "CIVAULT1"
//   8..31   nonce
//   32..    XSalsa20-Poly1305, authenticated - a tampered file is refused, not misparsed
//
// The nonce is SHA-512(plaintext || name), so it is unique per file without needing a random
// source, and a file that has not changed seals to the same bytes.
bool vaultIsSealed(std::string const& blob);
void vaultKeyFrom(std::string const& accountSecret, unsigned char key[32]);
bool vaultSeal(std::string const& plain, std::string const& name,
               const unsigned char key[32], std::string& out);
bool vaultOpen(std::string const& blob, const unsigned char key[32], std::string& out);

// base64url, no padding - what the grant travels as.
std::string b64uEncode(const unsigned char* in, size_t n);
bool b64uDecode(std::string const& in, std::string& out);

}   // namespace cicrypt
