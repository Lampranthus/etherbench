#define _DEFAULT_SOURCE

#include "fpga_tx_test.h"
#include "fpga_ctrl.h"
#include "fpga_stats.h"
#include "fpga_loopback_mode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>

#define FPGA_TX_RX_BUFFER_SIZE 2048
#define FPGA_TX_QUIET_TIME_MS 100
#define ESTIMATED_ETHERNET_OVERHEAD_BYTES 66

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static int make_rx_socket(int local_port)
{
    int sock;
    struct sockaddr_in addr;
    int flags;
    int reuse = 1;
    int rcvbuf = 32 * 1024 * 1024;

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket rx");
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(sock);
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        perror("setsockopt SO_RCVBUF");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)local_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind rx socket");
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

static int receive_until_done(
    int sock,
    int expected_packets,
    uint64_t *first_rx_ns,
    uint64_t *last_rx_ns
)
{
    uint8_t buffer[FPGA_TX_RX_BUFFER_SIZE];
    int received = 0;
    int quiet_elapsed_ms = 0;
    int total_extra_wait_ms = 0;
    const int step_us = 1000;

    *first_rx_ns = 0;
    *last_rx_ns = 0;

    while (received < expected_packets) {
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

                quiet_elapsed_ms++;
                total_extra_wait_ms++;

                if (received > 0 &&
                    quiet_elapsed_ms >= FPGA_TX_QUIET_TIME_MS) {
                    break;
                }

                continue;
            }

            perror("recvfrom tx test");
            break;
        }

        if (received == 0) {
            *first_rx_ns = now_ns();
        }

        *last_rx_ns = now_ns();
        received++;
        quiet_elapsed_ms = 0;
    }

    /*
     * Extra drain after reaching expected packets, in case there are
     * extra packets or duplicated packets.
     */
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
            break;
        }

        received++;
        *last_rx_ns = now_ns();
    }

    return received;
}

static const char *mode_from_stats(const fpga_stats_t *stats)
{
    if (stats == NULL) {
        return "unknown";
    }

    if (stats->mode.random) {
        return "random";
    }

    if (stats->mode.constant) {
        return "constant";
    }

    if (stats->mode.sequential) {
        return "sequential";
    }

    return "unknown";
}

int verify_fpga_tx_config(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms,
    int packet_count,
    int payload_size,
    const char *mode
)
{
    fpga_stats_t stats;
    const char *current_mode;

    if (query_fpga_stats(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms,
            &stats
        ) != 0) {
        fprintf(stderr, "Could not query FPGA stats while verifying TX config\n");
        return -1;
    }

    current_mode = mode_from_stats(&stats);

    if (stats.tx_config.packets_per_trigger != (uint32_t)packet_count) {
        fprintf(stderr,
                "FPGA pktn mismatch: expected=%d actual=%u\n",
                packet_count,
                stats.tx_config.packets_per_trigger);
        return -1;
    }

    if (stats.tx_config.payload_size_bytes != (uint16_t)payload_size) {
        fprintf(stderr,
                "FPGA udpmtu mismatch: expected=%d actual=%u\n",
                payload_size,
                stats.tx_config.payload_size_bytes);
        return -1;
    }

    if (strcmp(current_mode, mode) != 0) {
        fprintf(stderr,
                "FPGA mode mismatch: expected=%s actual=%s\n",
                mode,
                current_mode);
        return -1;
    }

    return 0;
}

int fpga_tx_test_run(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int packet_count,
    int payload_size,
    const char *mode,
    fpga_tx_test_result_t *result
)
{

    int rx_sock = -1;
    int ret = -1;

    uint64_t trigger_ns;
    uint64_t first_rx_ns;
    uint64_t last_rx_ns;

    int received_packets;

    if (fpga_ip == NULL || mode == NULL || result == NULL) {
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

    result->requested_packets = packet_count;
    result->payload_size = payload_size;
    snprintf(result->mode, sizeof(result->mode), "%s", mode);

    rx_sock = make_rx_socket(local_port);

    if (rx_sock < 0) {
        goto cleanup;
    }

    printf("Sending FPGA trigger...\n");

    trigger_ns = now_ns();

    if (fpga_ctrl_send_trigger(fpga_ip, fpga_ctrl_port) != 0) {
        fprintf(stderr, "Could not send trigger command\n");
        goto cleanup;
    }

    received_packets = receive_until_done(
        rx_sock,
        packet_count,
        &first_rx_ns,
        &last_rx_ns
    );

    result->received_packets = received_packets;

    if (received_packets <= packet_count) {
        result->lost_packets = packet_count - received_packets;
    } else {
        result->lost_packets = 0;
    }

    if (received_packets > 0 && first_rx_ns > 0 && last_rx_ns >= first_rx_ns) {
        result->elapsed_s =
            (double)(last_rx_ns - first_rx_ns) / 1000000000.0;

        result->trigger_to_last_s =
            (double)(last_rx_ns - trigger_ns) / 1000000000.0;
    }

    if (result->elapsed_s > 0.0) {
        result->payload_goodput_mbps =
            ((double)received_packets * (double)payload_size * 8.0) /
            result->elapsed_s /
            1000000.0;

        result->estimated_wire_mbps =
            ((double)received_packets *
             (double)(payload_size + ESTIMATED_ETHERNET_OVERHEAD_BYTES) *
             8.0) /
            result->elapsed_s /
            1000000.0;
    }

    ret = 0;

cleanup:
    if (rx_sock >= 0) {
        close(rx_sock);
    }

    return ret;
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

int append_fpga_tx_test_csv(
    const char *filename,
    const char *iface_name,
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    const fpga_tx_test_result_t *result
)
{
    FILE *file;
    time_t now;
    int write_header;

    if (filename == NULL ||
        iface_name == NULL ||
        fpga_ip == NULL ||
        result == NULL) {
        return -1;
    }

    write_header = !file_exists(filename);

    file = fopen(filename, "a");

    if (file == NULL) {
        perror("fopen FPGA TX CSV");
        return -1;
    }

    if (write_header) {
        fprintf(
            file,
            "timestamp,iface,fpga_ip,fpga_ctrl_port,local_port,"
            "requested_packets,payload_size,mode,"
            "received_packets,lost_packets,"
            "elapsed_s,trigger_to_last_s,"
            "payload_goodput_mbps,estimated_wire_mbps\n"
        );
    }

    now = time(NULL);

    fprintf(
        file,
        "%ld,%s,%s,%d,%d,%d,%d,%s,%d,%d,"
        "%.9f,%.9f,%.6f,%.6f\n",
        now,
        iface_name,
        fpga_ip,
        fpga_ctrl_port,
        local_port,
        result->requested_packets,
        result->payload_size,
        result->mode,
        result->received_packets,
        result->lost_packets,
        result->elapsed_s,
        result->trigger_to_last_s,
        result->payload_goodput_mbps,
        result->estimated_wire_mbps
    );

    fclose(file);

    return 0;
}
