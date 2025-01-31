/*********************************************************************
 *                     openNetVM
 *              https://sdnfv.github.io
 *
 *   BSD LICENSE
 *
 *   Copyright(c)
 *            2015-2019 George Washington University
 *            2015-2019 University of California Riverside
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * The name of the author may not be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ndpi_stats.c - an example using onvm, nDPI. Inspect packets using nDPI
 ********************************************************************/

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <unistd.h>

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_atomic.h>

#include <pcap/pcap.h>
#include "ndpi_main.h"
#include "ndpi_util.h"

#include "onvm_flow_table.h"
#include "onvm_nflib.h"
#include "onvm_pkt_helper.h"

#define NF_TAG "ndpi_stat"
#define TICK_RESOLUTION 1000

#define PKTMBUF_POOL_NAME "MProc_pktmbuf_pool"
#define PKT_READ_SIZE ((uint16_t)32)
#define LOCAL_EXPERIMENTAL_ETHER 0x88B5
#define DEFAULT_PKT_NUM 128
#define MAX_PKT_NUM NF_QUEUE_RINGSIZE
#define DEFAULT_NUM_CHILDREN 1

/* shared data structure containing host port info */
extern struct port_info *ports;

/* user defined settings */
static uint32_t destination = (uint16_t)-1;
static uint16_t num_children = DEFAULT_NUM_CHILDREN;
static uint8_t use_shared_core_allocation = 0;

static uint8_t d_addr_bytes[RTE_ETHER_ADDR_LEN];
static uint16_t packet_size = RTE_ETHER_HDR_LEN;
static uint32_t packet_number = DEFAULT_PKT_NUM;

/* pcap stucts */
const uint16_t MAX_SNAPLEN = (uint16_t)-1;
pcap_t *pd;

/* nDPI structs */
struct ndpi_detection_module_struct *module;
struct ndpi_workflow *workflow;
uint32_t current_ndpi_memory = 0, max_ndpi_memory = 0;
static u_int8_t quiet_mode = 0;
static u_int16_t decode_tunnels = 0;
FILE *csv_fp = NULL;
static FILE *results_file = NULL;
static struct timeval begin, end;

/* missing parameters */
int nDPI_LogLevel = 0;
char *_debug_protocols = NULL;
u_int8_t enable_protocol_guess = 1, enable_payload_analyzer = 0;
u_int8_t enable_joy_stats = 0;
u_int8_t human_readeable_string_len = 5;
u_int8_t max_num_udp_dissected_pkts = 16 /* 8 is enough for most protocols, Signal requires more */, max_num_tcp_dissected_pkts = 80 /* due to telnet */;

/* For advanced rings scaling */
rte_atomic16_t signal_exit_flag;
uint8_t ONVM_NF_SHARE_CORES;
struct child_spawn_info {
        struct onvm_nf_init_cfg *child_cfg;
        struct onvm_nf *parent;
};

void nf_setup(struct onvm_nf_local_ctx *nf_local_ctx);
void sig_handler(int sig);
void *start_child(void *arg);
int thread_main_loop(struct onvm_nf_local_ctx *nf_local_ctx);

static void run_advanced_rings(int argc, char *argv[]);
static void run_default_nflib_mode(int argc, char *argv[]);

/* nDPI methods */
void
setup_ndpi(void);
char *
formatTraffic(float numBits, int bits, char *buf);
char *
formatPackets(float numPkts, char *buf);
static void
node_proto_guess_walker(const void *node, ndpi_VISIT which, int depth, void *user_data);
static void
print_results(void);

/**
 * Source https://github.com/ntop/nDPI ndpiReader.c
 * @brief Traffic stats format
 */
char *
formatTraffic(float numBits, int bits, char *buf) {
        char unit;

        if (bits)
                unit = 'b';
        else
                unit = 'B';

        if (numBits < 1024) {
                snprintf(buf, 32, "%lu %c", (unsigned long)numBits, unit);
        } else if (numBits < (1024 * 1024)) {
                snprintf(buf, 32, "%.2f K%c", (float)(numBits) / 1024, unit);
        } else {
                float tmpMBits = ((float)numBits) / (1024 * 1024);

                if (tmpMBits < 1024) {
                        snprintf(buf, 32, "%.2f M%c", tmpMBits, unit);
                } else {
                        tmpMBits /= 1024;

                        if (tmpMBits < 1024) {
                                snprintf(buf, 32, "%.2f G%c", tmpMBits, unit);
                        } else {
                                snprintf(buf, 32, "%.2f T%c", (float)(tmpMBits) / 1024, unit);
                        }
                }
        }

        return (buf);
}

