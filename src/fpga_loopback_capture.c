#define _GNU_SOURCE

#include "fpga_loopback_capture.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CAPTURE_BATCH_SIZE 128
#define CAPTURE_FILE_BUFFER_SIZE (4 * 1024 * 1024)
#define REQUESTED_SOCKET_BUFFER (32 * 1024 * 1024)
#define PACE_BURST_BYTES 65536
#define ETHERNET_OVERHEAD_BYTES 66
#define LINK_BITS_PER_SECOND 1000000000ULL

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static void wait_until_ns(uint64_t target_ns)
{
    while (1) {
        uint64_t current_ns = now_ns();

        if (current_ns >= target_ns) {
            return;
        }

        if (target_ns - current_ns > 100000ULL) {
            usleep((useconds_t)((target_ns - current_ns - 100000ULL) / 1000ULL));
        }
    }
}

static int configure_socket_buffer(int sock, int option)
{
    int requested = REQUESTED_SOCKET_BUFFER;
    int actual = 0;
    socklen_t actual_len = sizeof(actual);

#if defined(SO_RCVBUFFORCE) && defined(SO_SNDBUFFORCE)
    if ((option == SO_RCVBUF || option == SO_SNDBUF) &&
        setsockopt(
            sock,
            SOL_SOCKET,
            option == SO_RCVBUF ? SO_RCVBUFFORCE : SO_SNDBUFFORCE,
            &requested,
            sizeof(requested)
        ) == 0) {
    } else
#endif
    if (setsockopt(
            sock,
            SOL_SOCKET,
            option,
            &requested,
            sizeof(requested)
        ) < 0) {
        perror(option == SO_RCVBUF ? "setsockopt SO_RCVBUF" :
                                    "setsockopt SO_SNDBUF");
    }

    if (getsockopt(
            sock,
            SOL_SOCKET,
            option,
            &actual,
            &actual_len
        ) == 0) {
        printf(
            "%s buffer reported by kernel: %d bytes\n",
            option == SO_RCVBUF ? "UDP receive" : "UDP send",
            actual
        );

        if (actual < REQUESTED_SOCKET_BUFFER) {
            fprintf(
                stderr,
                "Warning: UDP %s buffer is smaller than requested.\n"
                "For high-rate loopback captures, run as root:\n"
                "  sysctl -w net.core.%s=33554432\n",
                option == SO_RCVBUF ? "receive" : "send",
                option == SO_RCVBUF ? "rmem_max" : "wmem_max"
            );
        }
    }

    return 0;
}

static int bind_to_interface(int sock, const char *iface_name)
{
    if (iface_name == NULL || iface_name[0] == '\0') {
        return 0;
    }

    if (setsockopt(
            sock,
            SOL_SOCKET,
            SO_BINDTODEVICE,
            iface_name,
            strlen(iface_name) + 1
        ) < 0) {
        perror("setsockopt SO_BINDTODEVICE");
        return -1;
    }

    return 0;
}

static int make_rx_socket(const char *iface_name, int local_port)
{
    int sock;
    int flags;
    int reuse = 1;
    struct sockaddr_in address;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket loopback capture RX");
        return -1;
    }

    if (setsockopt(
            sock,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse,
            sizeof(reuse)
        ) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(sock);
        return -1;
    }

    configure_socket_buffer(sock, SO_RCVBUF);
    configure_socket_buffer(sock, SO_SNDBUF);

    if (bind_to_interface(sock, iface_name) != 0) {
        close(sock);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)local_port);

    if (bind(sock, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind loopback capture RX");
        close(sock);
        return -1;
    }

    flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl loopback capture RX");
        close(sock);
        return -1;
    }

    return sock;
}

static int build_destination(
    const char *fpga_ip,
    int fpga_data_port,
    struct sockaddr_in *destination
)
{
    if (destination == NULL) {
        return -1;
    }

    memset(destination, 0, sizeof(*destination));
    destination->sin_family = AF_INET;
    destination->sin_port = htons((uint16_t)fpga_data_port);

    if (inet_pton(AF_INET, fpga_ip, &destination->sin_addr) != 1) {
        fprintf(stderr, "Invalid FPGA IP: %s\n", fpga_ip);
        return -1;
    }

    return 0;
}

static void fill_sequential_payload(
    uint8_t *payload,
    int payload_size,
    uint16_t *sequence
)
{
    for (int offset = 0; offset < payload_size; offset += 2) {
        payload[offset] = (uint8_t)((*sequence >> 8) & 0xFF);
        payload[offset + 1] = (uint8_t)(*sequence & 0xFF);
        (*sequence)++;
    }
}

