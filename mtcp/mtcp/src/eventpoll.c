#include <sys/queue.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <assert.h>

#include "mtcp.h"
#include "tcp_stream.h"
#include "eventpoll.h"
#include "tcp_in.h"
#include "pipe.h"
#include "debug.h"

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

#define SPIN_BEFORE_SLEEP FALSE
#define SPIN_THRESH 10000000

/*----------------------------------------------------------------------------*/
char *event_str[] = {"NONE", "IN", "PRI", "OUT", "ERR", "HUP", "RDHUP"};
/*----------------------------------------------------------------------------*/
char * 
EventToString(uint32_t event)
{
	switch (event) {
		case MTCP_EPOLLNONE:
			return event_str[0];
			break;
		case MTCP_EPOLLIN:
			return event_str[1];
			break;
		case MTCP_EPOLLPRI:
			return event_str[2];
			break;
		case MTCP_EPOLLOUT:
			return event_str[3];
			break;
		case MTCP_EPOLLERR:
			return event_str[4];
			break;
		case MTCP_EPOLLHUP:
			return event_str[5];
			break;
		case MTCP_EPOLLRDHUP:
			return event_str[6];
			break;
		default:
			assert(0);
	}
	
	assert(0);
	return NULL;
}
/*----------------------------------------------------------------------------*/
struct event_queue *
CreateEventQueue(int size)
{
	struct event_queue *eq;

	eq = (struct event_queue *)calloc(1, sizeof(struct event_queue));
	if (!eq)
		return NULL;

	eq->start = 0;
	eq->end = 0;
	eq->size = size;
	eq->events = (struct mtcp_epoll_event_int *)
			calloc(size, sizeof(struct mtcp_epoll_event_int));
	if (!eq->events) {
		free(eq);
		return NULL;
	}
	eq->num_events = 0;

	return eq;
}
/*----------------------------------------------------------------------------*/
void 
DestroyEventQueue(struct event_queue *eq)
{
	if (eq->events)
		free(eq->events);

	free(eq);
}
/*----------------------------------------------------------------------------*/
int 
mtcp_epoll_create(mctx_t mctx, int size)
{
	mtcp_manager_t mtcp = g_mtcp[mctx->cpu]; // get mtcp on a certain cpu
	struct mtcp_epoll *ep;
	socket_map_t epsocket;

	if (size <= 0) {
		errno = EINVAL;
		return -1;
	}

	epsocket = AllocateSocket(mctx, MTCP_SOCK_EPOLL, FALSE);
	if (!epsocket) {
		errno = ENFILE;
		return -1;
	}

	ep = (struct mtcp_epoll *)calloc(1, sizeof(struct mtcp_epoll));
	if (!ep) {
		FreeSocket(mctx, epsocket->id, FALSE);
		return -1;
	}

	/* create event queues */
	ep->usr_queue = CreateEventQueue(size);
	if (!ep->usr_queue) {
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}

	ep->usr_shadow_queue = CreateEventQueue(size);
	if (!ep->usr_shadow_queue) {
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}

	ep->mtcp_queue = CreateEventQueue(size);
	if (!ep->mtcp_queue) {
		DestroyEventQueue(ep->usr_shadow_queue);
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}

	TRACE_EPOLL("epoll structure of size %d created.\n", size);

	mtcp->ep = ep; //let ep be mtcp's property
	epsocket->ep = ep;

	if (pthread_mutex_init(&ep->epoll_lock, NULL)) {
		DestroyEventQueue(ep->mtcp_queue);
		DestroyEventQueue(ep->usr_shadow_queue);
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}
	if (pthread_cond_init(&ep->epoll_cond, NULL)) {
		DestroyEventQueue(ep->mtcp_queue);
		DestroyEventQueue(ep->usr_shadow_queue);
		DestroyEventQueue(ep->usr_queue);
		FreeSocket(mctx, epsocket->id, FALSE);
		free(ep);
		return -1;
	}

	return epsocket->id;
}
/*----------------------------------------------------------------------------*/
int 
CloseEpollSocket(mctx_t mctx, int epid)
{
	mtcp_manager_t mtcp;
	struct mtcp_epoll *ep;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		return -1;
	}

	ep = mtcp->smap[epid].ep;
	if (!ep) {
		errno = EINVAL;
		return -1;
	}

	DestroyEventQueue(ep->usr_queue);
	DestroyEventQueue(ep->usr_shadow_queue);
	DestroyEventQueue(ep->mtcp_queue);

	pthread_mutex_lock(&ep->epoll_lock);
	mtcp->ep = NULL;
	mtcp->smap[epid].ep = NULL;
	pthread_cond_signal(&ep->epoll_cond);
	pthread_mutex_unlock(&ep->epoll_lock);

	pthread_cond_destroy(&ep->epoll_cond);
	pthread_mutex_destroy(&ep->epoll_lock);
	free(ep);

	return 0;
}
/*----------------------------------------------------------------------------*/
static int 
RaisePendingStreamEvents(mtcp_manager_t mtcp, 
		struct mtcp_epoll *ep, socket_map_t socket)
{
	tcp_stream *stream = socket->stream;

