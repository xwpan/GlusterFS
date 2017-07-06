#ifndef _DPDK_H
#define _DPDK_H

#include "rpc-transport.h"
#include "logging.h"
#include "dict.h"
#include "mem-pool.h"
#include "globals.h"
#include "mem-types.h"
#include <mtcp_api.h>
#include <mtcp_epoll.h>
#include "debug.h"


#ifndef MAX_IOVEC
#define MAX_IOVEC 16
#endif /* MAX_IOVEC */

#define MAX_FLOW_NUM (1000)   //10000

#define GF_DEFAULT_DPDK_LISTEN_PORT  GF_DEFAULT_BASE_PORT  //24007

#define RPC_MAX_FRAGMENT_SIZE 0x7fffffff

/* The default window size will be 0, indicating not to set
 * it to any size. Default size of Linux is found to be
 * performance friendly.
 * Linux allows us to over-ride the max values for the system.
 * Should we over-ride them? Because if we set a value larger than the default
 * setsockopt will fail. Having larger values might be beneficial for
 * IB links.
 */
#define GF_DEFAULT_DPDK_WINDOW_SIZE   (0)
#define GF_MAX_DPDK_WINDOW_SIZE       (1 * GF_UNIT_MB)
#define GF_MIN_DPDK_WINDOW_SIZE       (0)
#define GF_USE_DEFAULT_KEEPALIVE        (-1) 	//在__socket_keepalive中

typedef enum gf_dpdk_mem_types_ {
        gf_dpdk_connect_error_state_t     = gf_common_mt_end + 1,
} gf_dpdk_mem_types_t;

typedef enum {
        SP_STATE_NADA = 0,
        SP_STATE_COMPLETE,
        SP_STATE_READING_FRAGHDR,
        SP_STATE_READ_FRAGHDR,
        SP_STATE_READING_FRAG,
} dpdk_sp_rpcrecord_state_t;

typedef enum {
        SP_STATE_RPCFRAG_INIT,
        SP_STATE_READING_MSGTYPE,
        SP_STATE_READ_MSGTYPE,
        SP_STATE_NOTIFYING_XID
} dpdk_sp_rpcfrag_state_t;

typedef enum {
        SP_STATE_SIMPLE_MSG_INIT,
        SP_STATE_READING_SIMPLE_MSG,
} dpdk_sp_rpcfrag_simple_msg_state_t;

typedef enum {
        SP_STATE_VECTORED_REQUEST_INIT,
        SP_STATE_READING_CREDBYTES,
        SP_STATE_READ_CREDBYTES,        /* read credential data. */
        SP_STATE_READING_VERFBYTES,
        SP_STATE_READ_VERFBYTES,        /* read verifier data */
        SP_STATE_READING_PROGHDR,
        SP_STATE_READ_PROGHDR,
        SP_STATE_READING_PROGHDR_XDATA,
        SP_STATE_READ_PROGHDR_XDATA,    /* It's a bad "name" in the generic
					   RPC state machine, but greatly
					   aids code review (and xdata is
					   the only "consumer" of this state)
					*/
        SP_STATE_READING_PROG,
} dpdk_sp_rpcfrag_vectored_request_state_t;

typedef enum {
        SP_STATE_REQUEST_HEADER_INIT,
        SP_STATE_READING_RPCHDR1,
        SP_STATE_READ_RPCHDR1,     /* read msg from beginning till and
                                    * including credlen
                                    */
} dpdk_sp_rpcfrag_request_header_state_t;

struct dpdk_ioq {
        union {
                struct list_head list;
                struct {
                        struct dpdk_ioq    *next;
                        struct dpdk_ioq    *prev;
                };
        };

        uint32_t           fraghdr;
        struct iovec       vector[MAX_IOVEC];
        int                count;
        struct iovec      *pending_vector;
        int                pending_count;
        struct iobref     *iobref;
};

typedef struct {
        dpdk_sp_rpcfrag_request_header_state_t header_state;
        dpdk_sp_rpcfrag_vectored_request_state_t vector_state;
        int vector_sizer_state;
} dpdk_sp_rpcfrag_request_state_t;

typedef enum {
        SP_STATE_VECTORED_REPLY_STATUS_INIT,
        SP_STATE_READING_REPLY_STATUS,
        SP_STATE_READ_REPLY_STATUS,
} dpdk_sp_rpcfrag_vectored_reply_status_state_t;

