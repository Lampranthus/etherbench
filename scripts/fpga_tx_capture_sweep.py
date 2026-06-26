#!/usr/bin/env python3
"""Ejecuta un barrido de capturas FPGA TX y genera 20 histogramas 256x256."""

from __future__ import annotations

import argparse
import csv
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.colors import LogNorm, Normalize

from histograma_bytes import histogram


DEFAULT_FPGA_IP = "192.168.1.12"
DEFAULT_CTRL_PORT = 55555
DEFAULT_LOCAL_PORT = 9999
DEFAULT_MODE = "sequential"
AXIS_VALUES = np.array([0, 31, 63, 95, 127, 159, 191, 223, 255])


@dataclass(frozen=True)
class SweepPoint:
    payload: int
    packets: int

    @property
    def filename(self) -> str:
        return f"payload_{self.payload:04d}_packets_{self.packets:06d}.bin"


SWEEP_POINTS = (
    SweepPoint(256, 5376),
    SweepPoint(320, 4301),
    SweepPoint(384, 3584),
    SweepPoint(448, 3072),
    SweepPoint(512, 2688),
    SweepPoint(576, 2390),
    SweepPoint(640, 2151),
    SweepPoint(704, 1955),
    SweepPoint(768, 1792),
    SweepPoint(832, 1655),
    SweepPoint(896, 1536),
    SweepPoint(960, 1434),
    SweepPoint(1024, 1344),
    SweepPoint(1088, 1265),
    SweepPoint(1152, 1195),
    SweepPoint(1216, 1132),
    SweepPoint(1280, 1076),
    SweepPoint(1344, 1024),
    SweepPoint(1408, 978),
    SweepPoint(1472, 935),
)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_output_dir() -> Path:
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    return repo_root() / "results" / f"fpga_tx_capture_sweep_{timestamp}"


def capture_command(
    binary: Path,
    fpga_ip: str,
    point: SweepPoint,
    output_file: Path,
    ctrl_port: int,
    local_port: int,
) -> list[str]:
    return [
        str(binary),
        "fpga-tx-capture",
        fpga_ip,
        str(point.packets),
        str(point.payload),
        DEFAULT_MODE,
        str(output_file),
        str(ctrl_port),
        str(local_port),
    ]


def expected_file_size(point: SweepPoint) -> int:
    return point.payload * point.packets


def append_manifest(
    path: Path,
    point: SweepPoint,
    output_file: Path,
    returncode: int,
    command: list[str],
) -> None:
    write_header = not path.exists()

    with path.open("a", newline="") as file:
        writer = csv.DictWriter(
            file,
            fieldnames=(
                "timestamp",
                "payload",
                "packets",
                "expected_bytes",
                "captured_bytes",
                "returncode",
                "output_file",
                "command",
            ),
        )
        if write_header:
            writer.writeheader()

        writer.writerow(
            {
                "timestamp": int(time.time()),
                "payload": point.payload,
                "packets": point.packets,
                "expected_bytes": expected_file_size(point),
                "captured_bytes": (
                    output_file.stat().st_size if output_file.exists() else 0
                ),
                "returncode": returncode,
                "output_file": output_file.name,
                "command": " ".join(command),
            }
        )


def run_captures(args: argparse.Namespace, output_dir: Path) -> None:
    binary = args.binary.resolve()
    manifest = output_dir / "capture_runs.csv"

    if not args.dry_run and not binary.is_file():
        raise FileNotFoundError(f"No se encontró el ejecutable: {binary}")

    for index, point in enumerate(SWEEP_POINTS, start=1):
        output_file = output_dir / point.filename
        expected_bytes = expected_file_size(point)

        if (
            args.resume
            and output_file.is_file()
            and output_file.stat().st_size == expected_bytes
        ):
            print(
                f"[{index:02d}/20] payload={point.payload}: "
                f"captura completa existente, se omite",
                flush=True,
            )
            continue

        command = capture_command(
            binary,
            args.fpga_ip,
            point,
            output_file,
            args.ctrl_port,
            args.local_port,
        )
        print(
            f"[{index:02d}/20] payload={point.payload} "
            f"packets={point.packets}\n  {' '.join(command)}",
            flush=True,
        )

        returncode = 0
        if not args.dry_run:
            result = subprocess.run(command, cwd=repo_root(), check=False)
            returncode = result.returncode
            append_manifest(
                manifest,
                point,
                output_file,
                returncode,
                command,
            )

        if returncode != 0:
            message = (
                f"La captura payload={point.payload} terminó con código "
                f"{returncode}"
            )
            if args.continue_on_error:
                print(f"Advertencia: {message}", flush=True)
            else:
                raise RuntimeError(
                    f"{message}. Captura parcial conservada en {output_file}. "
                    f"Reanuda con --output-dir {output_dir} --resume"
                )


def load_histograms(
    output_dir: Path,
    endian: str,
) -> list[tuple[SweepPoint, np.ndarray, int, int]]:
    loaded = []

    for point in SWEEP_POINTS:
        path = output_dir / point.filename
        if not path.is_file():
            raise FileNotFoundError(
                f"Falta la captura para payload={point.payload}: {path}"
            )

        counts, total_words, trailing_bytes = histogram(path, 16, endian)
        loaded.append((point, counts, total_words, trailing_bytes))

    return loaded


