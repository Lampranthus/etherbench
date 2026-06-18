#!/usr/bin/env python3
"""
Run payload sweeps for etherbench and generate summary CSV/SVG plots.

The script intentionally uses only Python's standard library so it can run on
small lab machines without extra plotting dependencies.
"""

from __future__ import annotations

import argparse
import csv
import math
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


DEFAULT_PAYLOAD_MIN = 256
DEFAULT_PAYLOAD_MAX = 1440
DEFAULT_PAYLOAD_STEP = 74
DEFAULT_REPEAT = 5
DEFAULT_RTT_PACKETS = 1000
DEFAULT_LOAD_PACKETS = 1_000_000
DEFAULT_CTRL_PORT = 55555
DEFAULT_DATA_PORT = 1234
DEFAULT_LOCAL_PORT = 9999
DEFAULT_MODE = "random"
DEFAULT_LINK_MBPS = 1000.0
ETHERNET_OVERHEAD_BYTES = 66


@dataclass(frozen=True)
class SeriesStyle:
    key: str
    label: str
    color: str
    y_axis: str
    dashed: bool = False


def theoretical_goodput_mbps(payload_size: int, link_mbps: float = DEFAULT_LINK_MBPS) -> float:
    return link_mbps * payload_size / (payload_size + ETHERNET_OVERHEAD_BYTES)


def theoretical_pps(payload_size: int, link_mbps: float = DEFAULT_LINK_MBPS) -> float:
    return (link_mbps * 1_000_000.0) / ((payload_size + ETHERNET_OVERHEAD_BYTES) * 8.0)


def parse_payloads(args: argparse.Namespace) -> list[int]:
    if args.payloads:
        values = [int(item.strip(), 0) for item in args.payloads.split(",")]
    else:
        values = list(range(args.payload_min, args.payload_max + 1, args.payload_step))
        if values[-1] != args.payload_max:
            values.append(args.payload_max)

    values = sorted(set(values))

    for value in values:
        if value < 256 or value > 1440:
            raise ValueError(f"payload {value} is outside 256..1440")

    return values


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def make_output_dir(base: Path | None) -> Path:
    if base is None:
        base = repo_root() / "results" / time.strftime("sweep_%Y%m%d_%H%M%S")

    base.mkdir(parents=True, exist_ok=True)
    return base


def command_for_test(
    binary: Path,
    test: str,
    iface: str,
    fpga_ip: str,
    packets: int,
    payload: int,
    mode: str,
    ctrl_port: int,
    data_port: int,
    local_port: int,
) -> list[str]:
    if test == "rtt":
        return [
            str(binary),
            "fpga-rtt",
            fpga_ip,
            str(packets),
            str(payload),
            str(data_port),
            str(local_port),
            str(ctrl_port),
        ]

    if test == "loopback":
        return [
            str(binary),
            "fpga-loopback-test",
            iface,
            fpga_ip,
            str(packets),
            str(payload),
            str(data_port),
            str(local_port),
            str(ctrl_port),
        ]

    if test == "tx":
        return [
            str(binary),
            "fpga-tx-test",
            iface,
            fpga_ip,
            str(packets),
            str(payload),
            mode,
            str(ctrl_port),
            str(local_port),
        ]

    raise ValueError(f"unknown test: {test}")


