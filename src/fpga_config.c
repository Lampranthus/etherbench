#include "fpga_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>

#define ARP_LINE_LEN 512

static int send_udp_probe(
    const char *ip,
    int port
)
{
    int sock;
    struct sockaddr_in addr;
    const char payload[] = "arp_probe\n";

    if (ip == NULL) {
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid FPGA IP: %s\n", ip);
        close(sock);
        return -1;
    }

    if (sendto(
            sock,
            payload,
            sizeof(payload) - 1,
            0,
            (struct sockaddr *)&addr,
            sizeof(addr)
        ) < 0) {
        perror("sendto");
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

static int read_arp_table(
    const char *iface_name,
    const char *fpga_ip,
    fpga_arp_result_t *result
)
{
    FILE *file;
    char line[ARP_LINE_LEN];

    char ip[64];
    char hw_type[64];
    char flags[64];
    char mac[64];
    char mask[64];
    char device[64];

    if (iface_name == NULL || fpga_ip == NULL || result == NULL) {
        return -1;
    }

    result->reachable = 0;
    snprintf(result->mac, FPGA_ARP_MAC_LEN, "unknown");

    file = fopen("/proc/net/arp", "r");

    if (file == NULL) {
        perror("fopen /proc/net/arp");
        return -1;
    }

    /*
     * Skip header.
     */
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        int matched;

        matched = sscanf(
            line,
            "%63s %63s %63s %63s %63s %63s",
            ip,
            hw_type,
            flags,
            mac,
            mask,
            device
        );

        if (matched != 6) {
            continue;
        }

        if (strcmp(ip, fpga_ip) == 0 &&
            strcmp(device, iface_name) == 0 &&
            strcmp(mac, "00:00:00:00:00:00") != 0) {

            result->reachable = 1;

            strncpy(result->mac, mac, FPGA_ARP_MAC_LEN - 1);
            result->mac[FPGA_ARP_MAC_LEN - 1] = '\0';

            fclose(file);
            return 0;
        }
    }

    fclose(file);
    return 0;
}

int fpga_check_arp(
    const char *iface_name,
    const char *fpga_ip,
    int fpga_port,
    int timeout_ms,
    fpga_arp_result_t *result
)
{
    int elapsed_ms = 0;
    const int step_ms = 100;

    if (iface_name == NULL || fpga_ip == NULL || result == NULL) {
        return -1;
    }

    result->reachable = 0;
    snprintf(result->mac, FPGA_ARP_MAC_LEN, "unknown");

    /*
     * This UDP probe forces Linux to perform ARP resolution.
     * The FPGA does not need to understand this payload.
     */
    if (send_udp_probe(fpga_ip, fpga_port) != 0) {
        return -1;
    }

    while (elapsed_ms <= timeout_ms) {
        if (read_arp_table(iface_name, fpga_ip, result) != 0) {
            return -1;
        }

        if (result->reachable) {
            return 0;
        }

        usleep(100000);
        elapsed_ms += step_ms;
    }

    return 0;
}

int fpga_set_permanent_arp(
    const char *iface_name,
    const char *fpga_ip,
    const char *fpga_mac
)
{
    char command[512];
    int ret;

    if (iface_name == NULL || fpga_ip == NULL || fpga_mac == NULL) {
        return -1;
    }

    snprintf(
        command,
        sizeof(command),
        "ip neigh replace %s lladdr %s dev %s nud permanent",
        fpga_ip,
        fpga_mac,
        iface_name
    );

    ret = system(command);

    if (ret != 0) {
        fprintf(stderr, "Could not set permanent ARP entry\n");
        fprintf(stderr, "Try running with sudo:\n");
        fprintf(stderr, "  sudo ./etherbench fpga-arp %s %s\n",
                iface_name,
                fpga_ip);
        return -1;
    }

    return 0;
}

int fpga_arp_setup(
    const char *iface_name,
    const char *fpga_ip,
    int fpga_port,
    int timeout_ms
)
{
    fpga_arp_result_t arp;

    if (fpga_check_arp(
            iface_name,
            fpga_ip,
            fpga_port,
            timeout_ms,
            &arp
        ) != 0) {
        return -1;
    }

    if (!arp.reachable) {
        fprintf(stderr, "FPGA did not respond to ARP\n");
        return -1;
    }

    printf("FPGA ARP OK: %s -> %s on %s\n",
           fpga_ip,
           arp.mac,
           iface_name);

    if (fpga_set_permanent_arp(
            iface_name,
            fpga_ip,
            arp.mac
        ) != 0) {
        return -1;
    }

    printf("Permanent ARP entry installed\n");

    return 0;
}