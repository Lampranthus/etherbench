#define _DEFAULT_SOURCE

#include "fpga_rtt.h"
#include "fpga_stats.h"
#include "fpga_ctrl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#define RTT_MAGIC_0 0x45
#define RTT_MAGIC_1 0x42
#define RTT_MAGIC_2 0x52
#define RTT_MAGIC_3 0x54

#define RTT_TEST_PROBES 3
#define RTT_LOOPBACK_WAIT_US 500000

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static int make_udp_socket(int local_port, int timeout_ms)
{
    int sock;
    struct sockaddr_in addr;
    struct timeval timeout;

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket");
        return -1;
    }

    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(
            sock,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)
        ) < 0) {
        perror("setsockopt SO_RCVTIMEO");
        close(sock);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)local_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind RTT socket");
        close(sock);
        return -1;
    }

    return sock;
}

static int build_dest_addr(
    const char *fpga_ip,
    int fpga_port,
    struct sockaddr_in *addr
)
{
    if (fpga_ip == NULL || addr == NULL) {
        return -1;
    }

    memset(addr, 0, sizeof(*addr));

    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)fpga_port);

    if (inet_pton(AF_INET, fpga_ip, &addr->sin_addr) != 1) {
        fprintf(stderr, "Invalid FPGA IP: %s\n", fpga_ip);
        return -1;
    }

    return 0;
}

static void build_rtt_payload(uint8_t *payload, int payload_size, uint32_t seq)
{
    int i;

    /*
     * Payload format:
     *
     * byte 0..3   = magic "EBRT"
     * byte 4..7   = sequence number, big endian
     * byte 8..end = deterministic pattern
     */
    payload[0] = RTT_MAGIC_0;
    payload[1] = RTT_MAGIC_1;
    payload[2] = RTT_MAGIC_2;
    payload[3] = RTT_MAGIC_3;

    payload[4] = (uint8_t)((seq >> 24) & 0xFF);
    payload[5] = (uint8_t)((seq >> 16) & 0xFF);
    payload[6] = (uint8_t)((seq >> 8) & 0xFF);
    payload[7] = (uint8_t)(seq & 0xFF);

    for (i = 8; i < payload_size; i++) {
        payload[i] = (uint8_t)((seq + i) & 0xFF);
    }
}

static int verify_rtt_payload(
    const uint8_t *tx_payload,
    const uint8_t *rx_payload,
    ssize_t rx_len,
    int payload_size
)
{
    if (rx_len != payload_size) {
        return -1;
    }

    if (memcmp(tx_payload, rx_payload, (size_t)payload_size) != 0) {
        return -1;
    }

    return 0;
}

static int ensure_fpga_loopback(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms
)
{
    fpga_stats_t stats;

    printf("Checking FPGA loopback mode...\n");

    if (query_fpga_stats(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms,
            &stats
        ) != 0) {
        fprintf(stderr, "Could not query FPGA regstats\n");
        return -1;
    }

    if (stats.mode.loopback) {
        printf("FPGA loopback is already enabled\n");
        return 0;
    }

    printf("FPGA loopback is disabled. Enabling loopback...\n");

    if (fpga_ctrl_enable_loopback(fpga_ip, fpga_ctrl_port) != 0) {
        fprintf(stderr, "Could not send loopback command\n");
        return -1;
    }

    usleep(RTT_LOOPBACK_WAIT_US);

    if (query_fpga_stats(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms,
            &stats
        ) != 0) {
        fprintf(stderr, "Could not query FPGA regstats after loopback command\n");
        return -1;
    }

    if (!stats.mode.loopback) {
        fprintf(stderr, "FPGA loopback did not enable\n");
        return -1;
    }

    printf("FPGA loopback enabled\n");

    return 0;
}

