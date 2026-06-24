#include "fpga_ctrl.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define FPGA_CTRL_MAX_PAYLOAD 64

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

static int send_simple_command(
    const char *fpga_ip,
    int fpga_port,
    const char *cmd
)
{
    uint8_t payload[FPGA_CTRL_MAX_PAYLOAD];
    size_t len;

    if (cmd == NULL) {
        return -1;
    }

    len = strlen(cmd);

    if (len + 1 > sizeof(payload)) {
        fprintf(stderr, "Command too large\n");
        return -1;
    }

    memcpy(payload, cmd, len);
    payload[len++] = '\n';

    return send_udp_payload(fpga_ip, fpga_port, payload, len);
}

static int ipv4_to_bytes(const char *ip_text, uint8_t ip_bytes[4])
{
    struct in_addr addr;

    if (ip_text == NULL || ip_bytes == NULL) {
        return -1;
    }

    if (inet_pton(AF_INET, ip_text, &addr) != 1) {
        fprintf(stderr, "Invalid IPv4 address: %s\n", ip_text);
        return -1;
    }

    /*
     * inet_pton stores IPv4 address in network byte order.
     * That is exactly big-endian byte order.
     */
    memcpy(ip_bytes, &addr.s_addr, 4);

    return 0;
}

static int send_ip_command(
    const char *fpga_ip,
    int fpga_port,
    const char *cmd,
    const char *ip_text
)
{
    uint8_t payload[16];
    uint8_t ip_bytes[4];
    size_t off = 0;
    size_t cmd_len;

    if (cmd == NULL || ip_text == NULL) {
        return -1;
    }

    cmd_len = strlen(cmd);

    if (cmd_len + 4 + 1 > sizeof(payload)) {
        return -1;
    }

    if (ipv4_to_bytes(ip_text, ip_bytes) != 0) {
        return -1;
    }

    memcpy(&payload[off], cmd, cmd_len);
    off += cmd_len;

    memcpy(&payload[off], ip_bytes, 4);
    off += 4;

    payload[off++] = '\n';

    return send_udp_payload(fpga_ip, fpga_port, payload, off);
}

static int send_u16_command(
    const char *fpga_ip,
    int fpga_port,
    const char *cmd,
    uint16_t value
)
{
    uint8_t payload[16];
    size_t off = 0;
    size_t cmd_len;

    if (cmd == NULL) {
        return -1;
    }

    cmd_len = strlen(cmd);

    if (cmd_len + 2 + 1 > sizeof(payload)) {
        return -1;
    }

    memcpy(&payload[off], cmd, cmd_len);
    off += cmd_len;

    payload[off++] = (uint8_t)((value >> 8) & 0xFF);
    payload[off++] = (uint8_t)(value & 0xFF);

    payload[off++] = '\n';

    return send_udp_payload(fpga_ip, fpga_port, payload, off);
}

static int send_u32_command(
    const char *fpga_ip,
    int fpga_port,
    const char *cmd,
    uint32_t value
)
{
    uint8_t payload[16];
    size_t off = 0;
    size_t cmd_len;

    if (cmd == NULL) {
        return -1;
    }

    cmd_len = strlen(cmd);

    if (cmd_len + 4 + 1 > sizeof(payload)) {
        return -1;
    }

    memcpy(&payload[off], cmd, cmd_len);
    off += cmd_len;

    payload[off++] = (uint8_t)((value >> 24) & 0xFF);
    payload[off++] = (uint8_t)((value >> 16) & 0xFF);
    payload[off++] = (uint8_t)((value >> 8) & 0xFF);
    payload[off++] = (uint8_t)(value & 0xFF);

    payload[off++] = '\n';

    return send_udp_payload(fpga_ip, fpga_port, payload, off);
}

int fpga_ctrl_set_gateway_ip(const char *fpga_ip, int fpga_port, const char *ip_text)
{
    return send_ip_command(fpga_ip, fpga_port, "ip_g", ip_text);
}

int fpga_ctrl_set_source_ip(const char *fpga_ip, int fpga_port, const char *ip_text)
{
    return send_ip_command(fpga_ip, fpga_port, "ip_s", ip_text);
}

int fpga_ctrl_set_dest_ip(const char *fpga_ip, int fpga_port, const char *ip_text)
{
    return send_ip_command(fpga_ip, fpga_port, "ip_d", ip_text);
}

int fpga_ctrl_set_subnet_mask(const char *fpga_ip, int fpga_port, const char *ip_text)
{
    return send_ip_command(fpga_ip, fpga_port, "subm", ip_text);
}

int fpga_ctrl_set_src_port(const char *fpga_ip, int fpga_port, uint16_t udp_port)
{
    return send_u16_command(fpga_ip, fpga_port, "srport", udp_port);
}

int fpga_ctrl_set_dst_port(const char *fpga_ip, int fpga_port, uint16_t udp_port)
{
    return send_u16_command(fpga_ip, fpga_port, "dsport", udp_port);
}

int fpga_ctrl_enable_loopback(const char *fpga_ip, int fpga_port)
{
    return send_simple_command(fpga_ip, fpga_port, "loopback");
}

int fpga_ctrl_send_trigger(const char *fpga_ip, int fpga_port)
{
    return send_simple_command(fpga_ip, fpga_port, ".trigger");
}

int fpga_ctrl_enable_random(const char *fpga_ip, int fpga_port)
{
    return send_simple_command(fpga_ip, fpga_port, "..random");
}

int fpga_ctrl_enable_constant(const char *fpga_ip, int fpga_port)
{
    return send_simple_command(fpga_ip, fpga_port, "constant");
}

int fpga_ctrl_set_content_mode(
    const char *fpga_ip,
    int fpga_port,
    const char *mode
)
{
    if (mode == NULL) {
        return -1;
    }

    if (strcmp(mode, "random") == 0) {
        return fpga_ctrl_enable_random(fpga_ip, fpga_port);
    }

    if (strcmp(mode, "constant") == 0) {
        return fpga_ctrl_enable_constant(fpga_ip, fpga_port);
    }

    if (strcmp(mode, "sequential") == 0) {
        return 0;
    }

    fprintf(stderr, "Unknown FPGA content mode: %s\n", mode);
    fprintf(stderr, "Use: random, constant, sequential\n");

    return -1;
}

int fpga_ctrl_enable_flood(const char *fpga_ip, int fpga_port)
{
    return send_simple_command(fpga_ip, fpga_port, "...flood");
}

int fpga_ctrl_set_udp_mtu(const char *fpga_ip, int fpga_port, uint16_t mtu)
{
    return send_u16_command(fpga_ip, fpga_port, "udpmtu", mtu);
}

int fpga_ctrl_set_packet_count(const char *fpga_ip, int fpga_port, uint32_t packet_count)
{
    return send_u32_command(fpga_ip, fpga_port, "pktn", packet_count);
}