def plot_sweep(
    loaded: list[tuple[SweepPoint, np.ndarray, int, int]],
    *,
    logarithmic: bool,
    endian: str,
    output_file: Path | None,
    show: bool,
    direction_title: str = "FPGA TX",
) -> None:
    figure, axes = plt.subplots(
        4,
        5,
        figsize=(28, 18),
        sharex=True,
        sharey=True,
        constrained_layout=True,
    )

    for index, (axis, item) in enumerate(zip(axes.flat, loaded)):
        point, counts, total_words, trailing_bytes = item
        positive = counts[counts > 0]
        minimum = int(positive.min()) if positive.size else 0
        maximum = int(positive.max()) if positive.size else 1
        displayed_counts = np.ma.masked_equal(counts, 0)
        colormap = plt.get_cmap("viridis").copy()
        colormap.set_bad("#22002b")

        if logarithmic:
            norm = LogNorm(
                vmin=max(1, minimum),
                vmax=max(maximum, minimum + 1),
            )
        elif maximum > minimum:
            norm = Normalize(vmin=minimum, vmax=maximum)
        else:
            norm = Normalize(vmin=0, vmax=maximum)

        image = axis.imshow(
            displayed_counts,
            origin="lower",
            cmap=colormap,
            interpolation="nearest",
            norm=norm,
            extent=(-0.5, 255.5, -0.5, 255.5),
        )
        captured_bytes = (total_words * 2) + trailing_bytes
        received_packets = min(
            point.packets,
            captured_bytes // point.payload,
        )
        received_percentage = (
            100.0 * received_packets / point.packets
            if point.packets > 0
            else 0.0
        )
        axis.set_title(
            f"Payload {point.payload}B\n"
            f"{received_packets:,}/{point.packets:,} paquetes",
            fontsize=10,
            fontweight="bold",
        )
        axis.set_xticks(AXIS_VALUES)
        axis.set_yticks(AXIS_VALUES)
        axis.tick_params(axis="both", labelsize=6)
        axis.grid(which="major", color="white", linewidth=0.25, alpha=0.25)
        colorbar = figure.colorbar(
            image,
            ax=axis,
            location="right",
            fraction=0.045,
            pad=0.02,
        )
        colorbar.ax.tick_params(labelsize=5)
        colorbar.set_label("Apariciones", fontsize=6)
        if not logarithmic and maximum > minimum and maximum - minimum <= 10:
            colorbar.set_ticks(np.arange(minimum, maximum + 1))

        if trailing_bytes:
            axis.text(
                0.98,
                0.02,
                "1 byte ignorado",
                transform=axis.transAxes,
                ha="right",
                va="bottom",
                color="white",
                fontsize=6,
            )

        row, column = divmod(index, 5)
        if row == 3:
            axis.set_xlabel("Byte bajo", fontsize=8)
        if column == 0:
            axis.set_ylabel("Byte alto", fontsize=8)

    figure.suptitle(
        f"Barrido de payload {direction_title} — datos secuenciales de 16 bits\n",
        fontsize=17,
        fontweight="bold",
    )

    if output_file is not None:
        output_file.parent.mkdir(parents=True, exist_ok=True)
        figure.savefig(output_file, dpi=180, bbox_inches="tight")
        print(f"Figura guardada en: {output_file}")

    if show:
        plt.show()
    else:
        plt.close(figure)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Ejecuta 20 capturas FPGA TX sequential y genera una figura "
            "multipanel con histogramas 256x256."
        )
    )
    parser.add_argument("--fpga-ip", default=DEFAULT_FPGA_IP)
    parser.add_argument("--ctrl-port", type=int, default=DEFAULT_CTRL_PORT)
    parser.add_argument("--local-port", type=int, default=DEFAULT_LOCAL_PORT)
    parser.add_argument(
        "--binary",
        type=Path,
        default=repo_root() / "etherbench",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="directorio de capturas; por defecto crea uno bajo results/",
    )
    parser.add_argument(
        "--figure",
        type=Path,
        help="guarda opcionalmente la figura en esta ruta",
    )
    parser.add_argument(
        "--endian",
        choices=("big", "little"),
        default="big",
        help="orden de las palabras de 16 bits; por defecto: big",
    )
    parser.add_argument("--log", action="store_true")
    parser.add_argument(
        "--resume",
        action="store_true",
        help="omite capturas existentes cuyo tamaño sea el esperado",
    )
    parser.add_argument(
        "--plot-only",
        action="store_true",
        help="no captura; grafica los 20 .bin existentes",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="muestra los comandos sin ejecutarlos ni graficar",
    )
    parser.add_argument(
        "--continue-on-error",
        action="store_true",
        help="continúa el barrido si una captura falla",
    )
    parser.add_argument(
        "--no-show",
        action="store_true",
        help="no abre la ventana interactiva",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_dir = (
        args.output_dir.resolve()
        if args.output_dir is not None
        else default_output_dir()
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    if not args.plot_only:
        run_captures(args, output_dir)

    if args.dry_run:
        print(f"Dry-run completado. Directorio previsto: {output_dir}")
        return 0

    loaded = load_histograms(output_dir, args.endian)
    figure = args.figure.resolve() if args.figure is not None else None
    plot_sweep(
        loaded,
        logarithmic=args.log,
        endian=args.endian,
        output_file=figure,
        show=not args.no_show,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
