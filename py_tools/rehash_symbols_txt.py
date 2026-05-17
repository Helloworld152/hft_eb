#!/usr/bin/env python3
import argparse
from pathlib import Path

import xxhash


def normalize_symbol(symbol):
    return symbol.strip()


def rehash_symbols(lines):
    seen_ids = {}
    output = []

    for lineno, raw_line in enumerate(lines, start=1):
        line = raw_line.rstrip("\n")
        stripped = line.strip()

        if not stripped or stripped.startswith("#"):
            output.append(raw_line)
            continue

        parts = line.split(":", 2)
        if len(parts) < 2:
            raise ValueError(f"line {lineno}: invalid format: {line}")

        symbol = normalize_symbol(parts[1])
        multiplier = parts[2].strip() if len(parts) == 3 else "1"
        symbol_id = xxhash.xxh64(symbol).intdigest()

        prev_symbol = seen_ids.get(symbol_id)
        if prev_symbol is not None and prev_symbol != symbol:
            raise ValueError(
                f"line {lineno}: hash collision between '{prev_symbol}' and '{symbol}' -> {symbol_id}"
            )

        seen_ids[symbol_id] = symbol
        output.append(f"{symbol_id}:{symbol}:{multiplier}\n")

    return output


def main():
    parser = argparse.ArgumentParser(description="Rehash an existing symbols.txt with xxHash64 ids")
    parser.add_argument(
        "input",
        nargs="?",
        default="/home/rying/hft_eb/conf/symbols.txt",
        help="Input symbols txt path",
    )
    parser.add_argument("-o", "--output", help="Output path, defaults to overwrite input")
    args = parser.parse_args()

    input_path = Path(args.input)
    output_path = Path(args.output) if args.output else input_path

    lines = input_path.read_text(encoding="utf-8").splitlines(keepends=True)
    rewritten = rehash_symbols(lines)
    output_path.write_text("".join(rewritten), encoding="utf-8")

    print(f"Rehashed {len(rewritten)} lines from {input_path} to {output_path}")


if __name__ == "__main__":
    main()
