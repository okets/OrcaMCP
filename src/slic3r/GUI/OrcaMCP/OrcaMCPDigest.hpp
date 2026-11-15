#pragma once
#include <boost/uuid/detail/sha1.hpp>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include "libslic3r/Point.hpp"
namespace Slic3r { namespace GUI { namespace OrcaMCP {
class Digest {
    boost::uuids::detail::sha1 hash;
public:
    void number(uint64_t value) {
        unsigned char bytes[8];
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(value >> (8 * i));
        hash.process_bytes(bytes, sizeof(bytes));
    }
    void scalar(float value) { uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); number(bits); }
    void scalar(double value) { uint64_t bits; std::memcpy(&bits, &value, sizeof(bits)); number(bits); }
    void xyz(const Vec3f& position) { for (int axis = 0; axis < 3; ++axis) scalar(position[axis]); }
    void text(const std::string& s) { number(s.size()); hash.process_bytes(s.data(), s.size()); }
    std::string finish() {
        unsigned words[5]; hash.get_digest(words);
        std::ostringstream out;
        out << std::hex << std::setfill('0');
        for (unsigned word : words) out << std::setw(8) << word;
        return out.str();
    }
};
}}}
