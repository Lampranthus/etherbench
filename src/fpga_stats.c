#include "fpga_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <errno.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#define FPGA_FRAME_BYTES 115
#define FPGA_START_MARKER 0xAA55AA55u
#define FPGA_END_MARKER   0xEEFFEEFFu

static uint16_t read_be16(const uint8_t *d, size_t off)
{
    return ((uint16_t)d[off] << 8) |
           ((uint16_t)d[off + 1]);
}

static uint32_t read_be32(const uint8_t *d, size_t off)
{
    return ((uint32_t)d[off] << 24) |
           ((uint32_t)d[off + 1] << 16) |
           ((uint32_t)d[off + 2] << 8) |
           ((uint32_t)d[off + 3]);
}

static uint64_t read_be40(const uint8_t *d, size_t off)
{
    return ((uint64_t)d[off] << 32) |
           ((uint64_t)d[off + 1] << 24) |
           ((uint64_t)d[off + 2] << 16) |
           ((uint64_t)d[off + 3] << 8) |
           ((uint64_t)d[off + 4]);
}

static void format_ip(char *out, size_t out_size, const uint8_t *d, size_t off)
{
    snprintf(
        out,
        out_size,
        "%u.%u.%u.%u",
        d[off],
        d[off + 1],
        d[off + 2],
        d[off + 3]
    );
}

static void format_mac(char *out, size_t out_size, const uint8_t *d, size_t off)
{
    snprintf(
        out,
        out_size,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        d[off],
        d[off + 1],
        d[off + 2],
        d[off + 3],
        d[off + 4],
        d[off + 5]
    );
}

static void decode_speed(uint8_t speed_raw, char *out, size_t out_size)
{
    switch (speed_raw) {
        case 0:
            snprintf(out, out_size, "10M");
            break;
        case 1:
            snprintf(out, out_size, "100M");
            break;
        case 2:
            snprintf(out, out_size, "1000M");
            break;
        default:
            snprintf(out, out_size, "unknown");
            break;
    }
}

