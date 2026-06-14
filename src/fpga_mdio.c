#include "fpga_mdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

static void print_byte_binary(uint8_t value)
{
    int bit;

    for (bit = 7; bit >= 0; bit--) {
        printf("%d", (value >> bit) & 1);
    }
}

static int parse_u8(const char *text, uint8_t *value)
{
    char *endptr;
    unsigned long parsed;

    if (text == NULL || value == NULL) {
        return -1;
    }

    parsed = strtoul(text, &endptr, 0);

    if (*endptr != '\0' || parsed > 0xFF) {
        return -1;
    }

    *value = (uint8_t)parsed;
    return 0;
}

static int parse_u16(const char *text, uint16_t *value)
{
    char *endptr;
    unsigned long parsed;

    if (text == NULL || value == NULL) {
        return -1;
    }

    parsed = strtoul(text, &endptr, 0);

    if (*endptr != '\0' || parsed > 0xFFFF) {
        return -1;
    }

    *value = (uint16_t)parsed;
    return 0;
}

static int send_udp_payload(
    const char *fpga_ip,
    int fpga_port,
    const uint8_t *payload,
    size_t payload_len
)
{
    int sock;
    struct sockaddr_in addr;

    if (fpga_ip == NULL || payload == NULL || payload_len == 0) {
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)fpga_port);

    if (inet_pton(AF_INET, fpga_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid FPGA IP: %s\n", fpga_ip);
        close(sock);
        return -1;
    }

    if (sendto(
            sock,
            payload,
            payload_len,
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

static int make_rx_socket(int rx_port, int timeout_ms)
{
    int sock;
    struct sockaddr_in addr;
    struct timeval timeout;

    sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        perror("socket rx");
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
    addr.sin_port = htons((uint16_t)rx_port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind rx socket");
        close(sock);
        return -1;
    }

    return sock;
}

static size_t build_mdio_read_payload(
    uint8_t *payload,
    size_t payload_size,
    uint8_t phy_addr,
    uint8_t reg_addr
)
{
    size_t off = 0;

    /*
     * Payload:
     * ..mdio_rphyaddr\xPPregaddr\xRRmdio_sta\n
     *
     * Sizes:
     * "..mdio_r" = 8
     * "phyaddr"  = 7 + 1 byte phy
     * "regaddr"  = 7 + 1 byte reg
     * "mdio_sta" = 8
     * "\n"       = 1
     * Total      = 33 bytes
     */
    if (payload == NULL || payload_size < 33) {
        return 0;
    }

    memcpy(&payload[off], "..mdio_r", 8);
    off += 8;

    memcpy(&payload[off], "phyaddr", 7);
    off += 7;
    payload[off++] = phy_addr;

    memcpy(&payload[off], "regaddr", 7);
    off += 7;
    payload[off++] = reg_addr;

    memcpy(&payload[off], "mdio_sta", 8);
    off += 8;

    payload[off++] = '\n';

    return off;
}

static size_t build_mdio_write_payload(
    uint8_t *payload,
    size_t payload_size,
    uint8_t phy_addr,
    uint8_t reg_addr,
    uint16_t value
)
{
    size_t off = 0;

    /*
     * Payload:
     * ..mdio_wphyaddr\xPPregaddr\xRRmdio_d\xHH\xLLmdio_sta\n
     *
     * Sizes:
     * "..mdio_w" = 8
     * "phyaddr"  = 7 + 1 byte phy
     * "regaddr"  = 7 + 1 byte reg
     * "mdio_d"   = 6 + 2 data bytes
     * "mdio_sta" = 8
     * "\n"       = 1
     * Total      = 41 bytes
     */
    if (payload == NULL || payload_size < 41) {
        return 0;
    }

    memcpy(&payload[off], "..mdio_w", 8);
    off += 8;

    memcpy(&payload[off], "phyaddr", 7);
    off += 7;
    payload[off++] = phy_addr;

    memcpy(&payload[off], "regaddr", 7);
    off += 7;
    payload[off++] = reg_addr;

    memcpy(&payload[off], "mdio_d", 6);
    off += 6;
    payload[off++] = (uint8_t)((value >> 8) & 0xFF);
    payload[off++] = (uint8_t)(value & 0xFF);

    memcpy(&payload[off], "mdio_sta", 8);
    off += 8;

    payload[off++] = '\n';

    return off;
}

static void print_mdio_result(const fpga_mdio_read_result_t *result)
{
    if (result == NULL) {
        return;
    }

    printf("reg=0x%02X value=0x%04X bin=",
           result->reg,
           result->value);

    print_byte_binary((uint8_t)(result->value >> 8));
    printf(" ");
    print_byte_binary((uint8_t)(result->value & 0xFF));
    printf("\n");
}

int fpga_mdio_write(
    const char *fpga_ip,
    int fpga_port,
    int delay_us,
    uint8_t phy_addr,
    uint8_t reg_addr,
    uint16_t value
)
{
    uint8_t payload[128];
    size_t payload_len;

    payload_len = build_mdio_write_payload(
        payload,
        sizeof(payload),
        phy_addr,
        reg_addr,
        value
    );

    if (payload_len == 0) {
        fprintf(stderr, "Could not build MDIO write payload\n");
        return -1;
    }

    if (send_udp_payload(fpga_ip, fpga_port, payload, payload_len) != 0) {
        return -1;
    }

    printf("write phy=0x%02X reg=0x%02X value=0x%04X OK\n",
           phy_addr,
           reg_addr,
           value);

    if (delay_us > 0) {
        usleep((unsigned int)delay_us);
    }

    return 0;
}

int fpga_mdio_read(
    const char *fpga_ip,
    int fpga_port,
    int rx_port,
    int timeout_ms,
    int delay_us,
    uint8_t phy_addr,
    uint8_t reg_addr,
    fpga_mdio_read_result_t *result
)
{
    int rx_sock;
    uint8_t tx_payload[128];
    uint8_t rx_buffer[256];
    size_t tx_len;
    ssize_t n;

    if (result == NULL) {
        return -1;
    }

    rx_sock = make_rx_socket(rx_port, timeout_ms);

    if (rx_sock < 0) {
        return -1;
    }

    tx_len = build_mdio_read_payload(
        tx_payload,
        sizeof(tx_payload),
        phy_addr,
        reg_addr
    );

    if (tx_len == 0) {
        fprintf(stderr, "Could not build MDIO read payload\n");
        close(rx_sock);
        return -1;
    }

    if (send_udp_payload(fpga_ip, fpga_port, tx_payload, tx_len) != 0) {
        close(rx_sock);
        return -1;
    }

    n = recvfrom(rx_sock, rx_buffer, sizeof(rx_buffer), 0, NULL, NULL);

    close(rx_sock);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            fprintf(stderr, "Timeout waiting for MDIO read response\n");
        } else {
            perror("recvfrom");
        }

        return -1;
    }

    if (n != 3) {
        fprintf(stderr, "Invalid MDIO response size: %zd bytes, expected 3\n", n);
        return -1;
    }

    result->raw[0] = rx_buffer[0];
    result->raw[1] = rx_buffer[1];
    result->raw[2] = rx_buffer[2];

    /*
     * FPGA MDIO read response:
     * byte 0: {000, reg[4:0]}
     * byte 1: data[15:8]
     * byte 2: data[7:0]
     */
    result->reg = rx_buffer[0] & 0x1F;
    result->value = ((uint16_t)rx_buffer[1] << 8) | rx_buffer[2];

    printf("read phy=0x%02X ", phy_addr);
    print_mdio_result(result);

    if (delay_us > 0) {
        usleep((unsigned int)delay_us);
    }

    return 0;
}

static int run_single_op(
    const char *fpga_ip,
    int fpga_port,
    int rx_port,
    int timeout_ms,
    int delay_us,
    uint8_t phy_addr,
    const char *op
)
{
    char op_copy[128];
    char *type;
    char *reg_text;
    char *value_text;

    uint8_t reg;
    uint16_t value;
    fpga_mdio_read_result_t result;

    if (op == NULL) {
        return -1;
    }

    snprintf(op_copy, sizeof(op_copy), "%s", op);

    type = strtok(op_copy, ":");
    reg_text = strtok(NULL, ":");
    value_text = strtok(NULL, ":");

    if (type == NULL || reg_text == NULL) {
        fprintf(stderr, "Invalid MDIO operation: %s\n", op);
        fprintf(stderr, "Use r:<reg> or w:<reg>:<value>\n");
        return -1;
    }

    if (parse_u8(reg_text, &reg) != 0) {
        fprintf(stderr, "Invalid MDIO register: %s\n", reg_text);
        return -1;
    }

    if (strcmp(type, "r") == 0 || strcmp(type, "read") == 0) {
        return fpga_mdio_read(
            fpga_ip,
            fpga_port,
            rx_port,
            timeout_ms,
            delay_us,
            phy_addr,
            reg,
            &result
        );
    }

    if (strcmp(type, "w") == 0 || strcmp(type, "write") == 0) {
        if (value_text == NULL) {
            fprintf(stderr, "Write operation requires value: %s\n", op);
            return -1;
        }

        if (parse_u16(value_text, &value) != 0) {
            fprintf(stderr, "Invalid MDIO write value: %s\n", value_text);
            return -1;
        }

        return fpga_mdio_write(
            fpga_ip,
            fpga_port,
            delay_us,
            phy_addr,
            reg,
            value
        );
    }

    fprintf(stderr, "Unknown MDIO operation type: %s\n", type);
    return -1;
}

int fpga_mdio_run_sequence(
    const char *fpga_ip,
    int fpga_port,
    int rx_port,
    int timeout_ms,
    int delay_us,
    uint8_t phy_addr,
    int op_count,
    char **ops
)
{
    int i;

    if (fpga_ip == NULL || ops == NULL || op_count <= 0) {
        return -1;
    }

    for (i = 0; i < op_count; i++) {
        if (run_single_op(
                fpga_ip,
                fpga_port,
                rx_port,
                timeout_ms,
                delay_us,
                phy_addr,
                ops[i]
            ) != 0) {
            return -1;
        }
    }

    return 0;
}