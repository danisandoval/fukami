#pragma once
// Platform-independent pieces of the ADR-0006 package reader: the typed-binding checks that never touch the file
// system. rrv_resource_package.cpp (macOS, Linux: descriptors and openat) and rrv_resource_package_win.cpp (Windows:
// handles) both include it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rrv::resource_package::detail {

inline constexpr std::string_view kVersion = "rrv-resource-package-v1";

[[noreturn]] inline void fail(const char* message) { throw std::runtime_error(std::string("resource package: ") + message); }

class Sha256 {
public:
    void update(const unsigned char* data, size_t count) {
        bits_ += static_cast<uint64_t>(count) * 8;
        while (count--) { block_[used_++] = *data++; if (used_ == 64) { transform(); used_ = 0; } }
    }
    std::string finish() {
        const uint64_t length = bits_; const unsigned char one = 0x80, zero = 0;
        update(&one, 1); while (used_ != 56) update(&zero, 1);
        std::array<unsigned char, 8> bytes{};
        for (int i = 0; i != 8; ++i) bytes[7 - i] = static_cast<unsigned char>(length >> (8 * i));
        update(bytes.data(), bytes.size());
        constexpr char hex[] = "0123456789abcdef"; std::string out; out.reserve(64);
        for (uint32_t word : state_) for (int shift = 28; shift >= 0; shift -= 4) out.push_back(hex[(word >> shift) & 15]);
        return out;
    }
private:
    static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
    void transform() {
        static constexpr uint32_t k[] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::array<uint32_t, 64> w{};
        for (size_t i=0;i<16;++i) w[i]=(uint32_t(block_[4*i])<<24)|(uint32_t(block_[4*i+1])<<16)|(uint32_t(block_[4*i+2])<<8)|block_[4*i+3];
        for (size_t i=16;i<64;++i) { const auto s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3); const auto s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1; }
        auto a=state_[0],b=state_[1],c=state_[2],d=state_[3],e=state_[4],f=state_[5],g=state_[6],h=state_[7];
        for(size_t i=0;i<64;++i){const auto t1=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^((~e)&g))+k[i]+w[i];const auto t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;
    }
    std::array<unsigned char,64> block_{}; size_t used_=0; uint64_t bits_=0;
    std::array<uint32_t,8> state_{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
};

// A package-relative path: `/`-separated, no empty, `.` or `..` component, ASCII letters, digits, `_`, `-` and `.` only.
inline void validate_relative(std::string_view value) {
    if (value.empty() || value.front() == '/' || value.back() == '/') fail("invalid package relative path");
    size_t begin = 0;
    while (begin < value.size()) { size_t end = value.find('/', begin); auto part = value.substr(begin, end == std::string_view::npos ? value.size()-begin : end-begin); if (part.empty() || part=="." || part=="..") fail("invalid package relative path"); for(char ch:part) if (!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.')) fail("invalid package relative path"); if(end==std::string_view::npos) break; begin=end+1; }
}

} // namespace rrv::resource_package::detail
