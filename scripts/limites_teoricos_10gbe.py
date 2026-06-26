#!/usr/bin/env python3
"""Overlay 10GbE sweep measurements on theoretical UDP goodput and PPS limits."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.lines import Line2D
from matplotlib.patches import Rectangle


DEFAULT_RESULTS_GLOB = "10gbe_*"
DEFAULT_LINK_MBPS = 10_000.0
ETHERNET_OVERHEAD_BYTES = 66
MIN_WIRE_BYTES = 84
COLORS = {
    "nic-to-corundum": "#1f77b4",
    "corundum-to-nic": "#d62728",
}
DIRECTION_TITLES = {
    "nic-to-corundum": "NIC a Corundum",
    "corundum-to-nic": "Corundum a NIC",
}
NONINTERACTIVE_BACKENDS = {"agg", "cairo", "pdf", "pgf", "ps", "svg", "template"}


@dataclass(frozen=True)
class DirectionData:
    direction: str
    payload: np.ndarray
    goodput: np.ndarray
    goodput_std: np.ndarray
    pps: np.ndarray
    pps_std: np.ndarray
    loss: np.ndarray
    loss_std: np.ndarray


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise FileNotFoundError(f"No se encontró el archivo: {path}")
    with path.open(newline="") as file:
        return list(csv.DictReader(file))


def latest_10gbe_result() -> Path:
    candidates = [
        path.parent
        for path in Path("results").glob(f"{DEFAULT_RESULTS_GLOB}/udp_summary.csv")
        if path.is_file()
    ]
    if not candidates:
        raise FileNotFoundError(
            "No encontré ningún results/10gbe_*/udp_summary.csv. "
            "Indica el directorio con --input-dir."
        )
    return max(candidates, key=lambda path: (path / "udp_summary.csv").stat().st_mtime)


def theoretical_goodput_mbps(payload: np.ndarray, link_mbps: float) -> np.ndarray:
    wire_bytes = np.maximum(MIN_WIRE_BYTES, payload + ETHERNET_OVERHEAD_BYTES)
    return link_mbps * payload / wire_bytes


def theoretical_pps(payload: np.ndarray, link_mbps: float) -> np.ndarray:
    wire_bytes = np.maximum(MIN_WIRE_BYTES, payload + ETHERNET_OVERHEAD_BYTES)
    return link_mbps * 1_000_000.0 / (wire_bytes * 8.0)


def load_direction_data(input_dir: Path) -> list[DirectionData]:
    rows = read_csv(input_dir / "udp_summary.csv")
    by_direction: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        direction = row.get("direction", "").strip()
        if not direction:
            continue
        by_direction.setdefault(direction, []).append(row)

    data: list[DirectionData] = []
    for direction in ("nic-to-corundum", "corundum-to-nic"):
        direction_rows = sorted(
            by_direction.get(direction, []),
            key=lambda row: int(row["payload_size"]),
        )
        if not direction_rows:
            continue
        data.append(
            DirectionData(
                direction=direction,
                payload=np.array([int(row["payload_size"]) for row in direction_rows]),
                goodput=np.array(
                    [float(row["goodput_mbps_mean"]) for row in direction_rows]
                ),
                goodput_std=np.array(
                    [
                        float(row.get("goodput_mbps_std") or 0.0)
                        for row in direction_rows
                    ]
                ),
                pps=np.array([float(row["pps_mean"]) for row in direction_rows]),
                pps_std=np.array(
                    [float(row.get("pps_std") or 0.0) for row in direction_rows]
                ),
                loss=np.array([float(row["lost_pct_mean"]) for row in direction_rows]),
                loss_std=np.array(
                    [float(row.get("lost_pct_std") or 0.0) for row in direction_rows]
                ),
            )
        )

    if not data:
        raise ValueError(
            f"{input_dir / 'udp_summary.csv'} no contiene direcciones UDP válidas"
        )
    return data


def decimal_mpps(value: float, _: int) -> str:
    return f"{value / 1_000_000:.2f} M"


def matplotlib_backend_is_noninteractive() -> bool:
    return plt.get_backend().lower() in NONINTERACTIVE_BACKENDS


def payload_ticks(data: list[DirectionData]) -> list[int]:
    measured = sorted({int(value) for item in data for value in item.payload})
    if not measured:
        return []
    ticks = [measured[0], *measured]
    if measured[-1] not in ticks:
        ticks.append(measured[-1])
    return sorted(set(ticks))


def plot_direction(
    item: DirectionData,
    axis: plt.Axes,
    *,
    link_mbps: float,
    x_range: np.ndarray,
    ticks: list[int],
    show_goodput_label: bool,
    show_pps_label: bool,
) -> None:
    color = COLORS.get(item.direction, "#1f77b4")
    pps_axis = axis.twinx()

    axis.plot(
        x_range,
        theoretical_goodput_mbps(x_range, link_mbps),
        color="#111111",
        linewidth=2.4,
        zorder=2,
    )
    pps_axis.plot(
        x_range,
        theoretical_pps(x_range, link_mbps),
        color="#111111",
        linewidth=2.4,
        linestyle=":",
        zorder=2,
    )
    axis.errorbar(
        item.payload,
        item.goodput,
        yerr=item.goodput_std,
        color=color,
        linestyle="-",
        marker="o",
        markersize=4,
        linewidth=2,
        elinewidth=1,
        capsize=2,
        zorder=4,
    )
    pps_axis.errorbar(
        item.payload,
        item.pps,
        yerr=item.pps_std,
        color=color,
        linestyle="-",
        marker="s",
        markersize=4,
        linewidth=2,
        elinewidth=1,
        capsize=2,
        zorder=4,
    )

    axis.set_title(
        DIRECTION_TITLES.get(item.direction, item.direction),
        fontsize=13,
        fontweight="bold",
        pad=10,
    )
    if show_goodput_label:
        axis.set_ylabel("Transferencia efectiva (Mbps)", fontweight="bold")
    if show_pps_label:
        pps_axis.set_ylabel("Paquetes por segundo (PPS)", fontweight="bold")
    axis.set_xlim(int(x_range[0]), int(x_range[-1]))
    axis.set_ylim(bottom=0)
    pps_axis.set_ylim(bottom=0)
    pps_axis.ticklabel_format(axis="y", style="plain", useOffset=False)
    pps_axis.yaxis.set_major_formatter(plt.FuncFormatter(decimal_mpps))
    axis.set_xticks(ticks)
    axis.tick_params(axis="x", rotation=45)
    axis.grid(True, which="major", color="#8d99a6", linestyle="--", linewidth=0.8, alpha=0.75)
    axis.grid(True, which="minor", color="#c8cdd2", linestyle=":", linewidth=0.55, alpha=0.65)
    axis.minorticks_on()


def plot_loss(
    item: DirectionData,
    axis: plt.Axes,
    *,
    x_range: np.ndarray,
    ticks: list[int],
    show_ylabel: bool,
) -> None:
    color = COLORS.get(item.direction, "#d62728")
    axis.axhline(0, color="#111111", linewidth=2, linestyle=":", zorder=2)
    axis.errorbar(
        item.payload,
        item.loss,
        yerr=item.loss_std,
        color=color,
        linestyle="-",
        marker="^",
        markersize=4,
        linewidth=2,
        elinewidth=1,
        capsize=2,
        zorder=4,
    )
    axis.set_xlabel("Tamaño del payload UDP (bytes)", fontweight="bold")
    if show_ylabel:
        axis.set_ylabel("Pérdida de paquetes (%)", fontweight="bold")
    axis.set_xlim(int(x_range[0]), int(x_range[-1]))
    axis.set_ylim(bottom=0)
    axis.set_xticks(ticks)
    axis.tick_params(axis="x", rotation=45)
    axis.grid(True, which="major", color="#8d99a6", linestyle="--", linewidth=0.8, alpha=0.75)
    axis.grid(True, which="minor", color="#c8cdd2", linestyle=":", linewidth=0.55, alpha=0.65)
    axis.minorticks_on()


def plot_10gbe_limits(
    data: list[DirectionData],
    *,
    input_dir: Path,
    output: Path | None,
    show: bool,
    link_mbps: float,
    title_label: str,
) -> None:
    min_payload = min(int(item.payload.min()) for item in data)
    max_payload = max(int(item.payload.max()) for item in data)
    x_range = np.arange(min_payload, max_payload + 1)
    ticks = payload_ticks(data)
    by_direction = {item.direction: item for item in data}

    fig, axes = plt.subplots(2, 2, figsize=(20, 14))
    directions = ["nic-to-corundum", "corundum-to-nic"]
    for column, direction in enumerate(directions):
        item = by_direction.get(direction)
        if item is None:
            axes[0, column].set_visible(False)
            axes[1, column].set_visible(False)
            continue
        plot_direction(
            item,
            axes[0, column],
            link_mbps=link_mbps,
            x_range=x_range,
            ticks=ticks,
            show_goodput_label=column == 0,
            show_pps_label=column == 1,
        )
        plot_loss(
            item,
            axes[1, column],
            x_range=x_range,
            ticks=ticks,
            show_ylabel=column == 0,
        )

    interface_handles = [
        Line2D(
            [0],
            [0],
            color=COLORS.get(item.direction, "#1f77b4"),
            linewidth=3,
            label=DIRECTION_TITLES.get(item.direction, item.direction),
        )
        for item in data
    ]
    encoding_handles = [
        Line2D([0], [0], color="#111111", linewidth=2.4, label="Goodput límite teórico"),
        Line2D(
            [0],
            [0],
            color="#111111",
            linewidth=2.2,
            linestyle=":",
            label="PPS límite teórico",
        ),
        Line2D([0], [0], color="#555555", marker="o", linestyle="-", label="Goodput medido"),
        Line2D([0], [0], color="#777777", marker="s", linestyle="-", label="PPS medidos"),
        Line2D([0], [0], color="#777777", marker="^", linestyle="-", label="Pérdidas medidas"),
    ]
    fig.suptitle(
        f"Rendimiento medido vs límite teórico Ethernet UDP a {link_mbps / 1000:g} Gb/s\n"
        f"{title_label} · {input_dir}",
        fontsize=15,
        fontweight="bold",
        y=0.99,
    )
    fig.legend(
        handles=interface_handles,
        title="Dirección",
        loc="upper center",
        bbox_to_anchor=(0.28, 0.948),
        ncol=2,
        fontsize=9,
        framealpha=1,
    )
    fig.legend(
        handles=encoding_handles,
        title="Métrica",
        loc="upper center",
        bbox_to_anchor=(0.73, 0.945),
        ncol=3,
        fontsize=9,
        framealpha=1,
    )
    fig.subplots_adjust(
        left=0.05,
        right=0.94,
        bottom=0.088,
        top=0.765,
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

    if show:
        print(f"Abriendo ventana de Matplotlib con backend: {plt.get_backend()}")
        plt.show()
    plt.close(fig)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Superpone un sweep 10GbE sobre los límites teóricos de goodput y PPS"
        )
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        help=(
            "directorio de resultados de scripts/etherbench_10gbe.py; "
            "por defecto usa el results/10gbe_* más reciente"
        ),
    )
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
    parser.add_argument(
        "--link-mbps",
        type=float,
        default=DEFAULT_LINK_MBPS,
        help="velocidad nominal del enlace en Mbps",
    )
    parser.add_argument(
        "--title-label",
        default="Interfaz 10GbE del servidor",
        help="texto descriptivo para el título de la figura",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        input_dir = args.input_dir.resolve() if args.input_dir else latest_10gbe_result()
        output = args.output
        show = not args.no_show
        if output is None and show and matplotlib_backend_is_noninteractive():
            output = input_dir / "limites_teoricos_10gbe.png"
            show = False
            print(
                "Matplotlib está usando un backend no interactivo "
                f"({plt.get_backend()}); guardaré la figura en: {output}"
            )

        print(f"Leyendo resumen 10GbE desde: {input_dir / 'udp_summary.csv'}")
        data = load_direction_data(input_dir)
        directions = ", ".join(DIRECTION_TITLES.get(item.direction, item.direction) for item in data)
        print(f"Direcciones encontradas: {directions}")
        plot_10gbe_limits(
            data,
            input_dir=input_dir,
            output=output,
            show=show,
            link_mbps=args.link_mbps,
            title_label=args.title_label,
        )
    except Exception as error:
        print(f"error: {error}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
