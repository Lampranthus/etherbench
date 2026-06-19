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