/**
 * Source https://github.com/ntop/nDPI ndpiReader.c
 * @brief Packets stats format
 */
char *
formatPackets(float numPkts, char *buf) {
        if (numPkts < 1000) {
                snprintf(buf, 32, "%.2f", numPkts);
        } else if (numPkts < (1000 * 1000)) {
                snprintf(buf, 32, "%.2f K", numPkts / 1000);
        } else {
                numPkts /= (1000 * 1000);
                snprintf(buf, 32, "%.2f M", numPkts);
        }

        return (buf);
}

/*
 * Print a usage message
 */
static void
usage(const char *progname) {
        printf("Usage:\n");
        printf("%s [EAL args] -- [NF_LIB args] -- -d <destination_nf> -w <output_file>\n", progname);
        printf("%s -F <CONFIG_FILE.json> [EAL args] -- [NF_LIB args] -- [NF args]\n\n", progname);
        printf("Flags:\n");
        printf(" - `-w <file_name>`: result file name to write to.\n");
        printf(" - `-d <nf_id>`: OPTIONAL destination NF to send packets to\n");
}

/*
 * Parse the application arguments.
 */
static int
parse_app_args(int argc, char *argv[], const char *progname) {
        int c;

        while ((c = getopt(argc, argv, "d:w:")) != -1) {
                switch (c) {
                        case 'w':
                                results_file = fopen(strdup(optarg), "w");
                                if (results_file == NULL) {
                                        RTE_LOG(INFO, APP, "Error in opening result file\n");
                                        return -1;
                                }
                                break;
                        case 'd':
                                destination = strtoul(optarg, NULL, 10);
                                RTE_LOG(INFO, APP, "destination nf = %d\n", destination);
                                break;
                        case '?':
                                usage(progname);
                                if (optopt == 'p')
                                        RTE_LOG(INFO, APP, "Option -%c requires an argument.\n", optopt);
                                else if (isprint(optopt))
                                        RTE_LOG(INFO, APP, "Unknown option `-%c'.\n", optopt);
                                else
                                        RTE_LOG(INFO, APP, "Unknown option character `\\x%x'.\n", optopt);
                                return -1;
                        default:
                                usage(progname);
                                return -1;
                }
        }

        return optind;
}

void
setup_ndpi(void) {
        pd = pcap_open_dead(DLT_EN10MB, MAX_SNAPLEN);

        NDPI_PROTOCOL_BITMASK all;
        struct ndpi_workflow_prefs prefs;

        memset(&prefs, 0, sizeof(prefs));
        prefs.decode_tunnels = decode_tunnels;
        prefs.num_roots = NUM_ROOTS;
        prefs.max_ndpi_flows = MAX_NDPI_FLOWS;
        prefs.quiet_mode = quiet_mode;

        workflow = ndpi_workflow_init(&prefs, pd);

        NDPI_BITMASK_SET_ALL(all);
        ndpi_set_protocol_detection_bitmask2(workflow->ndpi_struct, &all);

        memset(workflow->stats.protocol_counter, 0, sizeof(workflow->stats.protocol_counter));
        memset(workflow->stats.protocol_counter_bytes, 0, sizeof(workflow->stats.protocol_counter_bytes));
        memset(workflow->stats.protocol_flows, 0, sizeof(workflow->stats.protocol_flows));
}

/*
 * Source https://github.com/ntop/nDPI ndpiReader.c
 * Modified for single workflow
 */
