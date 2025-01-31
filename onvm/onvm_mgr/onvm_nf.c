/*********************************************************************
 *                     openNetVM
 *              https://sdnfv.github.io
 *
 *   BSD LICENSE
 *
 *   Copyright(c)
 *            2015-2019 George Washington University
 *            2015-2019 University of California Riverside
 *            2010-2019 Intel Corporation. All rights reserved.
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
 ********************************************************************/

/******************************************************************************

                              onvm_nf.c

       This file contains all functions related to NF management.

******************************************************************************/

#include "onvm_nf.h"
#include <rte_lpm.h>
#include "onvm_mgr.h"
#include "onvm_stats.h"

#define Max_Child 7

/* ID 0 is reserved */
uint16_t next_instance_id = 1;
uint16_t starting_instance_id = 1;

/************************Internal functions prototypes************************/
static uint64_t
onvm_nf_quick_multiplication(uint64_t handle_rate, uint32_t multiplier);

static void
onvm_nf_instance_stop(struct onvm_nf *parent_nf, struct onvm_nf *stop_instance);

static void
onvm_nf_instance_wakeup(struct onvm_nf *parent_nf);

static void
onvm_nf_scaling_nf(struct onvm_nf *parent_nf);

static void
onvm_nf_sleep_instance(struct onvm_nf *parent_nf, struct onvm_nf *sleep_nf);

/*
 * Function starting a NF.
 *
 * Input  : a pointer to the NF's informations
 * Output : an error code
 *
 */
inline static int
onvm_nf_start(struct onvm_nf_init_cfg *nf_init_cfg);

/*
 * Function to mark a NF as ready.
 *
 * Input  : a pointer to the NF's informations
 * Output : an error code
 *
 */
inline static int
onvm_nf_ready(struct onvm_nf *nf);

/*
 * Function stopping a NF.
 *
 * Input  : a pointer to the NF's informations
 * Output : an error code
 *
 */
inline static int
onvm_nf_stop(struct onvm_nf *nf);

/*
 * Function to move a NF to another core.
 *
 * Input  : instance id of the NF that needs to be moved
 *          new_core value of where the NF should be moved
 * Output : an error code
 *
 */
inline int
onvm_nf_relocate_nf(uint16_t nf, uint16_t new_core);

/*
 * Function that initializes an LPM object
 *
 * Input  : the address of an lpm_request struct
 * Output : a return code based on initialization of the LPM object
 *
 */
static void
onvm_nf_init_lpm_region(struct lpm_request *req_lpm);

/*
 * Function that initializes a hashtable for a flow_table struct
 *
 * Input : the address of a ft_request struct
 * Output : a return code based on initialization of a FT object (similar to LPM request)
 */
static void
onvm_nf_init_ft(struct ft_request *ft);

/*
 *  Set up the DPDK rings which will be used to pass packets, via
 *  pointers, between the multi-process server and NF processes.
 *  Each NF needs one RX queue.
 *
 *  Input: An nf struct
 *  Output: rte_exit if failed, none otherwise
 */
static void
onvm_nf_init_rings(struct onvm_nf *nf);

/********************************Interfaces***********************************/

uint16_t
onvm_nf_next_instance_id(void) {
        struct onvm_nf *nf;
        uint16_t instance_id;

        if (num_nfs >= MAX_NFS)
                return MAX_NFS;

        /* Do a first pass for NF IDs bigger than current next_instance_id */
        while (next_instance_id < MAX_NFS) {
                instance_id = next_instance_id++;
                /* Check if this id is occupied by another NF */
                nf = &nfs[instance_id];
                if (!onvm_nf_is_valid(nf))
                        return instance_id;
        }

        /* Reset to starting position */
        next_instance_id = starting_instance_id;

        /* Do a second pass for other NF IDs */
        while (next_instance_id < MAX_NFS) {
                instance_id = next_instance_id++;
                /* Check if this id is occupied by another NF */
                nf = &nfs[instance_id];
                if (!onvm_nf_is_valid(nf))
                        return instance_id;
        }

        /* This should never happen, means our num_nfs counter is wrong */
        RTE_LOG(ERR, APP, "Tried to allocated a next instance ID but num_nfs is corrupted\n");
        return MAX_NFS;
}

