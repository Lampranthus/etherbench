#include "config.h"
#include "iface_stats.h"
#include "net_stats.h"
#include "fpga_stats.h"
#include "fpga_mdio.h"
#include "fpga_config.h"
#include "fpga_setup.h"
#include "fpga_ctrl.h"
#include "fpga_rtt.h"
#include "fpga_loopback_load.h"
#include "fpga_loopback_mode.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

static void print_usage(const char *program_name)
{
    printf("Usage:\n");
    printf("  %s iface <interface_name>\n", program_name);
    printf("  %s net\n", program_name);
    printf("  %s fpga <fpga_ip> [fpga_port] [rx_port]\n", program_name);
    printf("  %s mdio-read <fpga_ip> <phy> <reg>\n", program_name);
    printf("  %s mdio-write <fpga_ip> <phy> <reg> <value>\n", program_name);
    printf("  %s mdio-seq <fpga_ip> <phy> <op1> <op2> ...\n", program_name);
    printf("  %s fpga-arp <iface> <fpga_ip> [fpga_port]\n", program_name);
    printf("  %s fpga-setup <iface> <fpga_ip> <phy> [fpga_port]\n", program_name);
    printf("  %s fpga-net <fpga_ip> <field> <value> [fpga_port]\n", program_name);
    printf("  %s fpga-test <fpga_ip> <command> [value] [fpga_port]\n", program_name);
    printf("  %s fpga-rtt <fpga_ip> <packets> <payload_size> [loopback_port] [local_port] [fpga_ctrl_port]\n", program_name);
    printf("  %s fpga-loopback-test-full <iface> <fpga_ip> <packets> <payload_size> [data_port] [local_port] [ctrl_port]\n", program_name);
    printf("  %s all <interface_name>\n", program_name);
    printf("\n");
    printf("Examples:\n");
    printf("  %s iface eth0\n", program_name);
    printf("  %s iface enp3s0\n", program_name);
    printf("  %s net\n", program_name);
    printf("  %s fpga 192.168.1.12\n", program_name);
    printf("  %s fpga 192.168.1.12 55555 9999\n", program_name);
    printf("  %s fpga-net 192.168.1.12 source-ip 192.168.1.12\n", program_name);
    printf("  %s fpga-net 192.168.1.12 dest-ip 192.168.1.10\n", program_name);
    printf("  %s fpga-net 192.168.1.12 src-port 1234\n", program_name);
    printf("  %s fpga-test 192.168.1.12 loopback\n", program_name);
    printf("  %s fpga-test 192.168.1.12 mtu 1440\n", program_name);
    printf("  %s fpga-test 192.168.1.12 pktn 1000\n", program_name);
    printf("  %s fpga-rtt 192.168.1.12 1000 64\n", program_name);
    printf("  %s fpga-rtt 192.168.1.12 1000 64 1234 9999 55555\n", program_name);
    printf("  %s fpga-loopback-test enp5s0 192.168.1.12 100000 1440\n", program_name);
    printf("  %s all eth0\n", program_name);
}

static int log_host_stats_snapshot(
    const char *iface_name,
    const char *iface_log_file,
    const char *net_log_file,
    iface_stats_t *iface_stats,
    net_stats_t *net_stats
)
{
    if (iface_name == NULL ||
        iface_log_file == NULL ||
        net_log_file == NULL ||
        iface_stats == NULL ||
        net_stats == NULL) {
        return -1;
    }

    if (read_iface_stats(iface_name, iface_stats) != 0) {
        fprintf(stderr, "Error: could not read interface stats for %s\n", iface_name);
        return -1;
    }

    if (read_net_stats(net_stats) != 0) {
        fprintf(stderr, "Error: could not read IP/UDP stats\n");
        return -1;
    }

    print_iface_stats(iface_stats);
    print_net_stats(net_stats);

    if (append_iface_stats_csv(iface_log_file, iface_stats) != 0) {
        fprintf(stderr, "Error: could not write log file: %s\n", iface_log_file);
        return -1;
    }

    if (append_net_stats_csv(net_log_file, net_stats) != 0) {
        fprintf(stderr, "Error: could not write log file: %s\n", net_log_file);
        return -1;
    }

    printf("\nSaved to: %s\n", iface_log_file);
    printf("Saved to: %s\n", net_log_file);

    return 0;
}

static int log_fpga_stats_snapshot(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms,
    const char *fpga_log_file,
    fpga_stats_t *fpga_stats
)
{
    if (fpga_ip == NULL ||
        fpga_log_file == NULL ||
        fpga_stats == NULL) {
        return -1;
    }

    printf("Sending regstats to FPGA %s:%d\n", fpga_ip, fpga_ctrl_port);
    printf("Listening for FPGA response on UDP port %d\n", local_port);

    if (query_fpga_stats(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms,
            fpga_stats
        ) != 0) {
        fprintf(stderr, "Error: could not query FPGA regstats\n");
        return -1;
    }

    print_fpga_stats(fpga_stats);

    if (append_fpga_stats_csv(fpga_log_file, fpga_stats) != 0) {
        fprintf(stderr, "Error: could not write log file: %s\n", fpga_log_file);
        return -1;
    }

    printf("\nSaved to: %s\n", fpga_log_file);

    return 0;
}

static uint64_t delta_u64(uint64_t after, uint64_t before)
{
    if (after >= before) {
        return after - before;
    }

    return 0;
}