static void
node_proto_guess_walker(const void *node, ndpi_VISIT which, int depth, void *user_data) {
        struct ndpi_flow_info *flow = *(struct ndpi_flow_info **)node;
        u_int16_t thread_id = *((u_int16_t *)user_data);
        u_int8_t proto_guessed;

        if ((which == ndpi_preorder) || (which == ndpi_leaf)) { /* Avoid walking the same node multiple times */
                if ((!flow->detection_completed) && flow->ndpi_flow)
                        flow->detected_protocol = ndpi_detection_giveup(workflow->ndpi_struct, flow->ndpi_flow, enable_protocol_guess, &proto_guessed);

                process_ndpi_collected_info(workflow, flow, csv_fp);
                workflow->stats.protocol_counter[flow->detected_protocol.app_protocol] +=
                    flow->src2dst_packets + flow->dst2src_packets;
                workflow->stats.protocol_counter_bytes[flow->detected_protocol.app_protocol] +=
                    flow->src2dst_bytes + flow->dst2src_bytes;
                workflow->stats.protocol_flows[flow->detected_protocol.app_protocol]++;
        }
}

/*
 * Source https://github.com/ntop/nDPI ndpiReader.c
 * Simplified nDPI reader result output for single workflow
 */
static void
print_results(void) {
        u_int32_t i;
        u_int32_t avg_pkt_size = 0;
        u_int64_t tot_usec;

        if (workflow->stats.total_wire_bytes == 0) {
                return;
        }

        for (i = 0; i < NUM_ROOTS; i++) {
                ndpi_twalk(workflow->ndpi_flows_root[i], node_proto_guess_walker, 0);
        }

        tot_usec = end.tv_sec * 1000000 + end.tv_usec - (begin.tv_sec * 1000000 + begin.tv_usec);

        printf("\nTraffic statistics:\n");
        printf("\tEthernet bytes:        %-13llu (includes ethernet CRC/IFC/trailer)\n",
               (long long unsigned int)workflow->stats.total_wire_bytes);
        printf("\tDiscarded bytes:       %-13llu\n", (long long unsigned int)workflow->stats.total_discarded_bytes);
        printf("\tIP packets:            %-13llu of %llu packets total\n",
               (long long unsigned int)workflow->stats.ip_packet_count,
               (long long unsigned int)workflow->stats.raw_packet_count);
        /* In order to prevent Floating point exception in case of no traffic*/
        if (workflow->stats.total_ip_bytes && workflow->stats.raw_packet_count)
                avg_pkt_size = (unsigned int)(workflow->stats.total_ip_bytes / workflow->stats.raw_packet_count);
        printf("\tIP bytes:              %-13llu (avg pkt size %u bytes)\n",
               (long long unsigned int)workflow->stats.total_ip_bytes, avg_pkt_size);
        printf("\tUnique flows:          %-13u\n", workflow->stats.ndpi_flow_count);

        printf("\tTCP Packets:           %-13lu\n", (unsigned long)workflow->stats.tcp_count);
        printf("\tUDP Packets:           %-13lu\n", (unsigned long)workflow->stats.udp_count);
        printf("\tVLAN Packets:          %-13lu\n", (unsigned long)workflow->stats.vlan_count);
        printf("\tMPLS Packets:          %-13lu\n", (unsigned long)workflow->stats.mpls_count);
        printf("\tPPPoE Packets:         %-13lu\n", (unsigned long)workflow->stats.pppoe_count);
        printf("\tFragmented Packets:    %-13lu\n", (unsigned long)workflow->stats.fragmented_count);
        printf("\tMax Packet size:       %-13u\n", workflow->stats.max_packet_len);
        printf("\tPacket Len < 64:       %-13lu\n", (unsigned long)workflow->stats.packet_len[0]);
        printf("\tPacket Len 64-128:     %-13lu\n", (unsigned long)workflow->stats.packet_len[1]);
        printf("\tPacket Len 128-256:    %-13lu\n", (unsigned long)workflow->stats.packet_len[2]);
        printf("\tPacket Len 256-1024:   %-13lu\n", (unsigned long)workflow->stats.packet_len[3]);
        printf("\tPacket Len 1024-1500:  %-13lu\n", (unsigned long)workflow->stats.packet_len[4]);
        printf("\tPacket Len > 1500:     %-13lu\n", (unsigned long)workflow->stats.packet_len[5]);

        if (tot_usec > 0) {
                char buf[32], buf1[32], when[64];
                float t = (float)(workflow->stats.ip_packet_count * 1000000) / (float)tot_usec;
                float b = (float)(workflow->stats.total_wire_bytes * 8 * 1000000) / (float)tot_usec;
                float traffic_duration;
                /* This currently assumes traffic starts to flow instantly */
                traffic_duration = tot_usec;
                printf("\tnDPI throughput:       %s pps / %s/sec\n", formatPackets(t, buf), formatTraffic(b, 1, buf1));
                t = (float)(workflow->stats.ip_packet_count * 1000000) / (float)traffic_duration;
                b = (float)(workflow->stats.total_wire_bytes * 8 * 1000000) / (float)traffic_duration;

                strftime(when, sizeof(when), "%d/%b/%Y %H:%M:%S", localtime(&begin.tv_sec));
                printf("\tAnalysis begin:        %s\n", when);
                strftime(when, sizeof(when), "%d/%b/%Y %H:%M:%S", localtime(&end.tv_sec));
                printf("\tAnalysis end:          %s\n", when);
                printf("\tTraffic throughput:    %s pps / %s/sec\n", formatPackets(t, buf), formatTraffic(b, 1, buf1));
                printf("\tTraffic duration:      %.3f sec\n", traffic_duration / 1000000);
        }

        for (i = 0; i <= ndpi_get_num_supported_protocols(workflow->ndpi_struct); i++) {
                if (workflow->stats.protocol_counter[i] > 0) {
                        if (results_file)
                                fprintf(results_file, "%s\t%llu\t%llu\t%u\n",
                                        ndpi_get_proto_name(workflow->ndpi_struct, i),
                                        (long long unsigned int)workflow->stats.protocol_counter[i],
                                        (long long unsigned int)workflow->stats.protocol_counter_bytes[i],
                                        workflow->stats.protocol_flows[i]);
                        printf(
                            "\t%-20s packets: %-13llu bytes: %-13llu "
                            "flows: %-13u\n",
                            ndpi_get_proto_name(workflow->ndpi_struct, i),
                            (long long unsigned int)workflow->stats.protocol_counter[i],
                            (long long unsigned int)workflow->stats.protocol_counter_bytes[i],
                            workflow->stats.protocol_flows[i]);
                }
        }

}

