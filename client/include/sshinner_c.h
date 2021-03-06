#ifndef _SSHINNER_C_H
#define _SSHINNER_C_H

#include <stdio.h>

#include <systemd/sd-id128.h> 

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <event2/listener.h>
#include <event2/util.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/bufferevent.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "sshinner.h"
#include "st_others.h"
#include "st_slist.h"

#include "sshinner_crypt.h"

enum CLT_TYPE {
    C_DAEMON, C_USR,
};


/* A connection queue. */
typedef struct conn_item {
    SLIST_HEAD      list;
    int             socket;
    union {
        unsigned long dat;
        void*         ptr;
    } arg;
    char            buf[512];
}C_ITEM, *P_C_ITEM;

struct event;
typedef struct _thread_obj {
    pthread_t       thread_id;        /* unique ID of this thread */
    struct event_base *base;    /* libevent handle this thread uses */
    struct event *p_notify_event;  /* listen event for notify pipe */
    int notify_receive_fd;      /* receiving end of notify pipe */
    int notify_send_fd;         /* sending end of notify pipe */

    pthread_mutex_t q_lock;
    SLIST_HEAD      conn_queue;    /* queue of new connections to handle */
} THREAD_OBJ, *P_THREAD_OBJ;

extern RET_T sc_create_ss5_worker_threads(size_t thread_num, P_THREAD_OBJ threads);


/**
 * 采用本地和服务器数据直接单独连接
 */
// 启动时候加载的端口映射
typedef struct _portmap {
    unsigned short usrport;
    unsigned short daemonport;
} PORTMAP, *P_PORTMAP;

// 实际传输时候的端口和bufferevent，为动态创建，动态关闭的
typedef struct _porttrans {
    SLIST_HEAD          list;
    unsigned short      l_port;      //本地实际传输的端口
    struct bufferevent *local_bev;
    struct bufferevent *srv_bev;

    int                 is_enc;
    ENCRYPT_CTX         ctx_enc;
    ENCRYPT_CTX         ctx_dec;

} PORTTRANS, *P_PORTTRANS;

// 目前硬编码了，实际在服务端是链表没有限制的
#define MAX_PORT_NUM 10


static const char* PUBLIC_KEY_FILE = "./ssl/public.key";

typedef struct _clt_opt
{
    enum CLT_TYPE       C_TYPE;    //DAEMON/USR
    sd_id128_t          mach_uuid;
    char hostname[128];
    char username[128];  
    unsigned long       userid;
    struct sockaddr_in  srv;
    struct bufferevent* srv_bev;    //主要是控制信息通信
    unsigned short      ss5_port;    // SS5代理只支持用DAEMON端启动，因为USR端没法单独启动
    sd_id128_t          session_uuid;

    unsigned char       enc_key[RC4_MD5_IV_LEN];

    PORTMAP             maps[MAX_PORT_NUM];

    pthread_t           main_thread_id;
    int                 thread_num;
    P_THREAD_OBJ        thread_objs;

    pthread_mutex_t     trans_lock;
    SLIST_HEAD          trans;
}CLT_OPT, *P_CLT_OPT;


extern struct event_base *base;
extern CLT_OPT cltopt;

/**
 * 通用类函数
 */
extern RET_T load_settings_client(P_CLT_OPT p_opt);
extern void dump_clt_opts(P_CLT_OPT p_opt);


/**
 * 客户端与USR/DAEMON通信类函数
 */
// 连接函数
void bufferevent_cb(struct bufferevent *bev, short events, void *ptr);
void accept_conn_cb(struct evconnlistener *listener,
    evutil_socket_t fd, struct sockaddr *address, int socklen,
    void *ctx);
void accept_error_cb(struct evconnlistener *listener, void *ctx);
// 数据处理类函数
void bufferread_cb(struct bufferevent *bev, void *ptr);


void ss5_accept_conn_cb(struct evconnlistener *listener,
    evutil_socket_t fd, struct sockaddr *address, int socklen,
    void *ctx);
void ss5_bufferread_cb_enc(struct bufferevent *bev, void *ptr);
void ss5_bufferevent_cb(struct bufferevent *bev, short events, void *ptr);

/**
 * 客户端与SRV的通信
 */
RET_T sc_connect_srv(int srv_fd);
RET_T sc_daemon_init_srv(int srv_fd);
RET_T sc_usr_init_srv(int srv_fd);
RET_T sc_daemon_ss5_init_srv(int srv_fd, const char* request, unsigned short l_port);

RET_T sc_send_head_cmd(int cmd, unsigned long extra_param, 
                        unsigned short usrport, unsigned daemonport);

void sc_set_eventcb_srv(int srv_fd, struct event_base *base);

void srv_bufferread_cb(struct bufferevent *bev, void *ptr);
void srv_bufferevent_cb(struct bufferevent *bev, short events, void *ptr);

P_PORTMAP sc_find_usr_portmap(unsigned short usrport);
P_PORTMAP sc_find_daemon_portmap(unsigned short daemonport, int createit);

extern P_PORTTRANS sc_find_trans(unsigned short l_sock);
extern P_PORTTRANS sc_create_trans(unsigned short l_sock);
extern RET_T sc_free_trans(P_PORTTRANS p_trans);
extern RET_T sc_free_all_trans(void);


/**
 * 线程池索引的映射
 */
static inline P_THREAD_OBJ sc_get_threadobj(unsigned long salt)
{
    return (&cltopt.thread_objs[ (salt % cltopt.thread_num)] ); 
}

#endif
