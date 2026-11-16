/**
 * @file doip_context.h
 * @brief Internal: everything one simulator instance owns.
 *
 * DoIP_Handle_t, which doip_api.h declares as an opaque type, is this struct.
 * Each module keeps its per-instance state in its own section here rather
 * than in file-level variables, so several ECUs can run in one process.
 * Not part of the installed headers.
 */
#ifndef DOIP_CONTEXT_H
#define DOIP_CONTEXT_H

#include "doip_api.h"
#include "state/doip_fsm.h"
#include "transport/doip_tcp.h"
#include "transport/doip_uds.h"
#if DOIP_ENABLE_TLS
#include "transport/doip_tls.h"
#endif
#include <pthread.h>
#include <sys/socket.h>

struct DoIP_Context {
    pthread_mutex_t    lock;            /* Serialises the public API calls on this instance   */
    bool               is_initialized;
    DoIP_Config_t      config;          /* Resolved copy: DoIP_Init() fills in the defaults    */
    DoIP_EcuIdentity_t identity;        /* UDS reads and writes it, UDP announces it           */

    /* ----- state/doip_fsm.c ----- */
    DoIP_FsmContext_t  fsm;

    /* ----- transport/doip_tcp.c ----- */
    int                tcp_fd;          /* Listening socket; -1 while closed                   */
    doip_client_t      clients[DOIP_MAX_TCP_CLIENTS];
#if DOIP_ENABLE_TLS
    doip_tls_server_t  tls;
#endif

    /* ----- transport/doip_udp.c ----- */
    int                     udp_fd;     /* -1 while closed                                     */
    bool                    announce_enabled;   /* DoIP_EnablePeriodicAnnounce()               */
    uint32_t                last_announce_ms;
    uint32_t                announcement_count;
    struct sockaddr_storage mcast_addr; /* Where vehicle announcements are sent                */
    socklen_t               mcast_addr_len;

    /* ----- transport/doip_uds.c ----- */
    unsigned           rng;             /* rand_r() state for Security Access seeds            */
#if UDS_SUPPORT_DTC
    uds_dtc_t          dtcs[UDS_DTC_COUNT];
#endif
};

#endif /* DOIP_CONTEXT_H */