static int file_exists_main(const char *filename)
{
    FILE *file = fopen(filename, "r");

    if (file == NULL) {
        return 0;
    }

    fclose(file);
    return 1;
}

static int append_loopback_loss_csv(
    const char *filename,
    const char *iface_name,
    const char *fpga_ip,
    int fpga_ctrl_port,
    int fpga_data_port,
    int local_port,
    const fpga_loopback_load_result_t *test,
    const iface_stats_t *iface_before,
    const iface_stats_t *iface_after,
    const net_stats_t *net_before,
    const net_stats_t *net_after,
    const fpga_stats_t *fpga_before,
    const fpga_stats_t *fpga_after
)
{
    FILE *file;
    time_t now;
    int write_header;

    uint64_t iface_tx_packets_delta;
    uint64_t iface_rx_packets_delta;
    uint64_t iface_tx_errors_delta;
    uint64_t iface_rx_errors_delta;
    uint64_t iface_tx_dropped_delta;
    uint64_t iface_rx_dropped_delta;

    uint64_t iface_tx_bytes_delta;
    uint64_t iface_rx_bytes_delta;

    uint64_t udp_out_delta;
    uint64_t udp_in_delta;
    uint64_t udp_no_ports_delta;
    uint64_t udp_in_errors_delta;
    uint64_t udp_rcvbuf_errors_delta;
    uint64_t udp_sndbuf_errors_delta;

    uint64_t fpga_rx_good_delta;
    uint64_t fpga_tx_good_delta;
    uint64_t fpga_rx_bad_delta;
    uint64_t fpga_tx_bad_delta;
    uint64_t fpga_rx_overflow_delta;
    uint64_t fpga_tx_overflow_delta;
    uint64_t fpga_bad_fcs_delta;

    int64_t host_tx_to_fpga_rx_gap;
    int64_t fpga_rx_to_tx_gap;
    int64_t fpga_tx_to_host_rx_gap;

    uint64_t ip_in_receives_delta;
    uint64_t ip_in_discards_delta;
    uint64_t ip_out_requests_delta;
    uint64_t ip_out_discards_delta;

    uint64_t fpga_ip_rx_header_error_delta;
    uint64_t fpga_ip_rx_payload_error_delta;
    uint64_t fpga_ip_invalid_header_delta;
    uint64_t fpga_ip_invalid_checksum_delta;
    uint64_t fpga_ip_tx_payload_error_delta;
    uint64_t fpga_ip_tx_arp_failed_delta;

    uint64_t fpga_udp_rx_header_error_delta;
    uint64_t fpga_udp_rx_payload_error_delta;
    uint64_t fpga_udp_tx_payload_error_delta;

    if (filename == NULL ||
        iface_name == NULL ||
        fpga_ip == NULL ||
        test == NULL ||
        iface_before == NULL ||
        iface_after == NULL ||
        net_before == NULL ||
        net_after == NULL ||
        fpga_before == NULL ||
        fpga_after == NULL) {
        return -1;
    }

    iface_tx_packets_delta = delta_u64(
        iface_after->tx_packets,
        iface_before->tx_packets
    );

    iface_rx_packets_delta = delta_u64(
        iface_after->rx_packets,
        iface_before->rx_packets
    );

    iface_tx_errors_delta = delta_u64(
        iface_after->tx_errors,
        iface_before->tx_errors
    );

    iface_rx_errors_delta = delta_u64(
        iface_after->rx_errors,
        iface_before->rx_errors
    );

    iface_tx_dropped_delta = delta_u64(
        iface_after->tx_dropped,
        iface_before->tx_dropped
    );

    iface_rx_dropped_delta = delta_u64(
        iface_after->rx_dropped,
        iface_before->rx_dropped
    );

    iface_tx_bytes_delta = delta_u64(
        iface_after->tx_bytes,
        iface_before->tx_bytes
    );

    iface_rx_bytes_delta = delta_u64(
        iface_after->rx_bytes,
        iface_before->rx_bytes
    );

    /*
     * Ajusta estos nombres si en tu struct net_stats_t se llaman diferente.
     * Estos son los nombres lógicos de /proc/net/snmp.
     */
    udp_out_delta = delta_u64(
        net_after->udp.out_datagrams,
        net_before->udp.out_datagrams
    );

    udp_in_delta = delta_u64(
        net_after->udp.in_datagrams,
        net_before->udp.in_datagrams
    );

    udp_no_ports_delta = delta_u64(
        net_after->udp.no_ports,
        net_before->udp.no_ports
    );

    udp_in_errors_delta = delta_u64(
        net_after->udp.in_errors,
        net_before->udp.in_errors
    );

    udp_rcvbuf_errors_delta = delta_u64(
        net_after->udp.rcvbuf_errors,
        net_before->udp.rcvbuf_errors
    );

    udp_sndbuf_errors_delta = delta_u64(
        net_after->udp.sndbuf_errors,
        net_before->udp.sndbuf_errors
    );

    ip_in_receives_delta = delta_u64(
        net_after->ip.in_receives,
        net_before->ip.in_receives
    );

    ip_in_discards_delta = delta_u64(
        net_after->ip.in_discards,
        net_before->ip.in_discards
    );

    ip_out_requests_delta = delta_u64(
        net_after->ip.out_requests,
        net_before->ip.out_requests
    );

    ip_out_discards_delta = delta_u64(
        net_after->ip.out_discards,
        net_before->ip.out_discards
    );

    fpga_rx_good_delta = delta_u64(
    fpga_after->eth.rx_fifo_good_frame,
    fpga_before->eth.rx_fifo_good_frame
    );

    fpga_tx_good_delta = delta_u64(
    fpga_after->eth.tx_fifo_good_frame,
    fpga_before->eth.tx_fifo_good_frame
    );

    fpga_rx_bad_delta = delta_u64(
    fpga_after->eth.rx_fifo_bad_frame,
    fpga_before->eth.rx_fifo_bad_frame
    );

    fpga_tx_bad_delta = delta_u64(
    fpga_after->eth.tx_fifo_bad_frame,
    fpga_before->eth.tx_fifo_bad_frame
    );

    fpga_rx_overflow_delta = delta_u64(
    fpga_after->eth.rx_fifo_overflow,
    fpga_before->eth.rx_fifo_overflow
    );

    fpga_tx_overflow_delta = delta_u64(
    fpga_after->eth.tx_fifo_overflow,
    fpga_before->eth.tx_fifo_overflow
    );

    fpga_bad_fcs_delta = delta_u64(
    fpga_after->eth.rx_error_bad_fcs,
    fpga_before->eth.rx_error_bad_fcs
    );

    fpga_ip_rx_header_error_delta = delta_u64(
        fpga_after->ip.rx_error_header_early_termination,
        fpga_before->ip.rx_error_header_early_termination
    );

    fpga_ip_rx_payload_error_delta = delta_u64(
        fpga_after->ip.rx_error_payload_early_termination,
        fpga_before->ip.rx_error_payload_early_termination
    );

    fpga_ip_invalid_header_delta = delta_u64(
        fpga_after->ip.rx_error_invalid_header,
        fpga_before->ip.rx_error_invalid_header
    );

    fpga_ip_invalid_checksum_delta = delta_u64(
        fpga_after->ip.rx_error_invalid_checksum,
        fpga_before->ip.rx_error_invalid_checksum
    );

    fpga_ip_tx_payload_error_delta = delta_u64(
        fpga_after->ip.tx_error_payload_early_termination,
        fpga_before->ip.tx_error_payload_early_termination
    );

    fpga_ip_tx_arp_failed_delta = delta_u64(
        fpga_after->ip.tx_error_arp_failed,
        fpga_before->ip.tx_error_arp_failed
    );

    fpga_udp_rx_header_error_delta = delta_u64(
        fpga_after->udp.rx_error_header_early_termination,
        fpga_before->udp.rx_error_header_early_termination
    );

    fpga_udp_rx_payload_error_delta = delta_u64(
        fpga_after->udp.rx_error_payload_early_termination,
        fpga_before->udp.rx_error_payload_early_termination
    );

    fpga_udp_tx_payload_error_delta = delta_u64(
        fpga_after->udp.tx_error_payload_early_termination,
        fpga_before->udp.tx_error_payload_early_termination
    );

    /*
     * Gaps útiles para localizar pérdidas.
     *
     * host_tx_to_fpga_rx_gap:
     *   paquetes enviados por la PC que no aparecen como RX good en FPGA.
     *
     * fpga_rx_to_tx_gap:
     *   paquetes que entraron bien a FPGA pero no salieron bien de FPGA.
     *
     * fpga_tx_to_host_rx_gap:
     *   paquetes que FPGA reportó como TX good pero no aparecen como RX en la PC.
     */
    host_tx_to_fpga_rx_gap =
        (int64_t)iface_tx_packets_delta - (int64_t)fpga_rx_good_delta;

    fpga_rx_to_tx_gap =
        (int64_t)fpga_rx_good_delta - (int64_t)fpga_tx_good_delta;

    fpga_tx_to_host_rx_gap =
        (int64_t)fpga_tx_good_delta - (int64_t)iface_rx_packets_delta;

    write_header = !file_exists_main(filename);

    file = fopen(filename, "a");

    if (file == NULL) {
        perror("fopen loopback loss CSV");
        return -1;
    }

    if (write_header) {
        fprintf(
            file,
            "timestamp,iface,fpga_ip,fpga_ctrl_port,fpga_data_port,local_port,"
            "requested_packets,payload_size,sent_packets,send_errors,drained_packets,"
            "elapsed_s,payload_mbps,estimated_wire_mbps,"
            "iface_tx_packets_delta,iface_rx_packets_delta,"
            "iface_tx_bytes_delta,iface_rx_bytes_delta,"
            "iface_tx_errors_delta,iface_rx_errors_delta,"
            "iface_tx_dropped_delta,iface_rx_dropped_delta,"
            "ip_in_receives_delta,ip_in_discards_delta,"
            "ip_out_requests_delta,ip_out_discards_delta,"
            "udp_out_delta,udp_in_delta,udp_no_ports_delta,"
            "udp_in_errors_delta,udp_rcvbuf_errors_delta,udp_sndbuf_errors_delta,"
            "fpga_rx_good_delta,fpga_tx_good_delta,"
            "fpga_rx_bad_delta,fpga_tx_bad_delta,"
            "fpga_rx_overflow_delta,fpga_tx_overflow_delta,fpga_bad_fcs_delta,"
            "fpga_ip_rx_header_error_delta,fpga_ip_rx_payload_error_delta,"
            "fpga_ip_invalid_header_delta,fpga_ip_invalid_checksum_delta,"
            "fpga_ip_tx_payload_error_delta,fpga_ip_tx_arp_failed_delta,"
            "fpga_udp_rx_header_error_delta,fpga_udp_rx_payload_error_delta,"
            "fpga_udp_tx_payload_error_delta,"
            "host_tx_to_fpga_rx_gap,fpga_rx_to_tx_gap,fpga_tx_to_host_rx_gap\n"
        );
    }

    now = time(NULL);

    fprintf(
        file,
        "%ld,%s,%s,%d,%d,%d,"
        "%d,%d,%d,%d,%d,"
        "%.9f,%.6f,%.6f,"

        /* iface deltas: 8 */
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","

        /* ip deltas: 4 */
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","

        /* udp deltas: 6 */
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","

        /* fpga eth deltas: 7 */
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","

        /* fpga ip deltas: 6 */
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","

        /* fpga udp deltas: 3 */
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","

        /* gaps: 3 */
        "%" PRId64 ",%" PRId64 ",%" PRId64 "\n",
        now,
        iface_name,
        fpga_ip,
        fpga_ctrl_port,
        fpga_data_port,
        local_port,

        test->packet_count,
        test->payload_size,
        test->sent_packets,
        test->send_errors,
        test->drained_packets,

        test->elapsed_s,
        test->payload_mbps,
        test->estimated_wire_mbps,

        iface_tx_packets_delta,
        iface_rx_packets_delta,
        iface_tx_bytes_delta,
        iface_rx_bytes_delta,
        iface_tx_errors_delta,
        iface_rx_errors_delta,
        iface_tx_dropped_delta,
        iface_rx_dropped_delta,

        ip_in_receives_delta,
        ip_in_discards_delta,
        ip_out_requests_delta,
        ip_out_discards_delta,

        udp_out_delta,
        udp_in_delta,
        udp_no_ports_delta,
        udp_in_errors_delta,
        udp_rcvbuf_errors_delta,
        udp_sndbuf_errors_delta,

        fpga_rx_good_delta,
        fpga_tx_good_delta,
        fpga_rx_bad_delta,
        fpga_tx_bad_delta,
        fpga_rx_overflow_delta,
        fpga_tx_overflow_delta,
        fpga_bad_fcs_delta,

        fpga_ip_rx_header_error_delta,
        fpga_ip_rx_payload_error_delta,
        fpga_ip_invalid_header_delta,
        fpga_ip_invalid_checksum_delta,
        fpga_ip_tx_payload_error_delta,
        fpga_ip_tx_arp_failed_delta,

        fpga_udp_rx_header_error_delta,
        fpga_udp_rx_payload_error_delta,
        fpga_udp_tx_payload_error_delta,

        host_tx_to_fpga_rx_gap,
        fpga_rx_to_tx_gap,
        fpga_tx_to_host_rx_gap
    );

    fclose(file);

    printf("\n=== LOOPBACK LOSS SUMMARY ===\n");
    printf("Host TX packets delta:        %" PRIu64 "\n", iface_tx_packets_delta);
    printf("Host RX packets delta:        %" PRIu64 "\n", iface_rx_packets_delta);
    printf("FPGA RX good delta:           %" PRIu64 "\n", fpga_rx_good_delta);
    printf("FPGA TX good delta:           %" PRIu64 "\n", fpga_tx_good_delta);
    printf("UDP OutDatagrams delta:       %" PRIu64 "\n", udp_out_delta);
    printf("UDP InDatagrams delta:        %" PRIu64 "\n", udp_in_delta);
    printf("UDP RcvbufErrors delta:       %" PRIu64 "\n", udp_rcvbuf_errors_delta);
    printf("UDP NoPorts delta:            %" PRIu64 "\n", udp_no_ports_delta);
    printf("FPGA RX overflow delta:       %" PRIu64 "\n", fpga_rx_overflow_delta);
    printf("FPGA TX overflow delta:       %" PRIu64 "\n", fpga_tx_overflow_delta);
    printf("Host TX -> FPGA RX gap:       %" PRId64 "\n", host_tx_to_fpga_rx_gap);
    printf("FPGA RX -> FPGA TX gap:       %" PRId64 "\n", fpga_rx_to_tx_gap);
    printf("FPGA TX -> Host RX gap:       %" PRId64 "\n", fpga_tx_to_host_rx_gap);
    printf("FPGA RX bad frame delta:      %" PRIu64 "\n", fpga_rx_bad_delta);
    printf("FPGA TX bad frame delta:      %" PRIu64 "\n", fpga_tx_bad_delta);
    printf("FPGA RX bad FCS delta:        %" PRIu64 "\n", fpga_bad_fcs_delta);
    printf("FPGA IP invalid header delta: %" PRIu64 "\n", fpga_ip_invalid_header_delta);
    printf("FPGA IP checksum delta:       %" PRIu64 "\n", fpga_ip_invalid_checksum_delta);
    printf("FPGA UDP RX payload delta:    %" PRIu64 "\n", fpga_udp_rx_payload_error_delta);
    printf("FPGA UDP TX payload delta:    %" PRIu64 "\n", fpga_udp_tx_payload_error_delta);

    printf("\nSaved loss summary to: %s\n", filename);

    return 0;
}