void
onvm_nf_check_status(void) {
        int i;
        void *msgs[MAX_NFS];
        struct onvm_nf *nf;
        struct onvm_nf_msg *msg;
        struct onvm_nf_init_cfg *nf_init_cfg;
        struct lpm_request *req_lpm;
        struct ft_request *ft;
        uint16_t stop_nf_id;
        int num_msgs = rte_ring_count(incoming_msg_queue);

        if (num_msgs == 0)
                return;

        if (rte_ring_dequeue_bulk(incoming_msg_queue, msgs, num_msgs, NULL) == 0)
                return;

        for (i = 0; i < num_msgs; i++) {
                msg = (struct onvm_nf_msg *)msgs[i];

                switch (msg->msg_type) {
                        case MSG_REQUEST_LPM_REGION:
                                // TODO: Add stats event handler here
                                req_lpm = (struct lpm_request *)msg->msg_data;
                                onvm_nf_init_lpm_region(req_lpm);
                                break;
                        case MSG_REQUEST_FT:
                                ft = (struct ft_request *)msg->msg_data;
                                onvm_nf_init_ft(ft);
                                break;
                        case MSG_NF_STARTING:
                                nf_init_cfg = (struct onvm_nf_init_cfg *)msg->msg_data;
                                if (onvm_nf_start(nf_init_cfg) == 0) {
                                        onvm_stats_gen_event_nf_info("NF Starting", &nfs[nf_init_cfg->instance_id]);
                                }
                                break;
                        case MSG_NF_READY:
                                nf = (struct onvm_nf *)msg->msg_data;
                                if (onvm_nf_ready(nf) == 0) {
                                        onvm_stats_gen_event_nf_info("NF Ready", nf);
                                }
                                break;
                        case MSG_NF_STOPPING:
                                nf = (struct onvm_nf *)msg->msg_data;
                                if (nf == NULL)
                                        break;

                                /* Saved as onvm_nf_stop frees the memory */
                                stop_nf_id = nf->instance_id;
                                if (onvm_nf_stop(nf) == 0) {
                                        onvm_stats_gen_event_info("NF Stopping", ONVM_EVENT_NF_STOP, &stop_nf_id);
                                }
                                break;
                }

                rte_mempool_put(nf_msg_pool, (void *)msg);
        }
}

int
onvm_nf_send_msg(uint16_t dest, uint8_t msg_type, void *msg_data) {
        int ret;
        struct onvm_nf_msg *msg;

        ret = rte_mempool_get(nf_msg_pool, (void **)(&msg));
        if (ret != 0) {
                RTE_LOG(INFO, APP, "Oh the huge manatee! Unable to allocate msg from pool :(\n");
                return ret;
        }

        msg->msg_type = msg_type;
        msg->msg_data = msg_data;

        return rte_ring_enqueue(nfs[dest].msg_q, (void *)msg);
}

