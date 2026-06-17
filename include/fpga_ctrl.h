#ifndef FPGA_CTRL_H
#define FPGA_CTRL_H

#include "config.h"

#include <stdint.h>

int fpga_ctrl_set_gateway_ip(const char *fpga_ip, int fpga_port, const char *ip_text);
int fpga_ctrl_set_source_ip(const char *fpga_ip, int fpga_port, const char *ip_text);
int fpga_ctrl_set_dest_ip(const char *fpga_ip, int fpga_port, const char *ip_text);
int fpga_ctrl_set_subnet_mask(const char *fpga_ip, int fpga_port, const char *ip_text);

int fpga_ctrl_set_src_port(const char *fpga_ip, int fpga_port, uint16_t udp_port);
int fpga_ctrl_set_dst_port(const char *fpga_ip, int fpga_port, uint16_t udp_port);

int fpga_ctrl_enable_loopback(const char *fpga_ip, int fpga_port);
int fpga_ctrl_send_trigger(const char *fpga_ip, int fpga_port);
int fpga_ctrl_enable_random(const char *fpga_ip, int fpga_port);
int fpga_ctrl_enable_constant(const char *fpga_ip, int fpga_port);
int fpga_ctrl_enable_flood(const char *fpga_ip, int fpga_port);

int fpga_ctrl_set_udp_mtu(const char *fpga_ip, int fpga_port, uint16_t mtu);
int fpga_ctrl_set_packet_count(const char *fpga_ip, int fpga_port, uint32_t packet_count);

int fpga_ctrl_set_content_mode(const char *fpga_ip, int fpga_port, const char *mode);

#endif