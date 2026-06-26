#!/usr/bin/env python3
"""Diagnose asymmetric 10GbE drops between a NIC namespace and Corundum."""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_CORUNDUM_NAMESPACE = "corundum0_ns"
DEFAULT_NIC_NAMESPACE = "nic_ns"
DEFAULT_CORUNDUM_INTERFACE = "corundum0"
DEFAULT_NIC_INTERFACE = "nic0"
DEFAULT_CORUNDUM_IP = "192.168.1.100"
DEFAULT_NIC_IP = "192.168.1.110"
DEFAULT_IPERF_PORT = 5201
DEFAULT_NETPERF_PORT = 12865
DEFAULT_DURATION = 5
DEFAULT_OMIT = 2
DEFAULT_PAYLOAD = 1472
DEFAULT_LINK_MBPS = 10_000.0
ETHERNET_OVERHEAD_BYTES = 66
SUSPICIOUS_PATTERNS = (
    "drop",
    "err",
    "miss",
    "over",
    "timeout",
    "fifo",
    "crc",
    "frame",
    "abort",
    "no_buffer",
    "nobuf",
    "discard",
    "restart",
    "fail",
    "coll",
)
BENIGN_COUNTER_PATTERNS = (
    "fdir_miss",
)


@dataclass(frozen=True)
class Endpoint:
    name: str
    namespace: str
    interface: str
    ip: str


@dataclass(frozen=True)
class TestCase:
    direction: str
    source: Endpoint
    destination: Endpoint


def run_command(
    command: list[str],
    *,
    check: bool = True,
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=check,
        capture_output=True,
        text=True,
        timeout=timeout,
        env={**os.environ, "LC_ALL": "C"},
    )


def namespace_command(namespace: str, command: list[str]) -> list[str]:
    return ["ip", "netns", "exec", namespace, *command]


def require_tools(tools: list[str]) -> None:
    missing = [tool for tool in tools if shutil.which(tool) is None]
    if missing:
        raise RuntimeError("faltan herramientas requeridas: " + ", ".join(missing))


def require_root() -> None:
    if os.geteuid() != 0:
        raise PermissionError("este diagnóstico debe correrse con sudo")


def parse_bitrate(value: str) -> float:
    text = value.strip()
    if not text:
        raise ValueError("bitrate vacío")
    multiplier = 1.0
    suffix = text[-1].lower()
    if suffix in {"k", "m", "g"}:
        text = text[:-1]
        multiplier = {"k": 1e3, "m": 1e6, "g": 1e9}[suffix]
    return float(text) * multiplier


def theoretical_goodput_bps(payload: int, link_mbps: float) -> float:
    return link_mbps * 1_000_000.0 * payload / (payload + ETHERNET_OVERHEAD_BYTES)


def configured_endpoints(args: argparse.Namespace) -> tuple[Endpoint, Endpoint]:
    return (
        Endpoint("corundum", args.corundum_namespace, args.corundum_interface, args.corundum_ip),
        Endpoint("nic", args.nic_namespace, args.nic_interface, args.nic_ip),
    )


def test_case(args: argparse.Namespace) -> TestCase:
    corundum, nic = configured_endpoints(args)
    if args.direction == "nic-to-corundum":
        return TestCase(args.direction, nic, corundum)
    return TestCase(args.direction, corundum, nic)


def make_output_dir(path: Path | None) -> Path:
    if path is None:
        path = Path("results") / time.strftime("debug_10gbe_%Y%m%d_%H%M%S")
    path.mkdir(parents=True, exist_ok=True)
    return path.resolve()


def existing_namespaces() -> set[str]:
    result = run_command(["ip", "netns", "list"])
    return {line.split()[0] for line in result.stdout.splitlines() if line.strip()}


def check_environment(args: argparse.Namespace) -> None:
    tools = ["ip", "ethtool"]
    if args.tool == "iperf3":
        tools.append("iperf3")
    else:
        tools.extend(["netperf", "netserver"])
    require_tools(tools)
    require_root()
    namespaces = existing_namespaces()
    for endpoint in configured_endpoints(args):
        if endpoint.namespace not in namespaces:
            raise RuntimeError(f"no existe el namespace: {endpoint.namespace}")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def safe_command(command: list[str], timeout: float | None = None) -> subprocess.CompletedProcess[str]:
    return run_command(command, check=False, timeout=timeout)


