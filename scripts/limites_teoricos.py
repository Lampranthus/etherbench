#!/usr/bin/env python3
"""Overlay 1GbE sweep measurements on theoretical goodput and PPS limits."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.lines import Line2D
from matplotlib.patches import Rectangle


DEFAULT_INPUTS = Path("results/device_comparison_256_1472_64/comparison_inputs.csv")
LINK_MBPS = 1000.0
ETHERNET_OVERHEAD_BYTES = 66
MIN_WIRE_BYTES = 84
COLORS = (
    "#1f77b4",
    "#d62728",
    "#2ca02c",
    "#9467bd",
    "#ff7f0e",
    "#17becf",
    "#8c564b",
    "#e377c2",
)


@dataclass(frozen=True)
class ComparisonInput:
    label: str
    directory: Path


def display_name(value: str) -> str:
    return value.replace("_", " ")


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise FileNotFoundError(f"No se encontró el archivo: {path}")
    with path.open(newline="") as file:
        return list(csv.DictReader(file))


def load_inputs(path: Path) -> list[ComparisonInput]:
    inputs: list[ComparisonInput] = []
    for row in read_csv(path):
        label = row.get("label", "").strip()
        directory_text = row.get("result_directory", "").strip()
        if not label or not directory_text:
            raise ValueError(f"Entrada incompleta en {path}: {row}")

        directory = Path(directory_text).expanduser()
        if not directory.is_absolute():
            directory = (path.parent / directory).resolve()
        if not directory.is_dir():
            raise FileNotFoundError(f"No se encontró el directorio: {directory}")
        inputs.append(ComparisonInput(display_name(label), directory))

    if not inputs:
        raise ValueError(f"No hay interfaces registradas en {path}")
    return inputs


def theoretical_goodput_mbps(payload: np.ndarray) -> np.ndarray:
    wire_bytes = np.maximum(MIN_WIRE_BYTES, payload + ETHERNET_OVERHEAD_BYTES)
    return LINK_MBPS * payload / wire_bytes


def theoretical_pps(payload: np.ndarray) -> np.ndarray:
    wire_bytes = np.maximum(MIN_WIRE_BYTES, payload + ETHERNET_OVERHEAD_BYTES)
    return LINK_MBPS * 1_000_000.0 / (wire_bytes * 8.0)


def summary_values(
    path: Path,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    rows = read_csv(path)
    rows = sorted(rows, key=lambda row: int(row["payload_size"]))
    return (
        np.array([int(row["payload_size"]) for row in rows]),
        np.array([float(row["goodput_mbps_mean"]) for row in rows]),
        np.array([float(row.get("goodput_mbps_std") or 0.0) for row in rows]),
        np.array([float(row["pps_mean"]) for row in rows]),
        np.array([float(row.get("pps_std") or 0.0) for row in rows]),
    )


def loss_values(path: Path) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    rows = read_csv(path)
    rows = sorted(rows, key=lambda row: int(row["payload_size"]))
    return (
        np.array([int(row["payload_size"]) for row in rows]),
        np.array([float(row["lost_pct_mean"]) for row in rows]),
        np.array([float(row.get("lost_pct_std") or 0.0) for row in rows]),
    )


def plot_direction(
    inputs: list[ComparisonInput],
    summary_name: str,
    direction_title: str,
    goodput_axis: plt.Axes,
    *,
    show_goodput_label: bool,
    show_pps_label: bool,
) -> None:
    theoretical_payload = np.arange(64, 1501)
    pps_axis = goodput_axis.twinx()
    measured_payloads: set[int] = set()

    goodput_axis.plot(
        theoretical_payload,
        theoretical_goodput_mbps(theoretical_payload),
        color="#111111",
        linewidth=2.4,
        zorder=2,
    )
    pps_axis.plot(
        theoretical_payload,
        theoretical_pps(theoretical_payload),
        color="#111111",
        linewidth=2.4,
        linestyle=":",
        zorder=2,
    )

    for index, item in enumerate(inputs):
        color = COLORS[index % len(COLORS)]
        payload, goodput, goodput_std, pps, pps_std = summary_values(
            item.directory / summary_name
        )
        measured_payloads.update(int(value) for value in payload)

        goodput_axis.errorbar(
            payload,
            goodput,
            yerr=goodput_std,
            color=color,
            linestyle="-",
            marker="o",
            markersize=4,
            linewidth=2,
            elinewidth=1,
            capsize=2,
            alpha=1,
            zorder=4,
        )
        pps_axis.errorbar(
            payload,
            pps,
            yerr=pps_std,
            color=color,
            linestyle="-",
            marker="s",
            markersize=4,
            linewidth=2,
            elinewidth=1,
            capsize=2,
            alpha=1,
            zorder=4,
        )

    goodput_axis.set_title(
        direction_title,
        fontsize=13,
        fontweight="bold",
        pad=10,
    )
    if show_goodput_label:
        goodput_axis.set_ylabel("Transferencia efectiva (Mbps)", fontweight="bold")
    if show_pps_label:
        pps_axis.set_ylabel("Paquetes por segundo (PPS)", fontweight="bold")
    goodput_axis.set_xlim(64, 1500)
    goodput_axis.set_ylim(bottom=0)
    pps_axis.set_ylim(bottom=0)
    goodput_axis.grid(True, which="major", color="#8d99a6", linestyle="--", linewidth=0.8, alpha=0.75)
    goodput_axis.grid(True, which="minor", color="#c8cdd2", linestyle=":", linewidth=0.55, alpha=0.65)
    goodput_axis.minorticks_on()
    pps_axis.ticklabel_format(axis="y", style="plain", useOffset=False)
    pps_axis.yaxis.set_major_formatter(
        plt.FuncFormatter(lambda value, _: f"{value / 1_000_000:.2f} M")
    )

    ticks = [64, *sorted(measured_payloads)]
    goodput_axis.set_xticks(sorted(set(ticks)))
    goodput_axis.tick_params(axis="x", rotation=45)


def plot_loss(
    inputs: list[ComparisonInput],
    summary_name: str,
    direction_title: str,
    loss_axis: plt.Axes,
    *,
    show_ylabel: bool,
) -> None:
    measured_payloads: set[int] = set()
    loss_axis.axhline(
        0,
        color="#111111",
        linewidth=2,
        linestyle=":",
        zorder=2,
    )

    for index, item in enumerate(inputs):
        payload, loss, loss_std = loss_values(item.directory / summary_name)
        measured_payloads.update(int(value) for value in payload)
        loss_axis.errorbar(
            payload,
            loss,
            yerr=loss_std,
            color=COLORS[index % len(COLORS)],
            linestyle="-",
            marker="^",
            markersize=4,
            linewidth=2,
            elinewidth=1,
            capsize=2,
            alpha=1,
            zorder=4,
    )

    if direction_title:
        loss_axis.set_title(direction_title, fontsize=13, fontweight="bold", pad=10)
    loss_axis.set_xlabel("Tamaño del payload UDP (bytes)", fontweight="bold")
    if show_ylabel:
        loss_axis.set_ylabel("Pérdida de paquetes (%)", fontweight="bold")
    loss_axis.set_xlim(64, 1500)
    loss_axis.set_ylim(bottom=0)
    loss_axis.grid(
        True,
        which="major",
        color="#8d99a6",
        linestyle="--",
        linewidth=0.8,
        alpha=0.75,
    )
    loss_axis.grid(
        True,
        which="minor",
        color="#c8cdd2",
        linestyle=":",
        linewidth=0.55,
        alpha=0.65,
    )
    loss_axis.minorticks_on()
    ticks = [64, *sorted(measured_payloads)]
    loss_axis.set_xticks(sorted(set(ticks)))
    loss_axis.tick_params(axis="x", rotation=45)


def plot_theoretical_limits() -> plt.Figure:
    payload = np.arange(64, 1501)
    fig, goodput_axis = plt.subplots(figsize=(11, 6.5))
    pps_axis = goodput_axis.twinx()

    goodput_axis.plot(
        payload,
        theoretical_goodput_mbps(payload),
        color="#111111",
        linewidth=2.5,
        label="Goodput límite teórico",
    )
    pps_axis.plot(
        payload,
        theoretical_pps(payload),
        color="#1f77b4",
        linewidth=2.5,
        linestyle=":",
        label="PPS límite teórico",
    )

    fig.suptitle(
        "Límites teóricos Ethernet UDP a 1 Gb/s",
        fontsize=15,
        fontweight="bold",
    )
    goodput_axis.set_xlabel("Tamaño del payload UDP (bytes)", fontweight="bold")
    goodput_axis.set_ylabel("Transferencia efectiva (Mbps)", fontweight="bold")
    pps_axis.set_ylabel("Paquetes por segundo (PPS)", fontweight="bold")

    goodput_axis.set_xlim(64, 1500)
    goodput_axis.set_ylim(bottom=0)
    pps_axis.set_ylim(bottom=0)
    goodput_axis.set_xticks([64, 256, 512, 768, 1024, 1280, 1440, 1500])
    goodput_axis.grid(
        True,
        which="major",
        color="#8d99a6",
        linestyle="--",
        linewidth=0.8,
        alpha=0.75,
    )
    goodput_axis.grid(
        True,
        which="minor",
        color="#c8cdd2",
        linestyle=":",
        linewidth=0.55,
        alpha=0.65,
    )
    goodput_axis.minorticks_on()
    pps_axis.ticklabel_format(axis="y", style="plain", useOffset=False)
    pps_axis.yaxis.set_major_formatter(
        plt.FuncFormatter(lambda value, _: f"{value / 1_000_000:.2f} M")
    )

    handles = [
        Line2D([0], [0], color="#111111", linewidth=2.5, label="Goodput"),
        Line2D(
            [0],
            [0],
            color="#1f77b4",
            linewidth=2.5,
            linestyle=":",
            label="PPS",
        ),
    ]
    fig.legend(
        handles=handles,
        title="Métrica",
        loc="upper center",
        bbox_to_anchor=(0.5, 0.93),
        ncol=2,
        fontsize=9,
        framealpha=1,
    )
    fig.subplots_adjust(left=0.1, right=0.9, bottom=0.12, top=0.82)
    return fig


def plot_comparison(
    inputs: list[ComparisonInput],
    output: Path | None = None,
    *,
    show: bool = True,
) -> None:
    fig, axes = plt.subplots(2, 2, figsize=(20, 14))
    plot_direction(
        inputs,
        "loopback_summary.csv",
        "Interfaz a FPGA",
        axes[0, 0],
        show_goodput_label=True,
        show_pps_label=False,
    )
    plot_direction(
        inputs,
        "tx_summary.csv",
        "FPGA a interfaz",
        axes[0, 1],
        show_goodput_label=False,
        show_pps_label=True,
    )
    plot_loss(
        inputs,
        "loopback_summary.csv",
        "",
        axes[1, 0],
        show_ylabel=True,
    )
    plot_loss(
        inputs,
        "tx_summary.csv",
        "",
        axes[1, 1],
        show_ylabel=False,
    )

    interface_handles = [
        Line2D(
            [0],
            [0],
            color=COLORS[index % len(COLORS)],
            linewidth=3,
            label=item.label,
        )
        for index, item in enumerate(inputs)
    ]
    encoding_handles = [
        Line2D(
            [0],
            [0],
            color="#111111",
            linewidth=2.4,
            label="Goodput límite teórico",
        ),
        Line2D(
            [0],
            [0],
            color="#111111",
            linewidth=2.2,
            linestyle=":",
            label="PPS límite teórico",
        ),
        Line2D(
            [0],
            [0],
            color="#555555",
            marker="o",
            linestyle="-",
            label="Goodput medido",
        ),
        Line2D(
            [0],
            [0],
            color="#777777",
            marker="s",
            linestyle="-",
            label="PPS medidos",
        ),
        Line2D(
            [0],
            [0],
            color="#777777",
            marker="^",
            linestyle="-",
            label="Pérdidas medidas",
        ),
    ]
    fig.suptitle(
        "Rendimiento medido vs límite teórico Ethernet UDP a 1 Gb/s",
        fontsize=15,
        fontweight="bold",
        y=0.99,
    )
    fig.legend(
        handles=interface_handles,
        title="Interfaces",
        loc="upper center",
        bbox_to_anchor=(0.28, 0.96),
        ncol=2,
        fontsize=9,
        framealpha=1,
    )
    fig.legend(
        handles=encoding_handles,
        title="Métrica",
        loc="upper center",
        bbox_to_anchor=(0.73, 0.95),
        ncol=3,
        fontsize=9,
        framealpha=1,
    )
    fig.subplots_adjust(
        left=0.05,
        right=0.94,
        bottom=0.088,
        top=0.79,
        wspace=0.18,
        hspace=0.19,
    )
    for x, width in ((0.022, 0.476), (0.503, 0.477)):
        fig.add_artist(
            Rectangle(
                (x, 0.025),
                width,
                0.78,
                transform=fig.transFigure,
                fill=False,
                edgecolor="#000000",
                linewidth=1.5,
                zorder=-1,
            )
        )

    if output is not None:
        output.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(output, dpi=180)
        print(f"Gráfica guardada en: {output.resolve()}")

    theoretical_fig = plot_theoretical_limits()

    if show:
        plt.show()
    plt.close(theoretical_fig)
    plt.close(fig)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Superpone sweeps 1GbE sobre límites teóricos de goodput y PPS"
    )
    parser.add_argument("--inputs-csv", type=Path, default=DEFAULT_INPUTS)
    parser.add_argument(
        "--output",
        type=Path,
        help="guarda una copia de la figura; por defecto solo se abre la ventana",
    )
    parser.add_argument(
        "--no-show",
        action="store_true",
        help="no abre la ventana; útil junto con --output",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        plot_comparison(
            load_inputs(args.inputs_csv),
            args.output,
            show=not args.no_show,
        )
    except Exception as error:
        print(f"error: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