static int receive_batches(
    int rx_sock,
    const struct in_addr *expected_addr,
    int payload_size,
    int packet_limit,
    uint8_t *batch_buffer,
    struct mmsghdr *messages,
    struct sockaddr_in *source_addrs,
    FILE *output,
    fpga_loopback_capture_result_t *result
)
{
    int captured_before = result->captured_packets;
    size_t stride = (size_t)payload_size + 1U;

    while (result->captured_packets < packet_limit) {
        int batch_limit = packet_limit - result->captured_packets;
        int received;

        if (batch_limit > CAPTURE_BATCH_SIZE) {
            batch_limit = CAPTURE_BATCH_SIZE;
        }

        for (int index = 0; index < batch_limit; index++) {
            messages[index].msg_len = 0;
            messages[index].msg_hdr.msg_flags = 0;
            messages[index].msg_hdr.msg_namelen =
                sizeof(source_addrs[index]);
        }

        received = recvmmsg(
            rx_sock,
            messages,
            (unsigned int)batch_limit,
            MSG_DONTWAIT,
            NULL
        );

        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }

            perror("recvmmsg loopback capture");
            return -1;
        }

        for (int index = 0; index < received; index++) {
            size_t received_size = messages[index].msg_len;
            uint8_t *payload =
                batch_buffer + ((size_t)index * stride);

            if (source_addrs[index].sin_addr.s_addr != expected_addr->s_addr) {
                result->ignored_packets++;
                continue;
            }

            if (received_size != (size_t)payload_size) {
                result->invalid_size_packets++;
                continue;
            }

            if (fwrite(payload, 1, received_size, output) != received_size) {
                perror("fwrite loopback capture");
                return -1;
            }

            result->captured_packets++;
            result->captured_bytes += (uint64_t)received_size;
        }

        if (received < batch_limit) {
            break;
        }
    }

    return result->captured_packets - captured_before;
}

