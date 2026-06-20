#!/usr/bin/env python3
"""Run reproducible 10GbE tests between Corundum and a conventional NIC."""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from etherbench_sweep import SeriesStyle, draw_plot


DEFAULT_CORUNDUM_NAMESPACE = "corundum0_ns"
DEFAULT_NIC_NAMESPACE = "nic_ns"
DEFAULT_CORUNDUM_INTERFACE = "corundum0"
DEFAULT_NIC_INTERFACE = "nic0"
DEFAULT_CORUNDUM_IP = "192.168.1.100"
DEFAULT_NIC_IP = "192.168.1.110"
DEFAULT_IPERF_PORT = 5201
DEFAULT_DURATION = 15
DEFAULT_OMIT = 2
DEFAULT_REPEAT = 3
DEFAULT_STREAMS = 4
DEFAULT_UDP_BANDWIDTH = "9G"
DEFAULT_UDP_PAYLOAD = 1440
DEFAULT_PAYLOAD_MIN = 256
DEFAULT_PAYLOAD_MAX = 1440
DEFAULT_PAYLOAD_STEP = 74
DEFAULT_RTT_PACKETS = 1000
DEFAULT_PING_INTERVAL = 0.001
DEFAULT_LINK_MBPS = 10_000.0
ETHERNET_OVERHEAD_BYTES = 66


@dataclass(frozen=True)
class Endpoint:
    name: str
    namespace: str
    interface: str
    ip: str


@dataclass(frozen=True)
class TestCase:
    direction: str
    protocol: str
    source: Endpoint
    destination: Endpoint


def run_command(
    command: list[str],
    *,
    check: bool = True,
    capture_output: bool = True,
    timeout: float | None = None,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        check=check,
        capture_output=capture_output,
        text=True,
        timeout=timeout,
        env={**os.environ, "LC_ALL": "C"},
    )


def namespace_command(namespace: str, command: list[str]) -> list[str]:
    return ["ip", "netns", "exec", namespace, *command]


def require_tools(tools: list[str]) -> None:
    missing = [tool for tool in tools if shutil.which(tool) is None]
    if missing:
        raise RuntimeError("missing required tools: " + ", ".join(missing))


def require_root() -> None:
    if os.geteuid() != 0:
        raise PermissionError("network namespace tests must run with sudo")


def configured_endpoints(args: argparse.Namespace) -> tuple[Endpoint, Endpoint]:
    corundum = Endpoint(
        "corundum",
        args.corundum_namespace,
        args.corundum_interface,
        args.corundum_ip,
    )
    nic = Endpoint(
        "nic",
        args.nic_namespace,
        args.nic_interface,
        args.nic_ip,
    )
    return corundum, nic


def existing_namespaces() -> set[str]:
    result = run_command(["ip", "netns", "list"])
    return {
        line.split()[0]
        for line in result.stdout.splitlines()
        if line.strip()
    }


def read_link(endpoint: Endpoint) -> dict[str, Any]:
    result = run_command(
        namespace_command(
            endpoint.namespace,
            ["ip", "-j", "-s", "link", "show", "dev", endpoint.interface],
        )
    )
    links = json.loads(result.stdout)
    if len(links) != 1:
        raise RuntimeError(f"could not identify interface {endpoint.interface}")
    return links[0]


def read_addresses(endpoint: Endpoint) -> list[dict[str, Any]]:
    result = run_command(
        namespace_command(
            endpoint.namespace,
            ["ip", "-j", "addr", "show", "dev", endpoint.interface],
        )
    )
    return json.loads(result.stdout)


def read_ethtool(endpoint: Endpoint) -> str:
    return run_command(
        namespace_command(endpoint.namespace, ["ethtool", endpoint.interface])
    ).stdout


def read_ethtool_driver(endpoint: Endpoint) -> str:
    result = run_command(
        namespace_command(endpoint.namespace, ["ethtool", "-i", endpoint.interface]),
        check=False,
    )
    return result.stdout if result.returncode == 0 else result.stderr


def address_is_configured(endpoint: Endpoint, addresses: list[dict[str, Any]]) -> bool:
    for link in addresses:
        for address in link.get("addr_info", []):
            if address.get("family") == "inet" and address.get("local") == endpoint.ip:
                return True
    return False


def ethtool_field(output: str, field: str) -> str:
    prefix = f"{field}:"
    for line in output.splitlines():
        stripped = line.strip()
        if stripped.startswith(prefix):
            return stripped[len(prefix) :].strip()
    return "unknown"