void
onvm_nf_scaling(unsigned difftime) {
        static uint64_t nf_rx_last[MAX_NFS] = {0};
        uint64_t nf_rx_pps;
        uint64_t rx_pps_for_service[MAX_SERVICES] = {0};

        for (int i = 0; i < MAX_NFS; i++) {
                if (!onvm_nf_is_valid(&nfs[i]))
                        continue;
                nf_rx_pps = (nfs[i].stats.rx - nf_rx_last[i]) / difftime;
                nf_rx_last[i] = nfs[i].stats.rx;

                if (nfs[i].thread_info.parent) {
                        if (nfs[i].idle_time >= 10) {
                                struct onvm_nf *parent_nf = &nfs[nfs[i].thread_info.parent];

                                if (nfs[i].instance_id == parent_nf->thread_info.sleep_instance[0]) {
                                        onvm_nf_instance_stop(parent_nf, &nfs[i]);
                                } else {
                                        printf("error might happend...\n");
                                }
                        } else {
                                if (nfs[i].thread_info.sleep_flag) {
                                        nfs[i].idle_time++;
                                        printf("instance %d idle for %d sec...\n", i, nfs[i].idle_time);
                                } else
                                        nfs[i].idle_time = 0;
                        }
                }
                rx_pps_for_service[nfs[i].service_id] += nf_rx_pps;
        }
        printf(
            "\n--------------------------------------------------------------------------------------------------------"
            "--------------\n");
        for (int i = 0; i < MAX_SERVICES; i++) {
                uint16_t nfs_for_service = nf_per_service_count[i];
                if (!nfs_for_service)
                        continue;

                uint32_t parent_instance_ID = services[i][0];
                uint64_t service_handle_rate = nfs[parent_instance_ID].handle_rate;
                uint64_t H_threshold = onvm_nf_quick_multiplication(service_handle_rate, nfs_for_service);
                uint64_t L_threshold = onvm_nf_quick_multiplication(service_handle_rate, nfs_for_service - 1);

                printf("Service : %d - child amount : %d - enable amount : %d\n", i,
                       nfs[parent_instance_ID].thread_info.nums_child, nfs_for_service);
                printf("H_threshold : %ld - L_threshold : %ld - rx_pps : %ld\n\n", H_threshold, L_threshold,
                       rx_pps_for_service[i]);

                if (rx_pps_for_service[i] >= H_threshold) {
                        nfs[parent_instance_ID].thread_info.wait_counter = 10;

                        if (nfs[parent_instance_ID].thread_info.sleep_count) {
                                struct onvm_nf *parent_nf = &nfs[parent_instance_ID];
                                onvm_nf_instance_wakeup(parent_nf);
                        } else if (nfs[parent_instance_ID].thread_info.nums_child < Max_Child &&
                                   nfs[parent_instance_ID].wait_flag == false) {
                                onvm_nf_scaling_nf(&nfs[parent_instance_ID]);
                        } else {
                                printf("Do back pressure in the future\n");
                                /* Drop the packet which will enter this overloading service */
                        }
                } else if (rx_pps_for_service[i] < L_threshold && nfs[parent_instance_ID].thread_info.nums_child !=
                                                                      nfs[parent_instance_ID].thread_info.sleep_count) {
                        if (nfs[parent_instance_ID].thread_info.wait_counter) {
                                printf("Wating counter to terminate service %d\n", i);
                                nfs[parent_instance_ID].thread_info.wait_counter--;
                        } else if (!nfs[parent_instance_ID].wait_flag) {
                                uint32_t sleep_instance = services[i][nfs_for_service - 1];
                                struct onvm_nf *sleep_nf = &nfs[sleep_instance];
                                struct onvm_nf *parent_nf = &nfs[parent_instance_ID];
                                onvm_nf_sleep_instance(parent_nf, sleep_nf);
                        }
                }
        }
}

/******************************Internal functions*****************************/

static inline uint64_t
onvm_nf_quick_multiplication(uint64_t handle_rate, uint32_t multiplier) {
        if (multiplier == 1) {
                return handle_rate;
        } else if (multiplier == 2) {
                return handle_rate << 1;
        } else if (multiplier == 3) {
                return (handle_rate << 1) + handle_rate;
        } else if (multiplier == 4) {
                return handle_rate << 2;
        } else if (multiplier == 5) {
                return (handle_rate << 2) + handle_rate;
        } else {
                return handle_rate * multiplier;
        }
}

static void
onvm_nf_instance_stop(struct onvm_nf *parent_nf, struct onvm_nf *stop_instance) {
        parent_nf->thread_info.sleep_count--;
        for (int i = 1; i <= parent_nf->thread_info.sleep_count; i++) {
                parent_nf->thread_info.sleep_instance[i - 1] = parent_nf->thread_info.sleep_instance[i];
        }
        parent_nf->wait_flag = true;
        onvm_nf_send_msg(stop_instance->instance_id, MSG_STOP, NULL);
}