typedef enum {
        SP_STATE_ACCEPTED_SUCCESS_REPLY_INIT,
        SP_STATE_READING_PROC_HEADER,
        SP_STATE_READING_PROC_OPAQUE,
        SP_STATE_READ_PROC_OPAQUE,
        SP_STATE_READ_PROC_HEADER,
} dpdk_sp_rpcfrag_vectored_reply_accepted_success_state_t;

typedef enum {
        SP_STATE_ACCEPTED_REPLY_INIT,
        SP_STATE_READING_REPLY_VERFLEN,
        SP_STATE_READ_REPLY_VERFLEN,
        SP_STATE_READING_REPLY_VERFBYTES,
        SP_STATE_READ_REPLY_VERFBYTES,
} dpdk_sp_rpcfrag_vectored_reply_accepted_state_t;

typedef struct {
        uint32_t accept_status;
        dpdk_sp_rpcfrag_vectored_reply_status_state_t status_state;
        dpdk_sp_rpcfrag_vectored_reply_accepted_state_t accepted_state;
        dpdk_sp_rpcfrag_vectored_reply_accepted_success_state_t accepted_success_state;
} dpdk_sp_rpcfrag_vectored_reply_state_t;

struct gf_dpdk_incoming_frag {
        char         *fragcurrent;
        uint32_t      bytes_read;
        uint32_t      remaining_size;
        struct iovec  vector;
        struct iovec *pending_vector;
        union {
                dpdk_sp_rpcfrag_request_state_t        request;
                dpdk_sp_rpcfrag_vectored_reply_state_t reply;
        } call_body;

        dpdk_sp_rpcfrag_simple_msg_state_t     simple_state;
        dpdk_sp_rpcfrag_state_t state;
};

#define GF_DPDK_RA_MAX 4096  // default: 1024

struct gf_dpdk_incoming {
        dpdk_sp_rpcrecord_state_t  record_state;
        struct gf_dpdk_incoming_frag frag;
        char                *proghdr_base_addr;
        struct iobuf        *iobuf; //RPC头
        size_t               iobuf_size;
        struct iovec         vector[2];
        int                  count;
        struct iovec         payload_vector; //实际读写数据
        struct iobref       *iobref;
        rpc_request_info_t  *request_info; 
        struct iovec        *pending_vector; //iobuf 或者 payload_vector
        int                  pending_count; // 1/0
        uint32_t             fraghdr;
        char                 complete_record;
        msg_type_t           msg_type;
        size_t               total_bytes_read;

	size_t               ra_read;
	size_t               ra_max;
	size_t               ra_served;
	char                *ra_buf;
};

/*  此数据结构可能用不到  */
/*
struct server_vars
{
	char request[HTTP_HEADER_LEN];
	int recv_len;
	int request_len;
	long int total_read, total_sent;
	uint8_t done;
	uint8_t rspheader_sent;
	uint8_t keep_alive;

	int fidx;					
	char fname[NAME_LIMIT];				
	long int fsize;					
};
*/

struct thread_context
{
	mctx_t mctx;
	int ep;
	rpc_transport_t  *trans_info[MAX_FLOW_NUM];
};


typedef struct {
        int32_t                sock;
        int32_t                idx; //不要?
        /* -1 = not connected. 0 = in progress. 1 = connected */
        char                   connected;
        /* 1 = connect failed for reasons other than EINPROGRESS/ENOENT
        see socket_connect for details */
        char                   connect_failed;
        char                   bio;
        char                   connect_finish_log;//连接失败写入日志标志
        char                   submit_log;

	struct thread_context  *ctx;
        union {
                struct list_head     ioq;
                struct {
                        struct dpdk_ioq        *ioq_next;
                        struct dpdk_ioq        *ioq_prev;
                };
        };
        struct gf_dpdk_incoming incoming;
        pthread_mutex_t        lock;
        int                    windowsize;
        char                   lowlat;
        char                   nodelay;
        int                    keepalive;
        int                    keepaliveidle;
        int                    keepaliveintvl;
        int                    timeout;
        uint32_t               backlog;
        gf_boolean_t           is_server;
} dpdk_private_t;


#endif
