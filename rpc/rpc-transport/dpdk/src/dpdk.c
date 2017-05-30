#include "dict.h"
#include "glusterfs.h"
#include "iobuf.h"
#include "logging.h"
#include "dpdk.h"
#include "byte-order.h"
#include "xlator.h"
#include "xdr-rpc.h"
#include "rpc-lib-messages.h"
#include "compat.h"
#include <signal.h>
#include <netdb.h>
//#define GF_RDMA_LOG_NAME "rpc-transport/rdma"
#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>
#include "debug.h"

#define MAX_FLOW_NUM  (10000)

#define RCVBUF_SIZE (2*1024)
#define SNDBUF_SIZE (8*1024)

#define MAX_EVENTS (MAX_FLOW_NUM * 3)

#define HTTP_HEADER_LEN 1024
#define URL_LEN 128

#define MAX_CPUS 16
#define MAX_FILES 30

#define NAME_LIMIT 128
#define FULLNAME_LIMIT 256

#define CPU_NUMS 1

FILE *fp;

static int num_cores;
static int core_limit;
static pthread_t app_thread[MAX_CPUS]; //最大可能的application线程数
static pthread_t cl_thread; //客户端线程数
static int done[MAX_CPUS];
static int cores[MAX_CPUS];
static char *conf_file = NULL;

static int nfiles;
static int finished;

struct dpdk_server_args
{
	rpc_transport_t *transport_ptr;
	int core;
};

static struct dpdk_server_args server_args[CPU_NUMS];


struct dpdk_client_args
{
	rpc_transport_t *transport_ptr;
	int port;
};


/* ugly #includes below */
#include "protocol-common.h"
#include "glusterfs3-xdr.h"
#include "xdr-nfs3.h"
#include "rpcsvc.h"

#include <fcntl.h>
#include <errno.h>
#include <rpc/xdr.h>
#include <sys/ioctl.h>
#define GF_LOG_ERRNO(errno) ((errno == ENOTCONN) ? GF_LOG_DEBUG : GF_LOG_ERROR)
#define SA(ptr) ((struct sockaddr *)ptr)

static int
dpdk_rwv (struct thread_context *ctx, rpc_transport_t *this, struct iovec *vector, 
			int count, struct iovec **pending_vector, int *pending_count, 
			size_t *bytes, int write);
static int
dpdk_read_simple_msg (struct thread_context *ctx, rpc_transport_t *this);

int32_t
dpdk_fill_inet6_inet_identifier (rpc_transport_t *this, struct sockaddr_storage *addr, int32_t addr_len, char *identifier);


//有用
#define dpdk_proto_reset_pending(ctx, priv) do {                         \
                struct gf_dpdk_incoming_frag *frag;                     \
                frag = &priv->incoming.frag;                            \
                                                                        \
                memset (&frag->vector, 0, sizeof (frag->vector));       \
                frag->pending_vector = &frag->vector;                   \
                frag->pending_vector->iov_base = frag->fragcurrent;     \
                priv->incoming.pending_vector =  frag->pending_vector;  \
        } while (0)

//有用
#define dpdk_proto_update_pending(ctx, priv)                             \
        do {                                                            \
                uint32_t remaining;                                     \
                struct gf_dpdk_incoming_frag *frag;                     \
                frag = &priv->incoming.frag;                            \
                if (frag->pending_vector->iov_len == 0) {               \
                        remaining = (RPC_FRAGSIZE (priv->incoming.fraghdr) \
                                     - frag->bytes_read);               \
                                                                        \
                        frag->pending_vector->iov_len =                 \
                                (remaining > frag->remaining_size)      \
                                ? frag->remaining_size : remaining;     \
                                                                        \
                        frag->remaining_size -=                         \
                                frag->pending_vector->iov_len;          \
                }                                                       \
        } while (0)

//有用
#define dpdk_proto_update_priv_after_read(ctx, priv, ret, bytes_read)    \
        {                                                               \
                struct gf_dpdk_incoming_frag *frag;                     \
                frag = &priv->incoming.frag;                            \
                                                                        \
                frag->fragcurrent += bytes_read;                        \
                frag->bytes_read += bytes_read;                         \
                                                                        \
                if ((ret > 0) || (frag->remaining_size != 0)) {         \
                        if (frag->remaining_size != 0 && ret == 0) {    \
                                dpdk_proto_reset_pending (ctx, priv);    \
                        }                                               \
                                                                        \
                        gf_log (this->name, GF_LOG_TRACE,               \
                                "partial read on non-blocking socket"); \
                                                                        \
                        break;                                          \
                }                                                       \
        }

//有用
#define dpdk_proto_init_pending(ctx, priv,size)                          \
        do {                                                            \
            uint32_t remaining = 0;                                     \
            struct gf_dpdk_incoming_frag *frag;                         \
            frag = &priv->incoming.frag;                                \
                                                                        \
            remaining = (RPC_FRAGSIZE (priv->incoming.fraghdr)          \
                         - frag->bytes_read);                           \
                                                                        \
            dpdk_proto_reset_pending (ctx, priv);                        \
                                                                        \
            frag->pending_vector->iov_len =                             \
                    (remaining > size) ? size : remaining;              \
                                                                        \
            frag->remaining_size = (size - frag->pending_vector->iov_len); \
                                                                        \
            } while(0)


//有用
/* This will be used in a switch case and breaks from the switch case if all
 * the pending data is not read.
 */
#define dpdk_proto_read(ctx, priv, ret)                                  \
                {                                                       \
                size_t bytes_read = 0;                                  \
                struct gf_dpdk_incoming *in;                            \
                in = &priv->incoming;                                   \
                                                                        \
                dpdk_proto_update_pending (ctx, priv);                   \
                                                                        \
                ret = dpdk_rwv (ctx, this,                             \
                                      in->pending_vector, 1,            \
                                      &in->pending_vector,              \
                                      &in->pending_count,               \
                                      &bytes_read, 0);                     \
                if (ret == -1)                                          \
                        break;                                          \
                dpdk_proto_update_priv_after_read (ctx, priv, ret, bytes_read); \
        }

//有用
struct dpdk_connect_error_state_ {
        xlator_t            *this;
        rpc_transport_t     *trans;
        gf_boolean_t         refd;
};
typedef struct dpdk_connect_error_state_ dpdk_connect_error_state_t;


//有用
static int
dpdk_shutdown (struct thread_context *ctx, rpc_transport_t *this) //最终调用
{
        int               ret = -1;
        dpdk_private_t *priv = this->private;

        priv->connected = -1;
        ret = mtcp_close (ctx->mctx, priv->sock);
        if (ret) {
                /* its already disconnected.. no need to understand
                   why it failed to shutdown in normal cases */
                gf_log (this->name, GF_LOG_DEBUG,
                        "shutdown() returned %d. %s",
                        ret, strerror (errno));
        }
        return ret;
}

//有用
static int
dpdk_disconnect (struct thread_context *ctx, rpc_transport_t *this)
{
        int               ret = -1;
        dpdk_private_t *priv =  this->private;

        gf_log (this->name, GF_LOG_TRACE,
                "disconnecting %p, sock=%d", this, priv->sock);
	//priv->lock需要加锁吗---no need
        if (priv->sock != -1) {
                ret = dpdk_shutdown (ctx, this);
                if (ret) {
                        gf_log (this->name, GF_LOG_DEBUG,
                                "dpdk_disconnect () failed: %s",
                                strerror (errno));
                }
        }
        return ret;
}

static int
__dpdk_connect_finish (struct thread_context *ctx, int fd)
{
        int       ret = -1;
        int       optval = 0;
        socklen_t optlen = sizeof (int);

        ret = mtcp_getsockopt (ctx->mctx, fd, SOL_SOCKET, SO_ERROR, (void *)&optval, &optlen);

        if (ret == 0 && optval) {
                errno = optval;
                ret = -1;
        }

        return ret;
}

//有用
static void
dpdk_reset (struct thread_context *ctx, rpc_transport_t *this)  //为什么要清理注册事件？
{
        dpdk_private_t *priv = NULL;
        priv = this->private;

        /* TODO: use mem-pool on incoming data */

        if (priv->incoming.iobref) {
                iobref_unref (priv->incoming.iobref);
                priv->incoming.iobref = NULL;
        }

        if (priv->incoming.iobuf) {
                iobuf_unref (priv->incoming.iobuf);
                priv->incoming.iobuf = NULL;
        }

        GF_FREE (priv->incoming.request_info);

        memset (&priv->incoming, 0, sizeof (priv->incoming));

        //event_unregister_close (this->ctx->event_pool, priv->sock, priv->idx);
	// 从mtcp中清理掉注册的事件
	mtcp_epoll_ctl (ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_DEL, priv->sock, NULL);

        priv->sock = -1;
        priv->connected = -1;

        return;
}

//有用
static void
dpdk_set_lastfrag (uint32_t *fragsize) {
        (*fragsize) |= 0x80000000U;
}

//xx
static void
dpdk_set_frag_header_size (uint32_t size, char *haddr)
{
        size = htonl (size);
        memcpy (haddr, &size, sizeof (size));
}

//xx
static void
dpdk_set_last_frag_header_size (uint32_t size, char *haddr)
{
        dpdk_set_lastfrag (&size);
        dpdk_set_frag_header_size (size, haddr);
}

//有用
static struct dpdk_ioq *
dpdk_ioq_new (rpc_transport_t *this, rpc_transport_msg_t *msg)
{
        struct dpdk_ioq       *entry = NULL;
        int               count = 0;
        uint32_t          size  = 0;


        /* TODO: use mem-pool */
        entry = GF_CALLOC (1, sizeof (*entry), gf_common_mt_ioq);//利用开始分配的内存池
        if (!entry)
                return NULL;

        count = msg->rpchdrcount + msg->proghdrcount + msg->progpayloadcount;//count = roc头 + prog头 + prog负载

        GF_ASSERT (count <= (MAX_IOVEC - 1));

        size = iov_length (msg->rpchdr, msg->rpchdrcount)
                + iov_length (msg->proghdr, msg->proghdrcount)
                + iov_length (msg->progpayload, msg->progpayloadcount);

        if (size > RPC_MAX_FRAGMENT_SIZE) { //size大于rpc最大片段的大小
                gf_log (this->name, GF_LOG_ERROR,
                        "msg size (%u) bigger than the maximum allowed size on "
                        "sockets (%u)", size, RPC_MAX_FRAGMENT_SIZE);
                GF_FREE (entry);
                return NULL;
        }

        dpdk_set_last_frag_header_size (size, (char *)&entry->fraghdr);

        entry->vector[0].iov_base = (char *)&entry->fraghdr;
        entry->vector[0].iov_len = sizeof (entry->fraghdr);
        entry->count = 1;

		//把msg信息拷贝入entry
        if (msg->rpchdr != NULL) {
                memcpy (&entry->vector[1], msg->rpchdr,
                        sizeof (struct iovec) * msg->rpchdrcount);
                entry->count += msg->rpchdrcount;
        }

        if (msg->proghdr != NULL) {
                memcpy (&entry->vector[entry->count], msg->proghdr,
                        sizeof (struct iovec) * msg->proghdrcount);
                entry->count += msg->proghdrcount;
        }

        if (msg->progpayload != NULL) {
                memcpy (&entry->vector[entry->count], msg->progpayload,
                        sizeof (struct iovec) * msg->progpayloadcount);
                entry->count += msg->progpayloadcount;
        }

        entry->pending_vector = entry->vector;
        entry->pending_count  = entry->count;

        if (msg->iobref != NULL)
                entry->iobref = iobref_ref (msg->iobref);

        INIT_LIST_HEAD (&entry->list);

        return entry;
}

//有用
static void
dpdk_ioq_entry_free (struct dpdk_ioq *entry)
{
        list_del_init (&entry->list);
        if (entry->iobref)
                iobref_unref (entry->iobref);

        /* TODO: use mem-pool */
        GF_FREE (entry);
        return;
}

//有用
static void
dpdk_ioq_flush (rpc_transport_t *this)
{
        dpdk_private_t *priv = NULL;
        struct dpdk_ioq       *entry = NULL;

        priv = this->private;

        while (!list_empty (&priv->ioq)) {
                entry = priv->ioq_next;
                dpdk_ioq_entry_free (entry);
        }
        return;
}

//有用
static int
dpdk_ioq_churn_entry (struct thread_context *ctx, rpc_transport_t *this, struct dpdk_ioq *entry) //依次提交传输层的请求
{
        int               ret = -1;

        ret = dpdk_rwv (ctx, this, entry->pending_vector, //待发送数据已经封装好了
                               entry->pending_count,
                               &entry->pending_vector,
                               &entry->pending_count, NULL, 1);

        if (ret == 0) {
                /* current entry was completely written */
                GF_ASSERT (entry->pending_count == 0);
                dpdk_ioq_entry_free (entry);
	}
        return ret;
}

