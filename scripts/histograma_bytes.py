#!/usr/bin/env python3
"""Genera mapas de calor para valores de 8 o 16 bits almacenados en binario."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import LogNorm


CHUNK_BYTES = 16 * 1024 * 1024


def histogram(
    path: Path,
    word_size: int,
    endian: str,
) -> tuple[np.ndarray, int, int]:
    if not path.is_file():
        raise FileNotFoundError(f"No se encontró el archivo: {path}")

    if word_size == 8:
        dtype = np.dtype(np.uint8)
        value_count = 256
        matrix_side = 16
    else:
        dtype = np.dtype(">u2" if endian == "big" else "<u2")
        value_count = 65536
        matrix_side = 256

    counts = np.zeros(value_count, dtype=np.uint64)
    total_values = 0
    trailing_bytes = 0
    item_size = dtype.itemsize
    chunk_items = max(1, CHUNK_BYTES // item_size)

    with path.open("rb") as file:
        while True:
            values = np.fromfile(file, dtype=dtype, count=chunk_items)
            if values.size == 0:
                break

            counts += np.bincount(values, minlength=value_count).astype(
                np.uint64,
                copy=False,
            )
            total_values += int(values.size)

    if word_size == 16:
        trailing_bytes = path.stat().st_size % 2

    return counts.reshape(matrix_side, matrix_side), total_values, trailing_bytes


def axis_ticks(word_size: int) -> tuple[np.ndarray, list[str]]:
    if word_size == 8:
        values = np.arange(16)
        return values, [f"{value:X}" for value in values]

    values = np.array([0, 31, 63, 95, 127, 159, 191, 223, 255])
    return values, [str(value) for value in values]


def plot_histogram(
    counts: np.ndarray,
    total_values: int,
    input_path: Path,
    *,
    word_size: int,
    endian: str,
    logarithmic: bool,
    annotate: bool,
) -> plt.Figure:
    figure, axis = plt.subplots(figsize=(10.5, 9))

    norm = None
    if logarithmic:
        positive_counts = counts[counts > 0]
        if positive_counts.size:
            norm = LogNorm(
                vmin=max(1, int(positive_counts.min())),
                vmax=max(1, int(positive_counts.max())),
            )

    image = axis.imshow(
        counts,
        origin="lower",
        cmap="viridis",
        interpolation="nearest",
        norm=norm,
        extent=(-0.5, counts.shape[1] - 0.5, -0.5, counts.shape[0] - 0.5),
    )

    if word_size == 8:
        title = "Distribución de bytes"
        total_label = f"{total_values:,} bytes"
        x_label = "4 bits menos significativos (nibble bajo)"
        y_label = "4 bits más significativos (nibble alto)"
    else:
        title = "Distribución de palabras de 16 bits"
        total_label = f"{total_values:,} palabras ({endian}-endian)"
        x_label = "8 bits menos significativos (byte bajo)"
        y_label = "8 bits más significativos (byte alto)"

    axis.set_title(
        f"{title}: {input_path.name}\nTotal analizado: {total_label}",
        fontweight="bold",
    )
    axis.set_xlabel(x_label, fontweight="bold")
    axis.set_ylabel(y_label, fontweight="bold")

    tick_values, tick_labels = axis_ticks(word_size)
    axis.set_xticks(tick_values, labels=tick_labels)
    axis.set_yticks(tick_values, labels=tick_labels)

    if word_size == 8:
        axis.set_xticks(np.arange(-0.5, 16, 1), minor=True)
        axis.set_yticks(np.arange(-0.5, 16, 1), minor=True)
        axis.grid(which="minor", color="white", linewidth=0.45, alpha=0.5)
        axis.tick_params(which="minor", bottom=False, left=False)
    else:
        axis.grid(which="major", color="white", linewidth=0.4, alpha=0.35)

    colorbar = figure.colorbar(image, ax=axis, pad=0.02)
    colorbar.set_label("Número de apariciones", fontweight="bold")

    if annotate:
        if word_size == 16:
            raise ValueError(
                "--annotate solamente está disponible para la matriz 16x16 "
                "del modo de 8 bits"
            )

        threshold = (float(counts.min()) + float(counts.max())) / 2.0
        for high_nibble in range(16):
            for low_nibble in range(16):
                count = int(counts[high_nibble, low_nibble])
                text_color = "white" if count <= threshold else "black"
                axis.text(
                    low_nibble,
                    high_nibble,
                    f"{count:,}",
                    ha="center",
                    va="center",
                    color=text_color,
                    fontsize=6,
                )

    figure.tight_layout()
    return figure


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Grafica la frecuencia de valores de 8 bits en una matriz 16x16 "
            "o de valores de 16 bits en una matriz 256x256."
        )
    )
    parser.add_argument("input", type=Path, help="archivo .bin que se analizará")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="guarda la gráfica, por ejemplo histograma.png",
    )
    parser.add_argument(
        "--word-size",
        type=int,
        choices=(8, 16),
        default=8,
        help="tamaño de cada valor almacenado; por defecto: 8",
    )
    parser.add_argument(
        "--endian",
        choices=("big", "little"),
        default="big",
        help="orden de bytes para palabras de 16 bits; por defecto: big",
    )
    parser.add_argument(
        "--log",
        action="store_true",
        help="usa escala logarítmica en la paleta de colores",
    )
    parser.add_argument(
        "--annotate",
        action="store_true",
        help="escribe el conteo en cada celda; solamente para 8 bits",
    )
    parser.add_argument(
        "--no-show",
        action="store_true",
        help="no abre la ventana interactiva",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    counts, total_values, trailing_bytes = histogram(
        args.input,
        args.word_size,
        args.endian,
    )
    figure = plot_histogram(
        counts,
        total_values,
        args.input,
        word_size=args.word_size,
        endian=args.endian,
        logarithmic=args.log,
        annotate=args.annotate,
    )

    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(args.output, dpi=180, bbox_inches="tight")
        print(f"Gráfica guardada en: {args.output}")

    value_digits = args.word_size // 4
    unit = "bytes" if args.word_size == 8 else "palabras de 16 bits"
    print(f"Valores analizados: {total_values:,} {unit}")
    print(
        f"Valor menos frecuente: 0x{int(np.argmin(counts)):0{value_digits}X} "
        f"({int(counts.min()):,})"
    )
    print(
        f"Valor más frecuente:  0x{int(np.argmax(counts)):0{value_digits}X} "
        f"({int(counts.max()):,})"
    )

    if trailing_bytes:
        print(
            "Advertencia: se ignoró 1 byte final porque el archivo no contiene "
            "un número par de bytes."
        )

    if not args.no_show:
        plt.show()
    else:
        plt.close(figure)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
