#include "report_hasher.h"
#include <iomanip>
#include <sstream>
#include <cstring>

// Standalone SHA-256 implementation (no external dependencies)
namespace {
    // SHA-256 constants
    const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

    inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    inline uint32_t sigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    inline uint32_t sigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    inline uint32_t gamma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    inline uint32_t gamma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

    void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
        uint32_t W[64];
        for (int i = 0; i < 16; ++i) {
            W[i] = (block[i*4] << 24) | (block[i*4+1] << 16) | (block[i*4+2] << 8) | block[i*4+3];
        }
        for (int i = 16; i < 64; ++i) {
            W[i] = gamma1(W[i-2]) + W[i-7] + gamma0(W[i-15]) + W[i-16];
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + sigma1(e) + ch(e, f, g) + K[i] + W[i];
            uint32_t t2 = sigma0(a) + maj(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void sha256(const uint8_t* data, size_t len, uint8_t hash[32]) {
        uint32_t state[8] = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };

        uint8_t block[64];
        size_t i = 0;
        while (i + 64 <= len) {
            sha256_transform(state, data + i);
            i += 64;
        }

        // Padding
        size_t remaining = len - i;
        std::memcpy(block, data + i, remaining);
        block[remaining++] = 0x80;
        
        if (remaining > 56) {
            std::memset(block + remaining, 0, 64 - remaining);
            sha256_transform(state, block);
            remaining = 0;
        }
        
        std::memset(block + remaining, 0, 56 - remaining);
        uint64_t bit_len = len * 8;
        for (int j = 7; j >= 0; --j) {
            block[56 + j] = bit_len & 0xff;
            bit_len >>= 8;
        }
        sha256_transform(state, block);

        // Output
        for (int i = 0; i < 8; ++i) {
            hash[i*4]   = (state[i] >> 24) & 0xff;
            hash[i*4+1] = (state[i] >> 16) & 0xff;
            hash[i*4+2] = (state[i] >> 8) & 0xff;
            hash[i*4+3] = state[i] & 0xff;
        }
    }
}

std::string canonicalize_json(const nlohmann::json& j) {
    // NOTE: nlohmann::json uses std::map internally by default,
    // which guarantees alphabetical key ordering.
    // Therefore dump() is deterministic for object keys.
    // Do NOT use ordered_json type, as it preserves insertion order.
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict);
}

std::string compute_report_hash(const nlohmann::json& report) {
    // Canonicalize JSON to deterministic string
    std::string canonical = canonicalize_json(report);
    
    // Compute SHA-256
    uint8_t hash[32];
    sha256(reinterpret_cast<const uint8_t*>(canonical.c_str()), 
           canonical.size(), 
           hash);
    
    // Convert to hex string
    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') 
            << static_cast<int>(hash[i]);
    }
    
    return oss.str();
}
