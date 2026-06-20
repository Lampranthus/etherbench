import importlib.util
import sys
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest import mock


SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts" / "etherbench_10gbe.py"
SPEC = importlib.util.spec_from_file_location("etherbench_10gbe", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class IperfResultTests(unittest.TestCase):
    def test_unknown_ethtool_values(self):
        self.assertTrue(MODULE.is_unknown_ethtool_value("unknown"))
        self.assertTrue(MODULE.is_unknown_ethtool_value("Unknown!"))
        self.assertFalse(MODULE.is_unknown_ethtool_value("10000Mb/s"))

    def test_parse_tcp_result(self):
        data = {
            "end": {
                "sum_sent": {"bits_per_second": 9.7e9, "retransmits": 3},
                "sum_received": {"bits_per_second": 9.6e9},
                "cpu_utilization_percent": {
                    "host_total": 42.5,
                    "remote_total": 31.25,
                },
            }
        }

        result = MODULE.parse_iperf_result(data, "tcp")

        self.assertEqual(result["throughput_bps"], 9.6e9)
        self.assertEqual(result["retransmits"], 3)
        self.assertEqual(result["cpu_host_percent"], 42.5)
        self.assertEqual(result["cpu_remote_percent"], 31.25)

    def test_parse_udp_result(self):
        data = {
            "end": {
                "sum": {
                    "bits_per_second": 8.9e9,
                    "lost_percent": 0.125,
                    "jitter_ms": 0.032,
                    "packets": 100000,
                },
                "cpu_utilization_percent": {
                    "host_total": 55.0,
                    "remote_total": 48.0,
                },
            }
        }

        result = MODULE.parse_iperf_result(data, "udp")

        self.assertEqual(result["throughput_bps"], 8.9e9)
        self.assertEqual(result["lost_percent"], 0.125)
        self.assertEqual(result["jitter_ms"], 0.032)
        self.assertEqual(result["packets"], 100000)

    def test_parse_ping_result(self):
        output = """
10 packets transmitted, 9 received, 10% packet loss, time 12ms
rtt min/avg/max/mdev = 0.071/0.084/0.102/0.009 ms
"""

        result = MODULE.parse_ping_result(output)

        self.assertEqual(result["sent"], 10)
        self.assertEqual(result["received"], 9)
        self.assertEqual(result["lost"], 1)
        self.assertEqual(result["lost_pct"], 10.0)
        self.assertEqual(result["avg_ms"], 0.084)

    def test_theoretical_10gbe_goodput(self):
        self.assertAlmostEqual(
            MODULE.theoretical_goodput_mbps(1440),
            9561.752988,
            places=6,
        )

    def test_summarize_and_plot_generate_four_graphs(self):
        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory)
            for payload in (256, 1440):
                for repeat in (1, 2):
                    MODULE.append_run(
                        output_dir / "rtt_runs.csv",
                        {
                            "timestamp": 1,
                            "direction": "nic-to-corundum",
                            "payload_size": payload,
                            "repeat": repeat,
                            "sent": 10,
                            "received": 10,
                            "lost": 0,
                            "lost_pct": 0.0,
                            "min_ms": 0.07,
                            "avg_ms": 0.08 + repeat / 1000,
                            "max_ms": 0.10,
                            "stddev_ms": 0.01,
                            "returncode": 0,
                        },
                    )
                    MODULE.append_run(
                        output_dir / "runs.csv",
                        {
                            "timestamp": 1,
                            "direction": "nic-to-corundum",
                            "protocol": "udp",
                            "repeat": repeat,
                            "source_interface": "nic0",
                            "destination_interface": "corundum0",
                            "duration_s": 1,
                            "omit_s": 0,
                            "streams": 1,
                            "udp_bandwidth": "8G",
                            "payload_size": payload,
                            "throughput_bps": 7.5e9,
                            "elapsed_s": 1.0,
                            "lost_percent": 0.1,
                            "jitter_ms": 0.02,
                            "packets": 500000,
                            "retransmits": 0,
                            "cpu_host_percent": 50,
                            "cpu_remote_percent": 40,
                            "returncode": 0,
                        },
                    )

            MODULE.summarize(output_dir)
            MODULE.plot(output_dir)

            self.assertTrue((output_dir / "rtt_summary.csv").exists())
            self.assertTrue((output_dir / "udp_summary.csv").exists())
            for name in (
                "rtt_payload_sweep.svg",
                "goodput_payload_sweep.svg",
                "loss_payload_sweep.svg",
                "pps_payload_sweep.svg",
            ):
                self.assertTrue((output_dir / name).exists(), name)


class CommandTests(unittest.TestCase):
    def setUp(self):
        self.corundum = MODULE.Endpoint(
            "corundum", "corundum0_ns", "corundum0", "192.168.1.100"
        )
        self.nic = MODULE.Endpoint("nic", "nic_ns", "nic0", "192.168.1.110")
        self.args = Namespace(
            duration=15,
            omit=2,
            streams=4,
            udp_bandwidth="9G",
            udp_payload=1440,
        )

    def test_test_cases_only_run_from_nic_to_corundum(self):
        args = Namespace(
            corundum_namespace="corundum0_ns",
            nic_namespace="nic_ns",
            corundum_interface="corundum0",
            nic_interface="nic0",
            corundum_ip="192.168.1.100",
            nic_ip="192.168.1.110",
            protocols=["tcp", "udp"],
        )

        tests = MODULE.test_cases(args)

        self.assertEqual(len(tests), 2)
        for test in tests:
            self.assertEqual(test.direction, "nic-to-corundum")
            self.assertEqual(test.source, self.nic)
            self.assertEqual(test.destination, self.corundum)

    def test_nic_to_corundum_udp_command(self):
        test = MODULE.TestCase(
            "nic-to-corundum", "udp", self.nic, self.corundum
        )

        command = MODULE.iperf_client_command(test, self.args, 5201)

        self.assertEqual(command[:4], ["ip", "netns", "exec", "nic_ns"])
        self.assertIn("192.168.1.100", command)
        self.assertIn("-u", command)
        self.assertIn("1440", command)

    def test_snapshots_only_collect_nic_counters(self):
        test = MODULE.TestCase(
            "nic-to-corundum", "udp", self.nic, self.corundum
        )

        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(MODULE, "read_link", return_value={"mtu": 1500}) as read_link:
                with mock.patch.object(
                    MODULE,
                    "read_ethtool_stats",
                    return_value="NIC statistics\n",
                ) as read_stats:
                    MODULE.write_snapshot(Path(directory), "before", test, 1)

            read_link.assert_called_once_with(self.nic)
            read_stats.assert_called_once_with(self.nic)
            files = [path.name for path in Path(directory).rglob("*") if path.is_file()]
            self.assertEqual(len(files), 2)
            self.assertTrue(all("_nic_" in name for name in files))
            self.assertFalse(any("_corundum_" in name for name in files))


if __name__ == "__main__":
    unittest.main()
