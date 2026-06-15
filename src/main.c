#include "config.h"
#include "iface_stats.h"
#include "net_stats.h"
#include "fpga_stats.h"
#include "fpga_mdio.h"
#include "fpga_config.h"
#include "fpga_setup.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void print_usage(const char *program_name)
{
    printf("Usage:\n");
    printf("  %s iface <interface_name>\n", program_name);
    printf("  %s net\n", program_name);
    printf("  %s fpga <fpga_ip> [fpga_port] [rx_port]\n", program_name);
    printf("  %s all <interface_name>\n", program_name);
    printf("\n");
    printf("Examples:\n");
    printf("  %s iface eth0\n", program_name);
    printf("  %s iface enp3s0\n", program_name);
    printf("  %s net\n", program_name);
    printf("  %s fpga 192.168.1.12\n", program_name);
    printf("  %s fpga 192.168.1.12 55555 9999\n", program_name);
    printf("  %s mdio-read <fpga_ip> <phy> <reg>\n", program_name);
    printf("  %s mdio-write <fpga_ip> <phy> <reg> <value>\n", program_name);
    printf("  %s mdio-seq <fpga_ip> <phy> <op1> <op2> ...\n", program_name);
    printf("  %s fpga-arp <iface> <fpga_ip> [fpga_port]\n", program_name);
    printf("  %s fpga-setup-1gbe <iface> <fpga_ip> <phy> [fpga_port]\n", program_name);
    printf("  %s all eth0\n", program_name);
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

        int fpga_port = ETHERBENCH_DEFAULT_FPGA_PORT;
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
                fpga_port,
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

        int fpga_port = ETHERBENCH_DEFAULT_FPGA_PORT;
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
                fpga_port,
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

        int fpga_port = ETHERBENCH_DEFAULT_FPGA_PORT;
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
                fpga_port,
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

        int fpga_port = ETHERBENCH_DEFAULT_FPGA_PORT;
        int timeout_ms = ETHERBENCH_DEFAULT_TIMEOUT_MS;

        if (argc < 4) {
            print_usage(argv[0]);
            return 1;
        }

        iface_name = argv[2];
        fpga_ip = argv[3];

        if (argc >= 5) {
            fpga_port = atoi(argv[4]);
        }

        if (fpga_arp_setup(
                iface_name,
                fpga_ip,
                fpga_port,
                timeout_ms
            ) != 0) {
            return 1;
        }

        return 0;
    }

    if (strcmp(argv[1], "fpga-setup-1gbe") == 0) {
        const char *iface_name;
        const char *fpga_ip;
        uint8_t phy_addr;

        int fpga_port = ETHERBENCH_DEFAULT_FPGA_PORT;
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
            fpga_port = atoi(argv[5]);
        }

        if (fpga_setup_ksz9031_1gbe(
                iface_name,
                fpga_ip,
                phy_addr,
                fpga_port,
                rx_port,
                timeout_ms,
                mdio_delay_us
            ) != 0) {
            return 1;
        }

        return 0;
    }

    print_usage(argv[0]);
    return 1;
}