def run_sweep(args: argparse.Namespace) -> Path:
    payloads = parse_payloads(args)
    output_dir = make_output_dir(args.output_dir)
    binary = args.binary.resolve()

    if not args.dry_run and not binary.exists():
        raise FileNotFoundError(f"etherbench binary not found: {binary}")

    manifest_path = output_dir / "sweep_runs.csv"

    try:
        with manifest_path.open("a", newline="") as file:
            writer = csv.DictWriter(
                file,
                fieldnames=[
                    "timestamp",
                    "test",
                    "payload_size",
                    "run",
                    "packets",
                    "returncode",
                    "command",
                ],
            )

            if file.tell() == 0:
                writer.writeheader()

            for test in args.tests:
                packets = args.rtt_packets if test == "rtt" else args.load_packets

                for payload in payloads:
                    for run_id in range(1, args.repeat + 1):
                        cmd = command_for_test(
                            binary,
                            test,
                            args.iface,
                            args.fpga_ip,
                            packets,
                            payload,
                            args.mode,
                            args.ctrl_port,
                            args.data_port,
                            args.local_port,
                        )

                        print(
                            f"[{test}] payload={payload} run={run_id}/{args.repeat}: "
                            + " ".join(cmd),
                            flush=True,
                        )

                        returncode = 0
                        if not args.dry_run:
                            result = subprocess.run(cmd, cwd=output_dir)
                            returncode = result.returncode

                            if returncode != 0 and args.stop_on_error:
                                raise RuntimeError(
                                    f"{test} payload={payload} run={run_id} failed "
                                    f"with exit code {returncode}"
                                )

                        writer.writerow(
                            {
                                "timestamp": int(time.time()),
                                "test": test,
                                "payload_size": payload,
                                "run": run_id,
                                "packets": packets,
                                "returncode": returncode,
                                "command": " ".join(cmd),
                            }
                        )
                        file.flush()

                        if not args.dry_run and args.update_plots_each_run:
                            summarize(output_dir)
                            plot(output_dir)
                            print("Updated partial summaries and plots", flush=True)

    finally:
        summarize(output_dir)
        plot(output_dir)

    return output_dir


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []

    with path.open(newline="") as file:
        return list(csv.DictReader(file))


def mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def stdev(values: list[float]) -> float:
    if len(values) < 2:
        return 0.0
    return statistics.stdev(values)


def group_by_payload(rows: list[dict[str, str]]) -> dict[int, list[dict[str, str]]]:
    grouped: dict[int, list[dict[str, str]]] = {}

    for row in rows:
        payload = int(row["payload_size"])
        grouped.setdefault(payload, []).append(row)

    return grouped


def summarize_rtt(output_dir: Path) -> list[dict[str, str]]:
    rows = read_csv(output_dir / "fpga_rtt_logs.csv")
    summary: list[dict[str, str]] = []

    for payload, items in sorted(group_by_payload(rows).items()):
        avg_values = [float(item["avg_ms"]) for item in items]
        lost_pct = [
            (float(item["lost"]) / float(item["sent"])) * 100.0
            for item in items
            if float(item["sent"]) > 0.0
        ]

        summary.append(
            {
                "test": "rtt",
                "payload_size": str(payload),
                "runs": str(len(items)),
                "avg_ms_mean": f"{mean(avg_values):.9f}",
                "avg_ms_std": f"{stdev(avg_values):.9f}",
                "lost_pct_mean": f"{mean(lost_pct):.9f}",
                "lost_pct_std": f"{stdev(lost_pct):.9f}",
            }
        )

    return summary


def summarize_loopback(output_dir: Path) -> list[dict[str, str]]:
    rows = read_csv(output_dir / "fpga_loopback_load_logs.csv")
    loss_rows = read_csv(output_dir / "fpga_loopback_loss_logs.csv")
    loss_by_payload = group_by_payload(loss_rows)
    summary: list[dict[str, str]] = []

    for payload, items in sorted(group_by_payload(rows).items()):
        goodput = [float(item["payload_mbps"]) for item in items]
        wire = [float(item["estimated_wire_mbps"]) for item in items]
        pps = [float(item["packets_per_second"]) for item in items]
        loss_pct = []

        for item in loss_by_payload.get(payload, []):
            sent = float(item["sent_packets"])
            host_rx = float(item["iface_rx_packets_delta"])
            if sent > 0.0:
                loss_pct.append(max(sent - host_rx, 0.0) / sent * 100.0)

        if not loss_pct:
            for item in items:
                sent = float(item["sent_packets"])
                drained = float(item["drained_packets"])
                if sent > 0.0:
                    loss_pct.append(max(sent - drained, 0.0) / sent * 100.0)

        summary.append(
            {
                "test": "loopback",
                "payload_size": str(payload),
                "runs": str(len(items)),
                "goodput_mbps_mean": f"{mean(goodput):.9f}",
                "goodput_mbps_std": f"{stdev(goodput):.9f}",
                "wire_mbps_mean": f"{mean(wire):.9f}",
                "wire_mbps_std": f"{stdev(wire):.9f}",
                "pps_mean": f"{mean(pps):.9f}",
                "pps_std": f"{stdev(pps):.9f}",
                "theoretical_goodput_mbps": f"{theoretical_goodput_mbps(payload):.9f}",
                "theoretical_pps": f"{theoretical_pps(payload):.9f}",
                "lost_pct_mean": f"{mean(loss_pct):.9f}",
                "lost_pct_std": f"{stdev(loss_pct):.9f}",
            }
        )

    return summary