def parse_ethtool_stats(output: str) -> dict[str, int]:
    stats: dict[str, int] = {}
    for line in output.splitlines():
        match = re.match(r"\s*([^:]+):\s*(-?\d+)\s*$", line)
        if match:
            stats[match.group(1).strip()] = int(match.group(2))
    return stats


def parse_ip_link_stats(output: str) -> dict[str, int]:
    links = json.loads(output)
    if not links:
        return {}
    stats64 = links[0].get("stats64", {})
    parsed: dict[str, int] = {}
    for direction, values in stats64.items():
        if isinstance(values, dict):
            for key, value in values.items():
                if isinstance(value, int):
                    parsed[f"ip_{direction}_{key}"] = value
    return parsed


def parse_snmp(output: str) -> dict[str, int]:
    parsed: dict[str, int] = {}
    pending_header: dict[str, list[str]] = {}
    for line in output.splitlines():
        if ":" not in line:
            continue
        section, rest = line.split(":", 1)
        fields = rest.split()
        if not fields:
            continue
        if section not in pending_header:
            pending_header[section] = fields
            continue
        headers = pending_header.pop(section)
        for key, value in zip(headers, fields):
            if re.fullmatch(r"-?\d+", value):
                parsed[f"snmp_{section}_{key}"] = int(value)
    return parsed


def read_endpoint_counters(endpoint: Endpoint, raw_dir: Path, tag: str) -> dict[str, int]:
    counters: dict[str, int] = {}

    commands = {
        "ip_link.json": namespace_command(
            endpoint.namespace,
            ["ip", "-j", "-s", "link", "show", "dev", endpoint.interface],
        ),
        "ethtool_stats.txt": namespace_command(
            endpoint.namespace,
            ["ethtool", "-S", endpoint.interface],
        ),
        "snmp.txt": namespace_command(endpoint.namespace, ["cat", "/proc/net/snmp"]),
        "softnet_stat.txt": namespace_command(endpoint.namespace, ["cat", "/proc/net/softnet_stat"]),
    }

    for suffix, command in commands.items():
        result = safe_command(command)
        path = raw_dir / tag / endpoint.name / suffix
        write_text(path, result.stdout + result.stderr)
        if result.returncode != 0:
            continue
        if suffix == "ip_link.json":
            counters.update(parse_ip_link_stats(result.stdout))
        elif suffix == "ethtool_stats.txt":
            counters.update({f"ethtool_{key}": value for key, value in parse_ethtool_stats(result.stdout).items()})
        elif suffix == "snmp.txt":
            counters.update(parse_snmp(result.stdout))

    return counters


def snapshot(endpoints: tuple[Endpoint, Endpoint], raw_dir: Path, tag: str) -> dict[str, dict[str, int]]:
    return {endpoint.name: read_endpoint_counters(endpoint, raw_dir, tag) for endpoint in endpoints}


