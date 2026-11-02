#include "moonmod/native_plugin_hash.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <fstream>



namespace moonmod {
namespace {

class Sha256 {
public:
    void Update(const char *data, std::size_t size) {
        total_bits_ += static_cast<std::uint64_t>(size) * 8;
        for (std::size_t index = 0; index < size; ++index) {
            block_[block_size_++] = static_cast<unsigned char>(data[index]);
            if (block_size_ == block_.size()) {
                Transform();
                block_size_ = 0;
            }
        }
    }

    std::string Finish() {
        const std::uint64_t message_bits = total_bits_;
        block_[block_size_++] = 0x80;
        if (block_size_ > 56) {
            while (block_size_ < block_.size()) block_[block_size_++] = 0;
            Transform();
            block_size_ = 0;
        }
        while (block_size_ < 56) block_[block_size_++] = 0;
        for (int index = 7; index >= 0; --index) {
            block_[56 + static_cast<std::size_t>(7 - index)] =
                static_cast<unsigned char>((message_bits >> (index * 8)) & 0xff);
        }
        Transform();

        constexpr char digits[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (const std::uint32_t value : state_) {
            for (int shift = 28; shift >= 0; shift -= 4) result.push_back(digits[(value >> shift) & 0xf]);
        }
        return result;
    }

private:
    static constexpr std::array<std::uint32_t, 64> kRound = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    void Transform() {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            words[index] = (static_cast<std::uint32_t>(block_[index * 4]) << 24) |
                           (static_cast<std::uint32_t>(block_[index * 4 + 1]) << 16) |
                           (static_cast<std::uint32_t>(block_[index * 4 + 2]) << 8) |
                           static_cast<std::uint32_t>(block_[index * 4 + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const std::uint32_t x = words[index - 15], y = words[index - 2];
            const std::uint32_t s0 = std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3);
            const std::uint32_t s1 = std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const std::uint32_t sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + sum1 + choice + kRound[index] + words[index];
            const std::uint32_t sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temporary1;
            d = c; c = b; b = a; a = temporary1 + temporary2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                           0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<unsigned char, 64> block_{};
    std::size_t block_size_ = 0;
    std::uint64_t total_bits_ = 0;
};

} // namespace

std::string NativePluginFileSha256(const std::filesystem::path &path) {
    constexpr std::uintmax_t kMaximumTrustedLibrarySize = 256 * 1024 * 1024;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaximumTrustedLibrarySize) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash.Update(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!input.eof()) return {};
    return hash.Finish();
}

} // namespace moonmod