static int parse_fpga_status_frame(
    const uint8_t *data,
    size_t len,
    fpga_stats_t *stats
)
{
    uint8_t mode;
    uint8_t speed_raw;

    if (data == NULL || stats == NULL) {
        return -1;
    }

    if (len < FPGA_FRAME_BYTES) {
        fprintf(stderr, "Error: FPGA frame too short: %zu bytes, expected %d\n",
                len, FPGA_FRAME_BYTES);
        return -1;
    }

    memset(stats, 0, sizeof(*stats));

    stats->frame_bytes = (int)len;
    stats->start_marker = read_be32(data, 0);
    stats->end_marker = read_be32(data, 111);

    if (stats->start_marker != FPGA_START_MARKER) {
        fprintf(stderr, "Error: invalid FPGA start marker: 0x%08X\n",
                stats->start_marker);
        return -1;
    }

    if (stats->end_marker != FPGA_END_MARKER) {
        fprintf(stderr, "Error: invalid FPGA end marker: 0x%08X\n",
                stats->end_marker);
        return -1;
    }

    /*
     * Ethernet counters.
     */
    stats->eth.tx_fifo_overflow = read_be32(data, 4);
    stats->eth.tx_fifo_bad_frame = read_be32(data, 8);
    stats->eth.tx_fifo_good_frame = read_be40(data, 12);

    stats->eth.rx_error_bad_frame = read_be32(data, 17);
    stats->eth.rx_error_bad_fcs = read_be32(data, 21);
    stats->eth.rx_fifo_overflow = read_be32(data, 25);
    stats->eth.rx_fifo_bad_frame = read_be32(data, 29);
    stats->eth.rx_fifo_good_frame = read_be40(data, 33) - 1; /* Subtract 1 to exclude the current status frame itself */

    stats->eth.eth_rx_error_header_early_termination = read_be32(data, 38);

    /*
     * IP counters.
     */
    stats->ip.rx_error_header_early_termination = read_be32(data, 42);
    stats->ip.rx_error_payload_early_termination = read_be32(data, 46);
    stats->ip.rx_error_invalid_header = read_be32(data, 50);
    stats->ip.rx_error_invalid_checksum = read_be32(data, 54);
    stats->ip.tx_error_payload_early_termination = read_be32(data, 58);
    stats->ip.tx_error_arp_failed = read_be32(data, 62);

    /*
     * UDP counters.
     */
    stats->udp.rx_error_header_early_termination = read_be32(data, 66);
    stats->udp.rx_error_payload_early_termination = read_be32(data, 70);
    stats->udp.tx_error_payload_early_termination = read_be32(data, 74);

    /*
     * Mode byte.
     *
     * bit layout:
     *   bits 6..5 = speed
     *   bit 4     = flood
     *   bit 3     = loopback
     *   bit 2     = tx_busy
     *   bit 1     = random
     *   bit 0     = constant
     */
    mode = data[78];

    speed_raw = (mode >> 5) & 0x03;
    stats->link.speed_raw = speed_raw;
    decode_speed(speed_raw, stats->link.speed, sizeof(stats->link.speed));

    stats->mode.flood = (mode >> 4) & 1;
    stats->mode.loopback = (mode >> 3) & 1;
    stats->mode.tx_busy = (mode >> 2) & 1;
    stats->mode.random = (mode >> 1) & 1;
    stats->mode.constant = mode & 1;
    stats->mode.sequential = !stats->mode.random && !stats->mode.constant;

    /*
     * TX configuration.
     */
    stats->tx_config.payload_size_bytes = read_be16(data, 79);
    stats->tx_config.packets_per_trigger = read_be32(data, 81);

    /*
     * Network configuration.
     */
    format_mac(stats->network.mac, sizeof(stats->network.mac), data, 85);
    format_ip(stats->network.local_ip, sizeof(stats->network.local_ip), data, 91);
    format_ip(stats->network.gateway_ip, sizeof(stats->network.gateway_ip), data, 95);
    format_ip(stats->network.subnet_mask, sizeof(stats->network.subnet_mask), data, 99);
    format_ip(stats->network.dest_ip, sizeof(stats->network.dest_ip), data, 103);

    stats->network.src_port = read_be16(data, 107);
    stats->network.dst_port = read_be16(data, 109);

    return 0;
}

static int make_rx_socket(int rx_port, int timeout_ms)
{
    int sock;
    int rcvbuf;
    struct sockaddr_in addr;
    struct timeval timeout;

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket");
        return -1;
    }

    rcvbuf = 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(sock);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)rx_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind rx socket");
        close(sock);
        return -1;
    }

    return sock;
}

