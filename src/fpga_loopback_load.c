#define _DEFAULT_SOURCE

#include "fpga_loopback_load.h"
#include "fpga_ctrl.h"
#include "fpga_stats.h"
#include "iface_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>

#define LOOPBACK_DRAIN_EVERY_N_PACKETS 1024
#define LOOPBACK_PACE_BURST_BYTES 65536
#define LOOPBACK_WAIT_US 500000

/*
 * Ethernet overhead estimate for wire goodput:
 *
 * Ethernet header      14 B
 * IPv4 header          20 B
 * UDP header            8 B
 * FCS                   4 B
 * Preamble + SFD        8 B
 * Inter-frame gap      12 B
 *
 * Total overhead       66 B
 *
 * This is an estimate. Payload Mbps is exact from application payload.
 */
#define ESTIMATED_ETHERNET_OVERHEAD_BYTES 66
#define DEFAULT_LINK_MBPS 1000
#define SEND_PACE_SLEEP_MARGIN_NS 100000ULL

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x;

    x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    *state = x;

    return x;
}

static void fill_pseudorandom_payload(
    uint8_t *payload,
    int payload_size,
    uint32_t seed
)
{
    int i;
    uint32_t state = seed;

    for (i = 0; i < payload_size; i++) {
        if ((i % 4) == 0) {
            xorshift32(&state);
        }

        payload[i] = (uint8_t)((state >> ((i % 4) * 8)) & 0xFF);
    }
}

static int make_rx_drain_socket(int local_port)
{
    int sock;
    struct sockaddr_in addr;
    int flags;

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket rx");
        return -1;
    }

    /*
     * Try to enlarge receive buffer.
     * Linux may cap this using net.core.rmem_max.
     */

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)local_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind rx drain socket");
        close(sock);
        return -1;
    }

    flags = fcntl(sock, F_GETFL, 0);

    if (flags < 0) {
        perror("fcntl F_GETFL");
        close(sock);
        return -1;
    }

    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl F_SETFL");
        close(sock);
        return -1;
    }

    return sock;
}

static int make_tx_socket(void)
{
    int sock;
    int sndbuf = 16 * 1024 * 1024;

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket tx");
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
        perror("setsockopt SO_SNDBUF");
    }

    return sock;
}

static int connect_tx_socket(
    int sock,
    const struct sockaddr_in *dest_addr
)
{
    if (dest_addr == NULL) {
        return -1;
    }

    if (connect(
            sock,
            (const struct sockaddr *)dest_addr,
            sizeof(*dest_addr)
        ) < 0) {
        perror("connect tx socket");
        return -1;
    }

    return 0;
}

static int build_dest_addr(
    const char *fpga_ip,
    int fpga_data_port,
    struct sockaddr_in *addr
)
{
    if (fpga_ip == NULL || addr == NULL) {
        return -1;
    }

    memset(addr, 0, sizeof(*addr));

    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)fpga_data_port);

    if (inet_pton(AF_INET, fpga_ip, &addr->sin_addr) != 1) {
        fprintf(stderr, "Invalid FPGA IP: %s\n", fpga_ip);
        return -1;
    }

    return 0;
}

static int drain_rx_socket(int sock)
{
    uint8_t buffer[2048];
    int drained = 0;

    while (1) {
        ssize_t n;

        n = recvfrom(
            sock,
            buffer,
            sizeof(buffer),
            MSG_DONTWAIT,
            NULL,
            NULL
        );

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }

            perror("recvfrom drain");
            break;
        }

        drained++;
    }

    return drained;
}

static int drain_rx_socket_until_quiet(
    int sock,
    int quiet_time_ms,
    int max_wait_ms
)
{
    uint8_t buffer[2048];
    int drained = 0;
    int quiet_elapsed_ms = 0;
    int total_elapsed_ms = 0;
    const int step_us = 1000;

    while (total_elapsed_ms < max_wait_ms) {
        ssize_t n;

        n = recvfrom(
            sock,
            buffer,
            sizeof(buffer),
            MSG_DONTWAIT,
            NULL,
            NULL
        );

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(step_us);
                quiet_elapsed_ms += 1;
                total_elapsed_ms += 1;

                if (quiet_elapsed_ms >= quiet_time_ms) {
                    break;
                }

                continue;
            }

            perror("recvfrom drain quiet");
            break;
        }

        drained++;
        quiet_elapsed_ms = 0;
    }

    return drained;
}

static int read_iface_tx_snapshot(
    const char *iface_name,
    uint64_t *tx_packets,
    int *speed_mbps
)
{
    iface_stats_t stats;

    if (iface_name == NULL || tx_packets == NULL || speed_mbps == NULL) {
        return -1;
    }

    if (read_iface_stats(iface_name, &stats) != 0) {
        return -1;
    }

    *tx_packets = stats.tx_packets;

    if (stats.speed_mbps > 0) {
        *speed_mbps = stats.speed_mbps;
    } else {
        *speed_mbps = DEFAULT_LINK_MBPS;
    }

    return 0;
}