//有用
static int
dpdk_ioq_churn (struct thread_context *ctx, rpc_transport_t *this)
{
       dpdk_private_t *priv = this->private;
        int               ret = 0;
        struct dpdk_ioq       *entry = NULL;
	struct mtcp_epoll_event ev;

        while (!list_empty (&priv->ioq)) {
                /* pick next entry */
                entry = priv->ioq_next; //发送数据已经在priv的ioq中了
                ret = dpdk_ioq_churn_entry (ctx, this, entry);
                if (ret != 0)
                        break;
        }

	// TODO
        if (list_empty (&priv->ioq)) { //不再对发送事件感兴趣，把该sock的POLLOUT选项关闭
                /* all pending writes done, not interested in POLLOUT */
                //priv->idx = event_select_on (this->ctx->event_pool,
                //                             priv->sock, priv->idx, -1, 0);
		ev.events = MTCP_EPOLLIN; //清除EPOLLOUT
		ev.data.sockid = priv->sock;
		//rpc_transport_t *rt = ctx->trans_info[priv->sock];
		//rt = this;
		mtcp_epoll_ctl (ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, priv->sock, &ev);
        }
        return ret;
}

//有用
static int
dpdk_read_accepted_successful_reply (struct thread_context *ctx, rpc_transport_t *this)
{
        dpdk_private_t *priv              = NULL;
        int               ret               = 0;
        struct iobuf     *iobuf             = NULL;
        gfs3_read_rsp     read_rsp          = {0, };
        ssize_t           size              = 0;
        ssize_t           default_read_size = 0;
        XDR               xdr;
        struct gf_dpdk_incoming      *in         = NULL;
        struct gf_dpdk_incoming_frag *frag       = NULL;

        priv = this->private;

        /* used to reduce the indirection */
        in = &priv->incoming;
        frag = &in->frag;

        switch (frag->call_body.reply.accepted_success_state) {

        case SP_STATE_ACCEPTED_SUCCESS_REPLY_INIT:
                default_read_size = xdr_sizeof ((xdrproc_t) xdr_gfs3_read_rsp,
                                                &read_rsp);

                /* We need to store the current base address because we will
                 * need it after a partial read. */
                in->proghdr_base_addr = frag->fragcurrent;

                dpdk_proto_init_pending (ctx, priv, default_read_size);

                frag->call_body.reply.accepted_success_state
                        = SP_STATE_READING_PROC_HEADER;

                /* fall through */

        case SP_STATE_READING_PROC_HEADER:
                dpdk_proto_read (ctx, priv, ret);

                /* there can be 'xdata' in read response, figure it out */
                default_read_size = frag->fragcurrent - in->proghdr_base_addr;
                xdrmem_create (&xdr, in->proghdr_base_addr, default_read_size,
                               XDR_DECODE);

                /* This will fail if there is xdata sent from server, if not,
                   well and good, we don't need to worry about  */
                xdr_gfs3_read_rsp (&xdr, &read_rsp);

                free (read_rsp.xdata.xdata_val);

                /* need to round off to proper roof (%4), as XDR packing pads
                   the end of opaque object with '0' */
                size = roof (read_rsp.xdata.xdata_len, 4);

                if (!size) {
                        frag->call_body.reply.accepted_success_state
                                = SP_STATE_READ_PROC_OPAQUE;
                        goto read_proc_opaque;
                }

                dpdk_proto_init_pending (ctx, priv, size);

                frag->call_body.reply.accepted_success_state
                        = SP_STATE_READING_PROC_OPAQUE;

        case SP_STATE_READING_PROC_OPAQUE:
                dpdk_proto_read (ctx, priv, ret);

                frag->call_body.reply.accepted_success_state
                        = SP_STATE_READ_PROC_OPAQUE;

        case SP_STATE_READ_PROC_OPAQUE:
        read_proc_opaque:
                if (in->payload_vector.iov_base == NULL) {

                        size = (RPC_FRAGSIZE (in->fraghdr) - frag->bytes_read);

                        iobuf = iobuf_get2 (this->ctx->iobuf_pool, size);
                        if (iobuf == NULL) {
                                ret = -1;
                                goto out;
                        }

                        if (in->iobref == NULL) {
                                in->iobref = iobref_new ();
                                if (in->iobref == NULL) {
                                        ret = -1;
                                        iobuf_unref (iobuf);
                                        goto out;
                                }
                        }

                        iobref_add (in->iobref, iobuf);
                        iobuf_unref (iobuf);

                        in->payload_vector.iov_base = iobuf_ptr (iobuf);

                        in->payload_vector.iov_len = size;
                }

                frag->fragcurrent = in->payload_vector.iov_base;

                frag->call_body.reply.accepted_success_state
                        = SP_STATE_READ_PROC_HEADER;

                /* fall through */

        case SP_STATE_READ_PROC_HEADER:
                /* now read the entire remaining msg into new iobuf */
                ret = dpdk_read_simple_msg (ctx, this);
                if ((ret == -1)
                    || ((ret == 0) && RPC_LASTFRAG (in->fraghdr))) {
                        frag->call_body.reply.accepted_success_state
                                = SP_STATE_ACCEPTED_SUCCESS_REPLY_INIT;
                }

                break;
        }

out:
        return ret;
}

#define rpc_reply_verflen_addr(fragcurrent) ((char *)fragcurrent - 4)
#define rpc_reply_accept_status_addr(fragcurrent) ((char *)fragcurrent - 4)

//有用
static int
dpdk_read_accepted_reply (struct thread_context *ctx, rpc_transport_t *this)
{
        dpdk_private_t *priv           = NULL;
        int               ret            = -1;
        char             *buf            = NULL;
        uint32_t          verflen        = 0, len = 0;
        uint32_t          remaining_size = 0;
        struct gf_dpdk_incoming      *in         = NULL;
        struct gf_dpdk_incoming_frag *frag       = NULL;

        priv = this->private;
        /* used to reduce the indirection */
        in = &priv->incoming;
        frag = &in->frag;

        switch (frag->call_body.reply.accepted_state) {

        case SP_STATE_ACCEPTED_REPLY_INIT:
                dpdk_proto_init_pending (ctx, priv,
                                             RPC_AUTH_FLAVOUR_N_LENGTH_SIZE);

                frag->call_body.reply.accepted_state
                        = SP_STATE_READING_REPLY_VERFLEN;

                /* fall through */

        case SP_STATE_READING_REPLY_VERFLEN:
                dpdk_proto_read (ctx, priv, ret);

                frag->call_body.reply.accepted_state
                        = SP_STATE_READ_REPLY_VERFLEN;

                /* fall through */

        case SP_STATE_READ_REPLY_VERFLEN:
                buf = rpc_reply_verflen_addr (frag->fragcurrent);

                verflen = ntoh32 (*((uint32_t *) buf));

                /* also read accept status along with verf data */
                len = verflen + RPC_ACCEPT_STATUS_LEN;

                dpdk_proto_init_pending (ctx, priv, len);

                frag->call_body.reply.accepted_state
                        = SP_STATE_READING_REPLY_VERFBYTES;

                /* fall through */

        case SP_STATE_READING_REPLY_VERFBYTES:
                dpdk_proto_read (ctx, priv, ret);

                frag->call_body.reply.accepted_state
                        = SP_STATE_READ_REPLY_VERFBYTES;

                buf = rpc_reply_accept_status_addr (frag->fragcurrent);

                frag->call_body.reply.accept_status
                        = ntoh32 (*(uint32_t *) buf);

                /* fall through */

        case SP_STATE_READ_REPLY_VERFBYTES:

                if (frag->call_body.reply.accept_status
                    == SUCCESS) {
                        ret = dpdk_read_accepted_successful_reply (ctx, this);
                } else {
                        /* read entire remaining msg into buffer pointed to by
                         * fragcurrent
                         */
                        ret = dpdk_read_simple_msg (ctx, this);
                }

                remaining_size = RPC_FRAGSIZE (in->fraghdr)
                        - frag->bytes_read;

                if ((ret == -1)
                    || ((ret == 0) && (remaining_size == 0)
                        && (RPC_LASTFRAG (in->fraghdr)))) {
                        frag->call_body.reply.accepted_state
                                = SP_STATE_ACCEPTED_REPLY_INIT;
                }

                break;
        }

        return ret;
}

//有用
static int
dpdk_read_denied_reply (struct thread_context *ctx, rpc_transport_t *this)
{
        return dpdk_read_simple_msg (ctx, this);
}

//有用
static int
dpdk_connect_finish (struct thread_context *ctx, rpc_transport_t *this)
{
        int                   ret        = -1;
        dpdk_private_t     *priv       = NULL;
        rpc_transport_event_t event      = 0;
        char                  notify_rpc = 0;

        priv = this->private;

        pthread_mutex_lock (&priv->lock);
        {
                if (priv->connected != 0)
                        goto unlock;

		ret = dpdk_fill_inet6_inet_identifier (this,
						 &this->myinfo.sockaddr,
						 this->myinfo.sockaddr_len, 
						 this->myinfo.identifier);
		if (ret == -1) {
                	gf_log (this->name, GF_LOG_ERROR, "cannot fill inet/inet6 identifier for server!\n");
		}

		ret = dpdk_fill_inet6_inet_identifier (this,
						 &this->peerinfo.sockaddr,
						 this->peerinfo.sockaddr_len, 
						 this->peerinfo.identifier);
		if (ret == -1) {
                	gf_log (this->name, GF_LOG_ERROR, "cannot fill inet/inet6 identifier for client!\n");
		}

                ret = __dpdk_connect_finish (ctx, priv->sock);

                if (ret == -1 && errno == EINPROGRESS)
                        ret = 1;

                if (ret == -1 && errno != EINPROGRESS) {
                        if (!priv->connect_finish_log) {
                                gf_log (this->name, GF_LOG_ERROR,
                                        "connection to %s failed (%s)",
                                        this->peerinfo.identifier,
                                        strerror (errno));
                                priv->connect_finish_log = 1;
                        }
                        dpdk_disconnect (ctx, this);
                        goto unlock;
                }

                if (ret == 0) {
                        notify_rpc = 1;

                        this->myinfo.sockaddr_len =
                                sizeof (this->myinfo.sockaddr);

                        ret = getsockname (priv->sock,
                                           SA (&this->myinfo.sockaddr),
                                           &this->myinfo.sockaddr_len);
                        if (ret == -1) {
                                gf_log (this->name, GF_LOG_WARNING,
                                        "getsockname on (%d) failed (%s)",
                                        priv->sock, strerror (errno));
                               dpdk_disconnect (ctx, this);
                                event = RPC_TRANSPORT_DISCONNECT;
                                goto unlock;
                        }

                        priv->connected = 1;
                        priv->connect_finish_log = 0;
                        event = RPC_TRANSPORT_CONNECT;
                }
        }
unlock:
        pthread_mutex_unlock (&priv->lock);

        if (notify_rpc) {
                rpc_transport_notify (this, event, this);
        }

        return ret;
}

//有用
void*
dpdk_connect_error_cbk (void *opaque)
{
        dpdk_connect_error_state_t *arg;

        GF_ASSERT (opaque);

        arg = opaque;
        THIS = arg->this;

        rpc_transport_notify (arg->trans, RPC_TRANSPORT_DISCONNECT, arg->trans);

        if (arg->refd)
                rpc_transport_unref (arg->trans);

        GF_FREE (opaque);
        return NULL;
}

//有用
static int32_t
dpdk_submit_request (rpc_transport_t *this, rpc_transport_req_t *req) //客户端发送请求
{
        dpdk_private_t *priv = NULL;
        int               ret = -1;
        char              need_poll_out = 0;
        char              need_append = 1;
        struct dpdk_ioq       *entry = NULL;
	struct thread_context *ctx = NULL;
	struct mtcp_epoll_event ev;

        priv = this->private;
	ctx = priv->ctx;

        pthread_mutex_lock (&priv->lock);
        {
                if (priv->connected != 1) {
                        if (!priv->submit_log && !priv->connect_finish_log) {
                                gf_log (this->name, GF_LOG_INFO,
                                        "not connected (priv->connected = %d)",
                                        priv->connected);
                                priv->submit_log = 1;
                        }
                        goto unlock;
                }

                priv->submit_log = 0;
                entry = dpdk_ioq_new (this, &req->msg); //新建io请求队列, req拷贝进entry
                if (!entry)
                        goto unlock;

                if (list_empty (&priv->ioq)) {//判断io请求队列是否为空
                        ret = dpdk_ioq_churn_entry (ctx, this, entry); 

                        if (ret == 0) {
                                need_append = 0; //需要添加到entry列表
			}
                        if (ret > 0) {
                                need_poll_out = 1; //需要注册可写事件
			}
                }

                if (need_append) {
                        list_add_tail (&entry->list, &priv->ioq);
                        ret = 0;
                }
                if (need_poll_out) { //poll out
                        /* first entry to wait. continue writing on POLLOUT */
			//注册事件，并且更改为poll_out模式
                        //priv->idx = event_select_on (ctx->event_pool, priv->sock, priv->idx, -1, 1);
			ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT; //mTCP没有实现seletc部分暂时in/out都设置
			ev.data.sockid = priv->sock;
		//	rpc_transport_t *rt = ctx->trans_info[priv->sock];
		//    rt = this;
			mtcp_epoll_ctl (ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, priv->sock, &ev);
                }
        }
unlock:
        pthread_mutex_unlock (&priv->lock);

        return ret;
}

