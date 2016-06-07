#include <errno.h>
#include <stdio.h>
#include <systemd/sd-id128.h> 


#include <json-c/json.h>
#include <json-c/json_tokener.h>

#include "sshinner_c.h"


/**
 * 客户端和远程服务器的交互
 */
void srv_bufferread_cb(struct bufferevent *bev, void *ptr)
{
    size_t n = 0;
    PKG_HEAD head;
    RET_T  ret;

    struct evbuffer *input = bufferevent_get_input(bev);
    struct evbuffer *output = bufferevent_get_output(bev);

    if ( evbuffer_remove(input, &head, HEAD_LEN) != HEAD_LEN)
    {
        st_d_print("Can not read HEAD_LEN(%d), drop it!", HEAD_LEN);
        return;
    }

    if (!sd_id128_equal(head.mach_uuid, cltopt.session_uuid))
    {
        SYS_ABORT("session_uuid not equal!");
    }

    if (head.type == 'C') 
    {
        if (head.ext == 'E') 
        {
            st_d_error("SERVER RETURNED ERROR!");
            exit(EXIT_SUCCESS);
        }
    }
    else
    {
        void *dat = malloc(head.dat_len);
        if (!dat)
        {
            st_d_error("Allocating %d error!", head.dat_len); 
            return;
        }
        
        memset(dat, 0, head.dat_len);
        size_t offset = 0;
        while ((n = evbuffer_remove(input, dat+offset, head.dat_len-offset)) > 0) 
        {
            if (n < (head.dat_len-offset)) 
            {
                offset += n;
                continue;
            }
            else
            break;
        }

        ulong crc = crc32(0L, dat, head.dat_len);
        if (crc != head.crc) 
        {
            st_d_error("Recv data may broken: %lu-%lu", crc, head.crc); 
            free(dat);
            return;
        }

        P_PORTMAP p_map = NULL;

        // 数据，根据 USR DAEMON 角色处理
        if (cltopt.C_TYPE == C_DAEMON) 
        {
            if (head.direct != USR_DAEMON) 
            {
                SYS_ABORT("Package direction check error!");
            }

            p_map = sc_find_create_portmap(head.daemonport); 
            if (!p_map) 
            {
                SYS_ABORT("USR should already exist!");
            }

            if (!p_map->bev) 
            {
                st_d_print("Daemon create local ev!");

                p_map->usrport = head.usrport;

                int fd = socket(AF_INET, SOCK_STREAM, 0);
                int reuseaddr_on = 1;
                if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_on, 
            		sizeof(reuseaddr_on)) == -1)
                {
                    st_d_print("Reuse socket opt faile!\n");
                    exit(EXIT_FAILURE);
                }
                struct sockaddr_in  local_srv;
                local_srv.sin_family = AF_INET;
                local_srv.sin_addr.s_addr = inet_addr("127.0.0.1");
                local_srv.sin_port = htons(head.daemonport);

                if (connect(fd, (struct sockaddr *)&local_srv, sizeof(local_srv))) 
                {
                    SYS_ABORT("Connect to server failed!\n"); 
                }
                else
                {
                    st_d_print("Connected to local OK!");
                }

                evutil_make_socket_nonblocking(fd);
                struct event_base *base = bufferevent_get_base(bev);
                struct bufferevent *n_bev = 
                    bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
                bufferevent_setcb(n_bev, bufferread_cb, NULL, NULL, p_map);
                p_map->bev = n_bev;
                bufferevent_enable(n_bev, EV_READ|EV_WRITE);

            }


            bufferevent_write(p_map->bev, dat, head.dat_len);
            free(dat);

        }
        else if (cltopt.C_TYPE == C_USR) 
        {
            if (head.direct != DAEMON_USR) 
            {
                SYS_ABORT("Package direction check error!");
            }

            p_map = sc_find_portmap(head.usrport);
            if (!p_map || !p_map->bev) 
            {
                SYS_ABORT("USR should already exist!");
            }

            bufferevent_write(p_map->bev, dat, head.dat_len);
            free(dat);
        }
    }
}


/**
 * 客户端处理本地USR/DAEMON的请求
 */