	if (!stream)
		return -1;
	if (stream->state < TCP_ST_ESTABLISHED)
		return -1;

	TRACE_EPOLL("Stream %d at state %s\n", 
			stream->id, TCPStateToString(stream));
	/* if there are payloads already read before epoll registration */
	/* generate read event */
	if (socket->epoll & MTCP_EPOLLIN) {
		struct tcp_recv_vars *rcvvar = stream->rcvvar;
		if (rcvvar->rcvbuf && rcvvar->rcvbuf->merged_len > 0) {
			TRACE_EPOLL("Socket %d: Has existing payloads\n", socket->id);
			AddEpollEvent(ep, USR_SHADOW_EVENT_QUEUE, socket, MTCP_EPOLLIN);
		} else if (stream->state == TCP_ST_CLOSE_WAIT) {
			TRACE_EPOLL("Socket %d: Waiting for close\n", socket->id);
			AddEpollEvent(ep, USR_SHADOW_EVENT_QUEUE, socket, MTCP_EPOLLIN);
		}
	}

	/* same thing to the write event */
	if (socket->epoll & MTCP_EPOLLOUT) {
		struct tcp_send_vars *sndvar = stream->sndvar;
		if (!sndvar->sndbuf || 
				(sndvar->sndbuf && sndvar->sndbuf->len < sndvar->snd_wnd)) {
			if (!(socket->events & MTCP_EPOLLOUT)) {
				TRACE_EPOLL("Socket %d: Adding write event\n", socket->id);
				AddEpollEvent(ep, USR_SHADOW_EVENT_QUEUE, socket, MTCP_EPOLLOUT);
			}
		}
	}

	return 0;
}
/*----------------------------------------------------------------------------*/
int 
mtcp_epoll_ctl(mctx_t mctx, int epid, 
		int op, int sockid, struct mtcp_epoll_event *event)
{
	mtcp_manager_t mtcp;
	struct mtcp_epoll *ep;
	socket_map_t socket;
	uint32_t events;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		return -1;
	}

	if (epid < 0 || epid >= CONFIG.max_concurrency) {
		TRACE_API("Epoll id %d out of range.\n", epid);
		errno = EBADF;
		return -1;
	}

	if (sockid < 0 || sockid >= CONFIG.max_concurrency) {
		TRACE_API("Socket id %d out of range.\n", sockid);
		errno = EBADF;
		return -1;
	}

	if (mtcp->smap[epid].socktype == MTCP_SOCK_UNUSED) {
		errno = EBADF;
		return -1;
	}

	if (mtcp->smap[epid].socktype != MTCP_SOCK_EPOLL) {
		errno = EINVAL;
		return -1;
	}

	ep = mtcp->smap[epid].ep;
	if (!ep || (!event && op != MTCP_EPOLL_CTL_DEL)) {
		errno = EINVAL;
		return -1;
	}
	socket = &mtcp->smap[sockid];

	if (op == MTCP_EPOLL_CTL_ADD) {
		if (socket->epoll) {
			errno = EEXIST;
			return -1;
		}

		/* EPOLLERR and EPOLLHUP are registered as default */
		events = event->events;
		events |= (MTCP_EPOLLERR | MTCP_EPOLLHUP);
		socket->ep_data = event->data;
		socket->epoll = events;

		TRACE_EPOLL("Adding epoll socket %d(type %d) ET: %u, IN: %u, OUT: %u\n", 
				socket->id, socket->socktype, socket->epoll & MTCP_EPOLLET, 
				socket->epoll & MTCP_EPOLLIN, socket->epoll & MTCP_EPOLLOUT);

		if (socket->socktype == MTCP_SOCK_STREAM) {
			RaisePendingStreamEvents(mtcp, ep, socket);
		} else if (socket->socktype == MTCP_SOCK_PIPE) {
			RaisePendingPipeEvents(mctx, epid, sockid);
		}

	} else if (op == MTCP_EPOLL_CTL_MOD) {
		if (!socket->epoll) {
			pthread_mutex_unlock(&ep->epoll_lock);
			errno = ENOENT;
			return -1;
		}

		events = event->events;
		events |= (MTCP_EPOLLERR | MTCP_EPOLLHUP);
		socket->ep_data = event->data;
		socket->epoll = events;

		if (socket->socktype == MTCP_SOCK_STREAM) {
			RaisePendingStreamEvents(mtcp, ep, socket);
		} else if (socket->socktype == MTCP_SOCK_PIPE) {
			RaisePendingPipeEvents(mctx, epid, sockid);
		}

	} else if (op == MTCP_EPOLL_CTL_DEL) {
		if (!socket->epoll) {
			errno = ENOENT;
			return -1;
		}

		socket->epoll = MTCP_EPOLLNONE;
	}

	return 0;
}
/*----------------------------------------------------------------------------*/
int 
mtcp_epoll_wait(mctx_t mctx, int epid, 
		struct mtcp_epoll_event *events, int maxevents, int timeout)
{
	// pxw
//	FILE *fp = fopen ("/home/pxw/glusterfs-3.9.1/rpc/rpc-transport/dpdk/log_mtcp", "a+");
		
//	fprintf (fp, "mtcp_epoll_wait\n");