//有用
static int32_t
dpdk_submit_reply (rpc_transport_t *this, rpc_transport_reply_t *reply)
{
        dpdk_private_t *priv = NULL;
        int               ret = -1;
        char              need_poll_out = 0;
        char              need_append = 1;
        struct dpdk_ioq       *entry = NULL;
	struct thread_context *ctx = NULL;
	struct mtcp_epoll_event ev;

        priv = this->private;
	ctx = priv->ctx;

        pthread_mutex_lock (&priv->lock);
        {
                if (priv->connected != 1) {
                        if (!priv->submit_log && !priv->connect_finish_log) {
                                gf_log (this->name, GF_LOG_INFO,
                                        "not connected (priv->connected = %d)",
                                        priv->connected);
                                priv->submit_log = 1;
                        }
                        goto unlock;
                }

                priv->submit_log = 0;
                entry = dpdk_ioq_new (this, &reply->msg);
                if (!entry)
                        goto unlock;

                if (list_empty (&priv->ioq)) {
                        ret = dpdk_ioq_churn_entry (ctx, this, entry);

                        if (ret == 0) {
                                need_append = 0;
			}
                        if (ret > 0) {
                                need_poll_out = 1;
			}
                }

                if (need_append) {
                        list_add_tail (&entry->list, &priv->ioq);
                        ret = 0;
                }
                if (need_poll_out) {
			ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT; 
			ev.data.sockid = priv->sock;
		//	rpc_transport_t *rt = ctx->trans_info[priv->sock];
           // rt = this;
			mtcp_epoll_ctl (ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_MOD, priv->sock, &ev);
                }
        }
unlock:
        pthread_mutex_unlock (&priv->lock);

        return ret;
}



static int
dpdk_read_simple_reply (struct thread_context *ctx, rpc_transport_t *this)
{
        return dpdk_read_simple_msg (ctx, this);
}


#define rpc_reply_status_addr(fragcurrent) ((char *)fragcurrent - 4)

static int
dpdk_read_vectored_reply (struct thread_context *ctx, rpc_transport_t *this)
{
        dpdk_private_t *priv           = NULL;
        int               ret            = 0;
        char             *buf            = NULL;
        uint32_t          remaining_size = 0;
        struct gf_dpdk_incoming      *in         = NULL;
        struct gf_dpdk_incoming_frag *frag       = NULL;

        priv = this->private;
        in = &priv->incoming;
        frag = &in->frag;

        switch (frag->call_body.reply.status_state) {

        case SP_STATE_ACCEPTED_REPLY_INIT:
                dpdk_proto_init_pending (ctx, priv, RPC_REPLY_STATUS_SIZE);

                frag->call_body.reply.status_state
                        = SP_STATE_READING_REPLY_STATUS;

                /* fall through */

        case SP_STATE_READING_REPLY_STATUS:
                dpdk_proto_read (ctx, priv, ret);

                buf = rpc_reply_status_addr (frag->fragcurrent);

                frag->call_body.reply.accept_status
                        = ntoh32 (*((uint32_t *) buf));

                frag->call_body.reply.status_state
                        = SP_STATE_READ_REPLY_STATUS;

                /* fall through */

        case SP_STATE_READ_REPLY_STATUS:
                if (frag->call_body.reply.accept_status == MSG_ACCEPTED) {
                        ret = dpdk_read_accepted_reply (ctx, this);
                } else {
                        ret = dpdk_read_denied_reply (ctx, this);
                }

                remaining_size = RPC_FRAGSIZE (in->fraghdr) - frag->bytes_read;

                if ((ret == -1)
                    || ((ret == 0) && (remaining_size == 0)
                        && (RPC_LASTFRAG (in->fraghdr)))) {
                        frag->call_body.reply.status_state
                                = SP_STATE_VECTORED_REPLY_STATUS_INIT;
                        in->payload_vector.iov_len
                                = (unsigned long)frag->fragcurrent
                                - (unsigned long)in->payload_vector.iov_base;
                }
                break;
        }
        return ret;
}


#define rpc_xid_addr(buf) (buf)

static int
dpdk_read_reply (struct thread_context *ctx, rpc_transport_t *this)
{
        dpdk_private_t   *priv         = NULL;
        char               *buf          = NULL;
        int32_t             ret          = -1;
        rpc_request_info_t *request_info = NULL;
        char                map_xid      = 0;
        struct gf_dpdk_incoming      *in         = NULL;
        struct gf_dpdk_incoming_frag *frag       = NULL;

        priv = this->private;
        in = &priv->incoming;
        frag = &in->frag;

        buf = rpc_xid_addr (iobuf_ptr (in->iobuf));

        if (in->request_info == NULL) {
                in->request_info = GF_CALLOC (1, sizeof (*request_info),
                                              gf_common_mt_rpc_trans_reqinfo_t);
                if (in->request_info == NULL) {
                        goto out;
                }

                map_xid = 1;
        }

        request_info = in->request_info;

        if (map_xid) {
                request_info->xid = ntoh32 (*((uint32_t *) buf));

                /* release priv->lock, so as to avoid deadlock b/w conn->lock
                 * and priv->lock, since we are doing an upcall here.
                 */
                frag->state = SP_STATE_NOTIFYING_XID;
                pthread_mutex_unlock (&priv->lock);
                {
                        ret = rpc_transport_notify (this,
                                                    RPC_TRANSPORT_MAP_XID_REQUEST,
                                                    in->request_info);
                }
                pthread_mutex_lock (&priv->lock);

                /* Transition back to externally visible state. */
                frag->state = SP_STATE_READ_MSGTYPE;

                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "notify for event MAP_XID failed for %s",
                                this->peerinfo.identifier);
                        goto out;
                }
        }

        if ((request_info->prognum == GLUSTER_FOP_PROGRAM)
            && (request_info->procnum == GF_FOP_READ)) {
                if (map_xid && request_info->rsp.rsp_payload_count != 0) {
                        in->iobref = iobref_ref (request_info->rsp.rsp_iobref);
                        in->payload_vector = *request_info->rsp.rsp_payload;
                }

                ret = dpdk_read_vectored_reply (ctx, this);
        } else {
                ret = dpdk_read_simple_reply (ctx, this);
        }
out:
        return ret;
}


#define rpc_cred_addr(buf) (buf + RPC_MSGTYPE_SIZE + RPC_CALL_BODY_SIZE - 4)

#define rpc_verf_addr(fragcurrent) (fragcurrent - 4)

#define rpc_msgtype_addr(buf) (buf + 4)

#define rpc_prognum_addr(buf) (buf + RPC_MSGTYPE_SIZE + 4)
#define rpc_progver_addr(buf) (buf + RPC_MSGTYPE_SIZE + 8)
#define rpc_procnum_addr(buf) (buf + RPC_MSGTYPE_SIZE + 12)


static int
dpdk_read_simple_request (struct thread_context *ctx, rpc_transport_t *this)
{
        return dpdk_read_simple_msg (ctx, this);
}


static int
dpdk_read_simple_msg (struct thread_context *ctx, rpc_transport_t *this)
{
        int                           ret            = 0;
        uint32_t                      remaining_size = 0;
        size_t                        bytes_read     = 0;
        dpdk_private_t             *priv           = NULL;
        struct gf_dpdk_incoming      *in             = NULL;
        struct gf_dpdk_incoming_frag *frag           = NULL;

        priv = this->private;
        in = &priv->incoming;
        frag = &in->frag;

        switch (frag->simple_state) {

        case SP_STATE_SIMPLE_MSG_INIT:
                remaining_size = RPC_FRAGSIZE (in->fraghdr) - frag->bytes_read;

                dpdk_proto_init_pending (ctx, priv, remaining_size);

                frag->simple_state = SP_STATE_READING_SIMPLE_MSG;

                /* fall through */

        case SP_STATE_READING_SIMPLE_MSG:
                ret = 0;

                remaining_size = RPC_FRAGSIZE (in->fraghdr) - frag->bytes_read;

                if (remaining_size > 0) {
                        ret = dpdk_rwv (ctx, this,
                                              in->pending_vector, 1,
                                              &in->pending_vector,
                                              &in->pending_count,
                                              &bytes_read, 0);
                }

                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "reading from socket failed. Error (%s), "
                                "peer (%s)", strerror (errno),
                                this->peerinfo.identifier);
                        break;
                }

                frag->bytes_read += bytes_read;
                frag->fragcurrent += bytes_read;

                if (ret > 0) {
                        gf_log (this->name, GF_LOG_TRACE,
                                "partial read on non-blocking socket.");
                        break;
                }

                if (ret == 0) {
                        frag->simple_state =  SP_STATE_SIMPLE_MSG_INIT;
                }
        }

        return ret;
}


static int
dpdk_read_vectored_request (struct thread_context *ctx, rpc_transport_t *this, rpcsvc_vector_sizer vector_sizer)
{
        dpdk_private_t *priv                   = NULL;
        int               ret                    = 0;
        uint32_t          credlen                = 0, verflen = 0;
        char             *addr                   = NULL;
        struct iobuf     *iobuf                  = NULL;
        uint32_t          remaining_size         = 0;
        ssize_t           readsize               = 0;
        size_t            size                   = 0;
        struct gf_dpdk_incoming      *in         = NULL;
        struct gf_dpdk_incoming_frag *frag       = NULL;
        dpdk_sp_rpcfrag_request_state_t   *request    = NULL;

        priv = this->private;

        /* used to reduce the indirection */
        in = &priv->incoming;
        frag = &in->frag;
        request = &frag->call_body.request;

        switch (request->vector_state) {
        case SP_STATE_VECTORED_REQUEST_INIT:
                request->vector_sizer_state = 0;

                addr = rpc_cred_addr (iobuf_ptr (in->iobuf));

                /* also read verf flavour and verflen */
                credlen = ntoh32 (*((uint32_t *)addr))
                        +  RPC_AUTH_FLAVOUR_N_LENGTH_SIZE;

                dpdk_proto_init_pending (ctx, priv, credlen);

                request->vector_state = SP_STATE_READING_CREDBYTES;

                /* fall through */

        case SP_STATE_READING_CREDBYTES:
                dpdk_proto_read (ctx, priv, ret);

                request->vector_state = SP_STATE_READ_CREDBYTES;

                /* fall through */

        case SP_STATE_READ_CREDBYTES:
                addr = rpc_verf_addr (frag->fragcurrent);
                verflen = ntoh32 (*((uint32_t *)addr));

                if (verflen == 0) {
                        request->vector_state = SP_STATE_READ_VERFBYTES;
                        goto sp_state_read_verfbytes;
                }
                dpdk_proto_init_pending (ctx, priv, verflen);

                request->vector_state = SP_STATE_READING_VERFBYTES;

                /* fall through */

        case SP_STATE_READING_VERFBYTES:
                dpdk_proto_read (ctx, priv, ret);

                request->vector_state = SP_STATE_READ_VERFBYTES;

                /* fall through */

        case SP_STATE_READ_VERFBYTES:
sp_state_read_verfbytes:
		/* set the base_addr 'persistently' across multiple calls
		   into the state machine */
                in->proghdr_base_addr = frag->fragcurrent;

                request->vector_sizer_state =
                        vector_sizer (request->vector_sizer_state,
                                      &readsize, in->proghdr_base_addr,
                                      frag->fragcurrent);
                dpdk_proto_init_pending (ctx, priv, readsize);

                request->vector_state = SP_STATE_READING_PROGHDR;

                /* fall through */

        case SP_STATE_READING_PROGHDR:
                dpdk_proto_read (ctx, priv, ret);

		request->vector_state =	SP_STATE_READ_PROGHDR;

		/* fall through */

	case SP_STATE_READ_PROGHDR:
sp_state_read_proghdr:
                request->vector_sizer_state =
                        vector_sizer (request->vector_sizer_state,
                                      &readsize, in->proghdr_base_addr,
                                      frag->fragcurrent);
                if (readsize == 0) {
                        request->vector_state = SP_STATE_READ_PROGHDR_XDATA;
			goto sp_state_read_proghdr_xdata;
                }

		dpdk_proto_init_pending (ctx, priv, readsize);

                request->vector_state =	SP_STATE_READING_PROGHDR_XDATA;

		/* fall through */

	case SP_STATE_READING_PROGHDR_XDATA:
		dpdk_proto_read (ctx, priv, ret);

		request->vector_state =	SP_STATE_READ_PROGHDR;
		/* check if the vector_sizer() has more to say */
		goto sp_state_read_proghdr;

        case SP_STATE_READ_PROGHDR_XDATA:
sp_state_read_proghdr_xdata:
                if (in->payload_vector.iov_base == NULL) {

                        size = RPC_FRAGSIZE (in->fraghdr) - frag->bytes_read;
                        iobuf = iobuf_get2 (this->ctx->iobuf_pool, size);
                        if (!iobuf) {
                                ret = -1;
                                break;
                        }

                        if (in->iobref == NULL) {
                                in->iobref = iobref_new ();
                                if (in->iobref == NULL) {
                                        ret = -1;
                                        iobuf_unref (iobuf);
                                        break;
                                }
                        }

                        iobref_add (in->iobref, iobuf);
                        iobuf_unref (iobuf);

                        in->payload_vector.iov_base = iobuf_ptr (iobuf);

                        frag->fragcurrent = iobuf_ptr (iobuf);
                }

                request->vector_state = SP_STATE_READING_PROG;

                /* fall through */

        case SP_STATE_READING_PROG:
                /* now read the remaining rpc msg into buffer pointed by
                 * fragcurrent
                 */

                ret = dpdk_read_simple_msg (ctx, this);

                remaining_size = RPC_FRAGSIZE (in->fraghdr) - frag->bytes_read;

                if ((ret == -1) ||
                    ((ret == 0) && (remaining_size == 0)
                     && RPC_LASTFRAG (in->fraghdr))) {
                        request->vector_state = SP_STATE_VECTORED_REQUEST_INIT;
                        in->payload_vector.iov_len
                                = ((unsigned long)frag->fragcurrent
                                   - (unsigned long)in->payload_vector.iov_base);
                }
                break;
        }

        return ret;
}