static int
packet_handler(struct rte_mbuf *pkt, struct onvm_pkt_meta *meta,
               __attribute__((unused)) struct onvm_nf_local_ctx *nf_local_ctx) {
        struct pcap_pkthdr pkt_hdr;
        struct timeval time;
        u_char *packet;
        ndpi_protocol prot;

        time.tv_usec = pkt->udata64;
        time.tv_sec = pkt->tx_offload;
        pkt_hdr.ts = time;
        pkt_hdr.caplen = rte_pktmbuf_data_len(pkt);
        pkt_hdr.len = rte_pktmbuf_data_len(pkt);
        packet = rte_pktmbuf_mtod(pkt, u_char *);

        prot = ndpi_workflow_process_packet(workflow, &pkt_hdr, packet, csv_fp);
        workflow->stats.protocol_counter[prot.app_protocol]++;
        workflow->stats.protocol_counter_bytes[prot.app_protocol] += pkt_hdr.len;

        if (destination != (uint16_t)-1) {
                meta->action = ONVM_NF_ACTION_TONF;
                meta->destination = destination;
        } else {
                meta->action = ONVM_NF_ACTION_OUT;
                meta->destination = pkt->port;

                if (onvm_pkt_swap_src_mac_addr(pkt, meta->destination, ports) != 0) {
                        RTE_LOG(INFO, APP, "ERROR: Failed to swap src mac with dst mac!\n");
                }
        }
        return 0;
}

