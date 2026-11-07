#!/usr/bin/env python3
"""Resume dumps PPM do GS sem dependências externas."""

import argparse
from collections import Counter
from pathlib import Path


def read_ppm(path):
    data = path.read_bytes()
    position = 0
    tokens = []
    while len(tokens) < 4:
        while position < len(data) and data[position] in b" \t\r\n":
            position += 1
        if position < len(data) and data[position] == ord("#"):
            position = data.index(b"\n", position) + 1
            continue
        start = position
        while position < len(data) and data[position] not in b" \t\r\n":
            position += 1
        if start == position:
            raise ValueError("cabeçalho PPM incompleto")
        tokens.append(data[start:position])
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise ValueError("esperado PPM P6 com canais de 8 bits")
    width, height = map(int, tokens[1:3])
    if position >= len(data) or data[position] not in b" \t\r\n":
        raise ValueError("separador do raster ausente")
    position += 1
    raster = data[position:]
    if width <= 0 or height <= 0 or len(raster) != width * height * 3:
        raise ValueError("tamanho do raster incompatível com o cabeçalho")
    return width, height, raster


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dumps", type=Path, nargs="+")
    parser.add_argument(
        "--reject-flat-gray", action="store_true",
        help="retorna 1 se um dump contém apenas preto e cinza RGB(128,128,128)",
    )
    args = parser.parse_args()
    failed = False
    for path in args.dumps:
        try:
            width, height, raster = read_ppm(path)
        except (OSError, ValueError) as error:
            parser.exit(2, f"{path}: {error}\n")
        colors = Counter(zip(raster[::3], raster[1::3], raster[2::3]))
        nonblack = width * height - colors[(0, 0, 0)]
        flat_gray = colors[(128, 128, 128)] > 0 and set(colors) <= {
            (0, 0, 0), (128, 128, 128),
        }
        print(f"{path.name}: {width}x{height}, cores={len(colors)}, "
              f"não pretos={nonblack}, faixa cinza={flat_gray}")
        print(f"  cores dominantes: {colors.most_common(5)}")
        failed |= args.reject_flat_gray and flat_gray
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