def diff_snapshots(
    before: dict[str, dict[str, int]],
    after: dict[str, dict[str, int]],
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for endpoint, before_counters in before.items():
        after_counters = after.get(endpoint, {})
        keys = sorted(set(before_counters) | set(after_counters))
        for key in keys:
            start = before_counters.get(key, 0)
            end = after_counters.get(key, 0)
            delta = end - start
            if delta == 0:
                continue
            key_lower = key.lower()
            suspicious = (
                any(pattern in key_lower for pattern in SUSPICIOUS_PATTERNS)
                and not any(pattern in key_lower for pattern in BENIGN_COUNTER_PATTERNS)
            )
            rows.append(
                {
                    "endpoint": endpoint,
                    "counter": key,
                    "before": start,
                    "after": end,
                    "delta": delta,
                    "suspicious": "yes" if suspicious else "no",
                }
            )
    rows.sort(key=lambda row: (row["suspicious"] != "yes", row["endpoint"], row["counter"]))
    return rows


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        path.write_text("")
        return
    with path.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def iperf_server_command(test: TestCase, args: argparse.Namespace) -> list[str]:
    command = ["iperf3", "-s", "-1", "-p", str(args.iperf_port), "-J"]
    if args.socket_buffer:
        command.extend(["-w", args.socket_buffer])
    return namespace_command(test.destination.namespace, command)


def iperf_client_command(test: TestCase, args: argparse.Namespace) -> list[str]:
    total_bps = theoretical_goodput_bps(args.payload, args.link_mbps) * args.load_factor
    stream_bps = total_bps / args.udp_streams
    command = [
        "iperf3",
        "-c",
        test.destination.ip,
        "-p",
        str(args.iperf_port),
        "-t",
        str(args.duration),
        "-O",
        str(args.omit),
        "-J",
    ]
    if args.udp_streams > 1:
        command.extend(["-P", str(args.udp_streams)])
    command.extend(
        [
            "-u",
            "-b",
            str(round(stream_bps)),
            "-l",
            str(args.payload),
            "--pacing-timer",
            str(args.pacing_timer),
        ]
    )
    return namespace_command(test.source.namespace, command)


def udp_receiver_summary(data: dict[str, Any]) -> dict[str, Any]:
    end = data.get("end", {})
    for key in ("sum_received", "sum"):
        value = end.get(key)
        if isinstance(value, dict) and "bits_per_second" in value:
            return value
    for stream in end.get("streams", []):
        udp = stream.get("udp", {}) if isinstance(stream, dict) else {}
        if isinstance(udp, dict) and udp.get("sender") is False:
            return udp
    return {}


def number(data: dict[str, Any], key: str) -> float:
    try:
        return float(data.get(key, 0.0))
    except (TypeError, ValueError):
        return 0.0


def short_text(value: str, limit: int = 700) -> str:
    text = value.strip()
    if len(text) <= limit:
        return text
    return text[:limit] + " ...[truncated]"


def run_iperf3(test: TestCase, args: argparse.Namespace, output_dir: Path) -> dict[str, Any]:
    server_command = iperf_server_command(test, args)
    client_command = iperf_client_command(test, args)
    print("server: " + " ".join(server_command))
    print("client: " + " ".join(client_command))

    if args.dry_run:
        return {"tool": "iperf3", "returncode": 0}

    server = subprocess.Popen(
        server_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env={**os.environ, "LC_ALL": "C"},
    )
    try:
        time.sleep(args.server_wait)
        client = safe_command(client_command, timeout=args.duration + args.omit + 20)
        server_stdout, server_stderr = server.communicate(timeout=10)
    except Exception:
        server.terminate()
        try:
            server_stdout, server_stderr = server.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
            server_stdout, server_stderr = server.communicate()
        raise

    write_text(output_dir / "raw" / "traffic" / "iperf_client.json", client.stdout)
    write_text(output_dir / "raw" / "traffic" / "iperf_client.stderr.txt", client.stderr)
    write_text(output_dir / "raw" / "traffic" / "iperf_server.json", server_stdout)
    write_text(output_dir / "raw" / "traffic" / "iperf_server.stderr.txt", server_stderr)

    data = json.loads(server_stdout) if server_stdout.strip() else {}
    if not udp_receiver_summary(data):
        data = json.loads(client.stdout) if client.stdout.strip() else {}
    summary = udp_receiver_summary(data)
    throughput_mbps = number(summary, "bits_per_second") / 1_000_000.0
    status = "ok" if client.returncode == 0 and throughput_mbps > 0.0 else "failed_or_unparsed"
    return {
        "tool": "iperf3",
        "status": status,
        "returncode": client.returncode,
        "server_returncode": server.returncode,
        "throughput_mbps": throughput_mbps,
        "lost_percent": number(summary, "lost_percent"),
        "lost_packets": number(summary, "lost_packets"),
        "packets": number(summary, "packets"),
        "client_stderr": short_text(client.stderr),
        "server_stderr": short_text(server_stderr),
        "client_stdout_hint": short_text(client.stdout),
        "server_stdout_hint": short_text(server_stdout),
    }


def netperf_server_command(test: TestCase, args: argparse.Namespace) -> list[str]:
    return namespace_command(
        test.destination.namespace,
        ["netserver", "-D", "-p", str(args.netperf_port)],
    )


def netperf_client_command(test: TestCase, args: argparse.Namespace) -> list[str]:
    command = [
        "netperf",
        "-H",
        test.destination.ip,
        "-p",
        str(args.netperf_port),
        "-l",
        str(args.duration),
        "-t",
        "UDP_STREAM",
        "-f",
        "m",
        "-P",
        "0",
        "--",
        "-m",
        str(args.payload),
    ]
    if args.netperf_buffer:
        command.extend(["-s", args.netperf_buffer, "-S", args.netperf_buffer])
    return namespace_command(test.source.namespace, command)


def parse_netperf_throughput(output: str) -> float:
    numeric_rows = []
    for line in output.splitlines():
        values = [float(value) for value in re.findall(r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)", line)]
        if len(values) >= 3:
            numeric_rows.append(values)
    if not numeric_rows:
        return 0.0
    return numeric_rows[-1][-1]


def run_netperf(test: TestCase, args: argparse.Namespace, output_dir: Path) -> dict[str, Any]:
    server_command = netperf_server_command(test, args)
    client_command = netperf_client_command(test, args)
    print("server: " + " ".join(server_command))
    print("client: " + " ".join(client_command))

    if args.dry_run:
        return {"tool": "netperf", "returncode": 0}

    server = subprocess.Popen(
        server_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env={**os.environ, "LC_ALL": "C"},
    )
    try:
        time.sleep(args.server_wait)
        client = safe_command(client_command, timeout=args.duration + 20)
    finally:
        server.terminate()
        try:
            server_stdout, server_stderr = server.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
            server_stdout, server_stderr = server.communicate()

    write_text(output_dir / "raw" / "traffic" / "netperf_stdout.txt", client.stdout)
    write_text(output_dir / "raw" / "traffic" / "netperf_stderr.txt", client.stderr)
    write_text(output_dir / "raw" / "traffic" / "netserver_stdout.txt", server_stdout)
    write_text(output_dir / "raw" / "traffic" / "netserver_stderr.txt", server_stderr)

    throughput_mbps = parse_netperf_throughput(client.stdout)
    return {
        "tool": "netperf",
        "status": "ok" if client.returncode == 0 and throughput_mbps > 0.0 else "failed_or_unparsed",
        "returncode": client.returncode,
        "server_returncode": server.returncode,
        "throughput_mbps": throughput_mbps,
        "client_stderr": short_text(client.stderr),
        "server_stderr": short_text(server_stderr),
        "client_stdout_hint": short_text(client.stdout),
        "server_stdout_hint": short_text(server_stdout),
    }


def write_summary(output_dir: Path, args: argparse.Namespace, traffic: dict[str, Any], rows: list[dict[str, Any]]) -> None:
    suspicious = [row for row in rows if row["suspicious"] == "yes" and row["delta"] > 0]
    by_endpoint: dict[str, int] = {}
    for row in suspicious:
        by_endpoint[row["endpoint"]] = by_endpoint.get(row["endpoint"], 0) + int(row["delta"])

    lines = [
        "# 10GbE debug summary",
        f"direction: {args.direction}",
        f"tool: {traffic.get('tool')}",
        f"traffic_status: {traffic.get('status', '')}",
        f"client_returncode: {traffic.get('returncode', '')}",
        f"server_returncode: {traffic.get('server_returncode', '')}",
        f"payload: {args.payload} bytes",
        f"duration: {args.duration} s",
        f"throughput_mbps: {traffic.get('throughput_mbps', '')}",
        f"lost_percent: {traffic.get('lost_percent', '')}",
        f"lost_packets: {traffic.get('lost_packets', '')}",
        "",
    ]
    if traffic.get("status") != "ok":
        lines.extend(
            [
                "Traffic test warning:",
                "- La prueba no reportó throughput válido; los contadores de datos no son concluyentes todavía.",
                "- Revisa raw/traffic/*stderr.txt para ver por qué falló el cliente o servidor.",
            ]
        )
        for field in ("client_stderr", "server_stderr", "client_stdout_hint", "server_stdout_hint"):
            if traffic.get(field):
                lines.append(f"- {field}: {traffic[field]}")
        lines.append("")

    lines.append("Suspicious counter deltas by endpoint:")
    if by_endpoint:
        for endpoint, delta in sorted(by_endpoint.items(), key=lambda item: item[1], reverse=True):
            lines.append(f"- {endpoint}: {delta}")
    else:
        lines.append("- No suspicious drop/error counters increased.")

    lines.extend(["", "Top suspicious counters:"])
    for row in suspicious[:25]:
        lines.append(f"- {row['endpoint']}: {row['counter']} +{row['delta']}")

    if by_endpoint:
        worst = max(by_endpoint, key=by_endpoint.get)
        lines.extend(
            [
                "",
                f"Hint: the largest suspicious delta is on '{worst}'.",
                "If this endpoint is the receiver, focus on RX queues/rings/IRQ/NAPI/buffers there.",
                "If this endpoint is the sender, focus on TX queueing, pacing, or driver TX drops.",
            ]
        )

    write_text(output_dir / "summary.txt", "\n".join(lines) + "\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Collect before/after counters around a 10GbE traffic test"
    )
    parser.add_argument(
        "--direction",
        choices=["nic-to-corundum", "corundum-to-nic"],
        default="nic-to-corundum",
    )
    parser.add_argument("--tool", choices=["iperf3", "netperf"], default="iperf3")
    parser.add_argument("--corundum-namespace", default=DEFAULT_CORUNDUM_NAMESPACE)
    parser.add_argument("--nic-namespace", default=DEFAULT_NIC_NAMESPACE)
    parser.add_argument("--corundum-interface", default=DEFAULT_CORUNDUM_INTERFACE)
    parser.add_argument("--nic-interface", default=DEFAULT_NIC_INTERFACE)
    parser.add_argument("--corundum-ip", default=DEFAULT_CORUNDUM_IP)
    parser.add_argument("--nic-ip", default=DEFAULT_NIC_IP)
    parser.add_argument("--payload", type=int, default=DEFAULT_PAYLOAD)
    parser.add_argument("--duration", type=int, default=DEFAULT_DURATION)
    parser.add_argument("--omit", type=int, default=DEFAULT_OMIT)
    parser.add_argument("--load-factor", type=float, default=1.0)
    parser.add_argument("--link-mbps", type=float, default=DEFAULT_LINK_MBPS)
    parser.add_argument("--udp-streams", type=int, default=1)
    parser.add_argument("--pacing-timer", type=int, default=100)
    parser.add_argument("--socket-buffer", help="iperf3 socket buffer, for example 64M")
    parser.add_argument("--netperf-buffer", help="netperf UDP buffer, for example 50M")
    parser.add_argument(
        "--server-wait",
        type=float,
        default=1.0,
        help="seconds to wait after starting iperf3/netserver before launching the client",
    )
    parser.add_argument("--iperf-port", type=int, default=DEFAULT_IPERF_PORT)
    parser.add_argument("--netperf-port", type=int, default=DEFAULT_NETPERF_PORT)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    return parser


def validate_args(args: argparse.Namespace) -> None:
    if args.payload <= 0:
        raise ValueError("payload debe ser positivo")
    if args.duration <= 0:
        raise ValueError("duration debe ser positivo")
    if args.omit < 0:
        raise ValueError("omit no debe ser negativo")
    if not 0.0 < args.load_factor <= 1.0:
        raise ValueError("load-factor debe estar en 0..1")
    if args.udp_streams <= 0:
        raise ValueError("udp-streams debe ser positivo")


def main() -> int:
    args = build_parser().parse_args()
    try:
        validate_args(args)
        if not args.dry_run:
            check_environment(args)

        output_dir = make_output_dir(args.output_dir)
        raw_dir = output_dir / "raw" / "counters"
        endpoints = configured_endpoints(args)
        case = test_case(args)

        print(f"Guardando diagnóstico en: {output_dir}")
        print(f"Dirección: {case.direction} ({case.source.name} -> {case.destination.name})")

        before = snapshot(endpoints, raw_dir, "before") if not args.dry_run else {}
        if args.tool == "iperf3":
            traffic = run_iperf3(case, args, output_dir)
        else:
            traffic = run_netperf(case, args, output_dir)
        after = snapshot(endpoints, raw_dir, "after") if not args.dry_run else {}

        rows = diff_snapshots(before, after)
        write_csv(output_dir / "counter_deltas.csv", rows)
        write_csv(
            output_dir / "traffic_result.csv",
            [{key: value for key, value in traffic.items()}],
        )
        write_summary(output_dir, args, traffic, rows)

        print(f"Resumen: {output_dir / 'summary.txt'}")
        print(f"Deltas:  {output_dir / 'counter_deltas.csv'}")
        return 0
    except (
        json.JSONDecodeError,
        OSError,
        RuntimeError,
        ValueError,
        subprocess.SubprocessError,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
