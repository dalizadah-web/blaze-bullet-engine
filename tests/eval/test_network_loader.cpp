#include "blaze/eval/network.h"

#include "test_support.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace blaze {
namespace {

std::uint64_t checksum(const std::array<std::uint8_t, 8>& bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

TEST_CASE(network_loader_rejects_missing_and_corrupt_files) {
    CHECK(!NetworkLoader::load("build/blaze/does-not-exist.blaze-net").has_value());
    const std::filesystem::path path = "build/blaze/corrupt.blaze-net";
    std::ofstream output(path, std::ios::binary);
    output << "not a Blaze network";
    output.close();
    CHECK(!NetworkLoader::load(path.string()).has_value());
    std::filesystem::remove(path);
}

TEST_CASE(network_loader_validates_version_dimensions_payload_and_checksum) {
    const std::filesystem::path path = "build/blaze/test.blaze-net";
    const std::array<std::uint8_t, 8> payload = {0, 0, 1, 0, 2, 0, 3, 0};
    const std::uint64_t hash = checksum(payload);
    std::ofstream output(path, std::ios::binary);
    output.write("BLAZENET", 8);
    const std::uint32_t version = 1;
    const std::uint32_t features = 768;
    const std::uint32_t hidden = 256;
    const std::uint32_t payload_bytes = static_cast<std::uint32_t>(payload.size());
    output.write(reinterpret_cast<const char*>(&version), sizeof(version));
    output.write(reinterpret_cast<const char*>(&features), sizeof(features));
    output.write(reinterpret_cast<const char*>(&hidden), sizeof(hidden));
    output.write(reinterpret_cast<const char*>(&payload_bytes), sizeof(payload_bytes));
    output.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
    output.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    output.close();

    const auto network = NetworkLoader::load(path.string());
    CHECK(network.has_value());
    CHECK_EQ(network->version, 1U);
    CHECK_EQ(network->features, 768U);
    CHECK_EQ(network->hidden, 256U);
    CHECK_EQ(network->weights.size(), payload.size());
    std::filesystem::remove(path);
}

}  // namespace
}  // namespace blaze