void
nf_setup(__attribute__((unused)) struct onvm_nf_local_ctx *nf_local_ctx) {
        uint32_t i;
        struct rte_mempool *pktmbuf_pool;

        /* ndpi init */
        setup_ndpi();
        pktmbuf_pool = rte_mempool_lookup(PKTMBUF_POOL_NAME);
        if (pktmbuf_pool == NULL) {
                onvm_nflib_stop(nf_local_ctx);
                rte_exit(EXIT_FAILURE, "Cannot find mbuf pool!\n");
        }

        for (i = 0; i < packet_number; ++i) {
                struct onvm_pkt_meta *pmeta;
                struct rte_ether_hdr *ehdr;
                int j;

                struct rte_mbuf *pkt = rte_pktmbuf_alloc(pktmbuf_pool);
                if (pkt == NULL)
                        break;

                /* set up ether header and set new packet size */
                ehdr = (struct rte_ether_hdr *)rte_pktmbuf_append(pkt, packet_size);

                /* Using manager mac addr for source*/
                if (onvm_get_macaddr(0, &ehdr->s_addr) == -1) {
                        onvm_get_fake_macaddr(&ehdr->s_addr);
                }
                for (j = 0; j < RTE_ETHER_ADDR_LEN; ++j) {
                        ehdr->d_addr.addr_bytes[j] = d_addr_bytes[j];
                }
                ehdr->ether_type = LOCAL_EXPERIMENTAL_ETHER;

                pmeta = onvm_get_pkt_meta(pkt);
                pmeta->destination = destination;
                pmeta->action = ONVM_NF_ACTION_TONF;
                pkt->hash.rss = i;
                pkt->port = 0;

                onvm_nflib_return_pkt(nf_local_ctx->nf, pkt);
        }
}

/* Basic packet handler, just forwards all packets to destination */
static int
packet_handler_fwd(struct rte_mbuf *pkt, struct onvm_pkt_meta *meta,
                   __attribute__((unused)) struct onvm_nf_local_ctx *nf_local_ctx) {
        (void)pkt;
        meta->destination = destination;
        meta->action = ONVM_NF_ACTION_TONF;

        return 0;
}

void *
start_child(void *arg) {
        struct onvm_nf_local_ctx *child_local_ctx;
        struct onvm_nf_init_cfg *child_init_cfg;
        struct onvm_nf *parent;
        struct child_spawn_info *spawn_info;

        spawn_info = (struct child_spawn_info *)arg;
        child_init_cfg = spawn_info->child_cfg;
        parent = spawn_info->parent;
        child_local_ctx = onvm_nflib_init_nf_local_ctx();

        if (onvm_nflib_start_nf(child_local_ctx, child_init_cfg) < 0) {
                printf("Failed to spawn child NF\n");
                return NULL;
        }

        /* Keep track of parent for proper termination */
        child_local_ctx->nf->thread_info.parent = parent->instance_id;

        thread_main_loop(child_local_ctx);
        onvm_nflib_stop(child_local_ctx);
        free(spawn_info);
        return NULL;
}

int
thread_main_loop(struct onvm_nf_local_ctx *nf_local_ctx) {
        void *pkts[PKT_READ_SIZE];
        struct onvm_pkt_meta *meta;
        uint16_t i, nb_pkts;
        struct rte_mbuf *pktsTX[PKT_READ_SIZE];
        int tx_batch_size;
        struct rte_ring *rx_ring;
        struct rte_ring *msg_q;
        struct onvm_nf *nf;
        struct onvm_nf_msg *msg;
        struct rte_mempool *nf_msg_pool;

        nf = nf_local_ctx->nf;

        onvm_nflib_nf_ready(nf);
        nf_setup(nf_local_ctx);

        /* Get rings from nflib */
        rx_ring = nf->rx_q;
        msg_q = nf->msg_q;
        nf_msg_pool = rte_mempool_lookup(_NF_MSG_POOL_NAME);

        printf("Process %d handling packets using advanced rings\n", nf->instance_id);
        if (onvm_threading_core_affinitize(nf->thread_info.core) < 0)
                rte_exit(EXIT_FAILURE, "Failed to affinitize to core %d\n", nf->thread_info.core);

        while (!rte_atomic16_read(&signal_exit_flag)) {
                /* Check for a stop message from the manager */
                if (unlikely(rte_ring_count(msg_q) > 0)) {
                        msg = NULL;
                        rte_ring_dequeue(msg_q, (void **)(&msg));
                        if (msg->msg_type == MSG_STOP) {
                                rte_atomic16_set(&signal_exit_flag, 1);
                        } else {
                                printf("Received message %d, ignoring", msg->msg_type);
                        }
                        rte_mempool_put(nf_msg_pool, (void *)msg);
                }

                tx_batch_size = 0;
                /* Dequeue all packets in ring up to max possible */
                nb_pkts = rte_ring_dequeue_burst(rx_ring, pkts, PKT_READ_SIZE, NULL);

                if (unlikely(nb_pkts == 0)) {
                        if (ONVM_NF_SHARE_CORES) {
                                rte_atomic16_set(nf->shared_core.sleep_state, 1);
                                sem_wait(nf->shared_core.nf_mutex);
                        }
                        continue;
                }
                /* Process all the packets */
                for (i = 0; i < nb_pkts; i++) {
                        meta = onvm_get_pkt_meta((struct rte_mbuf *)pkts[i]);
                        packet_handler_fwd((struct rte_mbuf *)pkts[i], meta, nf_local_ctx);
                        pktsTX[tx_batch_size++] = pkts[i];
                }
                /* Process all packet actions */
                onvm_pkt_process_tx_batch(nf->nf_tx_mgr, pktsTX, tx_batch_size, nf);
                if (tx_batch_size < PACKET_READ_SIZE) {
                        onvm_pkt_flush_all_nfs(nf->nf_tx_mgr, nf);
                }
        }
        return 0;
}

