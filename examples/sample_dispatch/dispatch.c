#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <unistd.h>
#include <time.h>

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>

#include "onvm_nflib.h"
#include "onvm_pkt_helper.h"

#define NF_TAG "dispatch"

/* number of package between each print */
static uint32_t print_delay = 1000000;
const uint8_t arp_response = 2;

static uint64_t last_cycle;
static uint64_t cur_cycles;

/* shared data structure containing host port info */
extern struct port_info *ports;
extern struct onvm_mutex *parallel_mutex ;
/*
 * Print a usage message
 */
static void
usage(const char *progname) {
        printf("Usage:\n");
        printf("%s [EAL args] -- [NF_LIB args] -- -p <print_delay>\n", progname);
        printf("%s -F <CONFIG_FILE.json> [EAL args] -- [NF_LIB args] -- [NF args]\n\n", progname);
        printf("Flags:\n");
        printf(" - `-p <print_delay>`: number of packets between each print, e.g. `-p 1` prints every packets.\n");
}

/*
 * Parse the application arguments.
 */
static int
parse_app_args(int argc, char *argv[], const char *progname) {
        int c;

        while ((c = getopt(argc, argv, "p:")) != -1) {
                switch (c) {
                        case 'p':
                                print_delay = strtoul(optarg, NULL, 10);
                                RTE_LOG(INFO, APP, "print_delay = %d\n", print_delay);
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

static int
packet_handler(struct rte_mbuf *pkt, struct onvm_pkt_meta *meta,
               __attribute__((unused)) struct onvm_nf_local_ctx *nf_local_ctx) {
	(void) meta;
	/*       
        if(!onvm_pkt_is_ipv4(pkt)) {
            onvm_pkt_set_action(pkt, ONVM_NF_ACTION_TONF, arp_response);
            return 0;
        }
	*/
        // When use parallel action, must initial dst to 0 before setting
        
	uint8_t dst = 0;
        meta->flags = onvm_pkt_set_meta_bit(meta->flags, PKT_META_PAYLOAD_READ);
	dst |= (1 << 2);
	meta->flags = onvm_pkt_set_meta_bit(meta->flags, PKT_META_PAYLOAD_WRITE);
        dst |= (1 << 3);
	
        //dst |= (1 << 5);
        onvm_pkt_set_action(pkt, ONVM_NF_ACTION_PARA, dst);
	
	//onvm_pkt_set_action(pkt, ONVM_NF_ACTION_TONF, 2);

        return 0;
}

int
main(int argc, char *argv[]) {
        struct onvm_nf_local_ctx *nf_local_ctx;
        struct onvm_nf_function_table *nf_function_table;
        int arg_offset;
        const char *progname = argv[0];

        nf_local_ctx = onvm_nflib_init_nf_local_ctx();
        onvm_nflib_start_signal_handler(nf_local_ctx, NULL);

        nf_function_table = onvm_nflib_init_nf_function_table();
        nf_function_table->pkt_handler = &packet_handler;

        if ((arg_offset = onvm_nflib_init(argc, argv, NF_TAG, nf_local_ctx, nf_function_table)) < 0) {
                onvm_nflib_stop(nf_local_ctx);
                if (arg_offset == ONVM_SIGNAL_TERMINATION) {
                        printf("Exiting due to user termination\n");
                        return 0;
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

	struct onvm_nf *parent_nf = nf_local_ctx->nf;
	parent_nf->handle_rate = 10000000;

        cur_cycles = rte_get_tsc_cycles();
        last_cycle = rte_get_tsc_cycles();

        onvm_nflib_run(nf_local_ctx);
        onvm_nflib_stop(nf_local_ctx);

        printf("If we reach here, program is ending\n");
        return 0;
}
