#!/usr/bin/env python3
"""Run reproducible 10GbE tests between Corundum and a conventional NIC."""

from __future__ import annotations

import argparse
import csv
import json
import os
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
DEFAULT_DURATION = 15
DEFAULT_OMIT = 2
DEFAULT_REPEAT = 3
DEFAULT_STREAMS = 4
DEFAULT_UDP_BANDWIDTH = "9G"
DEFAULT_UDP_PAYLOAD = 1440


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


def read_ethtool_stats(endpoint: Endpoint) -> str:
    result = run_command(
        namespace_command(endpoint.namespace, ["ethtool", "-S", endpoint.interface]),
        check=False,
    )
    if result.returncode != 0:
        return result.stderr
    return result.stdout


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


def write_environment(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="") as file:
        writer = csv.DictWriter(file, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def make_output_dir(output_dir: Path | None) -> Path:
    if output_dir is None:
        output_dir = Path("results") / time.strftime("10gbe_%Y%m%d_%H%M%S")
    output_dir.mkdir(parents=True, exist_ok=True)
    return output_dir.resolve()


def test_cases(args: argparse.Namespace) -> list[TestCase]:
    corundum, nic = configured_endpoints(args)
    directions = {
        "corundum-to-nic": (corundum, nic),
        "nic-to-corundum": (nic, corundum),
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


def write_snapshot(
    directory: Path,
    phase: str,
    test: TestCase,
    repeat: int,
) -> None:
    snapshot_dir = directory / f"counters_{phase}"
    snapshot_dir.mkdir(exist_ok=True)

    for endpoint in (test.source, test.destination):
        stem = f"{test.direction}_{test.protocol}_run{repeat}_{endpoint.name}"
        link = read_link(endpoint)
        (snapshot_dir / f"{stem}_link.json").write_text(
            json.dumps(link, indent=2) + "\n"
        )
        (snapshot_dir / f"{stem}_ethtool.txt").write_text(
            read_ethtool_stats(endpoint)
        )


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
            "lost_percent": number(summary, "lost_percent"),
            "jitter_ms": number(summary, "jitter_ms"),
            "packets": number(summary, "packets"),
            "retransmits": 0.0,
            "cpu_host_percent": number(cpu, "host_total"),
            "cpu_remote_percent": number(cpu, "remote_total"),
        }

    received = end.get("sum_received", {})
    sent = end.get("sum_sent", {})
    return {
        "throughput_bps": number(received, "bits_per_second"),
        "lost_percent": 0.0,
        "jitter_ms": 0.0,
        "packets": 0.0,
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
    stem = f"{test.direction}_{test.protocol}_run{repeat}"

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
            "lost_percent": "",
            "jitter_ms": "",
            "packets": "",
            "retransmits": "",
            "cpu_host_percent": "",
            "cpu_remote_percent": "",
            "returncode": 0,
        }

    write_snapshot(output_dir, "before", test, repeat)
    server = subprocess.Popen(
        server_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
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
    write_snapshot(output_dir, "after", test, repeat)

    if client.returncode != 0:
        raise RuntimeError(
            f"iperf3 client failed with exit code {client.returncode}: {client.stderr}"
        )
    if server.returncode != 0:
        raise RuntimeError(
            f"iperf3 server failed with exit code {server.returncode}: {server_stderr}"
        )

    data = json.loads(client.stdout)
    if data.get("error"):
        raise RuntimeError(f"iperf3 reported: {data['error']}")
    metrics = parse_iperf_result(data, test.protocol)

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


def run_suite(args: argparse.Namespace) -> Path:
    require_tools(["ip", "ethtool", "iperf3", "ping"])
    if not args.dry_run:
        environment = check_environment(args)
    else:
        environment = []

    output_dir = make_output_dir(args.output_dir)
    if environment:
        write_environment(output_dir / "environment.csv", environment)

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
        help="run iperf3 tests in both directions and save raw results",
    )
    add_endpoint_arguments(run_parser)
    run_parser.add_argument(
        "--directions",
        nargs="+",
        choices=["corundum-to-nic", "nic-to-corundum"],
        default=["corundum-to-nic", "nic-to-corundum"],
    )
    run_parser.add_argument(
        "--protocols",
        nargs="+",
        choices=["tcp", "udp"],
        default=["tcp", "udp"],
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

    return parser


def validate_positive(value: int, name: str) -> None:
    if value <= 0:
        raise ValueError(f"{name} must be greater than zero")


def validate_args(args: argparse.Namespace) -> None:
    if args.command != "run":
        return
    validate_positive(args.duration, "duration")
    validate_positive(args.repeat, "repeat")
    validate_positive(args.streams, "streams")
    validate_positive(args.udp_payload, "udp payload")
    if args.omit < 0:
        raise ValueError("omit must not be negative")
    if args.port < 1 or args.port > 65535:
        raise ValueError("port must be between 1 and 65535")


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