int fpga_loopback_load_test(
    const char *iface_name,
    const char *fpga_ip,
    int fpga_data_port,
    int local_port,
    int packet_count,
    int payload_size,
    fpga_loopback_load_result_t *result
)
{
    int tx_sock;
    int rx_sock;

    struct sockaddr_in dest_addr;

    uint8_t *payload;

    int i;
    int sent_packets = 0;
    int send_errors = 0;
    int drained_packets = 0;

    uint64_t t0;
    uint64_t t1;
    uint64_t iface_tx_before = 0;
    int iface_speed_mbps = DEFAULT_LINK_MBPS;

    if (fpga_ip == NULL || result == NULL) {
        return -1;
    }

    if (packet_count <= 0) {
        fprintf(stderr, "Packet count must be greater than 0\n");
        return -1;
    }

    if (payload_size <= 0) {
        fprintf(stderr, "Payload size must be greater than 0\n");
        return -1;
    }

    memset(result, 0, sizeof(*result));

    payload = malloc((size_t)payload_size);

    if (payload == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        return -1;
    }

    fill_pseudorandom_payload(payload, payload_size, 0x12345678);

    tx_sock = make_tx_socket();

    if (tx_sock < 0) {
        free(payload);
        return -1;
    }

    rx_sock = make_rx_drain_socket(local_port);

    if (rx_sock < 0) {
        close(tx_sock);
        free(payload);
        return -1;
    }

    if (build_dest_addr(fpga_ip, fpga_data_port, &dest_addr) != 0) {
        close(rx_sock);
        close(tx_sock);
        free(payload);
        return -1;
    }

    if (connect_tx_socket(tx_sock, &dest_addr) != 0) {
        close(rx_sock);
        close(tx_sock);
        free(payload);
        return -1;
    }

    if (iface_name != NULL &&
        read_iface_tx_snapshot(
            iface_name,
            &iface_tx_before,
            &iface_speed_mbps
        ) != 0) {
        fprintf(stderr, "Warning: could not read TX counter for %s\n", iface_name);
        iface_name = NULL;
    }

    printf("Starting loopback load test\n");
    printf("  packets:      %d\n", packet_count);
    printf("  payload:      %d bytes\n", payload_size);
    printf("  data port:    %d\n", fpga_data_port);
    printf("  local port:   %d\n", local_port);

    t0 = now_ns();

    for (i = 0; i < packet_count; i++) {
        ssize_t n;

        n = send(
            tx_sock,
            payload,
            (size_t)payload_size,
            0
        );

        if (n < 0) {
            send_errors++;
            continue;
        }

        sent_packets++;

        /*
         * We do not validate packets here.
         * This only drains the local UDP queue so the kernel does not
         * generate NoPorts or fill the receive buffer too quickly.
         */
        if ((i % LOOPBACK_DRAIN_EVERY_N_PACKETS) == 0) {
            drained_packets += drain_rx_socket(rx_sock);
        }
    }

    t1 = now_ns();

    /*
    * Drain remaining loopback replies.
    *
    * This is important because the FPGA may still be transmitting
    * some packets after the PC has finished sending. If we close the
    * local UDP socket too early, Linux will answer late packets with:
    *
    *   ICMP Destination unreachable (Port unreachable)
    */

    drained_packets += drain_rx_socket_until_quiet(
        rx_sock,
        50,     /* quiet time: no packets for 50 ms */
        1000    /* max wait: 1000 ms */
    );

    close(rx_sock);
    close(tx_sock);
    free(payload);

    result->packet_count = packet_count;
    result->payload_size = payload_size;
    result->sent_packets = sent_packets;
    result->send_errors = send_errors;
    result->drained_packets = drained_packets;

    result->elapsed_s = (double)(t1 - t0) / 1000000000.0;

    if (result->elapsed_s > 0.0) {
        result->packets_per_second =
            (double)sent_packets / result->elapsed_s;

        result->payload_mbps =
            ((double)sent_packets * (double)payload_size * 8.0) /
            result->elapsed_s /
            1000000.0;

        result->estimated_wire_mbps =
            ((double)sent_packets *
             (double)(payload_size + ESTIMATED_ETHERNET_OVERHEAD_BYTES) *
             8.0) /
            result->elapsed_s /
            1000000.0;
    }

    return 0;
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

int append_fpga_loopback_load_csv(
    const char *filename,
    const char *fpga_ip,
    int fpga_ctrl_port,
    int fpga_data_port,
    int local_port,
    const fpga_loopback_load_result_t *result
)
{
    FILE *file;
    time_t now;
    int write_header;

    if (filename == NULL || fpga_ip == NULL || result == NULL) {
        return -1;
    }

    write_header = !file_exists(filename);

    file = fopen(filename, "a");

    if (file == NULL) {
        perror("fopen loopback load CSV");
        return -1;
    }

    if (write_header) {
        fprintf(
            file,
            "timestamp,fpga_ip,fpga_ctrl_port,fpga_data_port,local_port,"
            "packet_count,payload_size,sent_packets,send_errors,drained_packets,"
            "elapsed_s,packets_per_second,payload_mbps,estimated_wire_mbps\n"
        );
    }

    now = time(NULL);

    fprintf(
        file,
        "%ld,%s,%d,%d,%d,%d,%d,%d,%d,%d,%.9f,%.3f,%.6f,%.6f\n",
        now,
        fpga_ip,
        fpga_ctrl_port,
        fpga_data_port,
        local_port,
        result->packet_count,
        result->payload_size,
        result->sent_packets,
        result->send_errors,
        result->drained_packets,
        result->elapsed_s,
        result->packets_per_second,
        result->payload_mbps,
        result->estimated_wire_mbps
    );

    fclose(file);
    return 0;
}