static int send_regstats_command(const char *fpga_ip, int fpga_port)
{
    int sock;
    struct sockaddr_in addr;
    const uint8_t cmd[] = {
        'r', 'e', 'g', 's', 't', 'a', 't', 's', '\0'
    };

    if (fpga_ip == NULL) {
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket tx");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)fpga_port);

    if (inet_pton(AF_INET, fpga_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Error: invalid FPGA IP: %s\n", fpga_ip);
        close(sock);
        return -1;
    }

    if (sendto(sock, cmd, sizeof(cmd), 0,
               (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("sendto regstats");
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

int query_fpga_stats(
    const char *fpga_ip,
    int fpga_port,
    int rx_port,
    int timeout_ms,
    fpga_stats_t *stats
)
{
    int rx_sock;
    uint8_t buffer[4096];
    ssize_t n;

    rx_sock = make_rx_socket(rx_port, timeout_ms);

    if (rx_sock < 0) {
        return -1;
    }

    if (send_regstats_command(fpga_ip, fpga_port) != 0) {
        close(rx_sock);
        return -1;
    }

    n = recvfrom(rx_sock, buffer, sizeof(buffer), 0, NULL, NULL);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            fprintf(stderr, "Error: timeout waiting for FPGA regstats response\n");
        } else {
            perror("recvfrom");
        }

        close(rx_sock);
        return -1;
    }

    close(rx_sock);

    return parse_fpga_status_frame(buffer, (size_t)n, stats);
}

void print_fpga_stats(const fpga_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }

    printf("\n");
    printf("=== FPGA STATUS ===\n");
    printf("Frame bytes: %d\n", stats->frame_bytes);
    printf("Start marker: 0x%08X\n", stats->start_marker);
    printf("End marker:   0x%08X\n", stats->end_marker);

    printf("\n");
    printf("=== FPGA Ethernet counters ===\n");
    printf("TX FIFO overflow:                    %" PRIu64 "\n", stats->eth.tx_fifo_overflow);
    printf("TX FIFO bad frame:                   %" PRIu64 "\n", stats->eth.tx_fifo_bad_frame);
    printf("TX FIFO good frame:                  %" PRIu64 "\n", stats->eth.tx_fifo_good_frame);
    printf("RX error bad frame:                  %" PRIu64 "\n", stats->eth.rx_error_bad_frame);
    printf("RX error bad FCS:                    %" PRIu64 "\n", stats->eth.rx_error_bad_fcs);
    printf("RX FIFO overflow:                    %" PRIu64 "\n", stats->eth.rx_fifo_overflow);
    printf("RX FIFO bad frame:                   %" PRIu64 "\n", stats->eth.rx_fifo_bad_frame);
    printf("RX FIFO good frame:                  %" PRIu64 "\n", stats->eth.rx_fifo_good_frame);
    printf("ETH RX header early termination:     %" PRIu64 "\n",
           stats->eth.eth_rx_error_header_early_termination);

    printf("\n");
    printf("=== FPGA IP counters ===\n");
    printf("IP RX header early termination:      %" PRIu64 "\n",
           stats->ip.rx_error_header_early_termination);
    printf("IP RX payload early termination:     %" PRIu64 "\n",
           stats->ip.rx_error_payload_early_termination);
    printf("IP RX invalid header:                %" PRIu64 "\n",
           stats->ip.rx_error_invalid_header);
    printf("IP RX invalid checksum:              %" PRIu64 "\n",
           stats->ip.rx_error_invalid_checksum);
    printf("IP TX payload early termination:     %" PRIu64 "\n",
           stats->ip.tx_error_payload_early_termination);
    printf("IP TX ARP failed:                    %" PRIu64 "\n",
           stats->ip.tx_error_arp_failed);

    printf("\n");
    printf("=== FPGA UDP counters ===\n");
    printf("UDP RX header early termination:     %" PRIu64 "\n",
           stats->udp.rx_error_header_early_termination);
    printf("UDP RX payload early termination:    %" PRIu64 "\n",
           stats->udp.rx_error_payload_early_termination);
    printf("UDP TX payload early termination:    %" PRIu64 "\n",
           stats->udp.tx_error_payload_early_termination);

    printf("\n");
    printf("=== FPGA Link / Mode ===\n");
    printf("Speed:       %s\n", stats->link.speed);
    printf("Flood:       %s\n", stats->mode.flood ? "ON" : "off");
    printf("Loopback:    %s\n", stats->mode.loopback ? "ON" : "off");
    printf("TX busy:     %s\n", stats->mode.tx_busy ? "ON" : "off");
    printf("Random:      %s\n", stats->mode.random ? "ON" : "off");
    printf("Constant:    %s\n", stats->mode.constant ? "ON" : "off");
    printf("Sequential:  %s\n", stats->mode.sequential ? "ON" : "off");

    printf("\n");
    printf("=== FPGA TX config ===\n");
    printf("Payload size:        %u bytes\n", stats->tx_config.payload_size_bytes);
    printf("Packets per trigger: %u\n", stats->tx_config.packets_per_trigger);

    printf("\n");
    printf("=== FPGA Network config ===\n");
    printf("MAC:        %s\n", stats->network.mac);
    printf("Local IP:   %s\n", stats->network.local_ip);
    printf("Gateway:    %s\n", stats->network.gateway_ip);
    printf("Subnet:     %s\n", stats->network.subnet_mask);
    printf("Dest IP:    %s\n", stats->network.dest_ip);
    printf("Src port:   %u\n", stats->network.src_port);
    printf("Dst port:   %u\n", stats->network.dst_port);
}

static int file_exists(const char *filename)
{
    FILE *file = fopen(filename, "r");

    if (file == NULL) {
        return 0;
    }

    fclose(file);
    return 1;
}

int append_fpga_stats_csv(const char *filename, const fpga_stats_t *stats)
{
    FILE *file;
    time_t now;
    int write_header;

    if (filename == NULL || stats == NULL) {
        return -1;
    }

    write_header = !file_exists(filename);

    file = fopen(filename, "a");

    if (file == NULL) {
        return -1;
    }

    if (write_header) {
        fprintf(
            file,
            "timestamp,"
            "tx_fifo_overflow,tx_fifo_bad_frame,tx_fifo_good_frame,"
            "rx_error_bad_frame,rx_error_bad_fcs,rx_fifo_overflow,"
            "rx_fifo_bad_frame,rx_fifo_good_frame,"
            "eth_rx_error_header_early_termination,"
            "ip_rx_error_header_early_termination,"
            "ip_rx_error_payload_early_termination,"
            "ip_rx_error_invalid_header,ip_rx_error_invalid_checksum,"
            "ip_tx_error_payload_early_termination,ip_tx_error_arp_failed,"
            "udp_rx_error_header_early_termination,"
            "udp_rx_error_payload_early_termination,"
            "udp_tx_error_payload_early_termination,"
            "speed,flood,loopback,tx_busy,random,constant,sequential,"
            "payload_size_bytes,packets_per_trigger,"
            "mac,local_ip,gateway_ip,subnet_mask,dest_ip,src_port,dst_port\n"
        );
    }

    now = time(NULL);

    fprintf(
        file,
        "%ld,"
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ","
        "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%s,%d,%d,%d,%d,%d,%d,"
        "%u,%u,"
        "%s,%s,%s,%s,%s,%u,%u\n",
        now,
        stats->eth.tx_fifo_overflow,
        stats->eth.tx_fifo_bad_frame,
        stats->eth.tx_fifo_good_frame,
        stats->eth.rx_error_bad_frame,
        stats->eth.rx_error_bad_fcs,
        stats->eth.rx_fifo_overflow,
        stats->eth.rx_fifo_bad_frame,
        stats->eth.rx_fifo_good_frame,
        stats->eth.eth_rx_error_header_early_termination,
        stats->ip.rx_error_header_early_termination,
        stats->ip.rx_error_payload_early_termination,
        stats->ip.rx_error_invalid_header,
        stats->ip.rx_error_invalid_checksum,
        stats->ip.tx_error_payload_early_termination,
        stats->ip.tx_error_arp_failed,
        stats->udp.rx_error_header_early_termination,
        stats->udp.rx_error_payload_early_termination,
        stats->udp.tx_error_payload_early_termination,
        stats->link.speed,
        stats->mode.flood,
        stats->mode.loopback,
        stats->mode.tx_busy,
        stats->mode.random,
        stats->mode.constant,
        stats->mode.sequential,
        stats->tx_config.payload_size_bytes,
        stats->tx_config.packets_per_trigger,
        stats->network.mac,
        stats->network.local_ip,
        stats->network.gateway_ip,
        stats->network.subnet_mask,
        stats->network.dest_ip,
        stats->network.src_port,
        stats->network.dst_port
    );

    fclose(file);
    return 0;
}