def is_unknown_ethtool_value(value: str) -> bool:
    return value.strip().lower() in {"", "unknown", "unknown!", "-1"}


def check_endpoint(
    endpoint: Endpoint,
    *,
    allow_unknown_link_details: bool = False,
) -> dict[str, str]:
    link = read_link(endpoint)
    addresses = read_addresses(endpoint)
    ethtool_output = read_ethtool(endpoint)
    driver_output = read_ethtool_driver(endpoint)

    state = str(link.get("operstate", "UNKNOWN"))
    speed = ethtool_field(ethtool_output, "Speed")
    duplex = ethtool_field(ethtool_output, "Duplex")
    detected = ethtool_field(ethtool_output, "Link detected")
    driver = ethtool_field(driver_output, "driver")
    bus_info = ethtool_field(driver_output, "bus-info")
    mtu = str(link.get("mtu", "unknown"))
    ip_ready = address_is_configured(endpoint, addresses)
    speed_unknown = is_unknown_ethtool_value(speed)
    duplex_unknown = is_unknown_ethtool_value(duplex)

    print(f"[{endpoint.name}]")
    print(f"  namespace: {endpoint.namespace}")
    print(f"  interface: {endpoint.interface}")
    print(f"  address:   {endpoint.ip} ({'ok' if ip_ready else 'missing'})")
    print(f"  state:     {state}")
    print(f"  speed:     {speed}")
    print(f"  duplex:    {duplex}")
    print(f"  link:      {detected}")
    print(f"  MTU:       {mtu}")
    print(f"  driver:    {driver}")
    print(f"  bus:       {bus_info}")

    if not ip_ready:
        raise RuntimeError(f"{endpoint.ip} is not configured on {endpoint.interface}")
    if state != "UP":
        raise RuntimeError(f"{endpoint.interface} is not operationally UP")
    if detected.lower() != "yes":
        raise RuntimeError(f"link is not detected on {endpoint.interface}")
    if speed_unknown and allow_unknown_link_details:
        print(
            f"  warning:   {endpoint.interface} does not expose link speed via ethtool; "
            "10GbE speed cannot be confirmed here"
        )
    elif speed != "10000Mb/s":
        raise RuntimeError(f"{endpoint.interface} reports {speed}, expected 10000Mb/s")
    if duplex_unknown and allow_unknown_link_details:
        print(
            f"  warning:   {endpoint.interface} does not expose duplex via ethtool"
        )
    elif duplex.lower() != "full":
        raise RuntimeError(f"{endpoint.interface} does not report full duplex")

    return {
        "endpoint": endpoint.name,
        "namespace": endpoint.namespace,
        "interface": endpoint.interface,
        "ip": endpoint.ip,
        "state": state,
        "speed": speed,
        "duplex": duplex,
        "link_detected": detected,
        "mtu": mtu,
        "driver": driver,
        "bus_info": bus_info,
        "speed_verified": "no" if speed_unknown else "yes",
    }


