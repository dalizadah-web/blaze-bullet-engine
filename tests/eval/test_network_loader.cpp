#include "blaze/eval/network.h"

#include "test_support.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

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

TEST_CASE(network_evaluator_decodes_quantized_material_signal) {
    constexpr std::size_t input_bytes = 768U * 256U * 2U;
    constexpr std::size_t hidden_bias_bytes = 256U * 4U;
    constexpr std::size_t output_bytes = 256U * 2U;
    std::vector<std::uint8_t> payload(input_bytes + hidden_bias_bytes + output_bytes + 4U, 0);

    const auto put_i16 = [&](std::size_t offset, std::int16_t value) {
        const auto raw = static_cast<std::uint16_t>(value);
        payload[offset] = static_cast<std::uint8_t>(raw & 0xFFU);
        payload[offset + 1] = static_cast<std::uint8_t>(raw >> 8U);
    };
    const std::size_t white_pawn = 28U * 2U;
    const std::size_t black_pawn = (6U * 64U + 28U) * 2U;
    put_i16(white_pawn * 256U, 6400);
    put_i16(black_pawn * 256U, -6400);
    put_i16(white_pawn * 256U + 2U, -6400);
    put_i16(black_pawn * 256U + 2U, 6400);
    put_i16(input_bytes + hidden_bias_bytes, 64);
    put_i16(input_bytes + hidden_bias_bytes + 2U, -64);

    Network network;
    network.version = 1;
    network.features = 768;
    network.hidden = 256;
    network.weights = std::move(payload);
    std::string error;
    const auto evaluator = NetworkEvaluator::create(network, error);
    CHECK(evaluator.has_value());

    const auto parsed = Position::from_fen("4k3/8/8/8/4P3/8/8/4K3 w - - 0 1");
    CHECK(parsed.has_value());
    CHECK_EQ(evaluator->evaluate(*parsed), 100);
}

TEST_CASE(network_evaluator_rejects_loader_payload_with_wrong_inference_shape) {
    Network network;
    network.version = 1;
    network.features = 768;
    network.hidden = 256;
    network.weights.assign(8, 0);
    std::string error;
    CHECK(!NetworkEvaluator::create(network, error).has_value());
}

}  // namespace
}  // namespace blaze
