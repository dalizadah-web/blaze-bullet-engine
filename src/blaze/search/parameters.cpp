#include "blaze/search/parameters.h"

#include <charconv>
#include <cstddef>
#include <string>
#include <unordered_set>

namespace blaze {
namespace {

const SearchParameterDescriptor* descriptor_for(std::string_view name) {
    for (const auto& descriptor : search_parameter_descriptors) {
        if (descriptor.field != nullptr && descriptor.name == name) return &descriptor;
    }
    return nullptr;
}

bool validate_relations(const SearchParameters& parameters, std::string& error) {
    if (parameters.null_verify_depth <= parameters.null_min_depth) {
        error = "null_verify_depth must exceed null_min_depth";
        return false;
    }
    if (parameters.singular_double_depth < parameters.singular_min_depth) {
        error = "singular_double_depth must not be below singular_min_depth";
        return false;
    }
    if (parameters.tactical_pruning_max_depth > parameters.quiet_pruning_max_depth) {
        error = "tactical_pruning_max_depth must not exceed quiet_pruning_max_depth";
        return false;
    }
    return true;
}

}  // namespace

bool parse_search_parameters(
    std::string_view payload, SearchParameters& output, std::string& error) {
    error.clear();
    if (payload.empty()) {
        error = "SPSA Params payload must not be empty";
        return false;
    }
    if (payload.back() == ',') {
        error = "SPSA Params entries must use name=value";
        return false;
    }
    SearchParameters parsed;
    std::unordered_set<std::string> seen;
    std::size_t begin = 0;
    while (begin < payload.size()) {
        const std::size_t comma = payload.find(',', begin);
        const std::string_view assignment = payload.substr(
            begin, comma == std::string_view::npos ? payload.size() - begin : comma - begin);
        const std::size_t equal = assignment.find('=');
        if (equal == std::string_view::npos || equal == 0 || equal + 1 >= assignment.size()) {
            error = "SPSA Params entries must use name=value";
            return false;
        }
        const std::string_view name = assignment.substr(0, equal);
        const std::string_view value_text = assignment.substr(equal + 1);
        const auto* descriptor = descriptor_for(name);
        if (descriptor == nullptr) {
            error = "unknown SPSA parameter: " + std::string(name);
            return false;
        }
        if (!seen.insert(std::string(name)).second) {
            error = "duplicate SPSA parameter: " + std::string(name);
            return false;
        }
        int value = 0;
        const auto result = std::from_chars(
            value_text.data(), value_text.data() + value_text.size(), value);
        if (result.ec != std::errc{} || result.ptr != value_text.data() + value_text.size()) {
            error = "invalid integer for SPSA parameter: " + std::string(name);
            return false;
        }
        if (value < descriptor->minimum || value > descriptor->maximum) {
            error = "out-of-range SPSA parameter: " + std::string(name);
            return false;
        }
        parsed.*(descriptor->field) = value;
        begin = comma == std::string_view::npos ? payload.size() : comma + 1;
    }
    if (!validate_relations(parsed, error)) return false;
    output = parsed;
    error.clear();
    return true;
}

std::string serialize_search_parameters(const SearchParameters& parameters) {
    std::string result;
    for (const auto& descriptor : search_parameter_descriptors) {
        if (descriptor.field == nullptr) continue;
        if (!result.empty()) result.push_back(',');
        result += descriptor.name;
        result.push_back('=');
        result += std::to_string(parameters.*(descriptor.field));
    }
    return result;
}

}  // namespace blaze