static int
dpdk_read_request (struct thread_context *ctx, rpc_transport_t *this)
{
        dpdk_private_t *priv               = NULL;
        uint32_t          prognum            = 0, procnum = 0, progver = 0;
        uint32_t          remaining_size     = 0;
        int               ret                = -1;
        char             *buf                = NULL;
        rpcsvc_vector_sizer     vector_sizer = NULL;
        struct gf_dpdk_incoming      *in         = NULL;
        struct gf_dpdk_incoming_frag *frag       = NULL;
        dpdk_sp_rpcfrag_request_state_t   *request    = NULL;

        priv = this->private;

        /* used to reduce the indirection */
        in = &priv->incoming;
        frag = &in->frag;
        request = &frag->call_body.request;

        switch (request->header_state) {

        case SP_STATE_REQUEST_HEADER_INIT:

               dpdk_proto_init_pending (ctx, priv, RPC_CALL_BODY_SIZE);

                request->header_state = SP_STATE_READING_RPCHDR1;

                /* fall through */

        case SP_STATE_READING_RPCHDR1:
                dpdk_proto_read (ctx, priv, ret);

                request->header_state = SP_STATE_READ_RPCHDR1;

                /* fall through */

        case SP_STATE_READ_RPCHDR1:
                buf = rpc_prognum_addr (iobuf_ptr (in->iobuf));
                prognum = ntoh32 (*((uint32_t *)buf));

                buf = rpc_progver_addr (iobuf_ptr (in->iobuf));
                progver = ntoh32 (*((uint32_t *)buf));

                buf = rpc_procnum_addr (iobuf_ptr (in->iobuf));
                procnum = ntoh32 (*((uint32_t *)buf));

                if (priv->is_server) {
                        /* this check is needed as rpcsvc and rpc-clnt
                         * actor structures are not same */
                        vector_sizer =
                                rpcsvc_get_program_vector_sizer ((rpcsvc_t *)this->mydata,
                                                                 prognum, progver, procnum);
                }

                if (vector_sizer) {
                        ret = dpdk_read_vectored_request (ctx, this, vector_sizer);
                } else {
                        ret = dpdk_read_simple_request (ctx, this);
                }

                remaining_size = RPC_FRAGSIZE (in->fraghdr) - frag->bytes_read;

                if ((ret == -1)
                    || ((ret == 0)
                        && (remaining_size == 0)
                        && (RPC_LASTFRAG (in->fraghdr)))) {
                        request->header_state = SP_STATE_REQUEST_HEADER_INIT;
                }

                break;
        }
        return ret;
}




/* returns the number of bytes yet to be read in a fragment */
static int
dpdk_read_frag (struct thread_context *ctx, rpc_transport_t *this)
{
        dpdk_private_t *priv           = NULL;
        int32_t           ret            = 0;
        char             *buf            = NULL;
        uint32_t          remaining_size = 0;
        struct gf_dpdk_incoming      *in         = NULL;
        struct gf_dpdk_incoming_frag *frag       = NULL;

        priv = this->private;
        in = &priv->incoming;
        frag = &in->frag;

        switch (frag->state) { //是前一步解析头部的后续
        case SP_STATE_NADA:
                dpdk_proto_init_pending (ctx, priv, RPC_MSGTYPE_SIZE);

                frag->state = SP_STATE_READING_MSGTYPE;

                /* fall through */

        case SP_STATE_READING_MSGTYPE:
                dpdk_proto_read (ctx, priv, ret);

                frag->state = SP_STATE_READ_MSGTYPE;
                /* fall through */

        case SP_STATE_READ_MSGTYPE:
                buf = rpc_msgtype_addr (iobuf_ptr (in->iobuf));
                in->msg_type = ntoh32 (*((uint32_t *)buf));

                if (in->msg_type == CALL) {
                        ret = dpdk_read_request (ctx, this);
                } else if (in->msg_type == REPLY) {
                        ret = dpdk_read_reply (ctx, this);
                } else if (in->msg_type == (msg_type_t) GF_UNIVERSAL_ANSWER) {
                        gf_log ("rpc", GF_LOG_ERROR,
                                "older version of protocol/process trying to "
                                "connect from %s. use newer version on that node",
                                this->peerinfo.identifier);
                } else {
                        gf_log ("rpc", GF_LOG_ERROR,
                                "wrong MSG-TYPE (%d) received from %s",
                                in->msg_type,
                                this->peerinfo.identifier);
                        ret = -1;
                }

                remaining_size = RPC_FRAGSIZE (in->fraghdr) - frag->bytes_read;

                if ((ret == -1)
                    || ((ret == 0) && (remaining_size == 0)
                        && (RPC_LASTFRAG (in->fraghdr)))) {
                     /* frag->state = SP_STATE_NADA; */
                        frag->state = SP_STATE_RPCFRAG_INIT;
                }

                break;

        case SP_STATE_NOTIFYING_XID:
                /* Another epoll thread is notifying higher layers
                 *of reply's xid. */
                errno = EAGAIN;
                return -1;
                break;

        }

        return ret;
}


static int
dpdk_cached_read (struct thread_context *ctx, rpc_transport_t *this, struct iovec *opvector, int opcount)
{
	dpdk_private_t   *priv = NULL;
	struct gf_dpdk_incoming *in = NULL;
	int                 req_len = -1;
	int                 ret = -1;

	priv = this->private;
	in = &priv->incoming;
	req_len = iov_length (opvector, opcount);

	if (in->record_state == SP_STATE_READING_FRAGHDR) {
		in->ra_read = 0;
		in->ra_served = 0;
		in->ra_max = 0;
		in->ra_buf = NULL;
		goto uncached;
	}

	if (!in->ra_max) {
		/* first call after passing SP_STATE_READING_FRAGHDR */
		in->ra_max = min (RPC_FRAGSIZE (in->fraghdr), GF_DPDK_RA_MAX);
		/* Note that the in->iobuf is the primary iobuf into which
		   headers are read into, and in->frag.fragcurrent points to
 		   some position in the buffer. By using this itself as our
		   read-ahead cache, we can avoid memory copies in iov_load
		*/
		in->ra_buf = in->frag.fragcurrent;
	}

	/* fill read-ahead */
	if (in->ra_read < in->ra_max) {
		struct iovec iov = {0, };
		iov.iov_base = &in->ra_buf[in->ra_read];
		iov.iov_len = in->ra_max - in->ra_read;
		int sock = ((dpdk_private_t *)this->private)->sock;
		ret = mtcp_readv (ctx->mctx, sock, &iov, 1);
		if (ret > 0)
			in->ra_read += ret;

		/* we proceed to test if there is still cached data to
		   be served even if readahead could not progress */
	}

	/* serve cached */
	if (in->ra_served < in->ra_read) {
		ret = iov_load (opvector, opcount, &in->ra_buf[in->ra_served],
				min (req_len, (in->ra_read - in->ra_served)));

		in->ra_served += ret;
		/* Do not read uncached and cached in the same call */
		goto out;
	}

	if (in->ra_read < in->ra_max)
		/* If there was no cached data to be served, (and we are
		   guaranteed to have already performed an attempt to progress
		   readahead above), and we have not yet read out the full
		   readahead capacity, then bail out for now without doing
		   the uncached read below (as that will overtake future cached
		   read)
		*/
		goto out;
uncached:
	ret = mtcp_readv (ctx->mctx, ((dpdk_private_t *)this->private)->sock, opvector, opcount);

out:
	return ret;
}


/*
 * return value:
 *   0 = success (completed)
 *  -1 = error
 * > 0 = incomplete
 */

static int
dpdk_rwv (struct thread_context *ctx, rpc_transport_t *this, struct iovec *vector, 
			int count, struct iovec **pending_vector, int *pending_count, 
			size_t *bytes, int write) //write = 0 读; write = 1 写
{
        dpdk_private_t *priv = NULL;
        int               sock = -1;
        int               ret = -1;
        struct iovec     *opvector = NULL;
        int               opcount = 0;
        int               moved = 0;

        priv = this->private;
        sock = priv->sock;

        opvector = vector;
        opcount  = count;

        if (bytes != NULL) {
                *bytes = 0;
        }

        while (opcount > 0) {
                if (opvector->iov_len == 0) {
                        gf_log(this->name,GF_LOG_DEBUG, "would have passed zero length to read/write");
                        ++opvector;
                        --opcount;
                        continue;
                } else if (write) { // 1. send 
			
				ret = mtcp_writev (ctx->mctx, sock, opvector, IOV_MIN(opcount)); //modified
				if (ret == 0 || (ret == -1 && errno == EAGAIN)) { 
                                /* done for now */
                                break;
                        }
                        this->total_bytes_write += ret;
                } else {// 2. recv
			ret = dpdk_cached_read (ctx, this, opvector, opcount);	//modified

			if (ret == 0) {
				gf_log(this->name,GF_LOG_DEBUG,"EOF on socket");
				errno = ENODATA;
				ret = -1;
			}
                        if (ret == -1 && errno == EAGAIN) {
                                /* done for now */
                                break;
                        }
                        this->total_bytes_read += ret;
                }

                if (ret == 0) {
                        /* Mostly due to 'umount' in client */

                        gf_log (this->name, GF_LOG_DEBUG,
                                "EOF from peer %s", this->peerinfo.identifier);
                        opcount = -1;
                        errno = ENOTCONN;
                        break;
                }
                if (ret == -1) {
                        if (errno == EINTR)
                                continue;
                        opcount = -1;
                        break;
                }

                if (bytes != NULL) {
                        *bytes += ret;
                }

                moved = 0;

                while (moved < ret) {
                        if (!opcount) {
                                gf_log(this->name,GF_LOG_DEBUG,
                                       "ran out of iov, moved %d/%d",
                                       moved, ret);
                                goto ran_out;
                        }
                        if (!opvector[0].iov_len) {
                                opvector++;
                                opcount--;
                                continue;
                        }
                        if ((ret - moved) >= opvector[0].iov_len) {
                                moved += opvector[0].iov_len;
                                opvector++;
                                opcount--;
                        } else {
                                opvector[0].iov_len -= (ret - moved);
                                opvector[0].iov_base += (ret - moved);
                                moved += (ret - moved);
                        }
                }
        }

ran_out:

        if (pending_vector)
                *pending_vector = opvector;

        if (pending_count)
                *pending_count = opcount;

        return opcount;
}

static void
dpdk_reset_priv (dpdk_private_t *priv)
{
        struct gf_dpdk_incoming   *in    = NULL;

        /* used to reduce the indirection */
        in = &priv->incoming;

        if (in->iobref) {
                iobref_unref (in->iobref);
                in->iobref = NULL;
        }

        if (in->iobuf) {
                iobuf_unref (in->iobuf);
                in->iobuf = NULL;
        }

        if (in->request_info != NULL) {
                GF_FREE (in->request_info);
                in->request_info = NULL;
        }

        memset (&in->payload_vector, 0,
                sizeof (in->payload_vector));
}


