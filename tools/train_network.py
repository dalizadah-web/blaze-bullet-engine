#!/usr/bin/env python3
"""Train a small, independent Blaze NNUE-style evaluator.

The trainer uses only generated legal positions and a transparent classical
teacher. It does not read Stockfish source, networks, books, or tablebases.
The output is an optional local resource; no network is shipped by default.
"""

from __future__ import annotations

import argparse
import random
import struct
from pathlib import Path

import chess
import numpy as np

FEATURES = 768
HIDDEN = 256
INPUT_BYTES = FEATURES * HIDDEN * 2
HIDDEN_BIAS_BYTES = HIDDEN * 4
OUTPUT_BYTES = HIDDEN * 2
PAYLOAD_BYTES = INPUT_BYTES + HIDDEN_BIAS_BYTES + OUTPUT_BYTES + 4


def teacher(board: chess.Board) -> float:
    values = {
        chess.PAWN: 100.0,
        chess.KNIGHT: 320.0,
        chess.BISHOP: 335.0,
        chess.ROOK: 500.0,
        chess.QUEEN: 900.0,
        chess.KING: 0.0,
    }
    score = 0.0
    for square, piece in board.piece_map().items():
        rank = chess.square_rank(square)
        forward = rank if piece.color else 7 - rank
        center = abs(chess.square_file(square) - 3.5) + abs(rank - 3.5)
        positional = forward * 4.0 - center * 1.5
        term = values[piece.piece_type] + positional
        score += term if piece.color else -term
    return score if board.turn else -score


def feature_indices(board: chess.Board) -> list[int]:
    result: list[int] = []
    for square, piece in board.piece_map().items():
        piece_index = (piece.piece_type - 1) + (0 if piece.color else 6)
        result.append(piece_index * 64 + square)
    return result


def make_dataset(samples: int, seed: int) -> tuple[list[list[int]], np.ndarray]:
    rng = random.Random(seed)
    features: list[list[int]] = []
    labels: list[float] = []
    for _ in range(samples):
        board = chess.Board()
        for _ in range(rng.randrange(0, 80)):
            if board.is_game_over(claim_draw=True):
                break
            board.push(rng.choice(list(board.legal_moves)))
        features.append(feature_indices(board))
        labels.append(teacher(board))
    return features, np.asarray(labels, dtype=np.float32)


def train(features: list[list[int]], labels: np.ndarray, epochs: int, seed: int) -> tuple[np.ndarray, np.ndarray, np.ndarray, float]:
    rng = np.random.default_rng(seed)
    input_weights = rng.normal(0.0, 0.015, (FEATURES, HIDDEN)).astype(np.float32)
    hidden_bias = np.zeros(HIDDEN, dtype=np.float32)
    output_weights = rng.normal(0.0, 0.02, HIDDEN).astype(np.float32)
    output_bias = np.asarray([float(labels.mean())], dtype=np.float32)
    batch_size = 128
    learning_rate = 0.003

    order = np.arange(len(labels))
    for _ in range(epochs):
        rng.shuffle(order)
        for begin in range(0, len(order), batch_size):
            batch = order[begin:begin + batch_size]
            pre = np.empty((len(batch), HIDDEN), dtype=np.float32)
            for row, index in enumerate(batch):
                pre[row] = hidden_bias + input_weights[features[index]].sum(axis=0)
            hidden = np.maximum(pre, 0.0)
            prediction = hidden @ output_weights + output_bias[0]
            error = (prediction - labels[batch]) * (2.0 / len(batch))
            grad_output = hidden.T @ error
            grad_hidden = error[:, None] * output_weights[None, :] * (pre > 0.0)
            grad_input = np.zeros_like(input_weights)
            for row, index in enumerate(batch):
                np.add.at(grad_input, features[index], grad_hidden[row])
            input_weights -= learning_rate * grad_input
            hidden_bias -= learning_rate * grad_hidden.sum(axis=0)
            output_weights -= learning_rate * grad_output
            output_bias[0] -= learning_rate * error.sum()
    return input_weights, hidden_bias, output_weights, float(output_bias[0])


def quantized_payload(input_weights: np.ndarray, hidden_bias: np.ndarray, output_weights: np.ndarray, output_bias: float) -> bytes:
    input_q = np.clip(np.rint(input_weights * 64.0), -32768, 32767).astype("<i2")
    hidden_q = np.rint(hidden_bias * 64.0).astype("<i4")
    output_q = np.clip(np.rint(output_weights * 64.0), -32768, 32767).astype("<i2")
    bias_q = np.asarray([round(output_bias * 4096.0)], dtype="<i4")
    payload = input_q.tobytes() + hidden_q.tobytes() + output_q.tobytes() + bias_q.tobytes()
    if len(payload) != PAYLOAD_BYTES:
        raise RuntimeError(f"unexpected payload size: {len(payload)}")
    return payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=8000)
    parser.add_argument("--epochs", type=int, default=4)
    parser.add_argument("--seed", type=int, default=20260716)
    args = parser.parse_args()
    features, labels = make_dataset(args.samples, args.seed)
    weights = train(features, labels, args.epochs, args.seed)
    payload = quantized_payload(*weights)
    checksum = 1469598103934665603
    for byte in payload:
        checksum ^= byte
        checksum = (checksum * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    header = struct.pack("<8sIIIIQ", b"BLAZENET", 1, FEATURES, HIDDEN, len(payload), checksum)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + payload)
    print(f"wrote {args.output} ({len(payload)} payload bytes, seed={args.seed})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