static void
onvm_nf_instance_wakeup(struct onvm_nf *parent_nf) {
        printf("Wake up sleep instance for service %d\n", parent_nf->service_id);
        uint32_t wake_instance = parent_nf->thread_info.sleep_instance[--parent_nf->thread_info.sleep_count];
        struct onvm_nf *wake_nf = &nfs[wake_instance];
        wake_nf->thread_info.sleep_flag = false;
        nf_per_service_count[parent_nf->service_id]++;
}

static void
onvm_nf_scaling_nf(struct onvm_nf *parent_nf) {
        printf("Send scaling msg to service %d with instance %d\n", parent_nf->service_id, parent_nf->instance_id);
        struct onvm_nf_scaling *scale_info = NULL;
        parent_nf->wait_flag = true;
        onvm_nf_send_msg(parent_nf->instance_id, MSG_SCALE, scale_info);
}

static void
onvm_nf_sleep_instance(struct onvm_nf *parent_nf, struct onvm_nf *sleep_nf) {
        uint32_t service_id = parent_nf->service_id;
        uint32_t sleep_instance = sleep_nf->instance_id;
        nf_per_service_count[service_id]--;
        sleep_nf->thread_info.sleep_flag = true;
        parent_nf->thread_info.sleep_instance[parent_nf->thread_info.sleep_count++] = sleep_instance;
        printf("Sleep instance : %d\n", sleep_instance);
}

inline static int
onvm_nf_start(struct onvm_nf_init_cfg *nf_init_cfg) {
        struct onvm_nf *spawned_nf;
        uint16_t nf_id;
        int ret;

        if (nf_init_cfg == NULL || nf_init_cfg->status != NF_WAITING_FOR_ID)
                return 1;

        // if NF passed its own id on the command line, don't assign here
        // assume user is smart enough to avoid duplicates
        nf_id = nf_init_cfg->instance_id == (uint16_t)NF_NO_ID ? onvm_nf_next_instance_id() : nf_init_cfg->instance_id;
        spawned_nf = &nfs[nf_id];

        if (nf_id >= MAX_NFS) {
                // There are no more available IDs for this NF
                nf_init_cfg->status = NF_NO_IDS;
                return 1;
        }

        if (nf_init_cfg->service_id >= MAX_SERVICES) {
                // Service ID must be less than MAX_SERVICES and greater than 0
                nf_init_cfg->status = NF_SERVICE_MAX;
                return 1;
        }

        if (nf_per_service_count[nf_init_cfg->service_id] >= MAX_NFS_PER_SERVICE) {
                // Maximum amount of NF's per service spawned
                nf_init_cfg->status = NF_SERVICE_COUNT_MAX;
                return 1;
        }

        if (onvm_nf_is_valid(spawned_nf)) {
                // This NF is trying to declare an ID already in use
                nf_init_cfg->status = NF_ID_CONFLICT;
                return 1;
        }

        // Keep reference to this NF in the manager
        nf_init_cfg->instance_id = nf_id;

        /* If not successful return will contain the error code */
        ret = onvm_threading_get_core(&nf_init_cfg->core, nf_init_cfg->init_options, cores);
        if (ret != 0) {
                nf_init_cfg->status = ret;
                return 1;
        }

        spawned_nf->instance_id = nf_id;
        spawned_nf->service_id = nf_init_cfg->service_id;
        spawned_nf->status = NF_STARTING;
        spawned_nf->tag = nf_init_cfg->tag;
        spawned_nf->thread_info.core = nf_init_cfg->core;
        spawned_nf->flags.time_to_live = nf_init_cfg->time_to_live;
        spawned_nf->flags.pkt_limit = nf_init_cfg->pkt_limit;
        onvm_nf_init_rings(spawned_nf);

        // Let the NF continue its init process
        nf_init_cfg->status = NF_STARTING;
        return 0;
}

