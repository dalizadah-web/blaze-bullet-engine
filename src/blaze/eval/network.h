#ifndef BLAZE_EVAL_NETWORK_H
#define BLAZE_EVAL_NETWORK_H

#include "blaze/core/position.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace blaze {

class NetworkEvaluator final {
public:
    static std::optional<NetworkEvaluator> create(
        std::string_view path, std::string& error);

    NetworkEvaluator(NetworkEvaluator&& other) noexcept;
    NetworkEvaluator& operator=(NetworkEvaluator&& other) noexcept;
    ~NetworkEvaluator();

    int evaluate(const Position& position) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    NetworkEvaluator() = default;
};

}  // namespace blaze

#endif  // BLAZE_EVAL_NETWORK_H