def summarize_tx(output_dir: Path) -> list[dict[str, str]]:
    rows = read_csv(output_dir / "fpga_tx_test_logs.csv")
    summary: list[dict[str, str]] = []

    for payload, items in sorted(group_by_payload(rows).items()):
        goodput = [float(item["payload_goodput_mbps"]) for item in items]
        wire = [float(item["estimated_wire_mbps"]) for item in items]
        pps = []
        loss_pct = []

        for item in items:
            requested = float(item["requested_packets"])
            lost = float(item["lost_packets"])
            received = float(item["received_packets"])
            elapsed_s = float(item["elapsed_s"])

            if requested > 0.0:
                loss_pct.append(lost / requested * 100.0)

            if elapsed_s > 0.0:
                pps.append(received / elapsed_s)

        summary.append(
            {
                "test": "tx",
                "payload_size": str(payload),
                "runs": str(len(items)),
                "goodput_mbps_mean": f"{mean(goodput):.9f}",
                "goodput_mbps_std": f"{stdev(goodput):.9f}",
                "wire_mbps_mean": f"{mean(wire):.9f}",
                "wire_mbps_std": f"{stdev(wire):.9f}",
                "pps_mean": f"{mean(pps):.9f}",
                "pps_std": f"{stdev(pps):.9f}",
                "theoretical_goodput_mbps": f"{theoretical_goodput_mbps(payload):.9f}",
                "theoretical_pps": f"{theoretical_pps(payload):.9f}",
                "lost_pct_mean": f"{mean(loss_pct):.9f}",
                "lost_pct_std": f"{stdev(loss_pct):.9f}",
            }
        )

    return summary


def write_summary(path: Path, rows: list[dict[str, str]]) -> None:
    fieldnames = sorted({key for row in rows for key in row.keys()})
    preferred = [
        "test",
        "payload_size",
        "runs",
        "avg_ms_mean",
        "avg_ms_std",
        "goodput_mbps_mean",
        "goodput_mbps_std",
        "wire_mbps_mean",
        "wire_mbps_std",
        "pps_mean",
        "pps_std",
        "theoretical_goodput_mbps",
        "theoretical_pps",
        "lost_pct_mean",
        "lost_pct_std",
    ]
    fieldnames = [key for key in preferred if key in fieldnames]

    with path.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def summarize(output_dir: Path) -> None:
    rtt = summarize_rtt(output_dir)
    loopback = summarize_loopback(output_dir)
    tx = summarize_tx(output_dir)

    if rtt:
        write_summary(output_dir / "rtt_summary.csv", rtt)
    if loopback:
        write_summary(output_dir / "loopback_summary.csv", loopback)
    if tx:
        write_summary(output_dir / "tx_summary.csv", tx)


