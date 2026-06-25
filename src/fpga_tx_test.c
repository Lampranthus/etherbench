#define _GNU_SOURCE

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
#include <poll.h>
#include <fcntl.h>

#define FPGA_TX_RX_BUFFER_SIZE 2048
#define FPGA_TX_QUIET_TIME_MS 100
#define FPGA_TX_CAPTURE_BATCH_SIZE 128
#define FPGA_TX_REQUESTED_RCVBUF (32 * 1024 * 1024)
#define FPGA_TX_CAPTURE_FILE_BUFFER_SIZE (4 * 1024 * 1024)
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
    int rcvbuf = FPGA_TX_REQUESTED_RCVBUF;
    int actual_rcvbuf = 0;
    socklen_t actual_rcvbuf_len = sizeof(actual_rcvbuf);

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

#ifdef SO_RCVBUFFORCE
    if (setsockopt(
            sock,
            SOL_SOCKET,
            SO_RCVBUFFORCE,
            &rcvbuf,
            sizeof(rcvbuf)
        ) < 0)
#endif
    {
        if (setsockopt(
                sock,
                SOL_SOCKET,
                SO_RCVBUF,
                &rcvbuf,
                sizeof(rcvbuf)
            ) < 0) {
            perror("setsockopt SO_RCVBUF");
        }
    }

    if (getsockopt(
            sock,
            SOL_SOCKET,
            SO_RCVBUF,
            &actual_rcvbuf,
            &actual_rcvbuf_len
        ) == 0) {
        printf(
            "UDP receive buffer reported by kernel: %d bytes\n",
            actual_rcvbuf
        );

        if (actual_rcvbuf < FPGA_TX_REQUESTED_RCVBUF) {
            fprintf(
                stderr,
                "Warning: UDP receive buffer is smaller than requested.\n"
                "For high-PPS captures, run as root:\n"
                "  sysctl -w net.core.rmem_max=33554432\n"
                "  sysctl -w net.core.rmem_default=33554432\n"
            );
        }
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

int fpga_tx_capture_run(
    const char *fpga_ip,
    int fpga_ctrl_port,
    int local_port,
    int timeout_ms,
    int packet_count,
    int payload_size,
    const char *mode,
    const char *output_filename,
    fpga_tx_capture_result_t *result
)
{
    int rx_sock = -1;
    FILE *output = NULL;
    uint8_t *batch_buffer = NULL;
    struct mmsghdr *messages = NULL;
    struct iovec *iovecs = NULL;
    struct sockaddr_in *source_addrs = NULL;
    char *file_buffer = NULL;
    struct in_addr expected_addr;
    struct pollfd poll_fd;
    uint64_t trigger_ns;
    uint64_t first_rx_ns = 0;
    uint64_t last_rx_ns = 0;
    uint64_t valid_packet_deadline_ns;
    int ret = -1;

    if (fpga_ip == NULL ||
        mode == NULL ||
        output_filename == NULL ||
        result == NULL ||
        packet_count <= 0 ||
        payload_size <= 0 ||
        timeout_ms <= 0) {
        return -1;
    }

    if (inet_pton(AF_INET, fpga_ip, &expected_addr) != 1) {
        fprintf(stderr, "Invalid FPGA IP: %s\n", fpga_ip);
        return -1;
    }

    memset(result, 0, sizeof(*result));
    result->requested_packets = packet_count;
    result->payload_size = payload_size;
    snprintf(result->mode, sizeof(result->mode), "%s", mode);

    size_t capture_stride = (size_t)payload_size + 1U;
    batch_buffer = malloc(
        FPGA_TX_CAPTURE_BATCH_SIZE * capture_stride
    );
    messages = calloc(
        FPGA_TX_CAPTURE_BATCH_SIZE,
        sizeof(*messages)
    );
    iovecs = calloc(
        FPGA_TX_CAPTURE_BATCH_SIZE,
        sizeof(*iovecs)
    );
    source_addrs = calloc(
        FPGA_TX_CAPTURE_BATCH_SIZE,
        sizeof(*source_addrs)
    );
    file_buffer = malloc(FPGA_TX_CAPTURE_FILE_BUFFER_SIZE);

    if (batch_buffer == NULL ||
        messages == NULL ||
        iovecs == NULL ||
        source_addrs == NULL ||
        file_buffer == NULL) {
        fprintf(stderr, "Could not allocate FPGA TX capture buffers\n");
        goto cleanup;
    }

    for (int index = 0; index < FPGA_TX_CAPTURE_BATCH_SIZE; index++) {
        iovecs[index].iov_base =
            batch_buffer + ((size_t)index * capture_stride);
        iovecs[index].iov_len = capture_stride;
        messages[index].msg_hdr.msg_iov = &iovecs[index];
        messages[index].msg_hdr.msg_iovlen = 1;
        messages[index].msg_hdr.msg_name = &source_addrs[index];
        messages[index].msg_hdr.msg_namelen = sizeof(source_addrs[index]);
    }

    output = fopen(output_filename, "wb");

    if (output == NULL) {
        perror("fopen FPGA TX capture");
        goto cleanup;
    }

    if (setvbuf(
            output,
            file_buffer,
            _IOFBF,
            FPGA_TX_CAPTURE_FILE_BUFFER_SIZE
        ) != 0) {
        perror("setvbuf FPGA TX capture");
        goto cleanup;
    }

    rx_sock = make_rx_socket(local_port);

    if (rx_sock < 0) {
        goto cleanup;
    }

    poll_fd.fd = rx_sock;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;

    printf("Sending FPGA trigger and capturing UDP payloads...\n");
    trigger_ns = now_ns();

    if (fpga_ctrl_send_trigger(fpga_ip, fpga_ctrl_port) != 0) {
        fprintf(stderr, "Could not send trigger command\n");
        goto cleanup;
    }

    valid_packet_deadline_ns =
        trigger_ns + ((uint64_t)timeout_ms * 1000000ULL);

    while (result->captured_packets < packet_count) {
        uint64_t current_ns;
        uint64_t remaining_ns;
        int batch_limit;
        int captured_before_batch;
        int received_in_batch;
        int poll_timeout_ms;
        int poll_result;

        current_ns = now_ns();

        if (current_ns >= valid_packet_deadline_ns) {
            fprintf(
                stderr,
                "Capture stopped after %d ms without receiving a valid packet\n",
                timeout_ms
            );
            break;
        }

        remaining_ns = valid_packet_deadline_ns - current_ns;
        poll_timeout_ms = (int)((remaining_ns + 999999ULL) / 1000000ULL);
        poll_result = poll(&poll_fd, 1, poll_timeout_ms);

        if (poll_result == 0) {
            fprintf(
                stderr,
                "Capture stopped after %d ms without receiving a valid packet\n",
                timeout_ms
            );
            break;
        }

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            perror("poll FPGA TX capture");
            goto cleanup;
        }

        batch_limit = packet_count - result->captured_packets;
        if (batch_limit > FPGA_TX_CAPTURE_BATCH_SIZE) {
            batch_limit = FPGA_TX_CAPTURE_BATCH_SIZE;
        }

        for (int index = 0; index < batch_limit; index++) {
            messages[index].msg_len = 0;
            messages[index].msg_hdr.msg_flags = 0;
            messages[index].msg_hdr.msg_namelen =
                sizeof(source_addrs[index]);
        }

        received_in_batch = recvmmsg(
            rx_sock,
            messages,
            (unsigned int)batch_limit,
            MSG_DONTWAIT,
            NULL
        );

        if (received_in_batch < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }

            perror("recvmmsg FPGA TX capture");
            goto cleanup;
        }

        captured_before_batch = result->captured_packets;

        for (int index = 0; index < received_in_batch; index++) {
            size_t received_size = messages[index].msg_len;
            uint8_t *payload =
                batch_buffer + ((size_t)index * capture_stride);

            if (source_addrs[index].sin_addr.s_addr != expected_addr.s_addr) {
                result->ignored_packets++;
                continue;
            }

            if (received_size != (size_t)payload_size) {
                result->invalid_size_packets++;
                continue;
            }

            if (fwrite(
                    payload,
                    1,
                    received_size,
                    output
                ) != received_size) {
                perror("fwrite FPGA TX capture");
                goto cleanup;
            }

            if (result->captured_packets == 0) {
                first_rx_ns = now_ns();
            }

            result->captured_packets++;
            result->captured_bytes += (uint64_t)received_size;
        }

        if (result->captured_packets > captured_before_batch) {
            last_rx_ns = now_ns();
            valid_packet_deadline_ns =
                last_rx_ns + ((uint64_t)timeout_ms * 1000000ULL);
        }
    }

    if (fflush(output) != 0) {
        perror("fflush FPGA TX capture");
        goto cleanup;
    }

    result->lost_packets = packet_count - result->captured_packets;

    if (result->captured_packets > 0 && last_rx_ns >= first_rx_ns) {
        result->elapsed_s =
            (double)(last_rx_ns - first_rx_ns) / 1000000000.0;
        result->trigger_to_last_s =
            (double)(last_rx_ns - trigger_ns) / 1000000000.0;
    }

    ret = result->captured_packets == packet_count ? 0 : 2;

cleanup:
    if (rx_sock >= 0) {
        close(rx_sock);
    }

    if (output != NULL && fclose(output) != 0) {
        if (ret >= 0) {
            perror("fclose FPGA TX capture");
            ret = -1;
        }
    }

    free(file_buffer);
    free(source_addrs);
    free(iovecs);
    free(messages);
    free(batch_buffer);

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