	mtcp_manager_t mtcp;
	struct mtcp_epoll *ep;
	struct event_queue *eq;
	struct event_queue *eq_shadow;
	socket_map_t event_socket;
	int validity;
	int i, cnt, ret;
	int num_events;


	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
	//	fprintf (fp, "failed to get mtcp manager\n");
		return -1;
	}

	if (epid < 0 || epid >= CONFIG.max_concurrency) {
		TRACE_API("Epoll id %d out of range.\n", epid);
		errno = EBADF;
		return -1;
	}

	if (mtcp->smap[epid].socktype == MTCP_SOCK_UNUSED) {
	//	fprintf (fp, "aaa\n");
		errno = EBADF;
		return -1;
	}

	if (mtcp->smap[epid].socktype != MTCP_SOCK_EPOLL) {
	//	fprintf (fp, "bbb\n");
		errno = EINVAL;
		return -1;
	}

	ep = mtcp->smap[epid].ep;
	if (!ep || !events || maxevents <= 0) {
	//	fprintf (fp, "ccc\n");
		errno = EINVAL;
		return -1;
	}

	ep->stat.calls++;

//不执行
#if SPIN_BEFORE_SLEEP
	int spin = 0;
	while (ep->num_events == 0 && spin < SPIN_THRESH) {  //如果设置了spin_thresh, spin一会儿再休眠
		spin++;
	} 
#endif /* SPIN_BEFORE_SLEEP */

	if (pthread_mutex_lock(&ep->epoll_lock)) {
		if (errno == EDEADLK) {
	//	    fprintf (fp, "DEADLOCK\n");
			perror("mtcp_epoll_wait: epoll_lock blocked\n");
		}
		assert(0);
	}

