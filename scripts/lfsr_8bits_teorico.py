#!/usr/bin/env python3
"""Histograma 256x256 de palabras formadas por un LFSR de 8 bits."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


DEFAULT_SEED = 0xAB
DEFAULT_WORDS = 255
TAPS = (7, 5, 4, 3)
AXIS_VALUES = np.array([0, 31, 63, 95, 127, 159, 191, 223, 255])


def lfsr_next(state: int) -> int:
    """Desplaza a la izquierda e inserta el XOR de los taps en el bit 0."""
    feedback = 0
    for bit in TAPS:
        feedback ^= (state >> bit) & 1
    return ((state << 1) & 0xFF) | feedback


def generate_words(seed: int, word_count: int) -> tuple[np.ndarray, np.ndarray]:
    """Forma cada palabra con dos salidas consecutivas: [byte alto, byte bajo]."""
    if not 1 <= seed <= 0xFF:
        raise ValueError("La seed debe estar entre 0x01 y 0xFF")
    if word_count <= 0:
        raise ValueError("La cantidad de palabras debe ser mayor que cero")

    bytes_generated = np.empty(word_count * 2, dtype=np.uint8)
    state = seed

    for index in range(bytes_generated.size):
        bytes_generated[index] = state
        state = lfsr_next(state)

    high_bytes = bytes_generated[0::2].astype(np.uint16)
    low_bytes = bytes_generated[1::2].astype(np.uint16)
    words = (high_bytes << 8) | low_bytes
    return words, bytes_generated


def find_byte_period(seed: int) -> int:
    state = lfsr_next(seed)
    period = 1

    while state != seed:
        state = lfsr_next(state)
        period += 1
        if period > 255:
            raise RuntimeError("El LFSR no regresó a la seed en 255 pasos")

    return period


def plot_histogram(
    counts: np.ndarray,
    word_count: int,
    seed: int,
    byte_period: int,
    overlay_function: bool,
) -> plt.Figure:
    figure, axis = plt.subplots(figsize=(10.5, 9))

    image = axis.imshow(
        counts,
        origin="lower",
        cmap="viridis",
        interpolation="nearest",
        extent=(-0.5, 255.5, -0.5, 255.5),
        vmin=0,
    )

    axis.set_title(
        "Distribución teórica de pares consecutivos del LFSR\n"
        f"seed=0x{seed:02X}, taps=(7, 5, 4, 3), "
        f"palabras={word_count:,}, período LFSR={byte_period}",
        fontweight="bold",
    )
    axis.set_xlabel(
        "Segundo byte: 8 bits menos significativos (byte bajo)",
        fontweight="bold",
    )
    axis.set_ylabel(
        "Primer byte: 8 bits más significativos (byte alto)",
        fontweight="bold",
    )
    axis.set_xticks(AXIS_VALUES, labels=[str(value) for value in AXIS_VALUES])
    axis.set_yticks(AXIS_VALUES, labels=[str(value) for value in AXIS_VALUES])
    axis.grid(which="major", color="white", linewidth=0.4, alpha=0.35)

    if overlay_function:
        current_states = np.arange(1, 256)
        next_states = np.array(
            [lfsr_next(int(state)) for state in current_states]
        )
        for start, stop, label in (
            (0, 127, r"$x=\mathrm{LFSR}(y)$"),
            (127, 255, None),
        ):
            axis.plot(
                next_states[start:stop],
                current_states[start:stop],
                color="#ff3b30",
                linewidth=1.2,
                alpha=0.9,
                label=label,
                zorder=3,
            )
        axis.legend(loc="upper left", framealpha=0.95)

    colorbar = figure.colorbar(image, ax=axis, pad=0.02)
    colorbar.set_label("Número de apariciones", fontweight="bold")

    figure.tight_layout()
    return figure


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Genera un LFSR de 8 bits, agrupa cada dos salidas consecutivas "
            "como una palabra de 16 bits y grafica una matriz 256x256."
        )
    )
    parser.add_argument(
        "--seed",
        type=lambda value: int(value, 0),
        default=DEFAULT_SEED,
        help="estado inicial en decimal o hexadecimal; por defecto: 0xAB",
    )
    parser.add_argument(
        "--words",
        "--samples",
        dest="word_count",
        type=int,
        default=DEFAULT_WORDS,
        help="cantidad de palabras de 16 bits generadas; por defecto: 255",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="guarda la gráfica, por ejemplo lfsr_teorico.png",
    )
    parser.add_argument(
        "--binary-output",
        type=Path,
        help="guarda los bytes consecutivos del LFSR en un archivo .bin",
    )
    parser.add_argument(
        "--no-show",
        action="store_true",
        help="no abre la ventana interactiva",
    )
    parser.add_argument(
        "--no-overlay-function",
        action="store_true",
        help="no superpone la función de transición x=LFSR(y)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    words, bytes_generated = generate_words(args.seed, args.word_count)
    counts = np.bincount(words, minlength=65536).reshape(256, 256)
    byte_period = find_byte_period(args.seed)
    figure = plot_histogram(
        counts,
        args.word_count,
        args.seed,
        byte_period,
        overlay_function=not args.no_overlay_function,
    )

    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(args.output, dpi=180, bbox_inches="tight")
        print(f"Gráfica guardada en: {args.output}")

    if args.binary_output is not None:
        args.binary_output.parent.mkdir(parents=True, exist_ok=True)
        bytes_generated.tofile(args.binary_output)
        print(f"Secuencia guardada en: {args.binary_output}")

    byte_preview = " ".join(f"{value:02X}" for value in bytes_generated[:16])
    word_preview = " ".join(f"{value:04X}" for value in words[:8])
    print(f"Seed:                  0x{args.seed:02X}")
    print("Taps de feedback:      bits 7, 5, 4 y 3")
    print("Desplazamiento:        izquierda; feedback entra por el bit 0")
    print(f"Período del LFSR:      {byte_period}")
    print(f"Palabras generadas:    {args.word_count:,}")
    print(f"Bytes generados:       {bytes_generated.size:,}")
    print(f"Primeros bytes:        {byte_preview}")
    print(f"Primeras palabras:     {word_preview}")

    if not args.no_show:
        plt.show()
    else:
        plt.close(figure)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
