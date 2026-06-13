#ifndef NET_STATS_H
#define NET_STATS_H

#include <stdint.h>

typedef struct {
    uint64_t forwarding;
    uint64_t default_ttl;

    uint64_t in_receives;
    uint64_t in_hdr_errors;
    uint64_t in_addr_errors;
    uint64_t forw_datagrams;
    uint64_t in_unknown_protos;
    uint64_t in_discards;
    uint64_t in_delivers;

    uint64_t out_requests;
    uint64_t out_discards;
    uint64_t out_no_routes;

    uint64_t reasm_timeout;
    uint64_t reasm_reqds;
    uint64_t reasm_oks;
    uint64_t reasm_fails;

    uint64_t frag_oks;
    uint64_t frag_fails;
    uint64_t frag_creates;
} ip_stats_t;

typedef struct {
    uint64_t in_datagrams;
    uint64_t no_ports;
    uint64_t in_errors;
    uint64_t out_datagrams;

    uint64_t rcvbuf_errors;
    uint64_t sndbuf_errors;
    uint64_t in_csum_errors;
    uint64_t ignored_multi;
    uint64_t mem_errors;
} udp_stats_t;

typedef struct {
    ip_stats_t ip;
    udp_stats_t udp;
} net_stats_t;

int read_net_stats(net_stats_t *stats);
void print_net_stats(const net_stats_t *stats);
int append_net_stats_csv(const char *filename, const net_stats_t *stats);

#endif // NET_STATS_H