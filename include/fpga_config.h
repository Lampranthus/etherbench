#ifndef FPGA_CONFIG_H
#define FPGA_CONFIG_H

#define FPGA_ARP_MAC_LEN 64

typedef struct {
    int reachable;
    char mac[FPGA_ARP_MAC_LEN];
} fpga_arp_result_t;

int fpga_check_arp(
    const char *iface_name,
    const char *fpga_ip,
    int fpga_port,
    int timeout_ms,
    fpga_arp_result_t *result
);

int fpga_set_permanent_arp(
    const char *iface_name,
    const char *fpga_ip,
    const char *fpga_mac
);

int fpga_arp_setup(
    const char *iface_name,
    const char *fpga_ip,
    int fpga_port,
    int timeout_ms
);

#endif