def svg_escape(text: str) -> str:
    return (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def nice_range(values: list[float]) -> tuple[float, float]:
    if not values:
        return 0.0, 1.0

    lo = min(values)
    hi = max(values)

    if math.isclose(lo, hi):
        padding = abs(lo) * 0.1 if lo != 0.0 else 1.0
        return lo - padding, hi + padding

    padding = (hi - lo) * 0.08
    return lo - padding, hi + padding


def draw_plot(
    path: Path,
    title: str,
    x_label: str,
    left_label: str,
    right_label: str | None,
    rows: list[dict[str, str]],
    series: list[SeriesStyle],
) -> None:
    width = 1120
    height = 680
    left = 96
    right = 94 if right_label else 36
    top = 62
    bottom = 86
    plot_w = width - left - right
    plot_h = height - top - bottom

    payloads = [float(row["payload_size"]) for row in rows]
    x_min, x_max = nice_range(payloads)

    left_values: list[float] = []
    right_values: list[float] = []

    for row in rows:
        for item in series:
            if item.key not in row:
                continue

            value = float(row[item.key])
            err = 0.0
            if item.key.endswith("_mean"):
                err_key = item.key[:-5] + "_std"
                err = float(row.get(err_key, "0") or 0)
            target = left_values if item.y_axis == "left" else right_values
            target.extend([value - err, value + err])

    left_min, left_max = nice_range(left_values)
    right_min, right_max = nice_range(right_values)

    def sx(value: float) -> float:
        return left + (value - x_min) / (x_max - x_min) * plot_w

    def sy_left(value: float) -> float:
        return top + (left_max - value) / (left_max - left_min) * plot_h

    def sy_right(value: float) -> float:
        return top + (right_max - value) / (right_max - right_min) * plot_h

    lines: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#fbfbf8"/>',
        f'<text x="{width / 2}" y="34" text-anchor="middle" font-family="sans-serif" font-size="24" fill="#222">{svg_escape(title)}</text>',
        f'<rect x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" fill="#ffffff" stroke="#c9c9c1"/>',
    ]

    for i in range(6):
        t = i / 5
        y = top + t * plot_h
        x = left + t * plot_w
        left_tick = left_max - t * (left_max - left_min)
        x_tick = x_min + t * (x_max - x_min)
        lines.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + plot_w}" y2="{y:.2f}" stroke="#e8e8df"/>')
        lines.append(f'<text x="{left - 10}" y="{y + 4:.2f}" text-anchor="end" font-family="sans-serif" font-size="12" fill="#4a4a45">{left_tick:.3g}</text>')
        lines.append(f'<line x1="{x:.2f}" y1="{top}" x2="{x:.2f}" y2="{top + plot_h}" stroke="#f0f0e8"/>')
        lines.append(f'<text x="{x:.2f}" y="{top + plot_h + 24}" text-anchor="middle" font-family="sans-serif" font-size="12" fill="#4a4a45">{x_tick:.0f}</text>')

        if right_label:
            right_tick = right_max - t * (right_max - right_min)
            lines.append(f'<text x="{left + plot_w + 12}" y="{y + 4:.2f}" text-anchor="start" font-family="sans-serif" font-size="12" fill="#4a4a45">{right_tick:.3g}</text>')

    lines.append(f'<text x="{left + plot_w / 2}" y="{height - 24}" text-anchor="middle" font-family="sans-serif" font-size="15" fill="#222">{svg_escape(x_label)}</text>')
    lines.append(f'<text transform="translate(26 {top + plot_h / 2}) rotate(-90)" text-anchor="middle" font-family="sans-serif" font-size="15" fill="#222">{svg_escape(left_label)}</text>')

    if right_label:
        lines.append(f'<text transform="translate({width - 24} {top + plot_h / 2}) rotate(90)" text-anchor="middle" font-family="sans-serif" font-size="15" fill="#222">{svg_escape(right_label)}</text>')

    legend_x = left + 16
    legend_y = top + 20

    for idx, item in enumerate(series):
        y = legend_y + idx * 22
        dash = ' stroke-dasharray="8 6"' if item.dashed else ""
        lines.append(f'<line x1="{legend_x}" y1="{y}" x2="{legend_x + 26}" y2="{y}" stroke="{item.color}" stroke-width="3"{dash}/>')
        lines.append(f'<text x="{legend_x + 34}" y="{y + 4}" font-family="sans-serif" font-size="13" fill="#222">{svg_escape(item.label)}</text>')

    for item in series:
        coords = []
        y_map = sy_left if item.y_axis == "left" else sy_right

        for row in rows:
            if item.key not in row:
                continue

            x = sx(float(row["payload_size"]))
            y = y_map(float(row[item.key]))
            coords.append((x, y, row))

        if len(coords) >= 2:
            points = " ".join(f"{x:.2f},{y:.2f}" for x, y, _ in coords)
            dash = ' stroke-dasharray="8 6"' if item.dashed else ""
            lines.append(f'<polyline points="{points}" fill="none" stroke="{item.color}" stroke-width="2.5"{dash}/>')

        for x, y, row in coords:
            err = 0.0
            if item.key.endswith("_mean"):
                err_key = item.key[:-5] + "_std"
                err = float(row.get(err_key, "0") or 0)

            if err > 0.0:
                value = float(row[item.key])
                y1 = y_map(value - err)
                y2 = y_map(value + err)
                lines.append(f'<line x1="{x:.2f}" y1="{y1:.2f}" x2="{x:.2f}" y2="{y2:.2f}" stroke="{item.color}" stroke-width="1.3"/>')
                lines.append(f'<line x1="{x - 5:.2f}" y1="{y1:.2f}" x2="{x + 5:.2f}" y2="{y1:.2f}" stroke="{item.color}" stroke-width="1.3"/>')
                lines.append(f'<line x1="{x - 5:.2f}" y1="{y2:.2f}" x2="{x + 5:.2f}" y2="{y2:.2f}" stroke="{item.color}" stroke-width="1.3"/>')

            lines.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="4" fill="{item.color}" stroke="#ffffff" stroke-width="1"/>')

    lines.append("</svg>\n")
    path.write_text("\n".join(lines))