void sig_handler(int sig) {
        if (sig != SIGINT && sig != SIGTERM)
                return;

        /* Will stop the processing for all spawned threads in advanced rings mode */
        rte_atomic16_set(&signal_exit_flag, 1);
}

static void
run_advanced_rings(int argc, char *argv[]) {
		pthread_t nf_thread[num_children];
        struct onvm_configuration *onvm_config;
        struct onvm_nf_local_ctx *nf_local_ctx;
        struct onvm_nf_function_table *nf_function_table;
        struct onvm_nf *nf;
        const char *progname = argv[0];
        int arg_offset, i;

        nf_local_ctx = onvm_nflib_init_nf_local_ctx();
         /* If we're using advanced rings also pass a custom cleanup function,
         * this can be used to handle NF specific (non onvm) cleanup logic */
        rte_atomic16_init(&signal_exit_flag);
        rte_atomic16_set(&signal_exit_flag, 0);
        onvm_nflib_start_signal_handler(nf_local_ctx, sig_handler);
        /* No need to define a function table as adv rings won't run onvm_nflib_run */
        nf_function_table = NULL;

        if ((arg_offset = onvm_nflib_init(argc, argv, NF_TAG, nf_local_ctx, nf_function_table)) < 0) {
                onvm_nflib_stop(nf_local_ctx);
                if (arg_offset == ONVM_SIGNAL_TERMINATION) {
                        printf("Exiting due to user termination\n");
                        return;
                } else {
                        rte_exit(EXIT_FAILURE, "Failed ONVM init\n");
                }
        }

        argc -= arg_offset;
        argv += arg_offset;

        if (parse_app_args(argc, argv, progname) < 0) {
                onvm_nflib_stop(nf_local_ctx);
                rte_exit(EXIT_FAILURE, "Invalid command-line arguments\n");
        }

        nf = nf_local_ctx->nf;
        onvm_config = onvm_nflib_get_onvm_config();
        ONVM_NF_SHARE_CORES = onvm_config->flags.ONVM_NF_SHARE_CORES;

        for (i = 0; i < num_children; i++) {
                struct onvm_nf_init_cfg *child_cfg;
                child_cfg = onvm_nflib_init_nf_init_cfg(nf->tag);
                /* Prepare init data for the child */
                child_cfg->service_id = nf->service_id;
                struct child_spawn_info *child_data = malloc(sizeof(struct child_spawn_info));
                child_data->child_cfg = child_cfg;
                child_data->parent = nf;
                /* Increment the children count so that stats are displayed and NF does proper cleanup */
                rte_atomic16_inc(&nf->thread_info.children_cnt);
                pthread_create(&nf_thread[i], NULL, start_child, (void *)child_data);
        }
        
        thread_main_loop(nf_local_ctx);

        if (!pd)
                pcap_close(pd);
        if (results_file)
                fclose(results_file);

        onvm_nflib_stop(nf_local_ctx);

        for (i = 0; i < num_children; i++) {
                pthread_join(nf_thread[i], NULL);
        }
}


int
main(int argc, char *argv[]) {

		printf("\nRUNNING ADVANCED RINGS EXPERIMENT\n");
        run_advanced_rings(argc, argv);

        print_results();
        printf("If we reach here, program is ending\n");
        return 0;
}