inline static int
onvm_nf_ready(struct onvm_nf *nf) {
        // Ensure we've already called nf_start for this NF
        if (nf->status != NF_STARTING)
                return -1;
        /*
        uint16_t service_count = nf_per_service_count[nf->service_id]++;
        services[nf->service_id][service_count] = nf->instance_id;
        */
        num_nfs++;
        // Register this NF running within its service
        nf->status = NF_RUNNING;
        return 0;
}

inline static int
onvm_nf_stop(struct onvm_nf *nf) {
        uint16_t nf_id;
        uint16_t nf_status;
        uint16_t service_id;
        uint16_t nb_pkts, i;
        struct onvm_nf_msg *msg;
        struct rte_mempool *nf_info_mp;
        struct rte_mbuf *pkts[PACKET_READ_SIZE];
        uint16_t candidate_nf_id, candidate_core;
        int mapIndex;

        if (nf == NULL)
                return 1;

        nf_id = nf->instance_id;
        service_id = nf->service_id;
        nf_status = nf->status;
        candidate_core = nf->thread_info.core;

        /* Remove this NF from ther service map.
         * Need to shift all elements past it in the array left to avoid gaps
         */
        if (!nf->thread_info.sleep_flag) {
                nf_per_service_count[service_id]--;
        }

        /* Cleanup the allocated tag */
        if (nf->tag) {
                rte_free(nf->tag);
                nf->tag = NULL;
        }

        /* Cleanup should only happen if NF was starting or running */
        if (nf_status != NF_STARTING && nf_status != NF_RUNNING && nf_status != NF_PAUSED)
                return 1;

        nf->status = NF_STOPPED;
        nfs[nf->instance_id].status = NF_STOPPED;

        /* Tell parent we stopped running */
        if (nfs[nf_id].thread_info.parent != 0)
                rte_atomic16_dec(&nfs[nfs[nf_id].thread_info.parent].thread_info.children_cnt);

        /* Remove the NF from the core it was running on */
        cores[nf->thread_info.core].nf_count--;
        cores[nf->thread_info.core].is_dedicated_core = 0;

        /* Clean up possible left over objects in rings */
        while ((nb_pkts = rte_ring_dequeue_burst(nfs[nf_id].rx_q, (void **)pkts, PACKET_READ_SIZE, NULL)) > 0) {
                for (i = 0; i < nb_pkts; i++)
                        rte_pktmbuf_free(pkts[i]);
        }
        while ((nb_pkts = rte_ring_dequeue_burst(nfs[nf_id].tx_q, (void **)pkts, PACKET_READ_SIZE, NULL)) > 0) {
                for (i = 0; i < nb_pkts; i++)
                        rte_pktmbuf_free(pkts[i]);
        }
        nf_msg_pool = rte_mempool_lookup(_NF_MSG_POOL_NAME);
        while (rte_ring_dequeue(nfs[nf_id].msg_q, (void **)(&msg)) == 0) {
                rte_mempool_put(nf_msg_pool, (void *)msg);
        }

        /* Free info struct */
        /* Lookup mempool for nf struct */
        nf_info_mp = rte_mempool_lookup(_NF_MEMPOOL_NAME);
        if (nf_info_mp == NULL)
                return 1;

        rte_mempool_put(nf_info_mp, (void *)nf);

        /* Further cleanup is only required if NF was succesfully started */
        if (nf_status != NF_RUNNING && nf_status != NF_PAUSED)
                return 0;

        /* Decrease the total number of RUNNING NFs */
        num_nfs--;

        /* Reset stats */
        onvm_stats_clear_nf(nf_id);

        for (mapIndex = 0; mapIndex < MAX_NFS_PER_SERVICE; mapIndex++) {
                if (services[service_id][mapIndex] == nf_id) {
                        break;
                }
        }

        if (mapIndex < MAX_NFS_PER_SERVICE) {  // sanity error check
                services[service_id][mapIndex] = 0;
                for (; mapIndex < MAX_NFS_PER_SERVICE - 1; mapIndex++) {
                        // Shift the NULL to the end of the array
                        if (services[service_id][mapIndex + 1] == 0) {
                                // Short circuit when we reach the end of this service's list
                                break;
                        }
                        services[service_id][mapIndex] = services[service_id][mapIndex + 1];
                        services[service_id][mapIndex + 1] = 0;
                }
        }

        /* As this NF stopped we can reevaluate core mappings */
        if (ONVM_NF_SHUTDOWN_CORE_REASSIGNMENT) {
                /* As this NF stopped we can reevaluate core mappings */
                candidate_nf_id = onvm_threading_find_nf_to_reassign_core(candidate_core, cores);
                if (candidate_nf_id > 0) {
                        onvm_nf_relocate_nf(candidate_nf_id, candidate_core);
                }
        }

        return 0;
}