def load_summary(path: Path) -> list[dict[str, str]]:
    rows = read_csv(path)
    return sorted(rows, key=lambda row: int(row["payload_size"]))


def merge_pps_rows(
    loopback: list[dict[str, str]],
    tx: list[dict[str, str]],
) -> list[dict[str, str]]:
    merged: dict[int, dict[str, str]] = {}

    for row in loopback:
        payload = int(row["payload_size"])
        out = merged.setdefault(payload, {"payload_size": str(payload)})
        out["loopback_pps_mean"] = row["pps_mean"]
        out["loopback_pps_std"] = row["pps_std"]
        out["theoretical_pps"] = row["theoretical_pps"]

    for row in tx:
        payload = int(row["payload_size"])
        out = merged.setdefault(payload, {"payload_size": str(payload)})
        out["tx_pps_mean"] = row["pps_mean"]
        out["tx_pps_std"] = row["pps_std"]
        out["theoretical_pps"] = row["theoretical_pps"]

    return [
        row
        for _, row in sorted(merged.items())
        if "theoretical_pps" in row and ("loopback_pps_mean" in row or "tx_pps_mean" in row)
    ]


def plot(output_dir: Path) -> None:
    rtt = load_summary(output_dir / "rtt_summary.csv")
    loopback = load_summary(output_dir / "loopback_summary.csv")
    tx = load_summary(output_dir / "tx_summary.csv")

    if rtt:
        draw_plot(
            output_dir / "rtt_payload_sweep.svg",
            "RTT vs payload",
            "Payload size (bytes)",
            "RTT promedio (ms)",
            None,
            rtt,
            [
                SeriesStyle("avg_ms_mean", "RTT promedio", "#1f77b4", "left"),
            ],
        )

    if loopback:
        draw_plot(
            output_dir / "loopback_goodput_payload_sweep.svg",
            "Loopback: goodput vs payload",
            "Payload size (bytes)",
            "Goodput payload (Mbps)",
            None,
            loopback,
            [
                SeriesStyle("goodput_mbps_mean", "Goodput", "#2ca02c", "left"),
                SeriesStyle("theoretical_goodput_mbps", "Teorico", "#111111", "left", dashed=True),
            ],
        )

        draw_plot(
            output_dir / "loopback_loss_payload_sweep.svg",
            "Loopback: perdida vs payload",
            "Payload size (bytes)",
            "Perdida (%)",
            None,
            loopback,
            [
                SeriesStyle("lost_pct_mean", "Perdida", "#d62728", "left"),
            ],
        )

    if tx:
        draw_plot(
            output_dir / "tx_goodput_payload_sweep.svg",
            "Transmision FPGA: goodput vs payload",
            "Payload size (bytes)",
            "Goodput payload (Mbps)",
            None,
            tx,
            [
                SeriesStyle("goodput_mbps_mean", "Goodput", "#9467bd", "left"),
                SeriesStyle("theoretical_goodput_mbps", "Teorico", "#111111", "left", dashed=True),
            ],
        )

        draw_plot(
            output_dir / "tx_loss_payload_sweep.svg",
            "Transmision FPGA: perdida vs payload",
            "Payload size (bytes)",
            "Perdida (%)",
            None,
            tx,
            [
                SeriesStyle("lost_pct_mean", "Perdida", "#d62728", "left"),
            ],
        )

    pps_rows = merge_pps_rows(loopback, tx)
    pps_series: list[SeriesStyle] = []

    if pps_rows and any("loopback_pps_mean" in row for row in pps_rows):
        pps_series.append(SeriesStyle("loopback_pps_mean", "Loopback PPS", "#2ca02c", "left"))

    if pps_rows and any("tx_pps_mean" in row for row in pps_rows):
        pps_series.append(SeriesStyle("tx_pps_mean", "TX PPS", "#9467bd", "left"))

    if pps_rows:
        pps_series.append(SeriesStyle("theoretical_pps", "Teorico", "#111111", "left", dashed=True))

    if pps_rows and pps_series:
        draw_plot(
            output_dir / "pps_payload_sweep.svg",
            "PPS vs payload",
            "Payload size (bytes)",
            "Paquetes por segundo",
            None,
            pps_rows,
            pps_series,
        )


