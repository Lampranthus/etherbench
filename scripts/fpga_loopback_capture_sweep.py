#!/usr/bin/env python3
"""Barrido de capturas loopback con secuencia host de 16 bits."""

from __future__ import annotations

import argparse
import subprocess
import time
from pathlib import Path

from fpga_tx_capture_sweep import (
    DEFAULT_CTRL_PORT,
    DEFAULT_FPGA_IP,
    DEFAULT_LOCAL_PORT,
    SWEEP_POINTS,
    append_manifest,
    expected_file_size,
    load_histograms,
    plot_sweep,
    repo_root,
)


DEFAULT_IFACE = "eth0"
DEFAULT_DATA_PORT = 1234
MIN_ACCEPTED_COMPLETENESS = 0.9999


def default_output_dir() -> Path:
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    return repo_root() / "results" / f"fpga_loopback_capture_sweep_{timestamp}"


def capture_command(
    binary: Path,
    iface: str,
    fpga_ip: str,
    payload: int,
    packets: int,
    output_file: Path,
    data_port: int,
    local_port: int,
    ctrl_port: int,
) -> list[str]:
    return [
        str(binary),
        "fpga-loopback-capture",
        iface,
        fpga_ip,
        str(packets),
        str(payload),
        str(output_file),
        str(data_port),
        str(local_port),
        str(ctrl_port),
    ]


def run_captures(args: argparse.Namespace, output_dir: Path) -> None:
    binary = args.binary.resolve()
    manifest = output_dir / "capture_runs.csv"

    if not args.dry_run and not binary.is_file():
        raise FileNotFoundError(f"No se encontró el ejecutable: {binary}")

    for index, point in enumerate(SWEEP_POINTS, start=1):
        output_file = output_dir / point.filename
        expected_bytes = expected_file_size(point)
        captured_bytes = output_file.stat().st_size if output_file.is_file() else 0
        completeness = (
            captured_bytes / expected_bytes if expected_bytes > 0 else 0.0
        )

        if (
            args.resume
            and output_file.is_file()
            and completeness >= MIN_ACCEPTED_COMPLETENESS
        ):
            print(
                f"[{index:02d}/20] payload={point.payload}: "
                f"captura existente {completeness * 100:.5f}%, se omite",
                flush=True,
            )
            continue

        command = capture_command(
            binary,
            args.iface,
            args.fpga_ip,
            point.payload,
            point.packets,
            output_file,
            args.data_port,
            args.local_port,
            args.ctrl_port,
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

        if returncode == 2 and output_file.is_file():
            completeness = output_file.stat().st_size / expected_bytes
            message = (
                f"Captura parcial payload={point.payload}: "
                f"{completeness * 100:.5f}%"
            )

            if args.strict_capture or completeness < MIN_ACCEPTED_COMPLETENESS:
                raise RuntimeError(
                    f"{message}. Captura conservada en {output_file}. "
                    f"Reanuda con --output-dir {output_dir} --resume"
                )

            print(
                f"Advertencia: {message}; se acepta y continúa el barrido",
                flush=True,
            )
            continue

        if returncode != 0:
            message = (
                f"La captura loopback payload={point.payload} terminó "
                f"con código {returncode}"
            )
            if args.continue_on_error:
                print(f"Advertencia: {message}", flush=True)
            else:
                raise RuntimeError(
                    f"{message}. Captura parcial conservada en {output_file}. "
                    f"Reanuda con --output-dir {output_dir} --resume"
                )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Envía una secuencia uint16 desde Linux, captura el loopback de "
            "la FPGA y genera 20 histogramas 256x256."
        )
    )
    parser.add_argument("--iface", default=DEFAULT_IFACE)
    parser.add_argument("--fpga-ip", default=DEFAULT_FPGA_IP)
    parser.add_argument("--data-port", type=int, default=DEFAULT_DATA_PORT)
    parser.add_argument("--local-port", type=int, default=DEFAULT_LOCAL_PORT)
    parser.add_argument("--ctrl-port", type=int, default=DEFAULT_CTRL_PORT)
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
    )
    parser.add_argument("--log", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--plot-only", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--continue-on-error", action="store_true")
    parser.add_argument(
        "--strict-capture",
        action="store_true",
        help="detiene el barrido ante cualquier paquete faltante",
    )
    parser.add_argument("--no-show", action="store_true")
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
        direction_title="Linux → FPGA → Linux",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
