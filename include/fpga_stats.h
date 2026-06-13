#ifndef FPGA_STATS_H
#define FPGA_STATS_H

#include <stdint.h>

#define FPGA_IP_TEXT_LEN   32
#define FPGA_MAC_TEXT_LEN  32
#define FPGA_SPEED_TEXT_LEN 16

typedef struct {
    uint64_t tx_fifo_overflow;
    uint64_t tx_fifo_bad_frame;
    uint64_t tx_fifo_good_frame;

    uint64_t rx_error_bad_frame;
    uint64_t rx_error_bad_fcs;
    uint64_t rx_fifo_overflow;
    uint64_t rx_fifo_bad_frame;
    uint64_t rx_fifo_good_frame;

    uint64_t eth_rx_error_header_early_termination;
} fpga_eth_stats_t;

typedef struct {
    uint64_t rx_error_header_early_termination;
    uint64_t rx_error_payload_early_termination;
    uint64_t rx_error_invalid_header;
    uint64_t rx_error_invalid_checksum;
    uint64_t tx_error_payload_early_termination;
    uint64_t tx_error_arp_failed;
} fpga_ip_stats_t;

typedef struct {
    uint64_t rx_error_header_early_termination;
    uint64_t rx_error_payload_early_termination;
    uint64_t tx_error_payload_early_termination;
} fpga_udp_stats_t;

typedef struct {
    int flood;
    int loopback;
    int tx_busy;
    int random;
    int constant;
    int sequential;
} fpga_mode_t;

typedef struct {
    char speed[FPGA_SPEED_TEXT_LEN];
    uint8_t speed_raw;
} fpga_link_t;

typedef struct {
    uint16_t payload_size_bytes;
    uint32_t packets_per_trigger;
} fpga_tx_config_t;

typedef struct {
    char mac[FPGA_MAC_TEXT_LEN];
    char local_ip[FPGA_IP_TEXT_LEN];
    char gateway_ip[FPGA_IP_TEXT_LEN];
    char subnet_mask[FPGA_IP_TEXT_LEN];
    char dest_ip[FPGA_IP_TEXT_LEN];

    uint16_t src_port;
    uint16_t dst_port;
} fpga_network_t;

typedef struct {
    fpga_eth_stats_t eth;
    fpga_ip_stats_t ip;
    fpga_udp_stats_t udp;

    fpga_link_t link;
    fpga_mode_t mode;
    fpga_tx_config_t tx_config;
    fpga_network_t network;

    uint32_t start_marker;
    uint32_t end_marker;
    int frame_bytes;
} fpga_stats_t;

int query_fpga_stats(
    const char *fpga_ip,
    int fpga_port,
    int rx_port,
    int timeout_ms,
    fpga_stats_t *stats
);

void print_fpga_stats(const fpga_stats_t *stats);
int append_fpga_stats_csv(const char *filename, const fpga_stats_t *stats);

#endif // FPGA_STATS_H