int fpga_loopback_capture_run(
    const char *iface_name,
    const char *fpga_ip,
    int fpga_data_port,
    int local_port,
    int timeout_ms,
    int packet_count,
    int payload_size,
    const char *output_filename,
    fpga_loopback_capture_result_t *result
)
{
    int rx_sock = -1;
    FILE *output = NULL;
    char *file_buffer = NULL;
    uint8_t *tx_payload = NULL;
    uint8_t *batch_buffer = NULL;
    struct mmsghdr *messages = NULL;
    struct iovec *iovecs = NULL;
    struct sockaddr_in *source_addrs = NULL;
    struct sockaddr_in destination;
    struct in_addr expected_addr;
    struct pollfd poll_fd;
    uint16_t sequence = 0;
    uint64_t start_ns;
    uint64_t last_valid_ns;
    uint64_t packet_interval_ns;
    int burst_packets;
    int ret = -1;
    size_t stride;

    if (iface_name == NULL ||
        fpga_ip == NULL ||
        output_filename == NULL ||
        result == NULL ||
        packet_count <= 0 ||
        payload_size <= 0 ||
        (payload_size % 2) != 0) {
        return -1;
    }

    if (inet_pton(AF_INET, fpga_ip, &expected_addr) != 1) {
        fprintf(stderr, "Invalid FPGA IP: %s\n", fpga_ip);
        return -1;
    }

    memset(result, 0, sizeof(*result));
    result->requested_packets = packet_count;
    result->payload_size = payload_size;
    stride = (size_t)payload_size + 1U;

    tx_payload = malloc((size_t)payload_size);
    batch_buffer = malloc(CAPTURE_BATCH_SIZE * stride);
    messages = calloc(CAPTURE_BATCH_SIZE, sizeof(*messages));
    iovecs = calloc(CAPTURE_BATCH_SIZE, sizeof(*iovecs));
    source_addrs = calloc(CAPTURE_BATCH_SIZE, sizeof(*source_addrs));
    file_buffer = malloc(CAPTURE_FILE_BUFFER_SIZE);

    if (tx_payload == NULL ||
        batch_buffer == NULL ||
        messages == NULL ||
        iovecs == NULL ||
        source_addrs == NULL ||
        file_buffer == NULL) {
        fprintf(stderr, "Could not allocate loopback capture buffers\n");
        goto cleanup;
    }

    for (int index = 0; index < CAPTURE_BATCH_SIZE; index++) {
        iovecs[index].iov_base =
            batch_buffer + ((size_t)index * stride);
        iovecs[index].iov_len = stride;
        messages[index].msg_hdr.msg_iov = &iovecs[index];
        messages[index].msg_hdr.msg_iovlen = 1;
        messages[index].msg_hdr.msg_name = &source_addrs[index];
        messages[index].msg_hdr.msg_namelen = sizeof(source_addrs[index]);
    }

    output = fopen(output_filename, "wb");
    if (output == NULL) {
        perror("fopen loopback capture");
        goto cleanup;
    }
    setvbuf(output, file_buffer, _IOFBF, CAPTURE_FILE_BUFFER_SIZE);

    rx_sock = make_rx_socket(iface_name, local_port);
    if (rx_sock < 0) {
        goto cleanup;
    }

    if (build_destination(
            fpga_ip,
            fpga_data_port,
            &destination
        ) != 0) {
        goto cleanup;
    }

    poll_fd.fd = rx_sock;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;
    burst_packets = PACE_BURST_BYTES /
        (payload_size + ETHERNET_OVERHEAD_BYTES);
    if (burst_packets < 1) {
        burst_packets = 1;
    }
    packet_interval_ns =
        ((uint64_t)(payload_size + ETHERNET_OVERHEAD_BYTES) * 8ULL *
         1000000000ULL) /
        LINK_BITS_PER_SECOND;

    printf("Starting sequential loopback capture\n");
    printf("  interface:    %s\n", iface_name);
    printf("  packets:      %d\n", packet_count);
    printf("  payload:      %d bytes\n", payload_size);
    printf("  data port:    %d\n", fpga_data_port);
    printf("  local port:   %d\n", local_port);
    printf("  burst:        %d packets\n", burst_packets);

    start_ns = now_ns();
    last_valid_ns = start_ns;

    while (result->sent_packets < packet_count) {
        int burst_end = result->sent_packets + burst_packets;

        if (burst_end > packet_count) {
            burst_end = packet_count;
        }

        while (result->sent_packets < burst_end) {
            ssize_t sent;
            uint64_t send_deadline_ns;

            fill_sequential_payload(
                tx_payload,
                payload_size,
                &sequence
            );

            send_deadline_ns =
                now_ns() + ((uint64_t)timeout_ms * 1000000ULL);

            while (1) {
                sent = sendto(
                    rx_sock,
                    tx_payload,
                    (size_t)payload_size,
                    0,
                    (struct sockaddr *)&destination,
                    sizeof(destination)
                );

                if (sent == payload_size) {
                    result->sent_packets++;
                    break;
                }

                if (sent < 0 &&
                    (errno == EAGAIN ||
                     errno == EWOULDBLOCK ||
                     errno == ENOBUFS)) {
                    struct pollfd writable;
                    int poll_result;

                    if (receive_batches(
                            rx_sock,
                            &expected_addr,
                            payload_size,
                            packet_count,
                            batch_buffer,
                            messages,
                            source_addrs,
                            output,
                            result
                        ) < 0) {
                        goto cleanup;
                    }

                    if (now_ns() >= send_deadline_ns) {
                        fprintf(
                            stderr,
                            "Timed out waiting for UDP send buffer\n"
                        );
                        result->send_errors++;
                        goto cleanup;
                    }

                    writable.fd = rx_sock;
                    writable.events = POLLOUT;
                    writable.revents = 0;
                    poll_result = poll(&writable, 1, 10);

                    if (poll_result < 0 && errno != EINTR) {
                        perror("poll loopback capture TX");
                        result->send_errors++;
                        goto cleanup;
                    }

                    continue;
                }

                result->send_errors++;
                if (sent < 0) {
                    perror("send loopback capture");
                } else {
                    fprintf(
                        stderr,
                        "Short UDP send: expected=%d actual=%zd\n",
                        payload_size,
                        sent
                    );
                }
                goto cleanup;
            }
        }

        if (receive_batches(
                rx_sock,
                &expected_addr,
                payload_size,
                packet_count,
                batch_buffer,
                messages,
                source_addrs,
                output,
                result
            ) < 0) {
            goto cleanup;
        }

        if (result->captured_packets > 0) {
            last_valid_ns = now_ns();
        }

        wait_until_ns(
            start_ns +
            ((uint64_t)result->sent_packets * packet_interval_ns)
        );
    }

    while (result->captured_packets < packet_count) {
        int poll_result;
        int received;
        uint64_t now = now_ns();
        uint64_t deadline =
            last_valid_ns + ((uint64_t)timeout_ms * 1000000ULL);

        if (now >= deadline) {
            break;
        }

        poll_result = poll(
            &poll_fd,
            1,
            (int)((deadline - now + 999999ULL) / 1000000ULL)
        );
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll loopback capture");
            goto cleanup;
        }
        if (poll_result == 0) {
            break;
        }

        received = receive_batches(
            rx_sock,
            &expected_addr,
            payload_size,
            packet_count,
            batch_buffer,
            messages,
            source_addrs,
            output,
            result
        );
        if (received < 0) {
            goto cleanup;
        }
        if (received > 0) {
            last_valid_ns = now_ns();
        }
    }

    if (fflush(output) != 0) {
        perror("fflush loopback capture");
        goto cleanup;
    }

    result->missing_packets = packet_count - result->captured_packets;
    result->elapsed_s = (double)(now_ns() - start_ns) / 1000000000.0;
    ret = result->captured_packets == packet_count ? 0 : 2;

cleanup:
    if (rx_sock >= 0) {
        close(rx_sock);
    }
    if (output != NULL && fclose(output) != 0 && ret >= 0) {
        perror("fclose loopback capture");
        ret = -1;
    }
    free(file_buffer);
    free(source_addrs);
    free(iovecs);
    free(messages);
    free(batch_buffer);
    free(tx_payload);
    return ret;
}