static int
dpdk_proto_state_machine (struct thread_context *ctx, rpc_transport_t *this, rpc_transport_pollin_t **pollin)
{

        int               ret    = -1;
        dpdk_private_t *priv   = NULL;
        struct iobuf     *iobuf  = NULL;
        struct iobref    *iobref = NULL;
        struct iovec      vector[2];
        struct gf_dpdk_incoming      *in         = NULL; //不需要改名吗？
        struct gf_dpdk_incoming_frag *frag       = NULL;

        priv = this->private;
        /* used to reduce the indirection */
	// 此时消息还未曾读入priv->incoming中
        in = &priv->incoming; 
        frag = &in->frag;

        while (in->record_state != SP_STATE_COMPLETE) { //从头往下执行状态机，知道rpc记录完成服务请求为止
                switch (in->record_state) {
                case SP_STATE_NADA: //开始状态
                        in->total_bytes_read = 0;
                        in->payload_vector.iov_len = 0;

                        in->pending_vector = in->vector;
                        in->pending_vector->iov_base =  &in->fraghdr;

                        in->pending_vector->iov_len  = sizeof (in->fraghdr);

                        in->record_state = SP_STATE_READING_FRAGHDR;

                        /* fall through */
                case SP_STATE_READING_FRAGHDR://读取头部信息，把socket独到的信息存入incoming
                        ret = dpdk_rwv (ctx, this, in->pending_vector, 1,//修改
                                              &in->pending_vector,
                                              &in->pending_count,
                                              NULL, 0);
                        if (ret == -1)
                                goto out;

                        if (ret > 0) {
                                gf_log (this->name, GF_LOG_TRACE, "partial fragment header read");
                                goto out;
                        }

                        if (ret == 0) {
                                in->record_state = SP_STATE_READ_FRAGHDR;
                        }

                        /* fall through */
                case SP_STATE_READ_FRAGHDR://处理已经读取的头部信息

                        in->fraghdr = ntoh32 (in->fraghdr);
                        in->total_bytes_read += RPC_FRAGSIZE(in->fraghdr);

                        if (in->total_bytes_read >= GF_UNIT_GB) {
                                ret = -ENOMEM;
                                goto out;
                        }

                        iobuf = iobuf_get2 (this->ctx->iobuf_pool, //!!!从当前glusterfs的ctx获取一块iobuf存储帧头（不可改？）
                                            (in->total_bytes_read +
                                             sizeof (in->fraghdr)));
                        if (!iobuf) {
                                ret = -ENOMEM;
                                goto out;
                        }

                        if (in->iobuf == NULL) {
                            /* first fragment */
                            frag->fragcurrent = iobuf_ptr (iobuf);
                        } else {
                            /* second or further fragment */
                            memcpy(iobuf_ptr (iobuf), iobuf_ptr (in->iobuf),
                               in->total_bytes_read - RPC_FRAGSIZE(in->fraghdr));
                            iobuf_unref (in->iobuf);
                            frag->fragcurrent = (char *) iobuf_ptr (iobuf) +
                                in->total_bytes_read - RPC_FRAGSIZE(in->fraghdr);
                            frag->pending_vector->iov_base = frag->fragcurrent;
                            in->pending_vector = frag->pending_vector;
                        }

                        in->iobuf = iobuf;
                        in->iobuf_size = 0;
                        in->record_state = SP_STATE_READING_FRAG;
                        /* fall through */

                case SP_STATE_READING_FRAG://读取帧信息
                        ret = dpdk_read_frag (ctx, this);//修改

                        if ((ret == -1) ||
                            (frag->bytes_read != RPC_FRAGSIZE (in->fraghdr))) {
                                goto out;
                        }

                        frag->bytes_read = 0;

                        if (!RPC_LASTFRAG (in->fraghdr)) { //是否已经读取到了最后一个帧。不是，从头开始
                                in->pending_vector = in->vector;
                                in->pending_vector->iov_base = &in->fraghdr;
                                in->pending_vector->iov_len = sizeof(in->fraghdr);
                                in->record_state = SP_STATE_READING_FRAGHDR;
                                break;
                        }

                        /* we've read the entire rpc record, notify the
                         * upper layers.
                         */
                        if (pollin != NULL) {
                                int count = 0;
                                in->iobuf_size = (in->total_bytes_read -
                                                  in->payload_vector.iov_len);

                                memset (vector, 0, sizeof (vector));

                                if (in->iobref == NULL) {
                                        in->iobref = iobref_new ();
                                        if (in->iobref == NULL) {
                                                ret = -1;
                                                goto out;
                                        }
                                }

                                vector[count].iov_base = iobuf_ptr (in->iobuf);
                                vector[count].iov_len = in->iobuf_size;

                                iobref = in->iobref;

                                count++;

                                if (in->payload_vector.iov_base != NULL) {
                                        vector[count] = in->payload_vector;
                                        count++;
                                }
				//新建一个传输层可取对象
                                *pollin = rpc_transport_pollin_alloc (this,
                                                                      vector,
                                                                      count,
                                                                      in->iobuf,
                                                                      iobref,
                                                                      in->request_info);
                                iobuf_unref (in->iobuf);
                                in->iobuf = NULL;

                                if (*pollin == NULL) {
                                        gf_log (this->name, GF_LOG_WARNING, "transport pollin allocation failed");
                                        ret = -1;
                                        goto out;
                                }
                                if (in->msg_type == REPLY) //什么样的消息类型是REPLY？？
                                        (*pollin)->is_reply = 1;

                                in->request_info = NULL;
                        }
                        in->record_state = SP_STATE_COMPLETE;
                        break;

                case SP_STATE_COMPLETE: //结束
                        /* control should not reach here */
                        gf_log (this->name, GF_LOG_WARNING, "control reached to "
                                "SP_STATE_COMPLETE, which should not have "
                                "happened");
                        break;
                }
        }

        if (in->record_state == SP_STATE_COMPLETE) {
                in->record_state = SP_STATE_NADA;
                dpdk_reset_priv (priv); 
        }

out:
        if ((ret == -1) && (errno == EAGAIN)) {
                ret = 0;
        }

        return ret;
}

int
dpdk_event_handle_poll_out (struct thread_context *ctx, struct mtcp_epoll_event *events)
{
	rpc_transport_t *this = ctx->trans_info[events->data.sockid];
	dpdk_private_t *priv = this->private;
	struct mtcp_epoll_event *event = events;
	int ret = -1;	

	if (priv->connected != 1) {
		gf_log(this->name,GF_LOG_ERROR, "the pollin event is disconnected!");
		// reconnect
	}
	
        pthread_mutex_lock (&priv->lock);
        {
                if (priv->connected == 1) {
                        ret = dpdk_ioq_churn (ctx, this); //发送

                        if (ret == -1) {
                        	dpdk_disconnect (ctx, this);
                        }
                }
        }
        pthread_mutex_unlock (&priv->lock);

        if (ret == 0)
                ret = rpc_transport_notify (this, RPC_TRANSPORT_MSG_SENT, NULL); //发送
        return ret;
}

int
dpdk_event_handle_poll_in (struct thread_context *ctx, struct mtcp_epoll_event *events)
{
	rpc_transport_t *this = ctx->trans_info[events->data.sockid];
	dpdk_private_t *priv = this->private;
	struct mtcp_epoll_event *event = events;
	rpc_transport_pollin_t *pollin = NULL;
	int ret = -1;	

	if (priv->connected != 1) {
		gf_log(this->name,GF_LOG_ERROR, "the pollin event is disconnected!");
		// reconnect
	}
	
	pthread_mutex_lock (&priv->lock);
	{
		ret = dpdk_proto_state_machine(ctx, this, &pollin);
	}
	pthread_mutex_unlock (&priv->lock);

	if (pollin) {
		ret = rpc_transport_notify (this, RPC_TRANSPORT_MSG_RECEIVED, pollin);
		rpc_transport_pollin_destroy (pollin);
	}
	
	return ret;
}

int
dpdk_event_handle_poll_err (struct thread_context *ctx, struct mtcp_epoll_event *events)
{
	rpc_transport_t *this = ctx->trans_info[events->data.sockid];
	dpdk_private_t *priv = this->private;
	int ret = -1;
	
	pthread_mutex_lock (&priv->lock);
	{
		dpdk_ioq_flush (this);
		dpdk_reset(ctx, this);
	}
	pthread_mutex_unlock (&priv->lock);

	rpc_transport_notify (this, RPC_TRANSPORT_DISCONNECT, this);
	return ret;
}

int32_t
dpdk_fill_inet6_inet_identifier (rpc_transport_t *this, struct sockaddr_storage *addr, int32_t addr_len, char *identifier)
{
        union gf_sock_union sock_union;

        char    service[NI_MAXSERV] = {0,};
        char    host[NI_MAXHOST]    = {0,};
        int32_t ret                 = 0;
        int32_t tmpaddr_len         = 0;
        int32_t one_to_four         = 0;
        int32_t four_to_eight       = 0;
        int32_t twelve_to_sixteen   = 0;
        int16_t eight_to_ten        = 0;
        int16_t ten_to_twelve       = 0;

        memset (&sock_union, 0, sizeof (sock_union));
        sock_union.storage = *addr;
        tmpaddr_len = addr_len;

        if (sock_union.sa.sa_family == AF_INET6) {
                one_to_four = sock_union.sin6.sin6_addr.s6_addr32[0];
                four_to_eight = sock_union.sin6.sin6_addr.s6_addr32[1];
#ifdef GF_SOLARIS_HOST_OS
                eight_to_ten = S6_ADDR16(sock_union.sin6.sin6_addr)[4];
#else
                eight_to_ten = sock_union.sin6.sin6_addr.s6_addr16[4];
#endif

#ifdef GF_SOLARIS_HOST_OS
                ten_to_twelve = S6_ADDR16(sock_union.sin6.sin6_addr)[5];
#else
                ten_to_twelve = sock_union.sin6.sin6_addr.s6_addr16[5];
#endif

                twelve_to_sixteen = sock_union.sin6.sin6_addr.s6_addr32[3];

                /* ipv4 mapped ipv6 address has
                   bits 0-80: 0
                   bits 80-96: 0xffff
                   bits 96-128: ipv4 address
                */

                if (one_to_four == 0 &&
                    four_to_eight == 0 &&
                    eight_to_ten == 0 &&
                    ten_to_twelve == -1) {
                        struct sockaddr_in *in_ptr = &sock_union.sin;
                        memset (&sock_union, 0, sizeof (sock_union));

                        in_ptr->sin_family = AF_INET;
                        in_ptr->sin_port = ((struct sockaddr_in6 *)addr)->sin6_port;
                        in_ptr->sin_addr.s_addr = twelve_to_sixteen;
                        tmpaddr_len = sizeof (*in_ptr);
                }
        }

        ret = getnameinfo (&sock_union.sa,
                           tmpaddr_len,
                           host, sizeof (host),
                           service, sizeof (service),
                           NI_NUMERICHOST | NI_NUMERICSERV);
        if (ret != 0) {
                gf_log (this->name, GF_LOG_ERROR,
                        "getnameinfo failed (%s)", gai_strerror (ret));
        }

        sprintf (identifier, "%s:%s", host, service);

        return ret;
}

int32_t
dpdk_server_get_local_sockaddr(rpc_transport_t *this, struct sockaddr *saddr, socklen_t *addr_len)
{
	dict_t *options = NULL;
	data_t *listen_port_data = NULL, *listen_host_data = NULL;
	uint16_t listen_port = -1;
	char service[NI_MAXSERV], *listen_host = NULL;
	struct addrinfo hints, *res;
    int ret = 0;

	options = this->options;

	listen_port_data = dict_get (options, "transport.dpdk.listen-port");
	listen_host_data = dict_get (options, "transport.dpdk.bind-address");

	saddr->sa_family = AF_INET;


	if (listen_port_data)
	{
		listen_port = data_to_uint16 (listen_port_data);	
//	    gf_log ("pxw", GF_LOG_WARNING, "dpdk: listen port = %d", listen_port);
	} 
	
	if (listen_port == (uint16_t)-1)
		//saddr.sin_port = htons(80);
		listen_port = GF_DEFAULT_DPDK_LISTEN_PORT;
		
	if (listen_host_data)
	{
		listen_host = data_to_str (listen_host_data);
//	    gf_log ("pxw", GF_LOG_WARNING, "dpdk: listen host = %s", listen_host);
		
		// pxw
		struct sockaddr_in *in = (struct sockaddr_in *) saddr;
		in->sin_addr.s_addr = inet_addr(listen_host);
		in->sin_port = htons(listen_port);
        *addr_len = sizeof(struct sockaddr_in);
		return 0;
		
	} else {
		struct sockaddr_in *in = (struct sockaddr_in *) saddr;
		in->sin_addr.s_addr = htonl(INADDR_ANY);
		in->sin_port = htonl(listen_port);
		*addr_len = sizeof(struct sockaddr_in);

		return 0;
	}
	
	// using options to init sockaddr
/*	 memset (service, 0, sizeof (service));
	 sprintf (service, "%d", listen_port);

     memset (&hints, 0, sizeof (hints));
	 hints.ai_family = saddr->sa_family;
	 hints.ai_socktype = SOCK_STREAM;
	 hints.ai_flags = AI_PASSIVE;

	 ret = getaddrinfo (listen_host, service, &hints, &res);
	 if ( ret != 0 ) {
			gf_log (this->name, GF_LOG_ERROR,
					"getaddrinfo failed for host %s, service %s (%s)",
					listen_host, service, gai_strerror (ret));

			return -1;
	 }


	 if (!(*addr_len)) {
			memcpy (saddr, res->ai_addr, res->ai_addrlen);
			*addr_len = res->ai_addrlen;
	 }

	 freeaddrinfo (res);

	 return 0;
 */
}