int main(int argc, char **argv)
{
    const char *iface_log_file = ETHERBENCH_IFACE_LOG_FILE;
    const char *net_log_file = ETHERBENCH_NET_LOG_FILE;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "iface") == 0) {
        iface_stats_t iface_stats;
        const char *iface_name;

        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }

        iface_name = argv[2];

        if (read_iface_stats(iface_name, &iface_stats) != 0) {
            fprintf(stderr, "Error: could not read interface stats for %s\n", iface_name);
            return 1;
        }

        print_iface_stats(&iface_stats);

        if (append_iface_stats_csv(iface_log_file, &iface_stats) != 0) {
            fprintf(stderr, "Error: could not write log file: %s\n", iface_log_file);
            return 1;
        }

        printf("\nSaved to: %s\n", iface_log_file);
        return 0;
    }

    if (strcmp(argv[1], "net") == 0) {
        net_stats_t net_stats;

        if (read_net_stats(&net_stats) != 0) {
            fprintf(stderr, "Error: could not read IP/UDP stats\n");
            return 1;
        }

        print_net_stats(&net_stats);

        if (append_net_stats_csv(net_log_file, &net_stats) != 0) {
            fprintf(stderr, "Error: could not write log file: %s\n", net_log_file);
            return 1;
        }

        printf("\nSaved to: %s\n", net_log_file);
        return 0;
    }

    if (strcmp(argv[1], "all") == 0) {
        iface_stats_t iface_stats;
        net_stats_t net_stats;
        const char *iface_name;

        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }

        iface_name = argv[2];

        if (read_iface_stats(iface_name, &iface_stats) != 0) {
            fprintf(stderr, "Error: could not read interface stats for %s\n", iface_name);
            return 1;
        }

        if (read_net_stats(&net_stats) != 0) {
            fprintf(stderr, "Error: could not read IP/UDP stats\n");
            return 1;
        }

        print_iface_stats(&iface_stats);
        print_net_stats(&net_stats);

        if (append_iface_stats_csv(iface_log_file, &iface_stats) != 0) {
            fprintf(stderr, "Error: could not write log file: %s\n", iface_log_file);
            return 1;
        }

        if (append_net_stats_csv(net_log_file, &net_stats) != 0) {
            fprintf(stderr, "Error: could not write log file: %s\n", net_log_file);
            return 1;
        }

        printf("\nSaved to: %s\n", iface_log_file);
        printf("Saved to: %s\n", net_log_file);
        return 0;
    }

    if (strcmp(argv[1], "fpga") == 0) {
        fpga_stats_t fpga_stats;

        const char *fpga_ip;
        int fpga_port = 55555;
        int rx_port = 9999;
        int timeout_ms = 3000;

        const char *fpga_log_file = ETHERBENCH_FPGA_LOG_FILE;

        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }

        fpga_ip = argv[2];

        if (argc >= 4) {
            fpga_port = atoi(argv[3]);
        }

        if (argc >= 5) {
            rx_port = atoi(argv[4]);
        }

        printf("Sending regstats to FPGA %s:%d\n", fpga_ip, fpga_port);
        printf("Listening for FPGA response on UDP port %d\n", rx_port);

        if (query_fpga_stats(fpga_ip, fpga_port, rx_port, timeout_ms, &fpga_stats) != 0) {
            fprintf(stderr, "Error: could not query FPGA regstats\n");
            return 1;
        }

        print_fpga_stats(&fpga_stats);

        if (append_fpga_stats_csv(fpga_log_file, &fpga_stats) != 0) {
            fprintf(stderr, "Error: could not write log file: %s\n", fpga_log_file);
            return 1;
        }

        printf("\nSaved to: %s\n", fpga_log_file);
        return 0;
    }

    if (strcmp(argv[1], "mdio-read") == 0) {
        fpga_mdio_read_result_t result;

        const char *fpga_ip;
        uint8_t phy;
        uint8_t reg;

        int fpga_ctrl_port = ETHERBENCH_DEFAULT_FPGA_CTRL_PORT;
        int rx_port = ETHERBENCH_DEFAULT_RX_PORT;
        int timeout_ms = ETHERBENCH_DEFAULT_TIMEOUT_MS;
        int delay_us = ETHERBENCH_DEFAULT_MDIO_DELAY_US;

        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }

        fpga_ip = argv[2];
        phy = (uint8_t)strtoul(argv[3], NULL, 0);
        reg = (uint8_t)strtoul(argv[4], NULL, 0);

        if (fpga_mdio_read(
                fpga_ip,
                fpga_ctrl_port,
                rx_port,
                timeout_ms,
                delay_us,
                phy,
                reg,
                &result
            ) != 0) {
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "mdio-write") == 0) {
        const char *fpga_ip;
        uint8_t phy;
        uint8_t reg;
        uint16_t value;

        int fpga_ctrl_port = ETHERBENCH_DEFAULT_FPGA_CTRL_PORT;
        int delay_us = ETHERBENCH_DEFAULT_MDIO_DELAY_US;

        if (argc < 6) {
            print_usage(argv[0]);
            return 1;
        }

        fpga_ip = argv[2];
        phy = (uint8_t)strtoul(argv[3], NULL, 0);
        reg = (uint8_t)strtoul(argv[4], NULL, 0);
        value = (uint16_t)strtoul(argv[5], NULL, 0);

        if (fpga_mdio_write(
                fpga_ip,
                fpga_ctrl_port,
                delay_us,
                phy,
                reg,
                value
            ) != 0) {
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "mdio-seq") == 0) {
        const char *fpga_ip;
        uint8_t phy;

        int fpga_ctrl_port = ETHERBENCH_DEFAULT_FPGA_CTRL_PORT;
        int rx_port = ETHERBENCH_DEFAULT_RX_PORT;
        int timeout_ms = ETHERBENCH_DEFAULT_TIMEOUT_MS;
        int delay_us = ETHERBENCH_DEFAULT_MDIO_DELAY_US;
        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }

        fpga_ip = argv[2];
        phy = (uint8_t)strtoul(argv[3], NULL, 0);

        if (fpga_mdio_run_sequence(
                fpga_ip,
                fpga_ctrl_port,
                rx_port,
                timeout_ms,
                delay_us,
                phy,
                argc - 4,
                &argv[4]
            ) != 0) {
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "fpga-arp") == 0) {
        const char *iface_name;
        const char *fpga_ip;

        int fpga_ctrl_port = ETHERBENCH_DEFAULT_FPGA_CTRL_PORT;
        int timeout_ms = ETHERBENCH_DEFAULT_TIMEOUT_MS;

        if (argc < 4) {
            print_usage(argv[0]);
            return 1;
        }

        iface_name = argv[2];
        fpga_ip = argv[3];

        if (argc >= 5) {
            fpga_ctrl_port = atoi(argv[4]);
        }

        if (fpga_arp_setup(
                iface_name,
                fpga_ip,
                fpga_ctrl_port,
                timeout_ms
            ) != 0) {
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "fpga-setup") == 0) {
        const char *iface_name;
        const char *fpga_ip;
        uint8_t phy_addr;

        int fpga_ctrl_port = ETHERBENCH_DEFAULT_FPGA_CTRL_PORT;
        int rx_port = ETHERBENCH_DEFAULT_RX_PORT;
        int timeout_ms = ETHERBENCH_DEFAULT_TIMEOUT_MS;
        int mdio_delay_us = ETHERBENCH_DEFAULT_MDIO_DELAY_US;

        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }

        iface_name = argv[2];
        fpga_ip = argv[3];
        phy_addr = (uint8_t)strtoul(argv[4], NULL, 0);

        if (argc >= 6) {
            fpga_ctrl_port = atoi(argv[5]);
        }

        if (fpga_setup_ksz9031_1gbe(
                iface_name,
                fpga_ip,
                phy_addr,
                fpga_ctrl_port,
                rx_port,
                timeout_ms,
                mdio_delay_us
            ) != 0) {
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "fpga-net") == 0) {
        const char *fpga_ip;
        const char *field;
        const char *value;

        int fpga_ctrl_port = ETHERBENCH_DEFAULT_FPGA_CTRL_PORT;
        int ret = -1;

        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }

        fpga_ip = argv[2];
        field = argv[3];
        value = argv[4];

        if (argc >= 6) {
            fpga_ctrl_port = atoi(argv[5]);
        }

        if (strcmp(field, "gateway") == 0) {
            ret = fpga_ctrl_set_gateway_ip(fpga_ip, fpga_ctrl_port, value);
        } else if (strcmp(field, "source-ip") == 0 || strcmp(field, "local-ip") == 0) {
            ret = fpga_ctrl_set_source_ip(fpga_ip, fpga_ctrl_port, value);
        } else if (strcmp(field, "dest-ip") == 0) {
            ret = fpga_ctrl_set_dest_ip(fpga_ip, fpga_ctrl_port, value);
        } else if (strcmp(field, "subnet") == 0) {
            ret = fpga_ctrl_set_subnet_mask(fpga_ip, fpga_ctrl_port, value);
        } else if (strcmp(field, "src-port") == 0) {
            ret = fpga_ctrl_set_src_port(
                fpga_ip,
                fpga_ctrl_port,
                (uint16_t)strtoul(value, NULL, 0)
            );
        } else if (strcmp(field, "dst-port") == 0) {
            ret = fpga_ctrl_set_dst_port(
                fpga_ip,
                fpga_ctrl_port,
                (uint16_t)strtoul(value, NULL, 0)
            );
        } else {
            fprintf(stderr, "Unknown fpga-net field: %s\n", field);
            return 1;
        }

        if (ret != 0) {
            return 1;
        }

        printf("FPGA network command sent\n");
        return 0;
    }

    if (strcmp(argv[1], "fpga-test") == 0) {
        const char *fpga_ip;
        const char *cmd;

        int fpga_ctrl_port = ETHERBENCH_DEFAULT_FPGA_CTRL_PORT;
        int ret = -1;

        if (argc < 4) {
            print_usage(argv[0]);
            return 1;
        }

        fpga_ip = argv[2];
        cmd = argv[3];

        /*
        * For commands with value:
        *   ./etherbench fpga-test <ip> mtu 1440 [fpga_port]
        *   ./etherbench fpga-test <ip> pktn 1000 [fpga_port]
        *
        * For commands without value:
        *   ./etherbench fpga-test <ip> loopback [fpga_port]
        */
        if (strcmp(cmd, "mtu") == 0 || strcmp(cmd, "pktn") == 0) {
            if (argc < 5) {
                fprintf(stderr, "Command %s requires a value\n", cmd);
                return 1;
            }

            if (argc >= 6) {
                fpga_ctrl_port = atoi(argv[5]);
            }
        } else {
            if (argc >= 5) {
                fpga_ctrl_port = atoi(argv[4]);
            }
        }

        if (strcmp(cmd, "loopback") == 0) {
            ret = fpga_ctrl_enable_loopback(fpga_ip, fpga_ctrl_port);
        } else if (strcmp(cmd, "trigger") == 0) {
            ret = fpga_ctrl_send_trigger(fpga_ip, fpga_ctrl_port);
        } else if (strcmp(cmd, "random") == 0) {
            ret = fpga_ctrl_enable_random(fpga_ip, fpga_ctrl_port);
        } else if (strcmp(cmd, "flood") == 0) {
            ret = fpga_ctrl_enable_flood(fpga_ip, fpga_ctrl_port);
        } else if (strcmp(cmd, "mtu") == 0) {
            ret = fpga_ctrl_set_udp_mtu(
                fpga_ip,
                fpga_ctrl_port,
                (uint16_t)strtoul(argv[4], NULL, 0)
            );
        } else if (strcmp(cmd, "pktn") == 0) {
            ret = fpga_ctrl_set_packet_count(
                fpga_ip,
                fpga_ctrl_port,
                (uint32_t)strtoul(argv[4], NULL, 0)
            );
        } else {
            fprintf(stderr, "Unknown fpga-test command: %s\n", cmd);
            return 1;
        }

        if (ret != 0) {
            return 1;
        }

        printf("FPGA test command sent\n");
        return 0;
    }

    if (strcmp(argv[1], "fpga-rtt") == 0) {
        const char *fpga_ip;
        int packet_count;
        int payload_size;

        int fpga_ctrl_port = ETHERBENCH_DEFAULT_FPGA_CTRL_PORT;
        int fpga_data_port = ETHERBENCH_DEFAULT_FPGA_LOOPBACK_PORT;
        int local_port = ETHERBENCH_DEFAULT_RX_PORT;
        int timeout_ms = ETHERBENCH_DEFAULT_TIMEOUT_MS;

        fpga_rtt_result_t result;

        if (argc < 5) {
            print_usage(argv[0]);
            return 1;
        }

        fpga_ip = argv[2];
        packet_count = atoi(argv[3]);
        payload_size = atoi(argv[4]);

        if (argc >= 6) {
            fpga_data_port = atoi(argv[5]);
        }

        if (argc >= 7) {
            local_port = atoi(argv[6]);
        }

        if (argc >= 8) {
            fpga_ctrl_port = atoi(argv[7]);
        }

        if (fpga_rtt_test(
                fpga_ip,
                fpga_ctrl_port,
                fpga_data_port,
                local_port,
                timeout_ms,
                packet_count,
                payload_size,
                &result
            ) != 0) {
            return 1;
        }

        printf("\n=== RTT result ===\n");
        printf("Sent:      %d\n", result.sent);
        printf("Received:  %d\n", result.received);
        printf("Lost:      %d\n", result.lost);

        if (result.received > 0) {
            printf("Min:       %.6f ms\n", result.min_ms);
            printf("Avg:       %.6f ms\n", result.avg_ms);
            printf("Max:       %.6f ms\n", result.max_ms);
            printf("Stddev:    %.6f ms\n", result.stddev_ms);
        }

        if (append_fpga_rtt_csv(
                ETHERBENCH_FPGA_RTT_LOG_FILE,
                fpga_ip,
                fpga_ctrl_port,
                fpga_data_port,
                local_port,
                payload_size,
                &result
            ) != 0) {
            fprintf(stderr, "Error: could not write RTT log\n");
            return 1;
        }

        printf("Saved to: %s\n", ETHERBENCH_FPGA_RTT_LOG_FILE);

        return 0;
    }

    if (strcmp(argv[1], "fpga-loopback-test") == 0) {
        const char *iface_name;
        const char *fpga_ip;

        int packet_count;
        int payload_size;

        int fpga_ctrl_port = ETHERBENCH_DEFAULT_FPGA_CTRL_PORT;
        int fpga_data_port = ETHERBENCH_DEFAULT_FPGA_LOOPBACK_PORT;
        int local_port = ETHERBENCH_DEFAULT_RX_PORT;
        int timeout_ms = ETHERBENCH_DEFAULT_TIMEOUT_MS;

        const char *iface_log_file = ETHERBENCH_IFACE_LOG_FILE;
        const char *net_log_file = ETHERBENCH_NET_LOG_FILE;
        const char *fpga_log_file = ETHERBENCH_FPGA_LOG_FILE;

        iface_stats_t iface_before;
        iface_stats_t iface_after;
        net_stats_t net_before;
        net_stats_t net_after;
        fpga_stats_t fpga_before;
        fpga_stats_t fpga_after;

        fpga_loopback_load_result_t result;

        if (argc < 6) {
            print_usage(argv[0]);
            return 1;
        }

        iface_name = argv[2];
        fpga_ip = argv[3];

        packet_count = atoi(argv[4]);
        payload_size = atoi(argv[5]);

        if (argc >= 7) {
            fpga_data_port = atoi(argv[6]);
        }

        if (argc >= 8) {
            local_port = atoi(argv[7]);
        }

        if (argc >= 9) {
            fpga_ctrl_port = atoi(argv[8]);
        }

        if (packet_count <= 0) {
            fprintf(stderr, "Error: packet count must be greater than 0\n");
            return 1;
        }

        if (payload_size <= 0) {
            fprintf(stderr, "Error: payload size must be greater than 0\n");
            return 1;
        }

        printf("\n========================================\n");
        printf(" FPGA LOOPBACK LOAD FULL TEST\n");
        printf("========================================\n");
        printf("Interface:        %s\n", iface_name);
        printf("FPGA IP:          %s\n", fpga_ip);
        printf("Control port:     %d\n", fpga_ctrl_port);
        printf("Data port:        %d\n", fpga_data_port);
        printf("Local RX port:    %d\n", local_port);
        printf("Packets:          %d\n", packet_count);
        printf("Payload size:     %d bytes\n", payload_size);

        if (fpga_enable_loopback_if_needed(
                fpga_ip,
                fpga_ctrl_port,
                local_port,
                timeout_ms
            ) != 0) {
            return -1;
        } 

        printf("\n========================================\n");
        printf(" BEFORE TEST: HOST STATS\n");
        printf("========================================\n");       

        if (log_host_stats_snapshot(
                iface_name,
                iface_log_file,
                net_log_file,
                &iface_before,
                &net_before
            ) != 0) {
            fprintf(stderr, "Error: could not log initial host stats\n");
            return 1;
        }

        printf("\n========================================\n");
        printf(" BEFORE TEST: FPGA STATS\n");
        printf("========================================\n");

        if (log_fpga_stats_snapshot(
                fpga_ip,
                fpga_ctrl_port,
                local_port,
                timeout_ms,
                fpga_log_file,
                &fpga_before
            ) != 0) {
            fprintf(stderr, "Error: could not log initial FPGA stats\n");
            return 1;
        }

        printf("\n========================================\n");
        printf(" RUNNING LOOPBACK LOAD TEST\n");
        printf("========================================\n");

        if (fpga_loopback_load_test(
                fpga_ip,
                fpga_data_port,
                local_port,
                packet_count,
                payload_size,
                &result
            ) != 0) {
            fprintf(stderr, "Error: FPGA loopback load test failed\n");

            return 1;
        }

        printf("\n========================================\n");
        printf(" AFTER TEST: HOST STATS\n");
        printf("========================================\n");

        if (log_host_stats_snapshot(
                iface_name,
                iface_log_file,
                net_log_file,
                &iface_after,
                &net_after
            ) != 0) {
            fprintf(stderr, "Error: could not log final host stats\n");

            return 1;
        }

        printf("\n========================================\n");
        printf(" AFTER TEST: FPGA STATS\n");
        printf("========================================\n");

        if (log_fpga_stats_snapshot(
                fpga_ip,
                fpga_ctrl_port,
                local_port,
                timeout_ms,
                fpga_log_file,
                &fpga_after
            ) != 0) {
            fprintf(stderr, "Error: could not log final FPGA stats\n");

            return 1;
        }

        if (fpga_disable_loopback_if_needed(
                fpga_ip,
                fpga_ctrl_port,
                local_port,
                timeout_ms
            ) != 0) {
            return -1;
        }

        printf("\n========================================\n");
        printf(" FPGA LOOPBACK LOAD RESULT\n");
        printf("========================================\n");
        printf("Requested packets:      %d\n", result.packet_count);
        printf("Payload size:           %d bytes\n", result.payload_size);
        printf("Sent packets:           %d\n", result.sent_packets);
        printf("Send errors:            %d\n", result.send_errors);
        printf("Drained RX packets:     %d\n", result.drained_packets);
        printf("Elapsed:                %.6f s\n", result.elapsed_s);
        printf("Packets/s:              %.3f pkt/s\n", result.packets_per_second);
        printf("Payload goodput:        %.6f Mbps\n", result.payload_mbps);
        printf("Estimated wire rate:    %.6f Mbps\n", result.estimated_wire_mbps);

        if (append_fpga_loopback_load_csv(
                ETHERBENCH_FPGA_LOOPBACK_LOAD_LOG_FILE,
                fpga_ip,
                fpga_ctrl_port,
                fpga_data_port,
                local_port,
                &result
            ) != 0) {
            fprintf(stderr, "Error: could not write loopback load CSV\n");

            return 1;
        }

        if (append_loopback_loss_csv(
                ETHERBENCH_FPGA_LOOPBACK_LOSS_LOG_FILE,
                iface_name,
                fpga_ip,
                fpga_ctrl_port,
                fpga_data_port,
                local_port,
                &result,
                &iface_before,
                &iface_after,
                &net_before,
                &net_after,
                &fpga_before,
                &fpga_after
            ) != 0) {
            fprintf(stderr, "Error: could not write loopback loss CSV\n");

            return 1;
        }

        printf("\nSaved to: %s\n", ETHERBENCH_FPGA_LOOPBACK_LOAD_LOG_FILE);
        printf("Saved to: %s\n", ETHERBENCH_FPGA_LOOPBACK_LOSS_LOG_FILE);

        printf("\nFull loopback load test completed\n");

        return 0;
    }

    print_usage(argv[0]);
    return 1;
}