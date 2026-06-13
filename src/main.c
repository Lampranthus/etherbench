#include "iface_stats.h"
#include "net_stats.h"
#include "fpga_stats.h"

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
    printf("  %s all eth0\n", program_name);
}

int main(int argc, char **argv)
{
    const char *iface_log_file = "interface_log.csv";
    const char *net_log_file = "net_log.csv";

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

        const char *fpga_log_file = "fpga_log.csv";

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

    print_usage(argv[0]);
    return 1;
}