def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--output-dir", type=Path)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Run and plot etherbench payload sweeps")
    subparsers = parser.add_subparsers(dest="command", required=True)

    run_parser = subparsers.add_parser("run", help="run sweep, summarize and plot")
    run_parser.add_argument("--iface", required=True)
    run_parser.add_argument("--fpga-ip", required=True)
    run_parser.add_argument("--binary", type=Path, default=repo_root() / "etherbench")
    run_parser.add_argument("--tests", nargs="+", choices=["rtt", "loopback", "tx"], default=["rtt", "loopback", "tx"])
    run_parser.add_argument("--payload-min", type=int, default=DEFAULT_PAYLOAD_MIN)
    run_parser.add_argument("--payload-max", type=int, default=DEFAULT_PAYLOAD_MAX)
    run_parser.add_argument("--payload-step", type=int, default=DEFAULT_PAYLOAD_STEP)
    run_parser.add_argument("--payloads", help="comma-separated payload list, overrides min/max/step")
    run_parser.add_argument("--repeat", type=int, default=DEFAULT_REPEAT)
    run_parser.add_argument("--rtt-packets", type=int, default=DEFAULT_RTT_PACKETS)
    run_parser.add_argument("--load-packets", type=int, default=DEFAULT_LOAD_PACKETS)
    run_parser.add_argument("--ctrl-port", type=int, default=DEFAULT_CTRL_PORT)
    run_parser.add_argument("--data-port", type=int, default=DEFAULT_DATA_PORT)
    run_parser.add_argument("--local-port", type=int, default=DEFAULT_LOCAL_PORT)
    run_parser.add_argument("--mode", default=DEFAULT_MODE, choices=["random", "constant"])
    run_parser.add_argument("--dry-run", action="store_true")
    run_parser.add_argument("--stop-on-error", action="store_true")
    run_parser.add_argument(
        "--no-update-plots-each-run",
        dest="update_plots_each_run",
        action="store_false",
        help="only write summaries and SVG plots when the sweep finishes",
    )
    run_parser.set_defaults(update_plots_each_run=True)
    add_common_args(run_parser)

    summary_parser = subparsers.add_parser("summarize", help="rebuild summary CSVs from an output directory")
    add_common_args(summary_parser)

    plot_parser = subparsers.add_parser("plot", help="rebuild SVG plots from summary CSVs")
    add_common_args(plot_parser)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    try:
        if args.command == "run":
            output_dir = run_sweep(args)
            print(f"\nResults written to: {output_dir}")
            return 0

        if args.output_dir is None:
            parser.error("--output-dir is required for this command")

        if args.command == "summarize":
            summarize(args.output_dir)
            return 0

        if args.command == "plot":
            plot(args.output_dir)
            return 0

    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