int
dpdk_accept_connection(struct thread_context *ctx, int listener, rpc_transport_t *this)
{

	dpdk_private_t *priv = this->private;
        int ret = 0;
        int new_sock = -1;
        rpc_transport_t *new_trans = NULL;
        struct sockaddr_storage new_sockaddr = {0, };
        socklen_t addrlen = sizeof (new_sockaddr);
        dpdk_private_t *new_priv = NULL; //从属于 new_trans->private
		mctx_t mctx = ctx->mctx;
		struct mtcp_epoll_event ev;

        pthread_mutex_lock (&priv->lock);
        {
		// 1.创建accept sock,mtcp_accept(mctx_t mctx, int sockid, struct sockaddr *addr, socklen_t *addrlen);
		new_sock = mtcp_accept(mctx, listener, SA (&new_sockaddr), &addrlen);	
		if (new_sock >= 0) {
			if (new_sock >= MAX_FLOW_NUM) {
				gf_log(this->name,GF_LOG_ERROR, "Invalid socket id!\n");
				ret = -1;
				goto unlock;
			}
		} else {
			gf_log(this->name,GF_LOG_WARNING, "mtcp_accept() error\n");
			ret = -1;
			goto unlock;

		}
		 
		gf_log(this->name,GF_LOG_WARNING, "new connection sock %d accepted.\n", new_sock);

		// 2.创建rpc_transport_t new_trans
         new_trans = GF_CALLOC (1, sizeof (*new_trans), gf_common_mt_rpc_trans_t);
            if (!new_trans) {
                //sys_close (new_sock);
                goto unlock;
            }

		ret = pthread_mutex_init(&new_trans->lock, NULL);
		INIT_LIST_HEAD (&new_trans->list);
		new_trans->name = gf_strdup (this->name);

		memcpy (&new_trans->peerinfo.sockaddr, &new_sockaddr, addrlen);//get peerinfo
		new_trans->peerinfo.sockaddr_len = addrlen;

		new_trans->myinfo.sockaddr_len = sizeof (new_trans->myinfo.sockaddr);//myinfo
		//getsockname (new_sock, SA(&new_trans->myinfo.sockaddr), &new_trans->myinfo.sockaddr_len);
		ret = mtcp_getsockname (mctx, new_sock, SA (&new_trans->myinfo.sockaddr),
                                           &new_trans->myinfo.sockaddr_len);
		if (ret == -1) {
                	gf_log (this->name, GF_LOG_WARNING, "getsockname on %d failed!",new_sock);
                        mtcp_close (mctx, new_sock);
                        GF_FREE (new_trans->name);
                        GF_FREE (new_trans);
                        goto unlock;
                }
		ret = dpdk_fill_inet6_inet_identifier (new_trans,
						 &new_trans->myinfo.sockaddr,
						 new_trans->myinfo.sockaddr_len, 
						 new_trans->myinfo.identifier);
		if (ret == -1) {
                	gf_log (this->name, GF_LOG_ERROR, "cannot fill inet/inet6 identifier for server!\n");
		}

		ret = dpdk_fill_inet6_inet_identifier (new_trans,
						 &new_trans->peerinfo.sockaddr,
						 new_trans->peerinfo.sockaddr_len, 
						 new_trans->peerinfo.identifier);
		if (ret == -1) {
                	gf_log (this->name, GF_LOG_ERROR, "cannot fill inet/inet6 identifier for client!\n");
		}

		// 3.创建dpdk_private_t new_priv
    		new_priv = GF_CALLOC (1, sizeof (*new_priv), gf_common_mt_dpdk_private_t);
		if (!new_priv) {
			gf_log (this->name, GF_LOG_WARNING, "calloc new_priv fail");
			ret =  -1;
			goto unlock;
		
		}

       		memset(new_priv, 0, sizeof(*new_priv));
		pthread_mutex_init (&new_priv->lock, NULL);
			new_priv->sock = new_sock;
			new_priv->connected = -1;
			new_priv->ctx = ctx;
			INIT_LIST_HEAD (&new_priv->ioq);

				new_trans->private = new_priv;
                new_trans->ops = this->ops; 
                new_trans->init = this->init;
                new_trans->fini = this->fini;
				new_trans->ctx = this->ctx;
                new_trans->xl   = this->xl; 
                new_trans->mydata = this->mydata;
                new_trans->notify = this->notify;
                new_trans->listener = this;
       

		pthread_mutex_lock (&new_priv->lock);
                {
			new_priv->connected = 1; //连接
                        new_priv->is_server = _gf_true;
                        rpc_transport_ref (new_trans);
		}
       		 pthread_mutex_unlock (&new_priv->lock);

		// 4.new_sock加入mtcp epoll
		ev.events = MTCP_EPOLLIN;
		ev.data.sockid = new_sock;
		ctx->trans_info[new_sock] = new_trans;

		gf_log ("pxw", GF_LOG_WARNING, "new sockid = %d, new_priv->connected =%d, trans_info->orivate->connected = %d",
				new_sock, new_priv->connected, ((dpdk_private_t *)ctx->trans_info[new_sock]->private)->connected);

		mtcp_setsock_nonblock(mctx, new_sock);
		mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, new_sock, &ev);

		ret = rpc_transport_notify (this, RPC_TRANSPORT_ACCEPT, new_trans);

		if (!ret)
			gf_log ("pxw", GF_LOG_WARNING, "execute rpc_transport_notify (RPC_TRANSPORT_ACCEPT) successfully");

        }
unlock:
        pthread_mutex_unlock (&priv->lock);

        return ret;	

}


static void
dpdk_assign_port (struct sockaddr *sockaddr, uint16_t port)
{
	((struct sockaddr_in *)sockaddr)->sin_port = htons (port);
	return;
}

static int32_t
dpdk_af_inet_bind_to_port_lt_ceiling (struct thread_context *ctx, int fd, struct sockaddr *sockaddr,
                                 socklen_t sockaddr_len, uint32_t ceiling)
{
        int32_t        ret        = -1;
        uint16_t      port        = ceiling - 1;
        gf_boolean_t  ports[GF_PORT_MAX];
        int           i           = 0;

loop:
        ret = gf_process_reserved_ports (ports, ceiling);

        while (port) {
                if (port == GF_CLIENT_PORT_CEILING) {
                        ret = -1;
                        break;
                }

                /* ignore the reserved ports */
                if (ports[port] == _gf_true) {
                        port--;
                        continue;
                }

                dpdk_assign_port (sockaddr, port);

		ret = mtcp_bind(ctx->mctx, fd, sockaddr, sockaddr_len);

                if (ret == 0)
                        break;

                if (ret == -1 && errno == EACCES)
                        break;

                port--;
        }

        /* Incase if all the secure ports are exhausted, we are no more
         * binding to secure ports, hence instead of getting a random
         * port, lets define the range to restrict it from getting from
         * ports reserved for bricks i.e from range of 49152 - 65535
         * which further may lead to port clash */
        if (!port) {
                ceiling = port = GF_CLNT_INSECURE_PORT_CEILING;
                for (i = 0; i <= ceiling; i++)
                        ports[i] = _gf_false;
                goto loop;
        }

        return ret;
}


int32_t
dpdk_client_bind (struct thread_context *ctx, rpc_transport_t *this,
             struct sockaddr *sockaddr,
             socklen_t *sockaddr_len,
             int sock)
{
        int ret = 0;
	*sockaddr_len = sizeof (struct sockaddr_in);

        if (!this->bind_insecure) { //不知干嘛的
		ret = dpdk_af_inet_bind_to_port_lt_ceiling (ctx, sock, sockaddr,
                                                               *sockaddr_len,
                                                               GF_CLIENT_PORT_CEILING);
                if (ret == -1) {
                	gf_log (this->name, GF_LOG_DEBUG,
                        	"cannot bind inet socket (%d) "
                                "to port less than %d (%s)",
                                sock, GF_CLIENT_PORT_CEILING,
                                strerror (errno));
                        ret = 0;
                } else {
                        ret = dpdk_af_inet_bind_to_port_lt_ceiling (ctx, sock, sockaddr,
                                                               *sockaddr_len,
                                                               GF_IANA_PRIV_PORTS_START);
                        if (ret == -1) {
                                gf_log (this->name, GF_LOG_DEBUG,
                                        "failed while binding to less than "
                                        "%d (%s)", GF_IANA_PRIV_PORTS_START,
                                        strerror (errno));
                                ret = 0;
                        }
                }
	}
        return ret;
}


// 必须在options中给出remote-port和remote-host
static int32_t
dpdk_af_inet_client_get_remote_sockaddr (rpc_transport_t *this,  
                                  	  struct sockaddr *sockaddr,
                                  	  socklen_t *sockaddr_len)
{
        dict_t *options = this->options;
        data_t *remote_host_data = NULL;
        data_t *remote_port_data = NULL;
        char *remote_host = NULL;
        uint16_t remote_port = 0;
        struct addrinfo *addr_info = NULL;
        int32_t ret = 0;

        remote_host_data = dict_get (options, "remote-host");
        if (remote_host_data == NULL)
        {
                gf_log (this->name, GF_LOG_ERROR,
                        "option remote-host missing in volume %s", this->name);
                ret = -1;
                goto err;
        }

        remote_host = data_to_str (remote_host_data);
        if (remote_host == NULL)
        {
                gf_log (this->name, GF_LOG_ERROR,
                        "option remote-host has data NULL in volume %s", this->name);
                ret = -1;
                goto err;
        }

        remote_port_data = dict_get (options, "remote-port");
        if (remote_port_data == NULL)
        {
                gf_log (this->name, GF_LOG_TRACE,
                        "option remote-port missing in volume %s. Defaulting to %d",
                        this->name, GF_DEFAULT_DPDK_LISTEN_PORT);

                remote_port = GF_DEFAULT_DPDK_LISTEN_PORT; //24007
        }
        else
        {
                remote_port = data_to_uint16 (remote_port_data);
        }

        if (remote_port == (uint16_t)-1)
        {
                gf_log (this->name, GF_LOG_ERROR,
                        "option remote-port has invalid port in volume %s",
                        this->name);
                ret = -1;
                goto err;
        }

        /* TODO: gf_resolve is a blocking call. kick in some
           non blocking dns techniques */
        ret = gf_resolve_ip6 (remote_host, remote_port,
                              sockaddr->sa_family, &this->dnscache, &addr_info);
        if (ret == -1) {
                gf_log (this->name, GF_LOG_ERROR,
                        "DNS resolution failed on host %s", remote_host);
                goto err;
        }

        memcpy (sockaddr, addr_info->ai_addr, addr_info->ai_addrlen);
        *sockaddr_len = addr_info->ai_addrlen;

err:
        return ret;
}


/* affinitize application thread to a CPU core （using only one mtcp thread here | supposing no HT）
 * create mtcp context on a specific core : this will spawn an mtcp thread 
 * create epoll descriptor 
 */
int 
create_listen_socket(struct thread_context *ctx, rpc_transport_t *this)
{
	int listener;
	struct mtcp_epoll_event ev;
	struct sockaddr_storage sockaddr;
	socklen_t sockaddr_len = 0;
	peer_info_t *myinfo = &this->myinfo;
	int ret;

	/* create socket and set it as nonblocking */
	listener = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0); //创建socket ( 只支持ipv4 )
	if (listener < 0) {
		gf_log(this->name,GF_LOG_ERROR, "Failed to create listening socket!\n");
		return -1;
	}
	ret = mtcp_setsock_nonblock(ctx->mctx, listener);//非阻塞
	if (ret < 0) {
		gf_log(this->name,GF_LOG_ERROR, "Failed to set socket in nonblocking mode.\n");
		return -1;
	}

	/* get my saddr */
	ret = dpdk_server_get_local_sockaddr(this,  SA (&sockaddr), &sockaddr_len);
	if (ret < 0) {
		gf_log(this->name,GF_LOG_ERROR, "Failed to create listening socket!\n");
		return -1;
	} else 
	{
//		gf_log(this->name,GF_LOG_WARNING, "create server listening socket successfully!\n");
	}

	memcpy(&myinfo->sockaddr, &sockaddr, sockaddr_len);
	myinfo->sockaddr_len = sockaddr_len;

	((dpdk_private_t *)this->private)->sock = listener;

	ret = mtcp_bind(ctx->mctx, listener, // mTCP bind 绑定
			(struct sockaddr *)&sockaddr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		gf_log(this->name,GF_LOG_ERROR, "Failed to bind to the listening socket!\n");
		return -1;
	} else 
	{
//		gf_log(this->name,GF_LOG_WARNING, "bind server listening socket successfully!\n");
	}

	/* listen (backlog: 4K) */
	ret = mtcp_listen(ctx->mctx, listener, 10); //listen
	if (ret < 0) {
		gf_log(this->name,GF_LOG_ERROR, "mtcp_listen() failed!\n");
		((dpdk_private_t *)this->private)->sock = -1;
		return -1;
	} else 
	{
//		gf_log(this->name,GF_LOG_WARNING, "server listener starts listening!\n");
	}
	
	/* wait for incoming accept events */
	ev.events = MTCP_EPOLLIN;
	ev.data.sockid = listener;
