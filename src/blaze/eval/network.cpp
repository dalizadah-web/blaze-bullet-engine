#include "blaze/eval/network.h"

#include <array>
#include <filesystem>
#include <fstream>

namespace blaze {
namespace {

constexpr std::size_t header_size = 32;
constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::uint64_t checksum(const std::vector<std::uint8_t>& bytes, std::size_t begin) {
    std::uint64_t hash = fnv_offset;
    for (std::size_t index = begin; index < bytes.size(); ++index) {
        hash ^= bytes[index];
        hash *= fnv_prime;
    }
    return hash;
}

}  // namespace

std::optional<Network> NetworkLoader::load(std::string_view path) {
    std::string ignored;
    return load(path, ignored);
}

std::optional<Network> NetworkLoader::load(std::string_view path, std::string& error) {
    error.clear();
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input) {
        error = "network file cannot be opened";
        return std::nullopt;
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    input.seekg(0, std::ios::beg);
    if (size < static_cast<std::streamoff>(header_size) || size > 128 * 1024 * 1024) {
        error = "network file has an invalid size";
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!input) {
        error = "network file is truncated";
        return std::nullopt;
    }
    constexpr std::array<char, 8> magic = {'B', 'L', 'A', 'Z', 'E', 'N', 'E', 'T'};
    for (std::size_t index = 0; index < magic.size(); ++index) {
        if (bytes[index] != static_cast<std::uint8_t>(magic[index])) {
            error = "network magic does not match Blaze format";
            return std::nullopt;
        }
    }
    const std::uint32_t version = read_u32(bytes, 8);
    const std::uint32_t features = read_u32(bytes, 12);
    const std::uint32_t hidden = read_u32(bytes, 16);
    const std::uint32_t payload_bytes = read_u32(bytes, 20);
    const std::uint64_t expected_checksum = read_u64(bytes, 24);
    if (version != 1 || features != 768 || hidden != 256 || payload_bytes == 0 ||
        payload_bytes != bytes.size() - header_size) {
        error = "network architecture or payload header is incompatible";
        return std::nullopt;
    }
    if (checksum(bytes, header_size) != expected_checksum) {
        error = "network checksum mismatch";
        return std::nullopt;
    }
    Network network;
    network.version = version;
    network.features = features;
    network.hidden = hidden;
    network.weights.assign(bytes.begin() + header_size, bytes.end());
    return network;
}

}  // namespace blaze