static void
onvm_nf_init_lpm_region(struct lpm_request *req_lpm) {
        struct rte_lpm_config conf;
        struct rte_lpm *lpm_region;

        conf.max_rules = req_lpm->max_num_rules;
        conf.number_tbl8s = req_lpm->num_tbl8s;

        lpm_region = rte_lpm_create(req_lpm->name, req_lpm->socket_id, &conf);
        if (lpm_region) {
                req_lpm->status = 0;
        } else {
                req_lpm->status = -1;
        }
}

static void
onvm_nf_init_ft(struct ft_request *ft) {
        struct rte_hash *hash;

        hash = rte_hash_create(ft->ipv4_hash_params);
        if (hash) {
                ft->status = 0;
        } else {
                ft->status = -1;
        }
}

inline int
onvm_nf_relocate_nf(uint16_t dest, uint16_t new_core) {
        uint16_t *msg_data;

        msg_data = rte_malloc("Change core msg data", sizeof(uint16_t), 0);
        *msg_data = new_core;

        cores[nfs[dest].thread_info.core].nf_count--;

        onvm_nf_send_msg(dest, MSG_CHANGE_CORE, msg_data);

        /* We probably need logic that handles if everything is successful */

        /* TODO Add core number */
        onvm_stats_gen_event_nf_info("NF Ready", &nfs[dest]);

        cores[new_core].nf_count++;
        return 0;
}

static void
onvm_nf_init_rings(struct onvm_nf *nf) {
        unsigned instance_id;
        unsigned socket_id;
        const char *rq_name;
        const char *tq_name;
        const char *msg_q_name;
        const unsigned ringsize = NF_QUEUE_RINGSIZE;
        const unsigned msgringsize = NF_MSG_QUEUE_SIZE;

        instance_id = nf->instance_id;
        socket_id = rte_socket_id();
        rq_name = get_rx_queue_name(instance_id);
        tq_name = get_tx_queue_name(instance_id);
        msg_q_name = get_msg_queue_name(instance_id);
        nf->rx_q = rte_ring_create(rq_name, ringsize, socket_id, RING_F_SC_DEQ);        /* multi prod, single cons */
        nf->tx_q = rte_ring_create(tq_name, ringsize, socket_id, RING_F_SC_DEQ);        /* multi prod, single cons */
        nf->msg_q = rte_ring_create(msg_q_name, msgringsize, socket_id, RING_F_SC_DEQ); /* multi prod, single cons */

        if (nf->rx_q == NULL)
                rte_exit(EXIT_FAILURE, "Cannot create rx ring queue for NF %u\n", instance_id);

        if (nf->tx_q == NULL)
                rte_exit(EXIT_FAILURE, "Cannot create tx ring queue for NF %u\n", instance_id);

        if (nf->msg_q == NULL)
                rte_exit(EXIT_FAILURE, "Cannot create msg queue for NF %u\n", instance_id);
}