wait:
	eq = ep->usr_queue;
	eq_shadow = ep->usr_shadow_queue;

	/* wait until event occurs */
	while (eq->num_events == 0 && eq_shadow->num_events == 0 && timeout != 0) { //还没有事件时。。

	//	fprintf (fp, "app thread is going to sleep\n");

// true
#if INTR_SLEEPING_MTCP
		/* signal to mtcp thread if it is sleeping */
		if (mtcp->wakeup_flag && mtcp->is_sleeping) { //如果mtcp线程也在睡眠，先唤醒它。。
	//		fprintf (fp, "wake up mtcp thread first\n");
			pthread_kill(mtcp->ctx->thread, SIGUSR1);
		}
#endif
		ep->stat.waits++;
		ep->waiting = TRUE;
		if (timeout > 0) {
			struct timespec deadline;

			clock_gettime(CLOCK_REALTIME, &deadline);
			if (timeout >= 1000) {
				int sec;
				sec = timeout / 1000;
				deadline.tv_sec += sec;
				timeout -= sec * 1000;
			}

			deadline.tv_nsec += timeout * 1000000;

			if (deadline.tv_nsec >= 1000000000) {
				deadline.tv_sec++;
				deadline.tv_nsec -= 1000000000;
			}

			//deadline.tv_sec = mtcp->cur_tv.tv_sec;
			//deadline.tv_nsec = (mtcp->cur_tv.tv_usec + timeout * 1000) * 1000;
			ret = pthread_cond_timedwait(&ep->epoll_cond, &ep->epoll_lock, &deadline);
			if (ret && ret != ETIMEDOUT) {
				/* errno set by pthread_cond_timedwait() */
				pthread_mutex_unlock(&ep->epoll_lock);
				TRACE_ERROR("pthread_cond_timedwait failed. ret: %d, error: %s\n", 
						ret, strerror(errno));
				return -1;
			}
			timeout = 0;
		} else if (timeout < 0) { //pxw ----------------timeout设置成-1
    //		fprintf (fp, "app thread is going to cond_wait\n");
	//		fflush(fp);
			ret = pthread_cond_wait(&ep->epoll_cond, &ep->epoll_lock); //释放锁，等待mtcp唤醒
			//条件等待设置失败:
			if (ret) {
				/* errno set by pthread_cond_wait() */
				pthread_mutex_unlock(&ep->epoll_lock);
				TRACE_ERROR("pthread_cond_wait failed. ret: %d, error: %s\n", 
						ret, strerror(errno));
				return -1;
			}
		}

		//app thread被唤醒后：
		ep->waiting = FALSE;

		// pxw
	//	fprintf (fp, "app thread has been awaked\n");
	//	fflush(fp);

		if (mtcp->ctx->done || mtcp->ctx->exit || mtcp->ctx->interrupt) {
			mtcp->ctx->interrupt = FALSE;
			//ret = pthread_cond_signal(&ep->epoll_cond);
			pthread_mutex_unlock(&ep->epoll_lock);
			errno = EINTR;
			return -1;
		}
	
	}

	//线程准备获取事件：
//	fprintf(fp, "app thread got events\n");
//	fflush(fp);

	/* fetch events from the user event queue */
	cnt = 0;
	num_events = eq->num_events;
	for (i = 0; i < num_events && cnt < maxevents; i++) {
		event_socket = &mtcp->smap[eq->events[eq->start].sockid];
		validity = TRUE;
		if (event_socket->socktype == MTCP_SOCK_UNUSED)
			validity = FALSE;
		if (!(event_socket->epoll & eq->events[eq->start].ev.events))
			validity = FALSE;
		if (!(event_socket->events & eq->events[eq->start].ev.events))
			validity = FALSE;

		if (validity) {
			events[cnt++] = eq->events[eq->start].ev;  // copy
			assert(eq->events[eq->start].sockid >= 0);
		    // pxw
//			fprintf (fp, "event_socket = %d\n", eq->events[eq->start].ev.data.sockid); //mtcp_epoll_data中sockid是无穷大/小的数
//			fflush (fp);
/*
			fprintf (fp, "Socket %d: Handled event. event: %s, "
					"start: %u, end: %u, num: %u\n", 
					event_socket->id, 
					EventToString(eq->events[eq->start].ev.events), 
					eq->start, eq->end, eq->num_events);
			fflush (fp);
*/

			TRACE_EPOLL("Socket %d: Handled event. event: %s, "
					"start: %u, end: %u, num: %u\n", 
					event_socket->id, 
					EventToString(eq->events[eq->start].ev.events), 
					eq->start, eq->end, eq->num_events);
			ep->stat.handled++;
		} else {
			TRACE_EPOLL("Socket %d: event %s invalidated.\n", 
					eq->events[eq->start].sockid, 
					EventToString(eq->events[eq->start].ev.events));
			ep->stat.invalidated++;
		}
		event_socket->events &= (~eq->events[eq->start].ev.events);

		eq->start++;
		eq->num_events--;
		if (eq->start >= eq->size) {
			eq->start = 0;
		}
	}


	//有事件发生时：
