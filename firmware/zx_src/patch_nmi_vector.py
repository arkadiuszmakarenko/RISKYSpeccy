#!/usr/bin/env python3
import re
import sys
from pathlib import Path


def parse_symbol_addr(map_text: str, symbol: str) -> int:
    pattern = re.compile(rf"^{re.escape(symbol)}\s*=\s*\$([0-9A-Fa-f]+)", re.MULTILINE)
    match = pattern.search(map_text)
    if not match:
        raise ValueError(f"Symbol '{symbol}' not found in map")
    return int(match.group(1), 16)


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: patch_nmi_vector.py <bin> <map> [symbol]", file=sys.stderr)
        return 2

    bin_path = Path(sys.argv[1])
    map_path = Path(sys.argv[2])
    symbol = sys.argv[3] if len(sys.argv) >= 4 else "nmi_irq_wrapper"

    data = bytearray(bin_path.read_bytes())
    if len(data) < 0x69:
        raise ValueError("Binary is too small to contain NMI vector at 0x0066")

    map_text = map_path.read_text(encoding="ascii", errors="replace")
    addr = parse_symbol_addr(map_text, symbol)

    if not (0 <= addr <= 0xFFFF):
        raise ValueError(f"Invalid symbol address: 0x{addr:X}")

    # Z80 NMI vector at 0x0066: JP <addr>
    data[0x66] = 0xC3
    data[0x67] = addr & 0xFF
    data[0x68] = (addr >> 8) & 0xFF

    bin_path.write_bytes(data)
    print(f"Patched NMI vector at 0x0066 -> JP 0x{addr:04X} ({symbol})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