def ping_peer(source: Endpoint, destination: Endpoint) -> None:
    command = namespace_command(
        source.namespace,
        ["ping", "-c", "3", "-W", "1", destination.ip],
    )
    result = run_command(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(
            f"ping from {source.name} to {destination.name} failed:\n{result.stdout}{result.stderr}"
        )
    print(f"Ping {source.name} -> {destination.name}: ok")


def check_environment(args: argparse.Namespace) -> list[dict[str, str]]:
    require_tools(["ip", "ethtool", "iperf3", "ping"])
    require_root()
    corundum, nic = configured_endpoints(args)
    namespaces = existing_namespaces()

    for endpoint in (corundum, nic):
        if endpoint.namespace not in namespaces:
            raise RuntimeError(f"network namespace not found: {endpoint.namespace}")

    rows = [
        check_endpoint(corundum, allow_unknown_link_details=True),
        check_endpoint(nic),
    ]
    ping_peer(corundum, nic)
    ping_peer(nic, corundum)
    return rows


def check_test_runtime(
    args: argparse.Namespace,
    *,
    require_ping: bool = False,
) -> None:
    tools = ["ip", "iperf3"]
    if require_ping:
        tools.append("ping")
    require_tools(tools)
    require_root()
    corundum, nic = configured_endpoints(args)
    namespaces = existing_namespaces()

    for endpoint in (corundum, nic):
        if endpoint.namespace not in namespaces:
            raise RuntimeError(f"network namespace not found: {endpoint.namespace}")


def make_output_dir(output_dir: Path | None) -> Path:
    if output_dir is None:
        output_dir = Path("results") / time.strftime("10gbe_%Y%m%d_%H%M%S")
    output_dir.mkdir(parents=True, exist_ok=True)
    return output_dir.resolve()


def test_cases(args: argparse.Namespace) -> list[TestCase]:
    corundum, nic = configured_endpoints(args)
    directions = {
        "nic-to-corundum": (nic, corundum),
        "corundum-to-nic": (corundum, nic),
    }
    return [
        TestCase(direction, protocol, *directions[direction])
        for protocol in args.protocols
        for direction in args.directions
    ]


def iperf_server_command(test: TestCase, port: int) -> list[str]:
    return namespace_command(
        test.destination.namespace,
        ["iperf3", "-s", "-1", "-p", str(port), "-J"],
    )


def iperf_client_command(
    test: TestCase,
    args: argparse.Namespace,
    port: int,
) -> list[str]:
    command = namespace_command(
        test.source.namespace,
        [
            "iperf3",
            "-c",
            test.destination.ip,
            "-p",
            str(port),
            "-t",
            str(args.duration),
            "-O",
            str(args.omit),
            "-J",
        ],
    )

    if test.protocol == "tcp":
        command.extend(["-P", str(args.streams)])
    else:
        command.extend(
            [
                "-u",
                "-b",
                args.udp_bandwidth,
                "-l",
                str(args.udp_payload),
            ]
        )

    return command


def number(data: dict[str, Any], *path: str, default: float = 0.0) -> float:
    value: Any = data
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return default
        value = value[key]
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def parse_iperf_result(data: dict[str, Any], protocol: str) -> dict[str, float]:
    end = data.get("end", {})
    cpu = end.get("cpu_utilization_percent", {})

    if protocol == "udp":
        summary = end.get("sum", {})
        return {
            "throughput_bps": number(summary, "bits_per_second"),
            "elapsed_s": number(summary, "seconds"),
            "lost_percent": number(summary, "lost_percent"),
            "jitter_ms": number(summary, "jitter_ms"),
            "packets": number(summary, "packets"),
            "lost_packets": number(summary, "lost_packets"),
            "retransmits": 0.0,
            "cpu_host_percent": number(cpu, "host_total"),
            "cpu_remote_percent": number(cpu, "remote_total"),
        }

    received = end.get("sum_received", {})
    sent = end.get("sum_sent", {})
    return {
        "throughput_bps": number(received, "bits_per_second"),
        "elapsed_s": number(received, "seconds"),
        "lost_percent": 0.0,
        "jitter_ms": 0.0,
        "packets": 0.0,
        "lost_packets": 0.0,
        "retransmits": number(sent, "retransmits"),
        "cpu_host_percent": number(cpu, "host_total"),
        "cpu_remote_percent": number(cpu, "remote_total"),
    }


def run_test_case(
    test: TestCase,
    args: argparse.Namespace,
    output_dir: Path,
    repeat: int,
) -> dict[str, Any]:
    port = args.port
    server_command = iperf_server_command(test, port)
    client_command = iperf_client_command(test, args, port)
    raw_dir = output_dir / "iperf_json"
    raw_dir.mkdir(exist_ok=True)
    payload_part = f"_payload{args.udp_payload}" if test.protocol == "udp" else ""
    stem = f"{test.direction}_{test.protocol}{payload_part}_run{repeat}"

    print(f"[{test.direction}] protocol={test.protocol} run={repeat}/{args.repeat}")
    print("  server: " + " ".join(server_command))
    print("  client: " + " ".join(client_command))

    if args.dry_run:
        return {
            "timestamp": int(time.time()),
            "direction": test.direction,
            "protocol": test.protocol,
            "repeat": repeat,
            "source_interface": test.source.interface,
            "destination_interface": test.destination.interface,
            "duration_s": args.duration,
            "omit_s": args.omit,
            "streams": args.streams if test.protocol == "tcp" else 1,
            "udp_bandwidth": args.udp_bandwidth if test.protocol == "udp" else "",
            "payload_size": args.udp_payload if test.protocol == "udp" else "",
            "throughput_bps": "",
            "elapsed_s": "",
            "lost_percent": "",
            "jitter_ms": "",
            "packets": "",
            "lost_packets": "",
            "retransmits": "",
            "cpu_host_percent": "",
            "cpu_remote_percent": "",
            "returncode": 0,
        }

    server = subprocess.Popen(
        server_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env={**os.environ, "LC_ALL": "C"},
    )

    try:
        time.sleep(0.4)
        client = run_command(
            client_command,
            check=False,
            timeout=args.duration + args.omit + 15,
        )
        server_stdout, server_stderr = server.communicate(timeout=10)
    except Exception:
        server.terminate()
        try:
            server.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            server.kill()
            server.communicate()
        raise

    (raw_dir / f"{stem}_client.json").write_text(client.stdout)
    (raw_dir / f"{stem}_client.stderr.txt").write_text(client.stderr)
    (raw_dir / f"{stem}_server.json").write_text(server_stdout)
    (raw_dir / f"{stem}_server.stderr.txt").write_text(server_stderr)

    if client.returncode != 0:
        raise RuntimeError(
            f"iperf3 client failed with exit code {client.returncode}: {client.stderr}"
        )
    if server.returncode != 0:
        raise RuntimeError(
            f"iperf3 server failed with exit code {server.returncode}: {server_stderr}"
        )

    client_data = json.loads(client.stdout)
    server_data = json.loads(server_stdout)
    for role, data in (("client", client_data), ("server", server_data)):
        if data.get("error"):
            raise RuntimeError(f"iperf3 {role} reported: {data['error']}")

    # The destination is the authoritative source for received throughput and
    # UDP loss. TCP retransmissions are only reported by the sending client.
    metrics = parse_iperf_result(server_data, test.protocol)
    if test.protocol == "tcp":
        client_metrics = parse_iperf_result(client_data, test.protocol)
        metrics["retransmits"] = client_metrics["retransmits"]

    return {
        "timestamp": int(time.time()),
        "direction": test.direction,
        "protocol": test.protocol,
        "repeat": repeat,
        "source_interface": test.source.interface,
        "destination_interface": test.destination.interface,
        "duration_s": args.duration,
        "omit_s": args.omit,
        "streams": args.streams if test.protocol == "tcp" else 1,
        "udp_bandwidth": args.udp_bandwidth if test.protocol == "udp" else "",
        "payload_size": args.udp_payload if test.protocol == "udp" else "",
        **metrics,
        "returncode": client.returncode,
    }


def append_run(path: Path, row: dict[str, Any]) -> None:
    write_header = not path.exists() or path.stat().st_size == 0
    with path.open("a", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(row))
        if write_header:
            writer.writeheader()
        writer.writerow(row)


def theoretical_goodput_mbps(payload_size: int) -> float:
    return (
        DEFAULT_LINK_MBPS
        * payload_size
        / (payload_size + ETHERNET_OVERHEAD_BYTES)
    )


def theoretical_pps(payload_size: int) -> float:
    return (DEFAULT_LINK_MBPS * 1_000_000.0) / (
        (payload_size + ETHERNET_OVERHEAD_BYTES) * 8.0
    )


def parse_payloads(args: argparse.Namespace) -> list[int]:
    if args.payloads:
        payloads = [int(value.strip(), 0) for value in args.payloads.split(",")]
    else:
        payloads = list(
            range(args.payload_min, args.payload_max + 1, args.payload_step)
        )
        if not payloads:
            raise ValueError("payload range is empty; check min, max and step")
        if payloads[-1] != args.payload_max:
            payloads.append(args.payload_max)

    payloads = sorted(set(payloads))
    for payload in payloads:
        if payload < DEFAULT_PAYLOAD_MIN or payload > DEFAULT_PAYLOAD_MAX:
            raise ValueError(
                f"payload {payload} is outside "
                f"{DEFAULT_PAYLOAD_MIN}..{DEFAULT_PAYLOAD_MAX}"
            )
    return payloads


def parse_ping_result(output: str) -> dict[str, float]:
    packet_match = re.search(
        r"(\d+) packets transmitted, (\d+) received,.*?([\d.]+)% packet loss",
        output,
    )
    if packet_match is None:
        raise RuntimeError("could not parse ping packet summary")

    sent = int(packet_match.group(1))
    received = int(packet_match.group(2))
    lost_pct = float(packet_match.group(3))
    result = {
        "sent": float(sent),
        "received": float(received),
        "lost": float(max(sent - received, 0)),
        "lost_pct": lost_pct,
        "min_ms": 0.0,
        "avg_ms": 0.0,
        "max_ms": 0.0,
        "stddev_ms": 0.0,
    }

    rtt_match = re.search(
        r"=\s*([\d.]+)/([\d.]+)/([\d.]+)/([\d.]+)\s*ms",
        output,
    )
    if rtt_match is not None:
        result.update(
            {
                "min_ms": float(rtt_match.group(1)),
                "avg_ms": float(rtt_match.group(2)),
                "max_ms": float(rtt_match.group(3)),
                "stddev_ms": float(rtt_match.group(4)),
            }
        )
    return result


def run_rtt_test(
    source: Endpoint,
    destination: Endpoint,
    args: argparse.Namespace,
    output_dir: Path,
    payload: int,
    repeat: int,
) -> dict[str, Any]:
    direction = f"{source.name}-to-{destination.name}"
    command = namespace_command(
        source.namespace,
        [
            "ping",
            "-n",
            "-q",
            "-c",
            str(args.rtt_packets),
            "-i",
            str(args.ping_interval),
            "-W",
            "1",
            "-s",
            str(payload),
            destination.ip,
        ],
    )
    print(f"[{direction}] protocol=rtt payload={payload} run={repeat}/{args.repeat}")
    print("  command: " + " ".join(command))

    base_row: dict[str, Any] = {
        "timestamp": int(time.time()),
        "direction": direction,
        "payload_size": payload,
        "repeat": repeat,
        "sent": args.rtt_packets,
        "received": "",
        "lost": "",
        "lost_pct": "",
        "min_ms": "",
        "avg_ms": "",
        "max_ms": "",
        "stddev_ms": "",
        "returncode": 0,
    }
    if args.dry_run:
        return base_row

    result = run_command(
        command,
        check=False,
        timeout=max(args.rtt_packets * args.ping_interval + 10.0, 15.0),
    )
    raw_dir = output_dir / "ping_raw"
    raw_dir.mkdir(exist_ok=True)
    stem = f"{direction}_rtt_payload{payload}_run{repeat}"
    (raw_dir / f"{stem}.txt").write_text(result.stdout)
    (raw_dir / f"{stem}.stderr.txt").write_text(result.stderr)

    metrics = parse_ping_result(result.stdout)
    base_row.update(metrics)
    base_row["returncode"] = result.returncode
    if result.returncode not in (0, 1):
        raise RuntimeError(
            f"ping failed with exit code {result.returncode}: {result.stderr}"
        )
    return base_row


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="") as file:
        return list(csv.DictReader(file))


def mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def stdev(values: list[float]) -> float:
    return statistics.stdev(values) if len(values) > 1 else 0.0


def group_by_direction_payload(
    rows: list[dict[str, str]],
) -> dict[tuple[str, int], list[dict[str, str]]]:
    grouped: dict[tuple[str, int], list[dict[str, str]]] = {}
    for row in rows:
        if not row.get("payload_size"):
            continue
        key = (row.get("direction", "unknown"), int(row["payload_size"]))
        grouped.setdefault(key, []).append(row)
    return grouped


def write_summary(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    with path.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def summarize(output_dir: Path) -> None:
    rtt_summary: list[dict[str, Any]] = []
    for (direction, payload), rows in sorted(
        group_by_direction_payload(read_csv(output_dir / "rtt_runs.csv")).items()
    ):
        valid = [row for row in rows if row.get("avg_ms")]
        rtt_values = [float(row["avg_ms"]) for row in valid]
        loss_values = [float(row["lost_pct"]) for row in rows if row.get("lost_pct")]
        rtt_summary.append(
            {
                "direction": direction,
                "payload_size": payload,
                "runs": len(rows),
                "avg_ms_mean": f"{mean(rtt_values):.9f}",
                "avg_ms_std": f"{stdev(rtt_values):.9f}",
                "lost_pct_mean": f"{mean(loss_values):.9f}",
                "lost_pct_std": f"{stdev(loss_values):.9f}",
            }
        )

    udp_rows = [
        row
        for row in read_csv(output_dir / "runs.csv")
        if row.get("protocol") == "udp" and row.get("throughput_bps")
    ]
    udp_summary: list[dict[str, Any]] = []
    for (direction, payload), rows in sorted(
        group_by_direction_payload(udp_rows).items()
    ):
        goodput = [float(row["throughput_bps"]) / 1_000_000.0 for row in rows]
        loss = [float(row["lost_percent"]) for row in rows]
        lost_packets = [float(row.get("lost_packets") or 0.0) for row in rows]
        pps = [
            float(row["packets"]) / float(row["elapsed_s"])
            for row in rows
            if float(row.get("elapsed_s") or 0.0) > 0.0
        ]
        udp_summary.append(
            {
                "direction": direction,
                "payload_size": payload,
                "runs": len(rows),
                "goodput_mbps_mean": f"{mean(goodput):.9f}",
                "goodput_mbps_std": f"{stdev(goodput):.9f}",
                "pps_mean": f"{mean(pps):.9f}",
                "pps_std": f"{stdev(pps):.9f}",
                "lost_pct_mean": f"{mean(loss):.9f}",
                "lost_pct_std": f"{stdev(loss):.9f}",
                "lost_packets_mean": f"{mean(lost_packets):.9f}",
                "lost_packets_std": f"{stdev(lost_packets):.9f}",
                "theoretical_goodput_mbps": f"{theoretical_goodput_mbps(payload):.9f}",
                "theoretical_pps": f"{theoretical_pps(payload):.9f}",
            }
        )

    write_summary(output_dir / "rtt_summary.csv", rtt_summary)
    write_summary(output_dir / "udp_summary.csv", udp_summary)


def pivot_direction_rows(
    rows: list[dict[str, str]],
    metrics: list[str],
) -> list[dict[str, str]]:
    pivoted: dict[int, dict[str, str]] = {}
    for row in rows:
        payload = int(row["payload_size"])
        output = pivoted.setdefault(payload, {"payload_size": str(payload)})
        prefix = row.get("direction", "nic-to-corundum").replace("-", "_")
        for metric in metrics:
            if metric in row:
                output[f"{prefix}_{metric}"] = row[metric]
        for theoretical in ("theoretical_goodput_mbps", "theoretical_pps"):
            if theoretical in row:
                output[theoretical] = row[theoretical]
    return [pivoted[payload] for payload in sorted(pivoted)]


def plot(output_dir: Path) -> None:
    rtt_rows = pivot_direction_rows(
        read_csv(output_dir / "rtt_summary.csv"),
        ["avg_ms_mean", "avg_ms_std"],
    )
    udp_rows = pivot_direction_rows(
        read_csv(output_dir / "udp_summary.csv"),
        [
            "goodput_mbps_mean",
            "goodput_mbps_std",
            "pps_mean",
            "pps_std",
            "lost_pct_mean",
            "lost_pct_std",
        ],
    )

    if rtt_rows:
        draw_plot(
            output_dir / "rtt_payload_sweep.svg",
            "RTT entre NIC y Corundum vs. payload",
            "Tamaño del payload (bytes)",
            "RTT promedio (ms)",
            None,
            rtt_rows,
            [
                SeriesStyle(
                    "nic_to_corundum_avg_ms_mean",
                    "NIC a Corundum",
                    "#1f77b4",
                    "left",
                ),
                SeriesStyle(
                    "corundum_to_nic_avg_ms_mean",
                    "Corundum a NIC",
                    "#ff7f0e",
                    "left",
                    dashed=True,
                ),
            ],
        )

    if udp_rows:
        goodput_series = [
            SeriesStyle(
                "nic_to_corundum_goodput_mbps_mean",
                "NIC a Corundum",
                "#2ca02c",
                "left",
            ),
            SeriesStyle(
                "corundum_to_nic_goodput_mbps_mean",
                "Corundum a NIC",
                "#ff7f0e",
                "left",
                dashed=True,
            ),
        ]
        theoretical = SeriesStyle(
            "theoretical_goodput_mbps",
            "Límite teórico 10GbE",
            "#111111",
            "left",
            dashed=True,
        )
        draw_plot(
            output_dir / "goodput_payload_sweep.svg",
            "Goodput bidireccional vs. payload",
            "Tamaño del payload (bytes)",
            "Goodput de payload (Mbps)",
            None,
            udp_rows,
            [*goodput_series, theoretical],
        )
        draw_plot(
            output_dir / "loss_payload_sweep.svg",
            "Pérdidas UDP bidireccionales vs. payload",
            "Tamaño del payload (bytes)",
            "Pérdida (%)",
            None,
            udp_rows,
            [
                SeriesStyle(
                    "nic_to_corundum_lost_pct_mean",
                    "NIC a Corundum",
                    "#d62728",
                    "left",
                ),
                SeriesStyle(
                    "corundum_to_nic_lost_pct_mean",
                    "Corundum a NIC",
                    "#9467bd",
                    "left",
                    dashed=True,
                ),
            ],
        )
        draw_plot(
            output_dir / "pps_payload_sweep.svg",
            "PPS bidireccionales vs. payload",
            "Tamaño del payload (bytes)",
            "Paquetes por segundo",
            None,
            udp_rows,
            [
                SeriesStyle(
                    "nic_to_corundum_pps_mean",
                    "NIC a Corundum",
                    "#9467bd",
                    "left",
                ),
                SeriesStyle(
                    "corundum_to_nic_pps_mean",
                    "Corundum a NIC",
                    "#17becf",
                    "left",
                    dashed=True,
                ),
                SeriesStyle(
                    "theoretical_pps",
                    "Límite teórico 10GbE",
                    "#111111",
                    "left",
                    dashed=True,
                ),
            ],
        )


def run_sweep(args: argparse.Namespace) -> Path:
    if not args.dry_run:
        check_test_runtime(args, require_ping=True)
    output_dir = make_output_dir(args.output_dir)

    udp_tests = test_cases(args)
    payloads = parse_payloads(args)

    for payload in payloads:
        args.udp_payload = payload
        args.udp_bandwidth = str(
            round(theoretical_goodput_mbps(payload) * 1_000_000.0 * args.load_factor)
        )
        for repeat in range(1, args.repeat + 1):
            for test in udp_tests:
                rtt_row = run_rtt_test(
                    test.source,
                    test.destination,
                    args,
                    output_dir,
                    payload,
                    repeat,
                )
                append_run(output_dir / "rtt_runs.csv", rtt_row)
                udp_row = run_test_case(test, args, output_dir, repeat)
                append_run(output_dir / "runs.csv", udp_row)

    if not args.dry_run:
        summarize(output_dir)
        plot(output_dir)
    return output_dir


def run_suite(args: argparse.Namespace) -> Path:
    if not args.dry_run:
        check_test_runtime(args)

    output_dir = make_output_dir(args.output_dir)

    runs_path = output_dir / "runs.csv"
    for test in test_cases(args):
        for repeat in range(1, args.repeat + 1):
            row = run_test_case(test, args, output_dir, repeat)
            append_run(runs_path, row)

    return output_dir


def add_endpoint_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--corundum-namespace", default=DEFAULT_CORUNDUM_NAMESPACE)
    parser.add_argument("--nic-namespace", default=DEFAULT_NIC_NAMESPACE)
    parser.add_argument("--corundum-interface", default=DEFAULT_CORUNDUM_INTERFACE)
    parser.add_argument("--nic-interface", default=DEFAULT_NIC_INTERFACE)
    parser.add_argument("--corundum-ip", default=DEFAULT_CORUNDUM_IP)
    parser.add_argument("--nic-ip", default=DEFAULT_NIC_IP)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Test a 10GbE link between Corundum and a conventional NIC"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    check_parser = subparsers.add_parser(
        "check",
        help="validate namespaces, interfaces, 10GbE links and connectivity",
    )
    add_endpoint_arguments(check_parser)

    run_parser = subparsers.add_parser(
        "run",
        help="run bidirectional iperf3 tests between NIC and Corundum",
    )
    add_endpoint_arguments(run_parser)
    run_parser.add_argument(
        "--protocols",
        nargs="+",
        choices=["tcp", "udp"],
        default=["tcp", "udp"],
    )
    run_parser.add_argument(
        "--directions",
        nargs="+",
        choices=["nic-to-corundum", "corundum-to-nic"],
        default=["nic-to-corundum", "corundum-to-nic"],
    )
    run_parser.add_argument("--duration", type=int, default=DEFAULT_DURATION)
    run_parser.add_argument("--omit", type=int, default=DEFAULT_OMIT)
    run_parser.add_argument("--repeat", type=int, default=DEFAULT_REPEAT)
    run_parser.add_argument("--streams", type=int, default=DEFAULT_STREAMS)
    run_parser.add_argument("--udp-bandwidth", default=DEFAULT_UDP_BANDWIDTH)
    run_parser.add_argument("--udp-payload", type=int, default=DEFAULT_UDP_PAYLOAD)
    run_parser.add_argument("--port", type=int, default=DEFAULT_IPERF_PORT)
    run_parser.add_argument("--output-dir", type=Path)
    run_parser.add_argument("--dry-run", action="store_true")

    sweep_parser = subparsers.add_parser(
        "sweep",
        help="sweep payloads for bidirectional RTT and UDP measurements",
    )
    add_endpoint_arguments(sweep_parser)
    sweep_parser.add_argument("--payload-min", type=int, default=DEFAULT_PAYLOAD_MIN)
    sweep_parser.add_argument("--payload-max", type=int, default=DEFAULT_PAYLOAD_MAX)
    sweep_parser.add_argument("--payload-step", type=int, default=DEFAULT_PAYLOAD_STEP)
    sweep_parser.add_argument(
        "--payloads",
        help="comma-separated payload list; overrides min/max/step",
    )
    sweep_parser.add_argument("--repeat", type=int, default=DEFAULT_REPEAT)
    sweep_parser.add_argument(
        "--directions",
        nargs="+",
        choices=["nic-to-corundum", "corundum-to-nic"],
        default=["nic-to-corundum", "corundum-to-nic"],
    )
    sweep_parser.add_argument("--duration", type=int, default=DEFAULT_DURATION)
    sweep_parser.add_argument("--omit", type=int, default=DEFAULT_OMIT)
    sweep_parser.add_argument("--rtt-packets", type=int, default=DEFAULT_RTT_PACKETS)
    sweep_parser.add_argument(
        "--ping-interval",
        type=float,
        default=DEFAULT_PING_INTERVAL,
    )
    sweep_parser.add_argument(
        "--load-factor",
        type=float,
        default=1.0,
        help="fraction of theoretical 10GbE payload goodput offered by iperf3",
    )
    sweep_parser.add_argument("--port", type=int, default=DEFAULT_IPERF_PORT)
    sweep_parser.add_argument("--output-dir", type=Path)
    sweep_parser.add_argument("--dry-run", action="store_true")
    sweep_parser.set_defaults(
        streams=1,
        protocols=["udp"],
        udp_bandwidth="",
        udp_payload=DEFAULT_UDP_PAYLOAD,
    )

    summary_parser = subparsers.add_parser(
        "summarize",
        help="rebuild RTT and UDP summaries from a 10GbE result directory",
    )
    summary_parser.add_argument("--output-dir", type=Path, required=True)

    plot_parser = subparsers.add_parser(
        "plot",
        help="rebuild 10GbE SVG plots from summary CSV files",
    )
    plot_parser.add_argument("--output-dir", type=Path, required=True)

    return parser


