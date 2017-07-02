/*
Copyright (c) 2010 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*/

#include <event2/event.h>

#include <stdint.h>
#include <libutp/utp.h>

#include "transmission.h"
#include "log.h"
#include "net.h"
#include "session.h"
#include "crypto-utils.h" /* tr_rand_int_weak() */
#include "peer-io.h"
#include "peer-mgr.h"
#include "peer-socket.h"
#include "tr-assert.h"
#include "tr-utp.h"
#include "utils.h"

#ifndef WITH_UTP

#define MY_NAME "UTP"

#define dbgmsg(...) tr_logAddDeepNamed(MY_NAME, __VA_ARGS__)

void utp_close(struct UTPSocket* socket)
{
    tr_logAddNamedError(MY_NAME, "utp_close(%p) was called.", socket);
    dbgmsg("utp_close(%p) was called.", socket);
}

void utp_read_drained(struct UTPSocket* socket)
{
    tr_logAddNamedError(MY_NAME, "utp_read_drained(%p) was called.", socket);
    dbgmsg("utp_read_drained(%p) was called.", socket);
}

ssize_t utp_write(struct UTPSocket* socket, void* buf, size_t count)
{
    tr_logAddNamedError(MY_NAME, "utp_write(%p, %p, %zu) was called.", socket, buf, count);
    dbgmsg("utp_write(%p, %p, %zu) was called.", socket, buf, count);
    return -1;
}

void tr_utpInit(tr_session* session)
{
    TR_UNUSED(session);
}

int tr_utpPacket(unsigned char const* buf, size_t buflen, struct sockaddr const* from, socklen_t fromlen, tr_session* ss)
{
    TR_UNUSED(buf);
    TR_UNUSED(buflen);
    TR_UNUSED(from);
    TR_UNUSED(fromlen);
    TR_UNUSED(ss);

    return -1;
}

struct UTPSocket* utp_create_socket(struct struct_utp_context* ctx)
{
    TR_UNUSED(ctx);

    return NULL;
}

int utp_connect(struct UTPSocket* s, struct sockaddr const* to, socklen_t tolen)
{
    TR_UNUSED(s);
    TR_UNUSED(to);
    TR_UNUSED(tolen);

    return -1;
}

void tr_utpClose(tr_session* ss)
{
    TR_UNUSED(ss);
}

#else

/* utp_internal.cpp says "Should be called every 500ms" for utp_check_timeouts */
#define UTP_INTERVAL_US 500000

static void utp_on_accept(tr_session* const ss, struct UTPSocket* const s)
{
    struct sockaddr_storage from_storage;
    struct sockaddr* from = (struct sockaddr*)&from_storage;
    socklen_t fromlen = sizeof(from_storage);
    tr_address addr;
    tr_port port;

    if (!tr_sessionIsUTPEnabled(ss))
    {
        utp_close(s);
        return;
    }

    utp_getpeername(s, from, &fromlen);

    if (!tr_address_from_sockaddr_storage(&addr, &port, &from_storage))
    {
        tr_logAddNamedError("UTP", "Unknown socket family");
        utp_close(s);
        return;
    }

    tr_peerMgrAddIncoming(ss->peerMgr, &addr, port, tr_peer_socket_utp_create(s));
}

static void utp_send_to(
    tr_session* const ss,
    uint8_t const* const buf,
    size_t const buflen,
    struct sockaddr const* const to,
    socklen_t const tolen)
{
    if (to->sa_family == AF_INET && ss->udp_socket != TR_BAD_SOCKET)
    {
        sendto(ss->udp_socket, (void const*)buf, buflen, 0, to, tolen);
    }
    else if (to->sa_family == AF_INET6 && ss->udp6_socket != TR_BAD_SOCKET)
    {
        sendto(ss->udp6_socket, (void const*)buf, buflen, 0, to, tolen);
    }
}

#ifdef TR_UTP_TRACE

