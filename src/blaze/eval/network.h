#ifndef BLAZE_EVAL_NETWORK_H
#define BLAZE_EVAL_NETWORK_H

#include "blaze/core/position.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace blaze {

[[nodiscard]] int sf_nnue_public_score(int raw_network_output);

class NnueThreadState final {
public:
    NnueThreadState(NnueThreadState&&) noexcept;
    NnueThreadState& operator=(NnueThreadState&&) noexcept;
    ~NnueThreadState();

    NnueThreadState(const NnueThreadState&) = delete;
    NnueThreadState& operator=(const NnueThreadState&) = delete;

    void reset(const Position& position);
    void push(const Position& position_after, const StateInfo& move_state);
    void pop();
    [[nodiscard]] int evaluate(const Position& position) const;
    [[nodiscard]] int raw_evaluate(const Position& position) const;

private:
    struct Impl;
    explicit NnueThreadState(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
    friend class NetworkEvaluator;
};

class NetworkEvaluator final {
public:
    static std::optional<NetworkEvaluator> create(
        std::string_view path, std::string& error);

    NetworkEvaluator(NetworkEvaluator&& other) noexcept;
    NetworkEvaluator& operator=(NetworkEvaluator&& other) noexcept;
    ~NetworkEvaluator();

    [[nodiscard]] int evaluate(const Position& position) const;
    [[nodiscard]] int raw_evaluate(const Position& position) const;
    [[nodiscard]] NnueThreadState make_thread_state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    NetworkEvaluator() = default;
};

}  // namespace blaze

#endif  // BLAZE_EVAL_NETWORK_H
