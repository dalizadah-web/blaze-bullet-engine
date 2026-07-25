#ifndef BLAZE_EVAL_STOCKFISH_BRIDGE_H
#define BLAZE_EVAL_STOCKFISH_BRIDGE_H

#include <string>
#include <string_view>

namespace blaze {

bool sf_nnue_init(std::string_view path, std::string& error);
void sf_nnue_destroy();
int  sf_nnue_evaluate(const std::string& fen);

}

#endif