def validate_positive(value: int, name: str) -> None:
    if value <= 0:
        raise ValueError(f"{name} must be greater than zero")


def validate_args(args: argparse.Namespace) -> None:
    if args.command not in {"run", "sweep"}:
        return
    validate_positive(args.duration, "duration")
    validate_positive(args.repeat, "repeat")
    validate_positive(args.streams, "streams")
    validate_positive(args.udp_payload, "udp payload")
    if args.omit < 0:
        raise ValueError("omit must not be negative")
    if args.port < 1 or args.port > 65535:
        raise ValueError("port must be between 1 and 65535")
    if args.command == "sweep":
        validate_positive(args.payload_step, "payload step")
        validate_positive(args.rtt_packets, "RTT packets")
        if args.ping_interval <= 0.0:
            raise ValueError("ping interval must be greater than zero")
        if args.load_factor <= 0.0 or args.load_factor > 1.0:
            raise ValueError("load factor must be greater than zero and at most 1")
        parse_payloads(args)


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    try:
        validate_args(args)
        if args.command == "check":
            check_environment(args)
            print("10GbE environment check passed")
            return 0

        if args.command == "run":
            output_dir = run_suite(args)
            print(f"Results written to: {output_dir}")
            return 0

        if args.command == "sweep":
            output_dir = run_sweep(args)
            print(f"Sweep results written to: {output_dir}")
            return 0

        if args.command == "summarize":
            summarize(args.output_dir.resolve())
            print(f"Summaries written to: {args.output_dir.resolve()}")
            return 0

        if args.command == "plot":
            plot(args.output_dir.resolve())
            print(f"Plots written to: {args.output_dir.resolve()}")
            return 0
    except (
        json.JSONDecodeError,
        OSError,
        RuntimeError,
        ValueError,
        subprocess.SubprocessError,
    ) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