//	rpc_transport_t *rt = ctx->trans_info[listener];
//	rt = this;

	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, listener, &ev);

	return listener;
}

struct thread_context *
create_mtcp_thread(int core) //对某个核上的app thread而言，创建对应的mTCP thread_context，启动，与该app thread交互
{
	struct thread_context *ctx;

	mtcp_core_affinitize(core);   

	//ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));//用 GF_CALLOC
        ctx = (struct thread_context *)GF_CALLOC (1, sizeof (struct thread_context), gf_common_mt_thread_context_t);
	if (!ctx) {
		//gf_log(this->name,GF_LOG_ERROR, "Failed to create thread context!\n");
		return NULL;
	}

	ctx->mctx = mtcp_create_context(core);
	if (!ctx->mctx) {
		//gf_log(this->name,GF_LOG_ERROR, "Failed to create mtcp context!\n");
		free(ctx);
		return NULL;
	}

	ctx->ep = mtcp_epoll_create(ctx->mctx, MAX_EVENTS);
	if (ctx->ep < 0) {
		mtcp_destroy_context(ctx->mctx);
		free(ctx);
		//gf_log(this->name,GF_LOG_ERROR, "Failed to create epoll descriptor!\n");
		return NULL;
	}

/*	
	ctx->trans_info = (rpc_transport_t *)
			calloc(MAX_FLOW_NUM, sizeof(rpc_transport_t));
	if (!ctx->trans_info) {
		mtcp_close(ctx->mctx, ctx->ep);
		mtcp_destroy_context(ctx->mctx);
		free(ctx);
		gf_log("pxw", GF_LOG_ERROR, "Failed to create ctx->trans_info struct!\n");
		return NULL;
	}
 */	
	return ctx;
}

void *
run_server_routine(void *arg) //服务端app线程的routine
{
	int id = (int)(*((int *)arg));
	int core = 0;
	struct thread_context *ctx;
	mctx_t mctx;
	int listener;
	int ep;
	struct mtcp_epoll_event *events;
	int nevents;
	int i, ret;
	int do_accept;

	rpc_transport_t *this = (rpc_transport_t *)server_args[id].transport_ptr;
	core = server_args[id].core;
	gf_log ("pxw", GF_LOG_WARNING, "id = %d", id);
	//rpc_transport_t *this = (rpc_transport_t *)arg; //服务端监听主transport
	dpdk_private_t * priv = (dpdk_private_t *)this->private;
	peer_info_t *myinfo = &this->myinfo;  //rpc_transport_t中的自己的peer info
	
	/* initialization */
	ctx = create_mtcp_thread(core);
	if (!ctx) {
		gf_log(this->name,GF_LOG_ERROR, "Failed to initialize server thread.\n");
		return NULL;
	}
	priv->ctx = ctx;
	mctx = ctx->mctx;
	ep = ctx->ep;

	events = (struct mtcp_epoll_event *)
			GF_CALLOC (MAX_EVENTS, sizeof(struct mtcp_epoll_event), gf_common_mt_mtcp_epoll_event_t);
	if (!events) {
		gf_log(this->name,GF_LOG_ERROR, "Failed to create event struct!\n");
		exit(-1);
	}

	listener = create_listen_socket(ctx, this);//创建监听socket
	if (listener < 0) {
		gf_log(this->name,GF_LOG_ERROR, "Failed to create listening socket.\n");
		exit(-1);
	} else {
		gf_log (this->name, GF_LOG_WARNING, "create listener sockid = %d", listener);
		rpc_transport_ref(this);
	}

	done[core] = 1;

	while (done[core]) { 
	//	gf_log(this->name,GF_LOG_WARNING, "server thread in while routine ......");

		nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
		if (nevents < 0) {
			//gf_log (this->name, GF_LOG_WARNING, "mtcp_epoll_wait");
			break;
		}
		//gf_log (this->name, GF_LOG_WARNING," server get %d events. ", nevents);

		do_accept = 0;
		for (i = 0; i < nevents; i++) {

	    	if ((events[i].data.sockid == listener) )	{ //1.新连接请求
				do_accept = 1;

			} else { // 2.已经建立连接的事件

				//gf_log (this->name, GF_LOG_WARNING,"events sockid = %d", events[i].data.sockid);
				int sockid = events[i].data.sockid;
				rpc_transport_t *rt = ctx->trans_info[sockid];
				dpdk_private_t *ev_priv = (dpdk_private_t *)rt->private;
					
				//gf_log (this->name, GF_LOG_WARNING, "priv->connected = %d", ev_priv->connected);

				if (ev_priv->connected != 1) {	//若连接尚未建立，后面将不能执行
					gf_log (this->name, GF_LOG_ERROR, "client is not connected");
					if (ev_priv->connect_failed) {
						ret = dpdk_disconnect (ev_priv->ctx, this);
						ret = -1; 
					} else {
						ret = dpdk_connect_finish (ev_priv->ctx, this);
					}

				} else {
					ret = 0;
				}						


				//gf_log (this->name, GF_LOG_WARNING, "ret = %d, events[%d].events = %x",
				//										ret, i, events[i].events);

					
				if ( !ret && (events[i].events & MTCP_EPOLLOUT) ) { //3， 客户端可写事件
					//gf_log (this->name, GF_LOG_WARNING, "4-1");
				        //gf_log ("pxw", GF_LOG_WARNING, "epollout event in thread %d",id);
					ret = dpdk_event_handle_poll_out (ctx, &events[i]);

					//gf_log (this->name, GF_LOG_WARNING, "4-2");
				} 

				if ( !ret && (events[i].events & MTCP_EPOLLIN)) { //4. 客户端可读事件(finished)
					// how  to get rpc_transport_t
				    // gf_log (this->name, GF_LOG_WARNING, "5");
			            //	gf_log ("pxw", GF_LOG_WARNING, "epollin event  in thread %d",id);
					ret = dpdk_event_handle_poll_in (ctx, &events[i]);

					//gf_log (this->name, GF_LOG_WARNING, "6 , ret = %d", ret);
					//if (ret == 0) {
						/* connection closed by remote host */
					//	dpdk_disconnect(ctx, rt);
				//	} else if (ret < 0) {
						/* if not EAGAIN, it's an error */
					//	if (errno != EAGAIN) {
					//		dpdk_disconnect(ctx, rt);
					//	}
				//	}

				} 

				if ((ret < 0) || (events[i].events & MTCP_EPOLLERR)) { //5. mTCP错误
					gf_log(this->name,GF_LOG_ERROR, "mTCP epoll error occurs！\n");				
					dpdk_event_handle_poll_err(ctx, &events[i]);//包含mtcp_epoll_ctl(),清理mtcp ctx中注册的监听，同时清rpc_transport_t的数据结构
					//1. 如果是客户端umount也属于err类，不能执行mtcp_close
					//mtcp_close (ctx->mctx, events[i].data.sockid);
					//2. FreeSocket function needed
					rpc_transport_unref(rt);

				}
			}
		}

		if (do_accept) {
			while (1) {
				gf_log ("pxw", GF_LOG_WARNING, "accept in thread %d",id);
				ret = dpdk_accept_connection(ctx, listener, this); 
				if (ret < 0) {
					break;
				} 
			}
		}

		//gf_log(this->name,GF_LOG_WARNING, "server thread end a while loop ......");

	}

	/* destroy mtcp context: this will kill the mtcp thread */
	mtcp_destroy_context(mctx);
	pthread_exit(NULL);
}


//interrupt
void
SignalHandler (int signum)
{
	int i = 0;
	for ( i; i < CPU_NUMS; i++ ) {
		if (&app_thread[i] == pthread_self()) {
			done[i] = 0;
		} else {
			if (!done[i]) {
				pthread_kill (&app_thread[i], signum);
			}
		
		}
	}
}


/* 创建app thread 
 * 根据参数conf初始化线程（如果使用n个core，创建n个app thread 和 n个mtcp thread），默认为1
 * mTCP线程负责在app和网卡中收发数据包，并且设计网络协议的封装/解封装数据包
 * app线程负责调用rpc_transport_notify相关函数
 */
static int32_t
dpdk_listen (rpc_transport_t *this)  //某个核上的线程调用监听
{
	int i; 

	for (i = 0; i < CPU_NUMS; i++) { //process_cpu：用户config中初始化使用core数目
		cores[i] = i;
		done[i] = 0;
	 }

	/* register signal handler to mtcp */
	mtcp_register_signal(SIGINT, SignalHandler); //如果需要，放在listen和connect函数中
	
	for ( i = 0; i < CPU_NUMS; i++ ) {
		
		server_args[i].transport_ptr = this;
		server_args[i].core = cores[i];
		gf_log ("pxw", GF_LOG_WARNING, "i = %d", i);

		if (pthread_create(&app_thread[i],  NULL, run_server_routine, &cores[i])) {
			gf_log(this->name,GF_LOG_ERROR, "create server routine fail !\n");
			exit(1);
		} else {
			sleep(1);
			gf_log(this->name,GF_LOG_WARNING, "create server routine successful !\n");
		}

	}

	//for (i = ((process_cpu == -1) ? 0 : process_cpu); i < core_limit; i++) {
	//pthread_join(app_thread[0], NULL);
	gf_log(this->name,GF_LOG_WARNING, "main thread returns from dpdk to main routine.\n");	

	return 0;
}

/* 客户端clnt/cli初始化调用dpdk_connect
 *
 */
void *
run_client_routine (void *arg)
{
	struct dpdk_client_args *client_args;
	struct thread_context *ctx;
	mctx_t mctx;
	int ep;
	struct mtcp_epoll_event *events;
	struct mtcp_epoll_event ev;
	int nevents;
	int i;
	int do_accept;

        int                            ret             = -1;
        int                            th_ret          = -1;
        int                            sock            = -1;
        dpdk_private_t                *priv            = NULL;
        socklen_t                      sockaddr_len    = 0;
        sa_family_t                    sa_family       = {0, };
        char                          *local_addr      = NULL;
        union gf_sock_union            sock_union;
        struct sockaddr_in            *addr            = NULL;
        gf_boolean_t                   refd      = _gf_false;
        dpdk_connect_error_state_t    *args             = NULL;
        pthread_t                      th_id           = {0, };//用于error logging的线程
        gf_boolean_t                   ign_enoent      = _gf_false;
	int port;


	//client_args.transport_ptr = this;
	//client_args.port = port;
	client_args = (struct dpdk_client_args *)arg;
	rpc_transport_t *this = client_args->transport_ptr; //服务端监听主transport
	port = client_args->port;	

        priv = this->private;

        if (!priv) {
                gf_log_callingfn (this->name, GF_LOG_WARNING,
                        "connect() called on uninitialized transport");
                goto err;
        }
	
	/* initialization */
	ctx = create_mtcp_thread(0); //只用一个核
	if (!ctx) {
		gf_log(this->name,GF_LOG_ERROR, "Failed to initialize server thread.\n");
		return NULL;
	}
	mctx = ctx->mctx;
	ep = ctx->ep;

	events = (struct mtcp_epoll_event *)
			calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));
	if (!events) {
		gf_log(this->name,GF_LOG_ERROR, "Failed to create event struct!\n");
		exit(-1);
	}

        pthread_mutex_lock (&priv->lock);
        {
                if (priv->sock != -1) {
                        gf_log_callingfn (this->name, GF_LOG_TRACE,
                                          "connect () called on transport "
                                          "already connected");
                        errno = EINPROGRESS;
                        ret = -1;
                        goto unlock;
                }

                gf_log (this->name, GF_LOG_TRACE,
                        "connecting %p, sock=%d", this, priv->sock);

		sa_family = AF_INET;
		sock_union.sa.sa_family = AF_INET;

                ret = dpdk_af_inet_client_get_remote_sockaddr (this, &sock_union.sa, &sockaddr_len);
                if (ret == -1) {
                        /* logged inside client_get_remote_sockaddr */
                        goto unlock;
                }

                if (port > 0) {
                	sock_union.sin.sin_port = htons (port);
                }

                memcpy (&this->peerinfo.sockaddr, &sock_union.storage,
                        sockaddr_len);
                this->peerinfo.sockaddr_len = sockaddr_len;



		/* create socket and set it as nonblocking */
		priv->sock = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0); 

		if (priv->sock < 0) {
                        gf_log (this->name, GF_LOG_ERROR, "socket creation failed (%s)", strerror (errno));
                        ret = -1;
                        goto unlock;
		}

                SA (&this->myinfo.sockaddr)->sa_family = AF_INET;

                /* 如果指定了客户端源地址 */
                ret = dict_get_str (this->options, "transport.socket.source-addr",&local_addr);
                if (!ret ) {
                        addr = (struct sockaddr_in *)(&this->myinfo.sockaddr);
                        ret = inet_pton (AF_INET, local_addr,
                                         &(addr->sin_addr.s_addr));
                }

                ret = dpdk_client_bind (ctx, this, SA (&this->myinfo.sockaddr), //客户端选定一个可用port
                                   &this->myinfo.sockaddr_len, priv->sock); 
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "client bind failed: %s", strerror (errno));
                        goto handler;
                }

		ret = mtcp_setsock_nonblock(ctx->mctx, priv->sock);//非阻塞
		if (ret < 0) {
                	gf_log (this->name, GF_LOG_ERROR,
                        	"NBIO on %d failed (%s)",
                                priv->sock, strerror (errno));
                        goto handler;
		}

                ret = mtcp_connect (ctx->mctx, priv->sock, SA (&this->peerinfo.sockaddr), this->peerinfo.sockaddr_len);

                if (ret == -1) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "connection attempt on %s failed, (%s)",
                                this->peerinfo.identifier, strerror (errno));
                        priv->connect_failed = 1;
                        goto handler;
                }
                else {
                        priv->connect_failed = 0;
                        ret = 0;
                }