static void utp_log(tr_session* const ss, char const* const msg)
{
    TR_UNUSED(ss);

    fprintf(stderr, "[utp] %s\n", msg);
}

#endif

static uint64 utp_callback(utp_callback_arguments* args)
{
    tr_session* const session = utp_context_get_userdata(args->context);

    TR_ASSERT(tr_isSession(session));
    TR_ASSERT(session->utp_context == args->context);

    switch (args->callback_type)
    {
#ifdef TR_UTP_TRACE

    case UTP_LOG:
        utp_log(session, args->buf);
        break;

#endif

    case UTP_ON_ACCEPT:
        utp_on_accept(session, args->socket);
        break;

    case UTP_SENDTO:
        utp_send_to(session, args->buf, args->len, args->u1.address, args->u2.address_len);
        break;
    }

    return 0;
}

static void reset_timer(tr_session* ss)
{
    int sec;
    int usec;

    if (tr_sessionIsUTPEnabled(ss))
    {
        sec = 0;
        usec = UTP_INTERVAL_US / 2 + tr_rand_int_weak(UTP_INTERVAL_US);
    }
    else
    {
        /* If somebody has disabled uTP, then we still want to run
           utp_check_timeouts, in order to let closed sockets finish
           gracefully and so on.  However, since we're not particularly
           interested in that happening in a timely manner, we might as
           well use a large timeout. */
        sec = 2;
        usec = tr_rand_int_weak(1000000);
    }

    tr_timerAdd(ss->utp_timer, sec, usec);
}

static void timer_callback(evutil_socket_t s, short type, void* closure)
{
    TR_UNUSED(s);
    TR_UNUSED(type);

    tr_session* ss = closure;

    /* utp_internal.cpp says "Should be called each time the UDP socket is drained" but it's tricky with libevent */
    utp_issue_deferred_acks(ss->utp_context);

    utp_check_timeouts(ss->utp_context);
    reset_timer(ss);
}

void tr_utpInit(tr_session* session)
{
    if (session->utp_context != NULL)
    {
        return;
    }

    struct struct_utp_context* ctx = utp_init(2);

    if (ctx == NULL)
    {
        return;
    }

    utp_context_set_userdata(ctx, session);

    utp_set_callback(ctx, UTP_ON_ACCEPT, &utp_callback);
    utp_set_callback(ctx, UTP_SENDTO, &utp_callback);

    tr_peerIoUtpInit(ctx);

#ifdef TR_UTP_TRACE

    utp_set_callback(ctx, UTP_LOG, &utp_callback);

    utp_context_set_option(ctx, UTP_LOG_NORMAL, 1);
    utp_context_set_option(ctx, UTP_LOG_MTU, 1);
    utp_context_set_option(ctx, UTP_LOG_DEBUG, 1);

#endif

    session->utp_context = ctx;
}

int tr_utpPacket(unsigned char const* buf, size_t buflen, struct sockaddr const* from, socklen_t fromlen, tr_session* ss)
{
    if (!ss->isClosed && ss->utp_timer == NULL)
    {
        ss->utp_timer = evtimer_new(ss->event_base, timer_callback, ss);

        if (ss->utp_timer == NULL)
        {
            return -1;
        }

        reset_timer(ss);
    }

    int const ret = utp_process_udp(ss->utp_context, buf, buflen, from, fromlen);

    /* utp_internal.cpp says "Should be called each time the UDP socket is drained" but it's tricky with libevent */
    utp_issue_deferred_acks(ss->utp_context);

    return ret;
}

void tr_utpClose(tr_session* session)
{
    if (session->utp_timer != NULL)
    {
        evtimer_del(session->utp_timer);
        session->utp_timer = NULL;
    }

    if (session->utp_context != NULL)
    {
        utp_context_set_userdata(session->utp_context, NULL);
        utp_destroy(session->utp_context);
        session->utp_context = NULL;
    }
}

#endif /* #ifndef WITH_UTP ... else */