void bufferevent_cb(struct bufferevent *bev, short events, void *ptr)
{
    struct event_base *base = bufferevent_get_base(bev);
    int loop_terminate_flag = 0;

    //只有使用bufferevent_socket_connect进行的连接才会得到CONNECTED的事件
    if (events & BEV_EVENT_CONNECTED) 
    {
        st_d_print("GOT BEV_EVENT_CONNECTED event! ");
    } 
    else if (events & BEV_EVENT_ERROR) 
    {
        st_d_print("GOT BEV_EVENT_ERROR event! ");
        loop_terminate_flag = 1;
    } 
    else if (events & BEV_EVENT_EOF) 
    {
        st_d_print("GOT BEV_EVENT_EOF event! ");

        if (cltopt.C_TYPE == C_USR) 
        {
            st_d_print("Daemon terminated, so we exit!");
            exit(EXIT_SUCCESS);
        }
        else
        {
            bufferevent_free(bev); 
        }
    }
    else if (events & BEV_EVENT_TIMEOUT) 
    {
        st_d_print("GOT BEV_EVENT_TIMEOUT event! ");
    } 
    /*
    else if (events & BEV_EVENT_READING) 
    {
        st_d_print("GOT BEV_EVENT_READING event! ");
    } 
    else if (events & BEV_EVENT_WRITING) 
    {
        st_d_print("GOT BEV_EVENT_WRITING event! ");
    } 
    */ 

    if (loop_terminate_flag)
    {
        bufferevent_free(bev);
        event_base_loopexit(base, NULL);
    }

    return;
}


/**
 * 客户端程序，USR/DAEMON都是从应用程序读取到数据了，然后推送到SRV进行转发到服务器 
 */
void bufferread_cb(struct bufferevent *bev, void *ptr)
{
    P_PORTMAP p_map = (P_PORTMAP)ptr; 
    char h_buff[8196];  /*添加头部优化*/
    P_PKG_HEAD p_head = NULL;
    size_t n = 0;

    struct evbuffer *input = bufferevent_get_input(bev);
    struct evbuffer *output = bufferevent_get_output(bev);

    memset(h_buff, 0, sizeof(h_buff));

    if (cltopt.C_TYPE == C_USR)
    {
        p_head = GET_PKG_HEAD(h_buff);
        p_head->type = 'D';
        p_head->direct = USR_DAEMON;    // USR->DAEMON
        p_head->mach_uuid = cltopt.session_uuid;
        p_head->daemonport = p_map->daemonport;
        p_head->usrport = p_map->usrport;

        while ((n = evbuffer_remove(input, GET_PKG_BODY(h_buff), sizeof(h_buff) - HEAD_LEN)) > 0) 
        {
            p_head->dat_len = n;
            p_head->crc = crc32(0L, GET_PKG_BODY(h_buff), n);
            bufferevent_write(cltopt.srv_bev, h_buff, HEAD_LEN + p_head->dat_len);
        }
    }
    else
    {
        p_head = GET_PKG_HEAD(h_buff);
        p_head->type = 'D';
        p_head->direct = DAEMON_USR;    // USR->DAEMON
        p_head->mach_uuid = cltopt.session_uuid;
        p_head->daemonport = p_map->daemonport;
        p_head->usrport = p_map->usrport;

        while ((n = evbuffer_remove(input, GET_PKG_BODY(h_buff), sizeof(h_buff) - HEAD_LEN)) > 0) 
        {
            p_head->dat_len = n;
            p_head->crc = crc32(0L, GET_PKG_BODY(h_buff), n); 
            bufferevent_write(cltopt.srv_bev, h_buff, HEAD_LEN + p_head->dat_len);
        }
    }

    return;
}

/**
 * 只会在USR端被调用
 */
void accept_conn_cb(struct evconnlistener *listener,
    evutil_socket_t fd, struct sockaddr *address, int socklen,
    void *ctx)
{
    P_PORTMAP p_map = (P_PORTMAP)ctx; 
    char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

    getnameinfo (address, socklen,
               hbuf, sizeof(hbuf),sbuf, sizeof(sbuf),
               NI_NUMERICHOST | NI_NUMERICSERV);

    st_print("WELCOME NEW CONNECT (HOST=%s, PORT=%s)\n", hbuf, sbuf);

    /* We got a new connection! Set up a bufferevent for it. */
    struct event_base *base = evconnlistener_get_base(listener);
    struct bufferevent *bev = 
        bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);

    p_map->bev = bev;

    /**
     * 对于服务端，一般都是阻塞在读，而如果要写，一般在read_cb中写回就可以了
     */
    bufferevent_setcb(bev, bufferread_cb, NULL, bufferevent_cb, p_map);
    bufferevent_enable(bev, EV_READ|EV_WRITE);

    st_d_print("Allocate and attach new bufferevent for new connectino...");

    return;
}

void accept_error_cb(struct evconnlistener *listener, void *ctx)
{
    struct event_base *base = evconnlistener_get_base(listener);
    int err = EVUTIL_SOCKET_ERROR();

    st_d_error( "Got an error %d (%s) on the listener. "
                "Shutting down...\n", err, evutil_socket_error_to_string(err));

    //event_base_loopexit(base, NULL);

    return;
}