handler:
                if (ret < 0) {
                        /* Ignore error from connect. epoll events
                           should be handled in the socket handler.  shutdown(2)
                           will result in EPOLLERR, so cleanup is done in
                           socket_event_handler or socket_poller */
                        shutdown (priv->sock, SHUT_RDWR);
                }

                priv->connected = 0;
                priv->is_server = _gf_false;
		priv->ctx = ctx;
                rpc_transport_ref (this);
                refd = _gf_true;

		ev.events = MTCP_EPOLLIN | MTCP_EPOLLOUT;
		ev.data.sockid = priv->sock;;
	    //rpc_transport_t *rt = ctx->trans_info[priv->sock];
	    //rt = this;
		mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, priv->sock, &ev);//监听。。把自己加入epoll


unlock:
                sock = priv->sock;
        }
        pthread_mutex_unlock (&priv->lock);

	//****** start the loop ******
	if (ret != -1 && sock != -1) {
	mctx = ctx->mctx;
	while (1) { //usr app thread死循环：
		nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1); //等待事件
		if (nevents < 0) {
			break;
		}

		for (i = 0; i < nevents; i++) {
			if (events[i].events & MTCP_EPOLLERR) { //1. mTCP错误
				gf_log(this->name,GF_LOG_ERROR, "mTCP epoll error occurs！\n");				
				dpdk_event_handle_poll_err(ctx, &events[i]);//包含mtcp_epoll_ctl(),清理mtcp ctx中注册的监听，同时清rpc_transport_t的数据结构
				mtcp_close (ctx->mctx, events[i].data.sockid);
				rpc_transport_unref(ctx->trans_info[events[i].data.sockid]);

			} else if (events[i].events & MTCP_EPOLLIN) { //2. 客户端可读事件(finished)
				// how  to get rpc_transport_t
				ret = dpdk_event_handle_poll_in (ctx, &events[i]);
				if (ret == 0) {
					/* connection closed by remote host */
					dpdk_disconnect(ctx, events[i].data.sockid);
				} else if (ret < 0) {
					/* if not EAGAIN, it's an error */
					if (errno != EAGAIN) {
						dpdk_disconnect(ctx, ctx->trans_info[events[i].data.sockid]);
					}
				}

			} else if (events[i].events & MTCP_EPOLLOUT) { //3， 客户端可写事件
				ret = dpdk_event_handle_poll_out (ctx, &events[i]);
			} else {
				assert(0);
			}
		}

	/* destroy mtcp context: this will kill the mtcp thread */
	mtcp_destroy_context(mctx);
	pthread_exit(NULL);
	return 0;

	} 
}

err:
        /* if sock != -1, then cleanup is done from the event handler */
        if (ret == -1 && sock == -1) {
                /* Cleaup requires to send notification to upper layer which
                   intern holds the big_lock. There can be dead-lock situation
                   if big_lock is already held by the current thread.
                   So transfer the ownership to seperate thread for cleanup.
                */
                args = GF_CALLOC (1, sizeof (*args),
                                 gf_dpdk_connect_error_state_t);   //用途？？
                args->this = THIS;
                args->trans = this;
                args->refd = refd;
                th_ret = gf_thread_create_detached (&th_id,
                                                    dpdk_connect_error_cbk,
                                                    args);
                if (th_ret) {
                        /* Error will be logged by gf_thread_create_attached */
                        gf_log (this->name, GF_LOG_ERROR, "Thread creation "
                               "failed");
                        GF_FREE (args);
                        GF_ASSERT (0);
                }
        }

        return ret;
}



static int 
dpdk_connect (rpc_transport_t *this, int port)
{
	struct dpdk_client_args client_args;
	client_args.transport_ptr = this;
	client_args.port = port;

	if (pthread_create(&app_thread, NULL, run_client_routine, &client_args)) {
		gf_log(this->name,GF_LOG_ERROR, "create client routine fail !\n");
		exit(1);
	} else {
		gf_log(this->name,GF_LOG_WARNING, "create client routine successful !\n");
	}

	pthread_join(&app_thread, NULL);

	return NULL;
}


struct rpc_transport_ops tops = {
        .listen             = dpdk_listen, //loop
        .connect            = dpdk_connect,
        .disconnect         = dpdk_disconnect,
        .submit_request     = dpdk_submit_request,
        .submit_reply       = dpdk_submit_reply,
 //      .get_peername       = dpdk_getpeername,
 //      .get_peeraddr       = dpdk_getpeeraddr,
 //      .get_myname         = dpdk_getmyname,
 //      .get_myaddr         = dpdk_getmyaddr,
};


int32_t
init (rpc_transport_t *this)
{
	conf_file = "/home/dcslab/pxw/glusterfs-3.9.1/rpc/rpc-transport/dpdk/src/dpdk.conf";
        gf_log ("pxw", GF_LOG_WARNING, "DPDK.C INIT()");
	// 不同于socket.c， 对有所rpc_transport_t 只有最开始的会调用且在init()中初始化属性
	// socket.c 中的socket_init()部分：
        dpdk_private_t *priv = NULL;
        gf_boolean_t      tmp_bool = 0;//用于暂存字典属性
        char             *optstr = NULL;
        uint32_t          keepalive = 0;
        uint32_t          timeout = 0;
        uint32_t          backlog = 0;

        if (this->private) { //这个rpc_transport_t的私有性质
                gf_log_callingfn (this->name, GF_LOG_ERROR, "double init attempted");
                return -1;
        }

        priv = GF_CALLOC (1, sizeof (*priv), gf_common_mt_dpdk_private_t);
        if (!priv) {
                return -1;
        }
        memset(priv,0,sizeof(*priv));

        pthread_mutex_init (&priv->lock, NULL);

        priv->sock = -1;
       // priv->idx = -1;
        priv->connected = -1;
        INIT_LIST_HEAD (&priv->ioq);
	//其他的不必要


	/* initialize mtcp */
	if (conf_file == NULL) {
		gf_log(this->name,GF_LOG_ERROR, "You forgot to pass the mTCP startup config file!\n");
		return -1;
	} 

	//载入arp，route table等，启动load_module
	//用opts赋值0
	int ret = mtcp_init(conf_file); 

	if (ret) {
		gf_log(this->name,GF_LOG_ERROR, "Failed to initialize mTCP!\n");
		return -1;
	} else {
		gf_log ("pxw", GF_LOG_WARNING, "mtcp_init() success");
	}


        if (!this->options)
                goto out;
/*
        if (dict_get (this->options, "non-blocking-io")) {
                optstr = data_to_str (dict_get (this->options,
                                                "non-blocking-io"));

                if (gf_string2boolean (optstr, &tmp_bool) == -1) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "'non-blocking-io' takes only boolean options,"
                                " not taking any action");
                        tmp_bool = 1;
                }

                if (!tmp_bool) {
                        priv->bio = 1;
                        gf_log (this->name, GF_LOG_WARNING,
                                "disabling non-blocking IO");
                }
        }
        optstr = NULL;
        priv->keepalive = 1;
        priv->keepaliveintvl = 2;
        priv->keepaliveidle = 20;
        if (dict_get_str (this->options, "transport.socket.keepalive",
                          &optstr) == 0) {
                if (gf_string2boolean (optstr, &tmp_bool) == -1) {
                        gf_log (this->name, GF_LOG_ERROR,
                                "'transport.socket.keepalive' takes only "
                                "boolean options, not taking any action");
                        tmp_bool = 1;
                }

                if (!tmp_bool)
                        priv->keepalive = 0;
        }

        if (dict_get_uint32 (this->options,
                             "transport.socket.keepalive-interval",
                             &keepalive) == 0) {
                priv->keepaliveintvl = keepalive;
        }

        if (dict_get_uint32 (this->options,
                             "transport.socket.keepalive-time",
                             &keepalive) == 0) {
                priv->keepaliveidle = keepalive;
        }

        if (dict_get_uint32 (this->options, "transport.tcp-user-timeout",
                             &timeout) == 0) {
                priv->timeout = timeout;
        }
        gf_log (this->name, GF_LOG_DEBUG, "Configued "
                "transport.tcp-user-timeout=%d", priv->timeout);

        if (dict_get_uint32 (this->options,
                             "transport.socket.listen-backlog",
                             &backlog) == 0) {
                priv->backlog = backlog;
        }
	*/

out:
        this->private = priv;
		gf_log(this->name,GF_LOG_WARNING, "dpdk initialization success!\n");
        return 0;

        GF_FREE(priv);
        return -1;
}

void
fini (struct rpc_transport *this)
{
	dpdk_private_t *priv = NULL;
	struct thread_context *ctx;	

        if (!this)
                return;

        priv = this->private;
	ctx = priv->ctx;	

        if (priv) {
                if (priv->sock != -1) {
                        pthread_mutex_lock (&priv->lock);
                        {
                                dpdk_ioq_flush (this);
                                dpdk_reset (ctx, this);
                        }
                        pthread_mutex_unlock (&priv->lock);
                }
                gf_log (this->name, GF_LOG_TRACE,
                        "transport %p destroyed", this);

                pthread_mutex_destroy (&priv->lock);
                GF_FREE (priv);
        }

        this->private = NULL;

	//mtcp何时destroy?
	// mtcp can be destroyed only when listen transport calls fini()
	//mtcp_destroy_context (((dpdk_private_t *)this->private)->ctx->mctx);
	//int ret = mtcp_destroy(); //void value not ignored as it ought to be ...
	gf_log(this->name,GF_LOG_WARNING, "dpdk.c calls fini() !\n");
        return;
}

/* TODO: expand each option */
struct volume_options options[] = {	//关闭所有ssl相关功能
        { .key   = {"remote-port",
                    "transport.remote-port",
                    "transport.dpdk.remote-port"},
          .type  = GF_OPTION_TYPE_INT
        },
        { .key   = {"transport.dpdk.listen-port", "listen-port"}, //use
          .type  = GF_OPTION_TYPE_INT
        },
        { .key   = {"transport.dpdk.bind-address", "bind-address" }, //use
          .type  = GF_OPTION_TYPE_INTERNET_ADDRESS
        },
        { .key   = {"transport.dpdk.connect-path", "connect-path"},
          .type  = GF_OPTION_TYPE_ANY
        },
        { .key   = {"transport.dpdk.bind-path", "bind-path"},
          .type  = GF_OPTION_TYPE_ANY
        },
        { .key   = {"transport.dpdk.listen-path", "listen-path"},
          .type  = GF_OPTION_TYPE_ANY
        },
        { .key   = {"non-blocking-io"},
          .type  = GF_OPTION_TYPE_BOOL
        },
        { .key   = {"tcp-window-size"},
          .type  = GF_OPTION_TYPE_SIZET,
          .min   = GF_MIN_DPDK_WINDOW_SIZE,
          .max   = GF_MAX_DPDK_WINDOW_SIZE
        },
        { .key   = {"transport.tcp-user-timeout"},
          .type  = GF_OPTION_TYPE_INT,
        },
        { .key   = {"transport.dodk.nodelay"},
          .type  = GF_OPTION_TYPE_BOOL
        },
        { .key   = {"transport.dpdk.lowlat"},
          .type  = GF_OPTION_TYPE_BOOL
        },
        { .key   = {"transport.dpdk.keepalive"},
          .type  = GF_OPTION_TYPE_BOOL
        },
        { .key   = {"transport.dpdk.keepalive-interval"},
          .type  = GF_OPTION_TYPE_INT
        },
        { .key   = {"transport.dpdk.keepalive-time"},
          .type  = GF_OPTION_TYPE_INT
        },
        { .key   = {"transport.dpdk.listen-backlog"},
          .type  = GF_OPTION_TYPE_INT
        },
        { .key   = {"transport.dpdk.read-fail-log"},
          .type  = GF_OPTION_TYPE_BOOL
        },
        { .key = {NULL} }
};