static int disable_fpga_loopback(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms
)
{
    fpga_stats_t stats;

    printf("Checking FPGA loopback mode...\n");

    if (query_fpga_stats(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms,
            &stats
        ) != 0) {
        fprintf(stderr, "Could not query FPGA regstats\n");
        return -1;
    }

    if (!stats.mode.loopback) {
        printf("FPGA loopback is already disabled\n");
        return 0;
    }

    printf("FPGA loopback is enabled. Disabling loopback...\n");

    if (fpga_ctrl_enable_loopback(fpga_ip, fpga_ctrl_port) != 0) {
        fprintf(stderr, "Could not send loopback command\n"); 
        return -1;
    }

    usleep(RTT_LOOPBACK_WAIT_US);

    if (query_fpga_stats(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms,
            &stats
        ) != 0) {
        fprintf(stderr, "Could not query FPGA regstats after loopback command\n");
        return -1;
    }

    if (stats.mode.loopback) {
        fprintf(stderr, "FPGA loopback did not disable\n");
        return -1;
    }

    printf("FPGA loopback disabled\n");

    return 0;
}

static int send_and_receive_once(
    int sock,
    const struct sockaddr_in *dest_addr,
    uint8_t *tx_payload,
    uint8_t *rx_payload,
    int payload_size,
    uint32_t seq,
    double *rtt_ms
)
{
    uint64_t t0;
    uint64_t t1;
    ssize_t n;

    build_rtt_payload(tx_payload, payload_size, seq);

    t0 = now_ns();

    if (sendto(
            sock,
            tx_payload,
            (size_t)payload_size,
            0,
            (const struct sockaddr *)dest_addr,
            sizeof(*dest_addr)
        ) < 0) {
        perror("sendto RTT");
        return -1;
    }

    n = recvfrom(sock, rx_payload, (size_t)payload_size, 0, NULL, NULL);

    t1 = now_ns();

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 1;
        }

        perror("recvfrom RTT");
        return -1;
    }

    if (verify_rtt_payload(tx_payload, rx_payload, n, payload_size) != 0) {
        fprintf(stderr, "Invalid loopback payload for seq=%u\n", seq);
        return -1;
    }

    *rtt_ms = (double)(t1 - t0) / 1000000.0;

    return 0;
}

static int run_probe_test(
    int sock,
    const struct sockaddr_in *dest_addr,
    int payload_size
)
{
    uint8_t *tx_payload;
    uint8_t *rx_payload;
    double rtt_ms;
    int i;
    int ret;

    tx_payload = malloc((size_t)payload_size);
    rx_payload = malloc((size_t)payload_size);

    if (tx_payload == NULL || rx_payload == NULL) {
        free(tx_payload);
        free(rx_payload);
        fprintf(stderr, "Memory allocation failed\n");
        return -1;
    }

    printf("Running loopback probe test...\n");

    for (i = 0; i < RTT_TEST_PROBES; i++) {
        ret = send_and_receive_once(
            sock,
            dest_addr,
            tx_payload,
            rx_payload,
            payload_size,
            (uint32_t)i,
            &rtt_ms
        );

        if (ret != 0) {
            fprintf(stderr, "Loopback probe failed\n");
            free(tx_payload);
            free(rx_payload);
            return -1;
        }
    }

    free(tx_payload);
    free(rx_payload);

    printf("Loopback probe OK\n");

    return 0;
}

static void compute_stats(
    const double *values,
    int count,
    fpga_rtt_result_t *result
)
{
    int i;
    double sum = 0.0;
    double variance_sum = 0.0;

    result->min_ms = values[0];
    result->max_ms = values[0];

    for (i = 0; i < count; i++) {
        if (values[i] < result->min_ms) {
            result->min_ms = values[i];
        }

        if (values[i] > result->max_ms) {
            result->max_ms = values[i];
        }

        sum += values[i];
    }

    result->avg_ms = sum / (double)count;

    for (i = 0; i < count; i++) {
        double diff = values[i] - result->avg_ms;
        variance_sum += diff * diff;
    }

    result->stddev_ms = sqrt(variance_sum / (double)count);
}

