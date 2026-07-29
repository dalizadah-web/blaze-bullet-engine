#include "blaze/search/parameters.h"

#include "test_support.h"

#include <string>
#include <unordered_set>

namespace blaze {
namespace {

TEST_CASE(search_parameter_schema_has_unique_valid_defaults) {
    const SearchParameters defaults;
    std::unordered_set<std::string> names;
    for (const auto& descriptor : search_parameter_descriptors) {
        CHECK(descriptor.field != nullptr);
        CHECK(descriptor.minimum <= defaults.*(descriptor.field));
        CHECK(defaults.*(descriptor.field) <= descriptor.maximum);
        CHECK(names.insert(std::string(descriptor.name)).second);
    }
}

TEST_CASE(search_parameter_payload_round_trips_canonically) {
    SearchParameters expected;
    expected.rfp_base = 81;
    expected.lmr_divisor_hundredths = 247;
    expected.qsearch_delta_margin = 143;
    const std::string encoded = serialize_search_parameters(expected);

    SearchParameters parsed;
    std::string error;
    CHECK(parse_search_parameters(encoded, parsed, error));
    CHECK_EQ(serialize_search_parameters(parsed), encoded);
}

TEST_CASE(search_parameter_payload_is_atomic_and_strict) {
    SearchParameters parsed;
    std::string error;
    CHECK(!parse_search_parameters("rfp_base=80,rfp_base=81", parsed, error));
    CHECK(error.find("duplicate") != std::string::npos);
    CHECK(!parse_search_parameters("unknown=1", parsed, error));
    CHECK(error.find("unknown") != std::string::npos);
    CHECK(!parse_search_parameters("rfp_base=999", parsed, error));
    CHECK(error.find("out-of-range") != std::string::npos);
    CHECK(!parse_search_parameters(
        "quiet_pruning_max_depth=4,tactical_pruning_max_depth=5", parsed, error));
    CHECK(error.find("tactical_pruning") != std::string::npos);
    CHECK(!parse_search_parameters("rfp_base=80,", parsed, error));
    CHECK(error.find("name=value") != std::string::npos);

    CHECK(parse_search_parameters("rfp_base=80", parsed, error));
    CHECK(error.empty());
}

}  // namespace
}  // namespace blaze