//	fprintf (fp, "event comes\n");

	/* fetch eventes from user shadow event queue */
	eq = ep->usr_shadow_queue;
	num_events = eq->num_events; //获取的事件个数
	for (i = 0; i < num_events && cnt < maxevents; i++) {
		event_socket = &mtcp->smap[eq->events[eq->start].sockid];
	
		validity = TRUE;
		if (event_socket->socktype == MTCP_SOCK_UNUSED)
			validity = FALSE;
		if (!(event_socket->epoll & eq->events[eq->start].ev.events))
			validity = FALSE;
		if (!(event_socket->events & eq->events[eq->start].ev.events))
			validity = FALSE;

		if (validity) {
			events[cnt++] = eq->events[eq->start].ev;
			assert(eq->events[eq->start].sockid >= 0);

			TRACE_EPOLL("Socket %d: Handled event. event: %s, "
					"start: %u, end: %u, num: %u\n", 
					event_socket->id, 
					EventToString(eq->events[eq->start].ev.events), 
					eq->start, eq->end, eq->num_events);
			ep->stat.handled++;
		} else {
			TRACE_EPOLL("Socket %d: event %s invalidated.\n", 
					eq->events[eq->start].sockid, 
					EventToString(eq->events[eq->start].ev.events));
			ep->stat.invalidated++;
		}
		event_socket->events &= (~eq->events[eq->start].ev.events);

		eq->start++;
		eq->num_events--;
		if (eq->start >= eq->size) {
			eq->start = 0;
		}
	}


	if (cnt == 0 && timeout != 0)
		goto wait;

	pthread_mutex_unlock(&ep->epoll_lock);

	// pxw
//	fclose (fp);

	return cnt;
}
/*----------------------------------------------------------------------------*/
inline int 
AddEpollEvent(struct mtcp_epoll *ep, 
		int queue_type, socket_map_t socket, uint32_t event)
{
	//FILE *fp = fopen ("/home/pxw/glusterfs-3.9.1/rpc/rpc-transport/dpdk/AddEpollEvent", "a+");
	//fprintf (fp, "AddEpollEvent\n");

	struct event_queue *eq;
	int index;

	if (!ep || !socket || !event)
		return -1;
	
	ep->stat.issued++;

	if (socket->events & event) {
		return 0;
	}

	if (queue_type == MTCP_EVENT_QUEUE) {
		eq = ep->mtcp_queue;
		//fprintf (fp, "mtcp_queue\n");
	} else if (queue_type == USR_EVENT_QUEUE) {
		eq = ep->usr_queue;
		//fprintf (fp, "usr_queue\n");
		pthread_mutex_lock(&ep->epoll_lock);
	} else if (queue_type == USR_SHADOW_EVENT_QUEUE) {
		eq = ep->usr_shadow_queue;
		//fprintf (fp, "usr_shadow_queue\n");
	} else {
		TRACE_ERROR("Non-existing event queue type!\n");
		return -1;
	}

	if (eq->num_events >= eq->size) {
		TRACE_ERROR("Exceeded epoll event queue! num_events: %d, size: %d\n", 
				eq->num_events, eq->size);
		if (queue_type == USR_EVENT_QUEUE)
			pthread_mutex_unlock(&ep->epoll_lock);
		return -1;
	}

	index = eq->end++;

	socket->events |= event;
	eq->events[index].sockid = socket->id; //sockid 是？
	eq->events[index].ev.events = event;
	eq->events[index].ev.data = socket->ep_data; //data里的sockid是？

	//fprintf (fp, "eq->events.sockid = %d, eq->events.ev.events = %d, eq->events.ev.data.sockid = %d\n",
	//			socket->id, event, socket->ep_data.sockid);
	//fflush (fp);
	//fclose (fp);

	if (eq->end >= eq->size) {
		eq->end = 0;
	}
	eq->num_events++;

#if 0
	TRACE_EPOLL("Socket %d New event: %s, start: %u, end: %u, num: %u\n",
			ep->events[index].sockid, 
			EventToString(ep->events[index].ev.events), 
			ep->start, ep->end, ep->num_events);
#endif

	if (queue_type == USR_EVENT_QUEUE)
		pthread_mutex_unlock(&ep->epoll_lock);

	ep->stat.registered++;

	return 0;
}