int fpga_rtt_test(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int fpga_loopback_port,
    int local_port,
    int timeout_ms,
    int packet_count,
    int payload_size,
    fpga_rtt_result_t *result
)
{
    int sock;
    struct sockaddr_in dest_addr;

    uint8_t *tx_payload;
    uint8_t *rx_payload;
    double *rtt_values;

    int i;
    int received = 0;
    int lost = 0;

    if (fpga_ip == NULL || result == NULL) {
        return -1;
    }

    if (packet_count <= 0) {
        fprintf(stderr, "Packet count must be greater than 0\n");
        return -1;
    }

    if (payload_size < 8) {
        fprintf(stderr, "Payload size must be at least 8 bytes\n");
        return -1;
    }

    memset(result, 0, sizeof(*result));

    if (ensure_fpga_loopback(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms
        ) != 0) {
        return -1;
    }

    sock = make_udp_socket(local_port, timeout_ms);

    if (sock < 0) {
        return -1;
    }

    if (build_dest_addr(fpga_ip, fpga_loopback_port, &dest_addr) != 0) {
        close(sock);
        return -1;
    }

    if (run_probe_test(sock, &dest_addr, payload_size) != 0) {
        close(sock);
        return -1;
    }

    tx_payload = malloc((size_t)payload_size);
    rx_payload = malloc((size_t)payload_size);
    rtt_values = malloc((size_t)packet_count * sizeof(double));

    if (tx_payload == NULL || rx_payload == NULL || rtt_values == NULL) {
        fprintf(stderr, "Memory allocation failed\n");
        free(tx_payload);
        free(rx_payload);
        free(rtt_values);
        close(sock);
        return -1;
    }

    printf("Starting RTT test: packets=%d payload=%d bytes loopback_port=%d local_port=%d\n",
            packet_count,
            payload_size,
            fpga_loopback_port,
            local_port);

    for (i = 0; i < packet_count; i++) {
        double rtt_ms = 0.0;

        int ret = send_and_receive_once(
            sock,
            &dest_addr,
            tx_payload,
            rx_payload,
            payload_size,
            (uint32_t)(i + RTT_TEST_PROBES),
            &rtt_ms
        );

        if (ret == 0) {
            rtt_values[received] = rtt_ms;
            received++;
        } else if (ret == 1) {
            lost++;
        } else {
            free(tx_payload);
            free(rx_payload);
            free(rtt_values);
            close(sock);
            return -1;
        }
    }

    close(sock);

    if (disable_fpga_loopback(
            fpga_ip,
            fpga_ctrl_port,
            local_port,
            timeout_ms
        ) != 0) {
        return -1;
    }  

    result->sent = packet_count;
    result->received = received;
    result->lost = lost;

    if (received > 0) {
        compute_stats(rtt_values, received, result);
    }

    free(tx_payload);
    free(rx_payload);
    free(rtt_values);

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

int append_fpga_rtt_csv(
    const char *filename,
    const char *fpga_ip,
    int fpga_ctrl_port,
    int fpga_data_port,
    int local_port,
    int payload_size,
    const fpga_rtt_result_t *result
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
        perror("fopen RTT CSV");
        return -1;
    }

    if (write_header) {
        fprintf(
            file,
            "timestamp,fpga_ip,fpga_ctrl_port,fpga_data_port,local_port,"
            "payload_size,sent,received,lost,"
            "min_ms,avg_ms,max_ms,stddev_ms\n"
        );
    }

    now = time(NULL);

    fprintf(
        file,
        "%ld,%s,%d,%d,%d,%d,%d,%d,%d,%.9f,%.9f,%.9f,%.9f\n",
        now,
        fpga_ip,
        fpga_ctrl_port,
        fpga_data_port,
        local_port,
        payload_size,
        result->sent,
        result->received,
        result->lost,
        result->min_ms,
        result->avg_ms,
        result->max_ms,
        result->stddev_ms
    );

    fclose(file);
    return 0;
}