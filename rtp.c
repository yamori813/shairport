/*
 * Apple RTP protocol handler. This file is part of Shairport.
 * Copyright (c) James Laird 2013
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <memory.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include "common.h"
#include "player.h"

// only one RTP session can be active at a time.
static int running = 0;
static int please_shutdown;

static SOCKADDR rtp_client;
static int sock;
static pthread_t rtp_thread;

uint8_t packet[1024 * 4 * 4];

static int rtp_tcp_read_chunk(int sock) {
    int len;
    int t, n;
    n = 0;
    while(n != 4) {
        t = recv(sock, packet+n, 4 - n, 0);
        if (t == 0)
            return 0;
        n += t;
    }
    if(packet[0] == 0x24 && packet[1] == 0x00) {
        len = packet[2] * 0x100 + packet[3];
        n = 0;
        while(len != n) {
            t = recv(sock, packet + n, len - n, 0);
            if (t == 0)
                return 0;
            n += t;
        }
     } else {
        debug(1, "header error %x %x\n", packet[0], packet[1]);
        return 0;
     }
     return len;
}

static void *rtp_tcp_receiver(void *arg) {
    struct sockaddr_in cli_addr;
    socklen_t clilen;

    listen(sock, 5);
    clilen = sizeof(cli_addr);
    int new_sock = accept(sock, (struct sockaddr *)&cli_addr, &clilen);
    for(;;) {
        int len = rtp_tcp_read_chunk(new_sock);
        if (len == 0)
            break;
        player_put_tcp_packet(packet + 12, len - 12);
    }

    debug(1, "RTP thread interrupted. terminating.\n");
    close(new_sock);
    close(sock);
    config.output->stop();

    return NULL;
}

static void *rtp_receiver(void *arg) {
    // we inherit the signal mask (SIGUSR1)
    uint8_t packet[2048], *pktp;

    ssize_t nread;
    while (1) {
        if (please_shutdown)
            break;
        nread = recv(sock, packet, sizeof(packet), 0);
        if (nread < 0)
            break;

        ssize_t plen = nread;
        uint8_t type = packet[1] & ~0x80;
        if (type == 0x54) // sync
            continue;
        if (type == 0x60 || type == 0x56) {   // audio data / resend
            pktp = packet;
            if (type==0x56) {
                pktp += 4;
                plen -= 4;
            }
            seq_t seqno = ntohs(*(unsigned short *)(pktp+2));

            pktp += 12;
            plen -= 12;

            // check if packet contains enough content to be reasonable
            if (plen >= 16) {
                player_put_packet(seqno, pktp, plen);
                continue;
            }
            if (type == 0x56 && seqno == 0) {
                debug(2, "resend-related request packet received, ignoring.\n");
                continue;
            }
            debug(1, "Unknown RTP packet of type 0x%02X length %d seqno %d\n", type, nread, seqno);
            continue;
        }
        warn("Unknown RTP packet of type 0x%02X length %d", type, nread);
    }

    debug(1, "RTP thread interrupted. terminating.\n");
    close(sock);

    return NULL;
}

static int bind_tcp_port(SOCKADDR *remote) {
    struct sockaddr_in serv_addr;
    int port = 6000;

    sock = socket(remote->SAFAMILY, SOCK_STREAM, 0);
    if (sock < 0)
        die("could not open socket!");

    memset((char *)&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = remote->SAFAMILY;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    int ret = bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    if (ret < 0)
        die("could not bind a TCP port!");

    return port;
}

static int bind_port(SOCKADDR *remote) {
    struct addrinfo hints, *info;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = remote->SAFAMILY;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    int ret = getaddrinfo(NULL, "0", &hints, &info);

    if (ret < 0)
        die("failed to get usable addrinfo?! %s", gai_strerror(ret));

    sock = socket(remote->SAFAMILY, SOCK_DGRAM, IPPROTO_UDP);
    ret = bind(sock, info->ai_addr, info->ai_addrlen);

    freeaddrinfo(info);

    if (ret < 0)
        die("could not bind a UDP port!");

    int sport;
    SOCKADDR local;
    socklen_t local_len = sizeof(local);
    getsockname(sock, (struct sockaddr*)&local, &local_len);
#ifdef USE_IPV6
    if (local.SAFAMILY == AF_INET6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)&local;
        sport = htons(sa6->sin6_port);
    } else
#endif
    {
        struct sockaddr_in *sa = (struct sockaddr_in*)&local;
        sport = htons(sa->sin_port);
    }

    return sport;
}


int rtp_setup(SOCKADDR *remote, int type, int cport, int tport) {
    if (running)
        die("rtp_setup called with active stream!");

    debug(1, "rtp_setup: cport=%d tport=%d\n", cport, tport);

    // we do our own timing and ignore the timing port.
    // an audio perfectionist may wish to learn the protocol.

    memcpy(&rtp_client, remote, sizeof(rtp_client));
#ifdef USE_IPV6
    if (rtp_client.SAFAMILY == AF_INET6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)&rtp_client;
        sa6->sin6_port = htons(cport);
    } else
#endif
    {
        struct sockaddr_in *sa = (struct sockaddr_in*)&rtp_client;
        sa->sin_port = htons(cport);
    }

    int sport = type ? bind_tcp_port(remote) : bind_port(remote);

    debug(1, "rtp listening on %s port %d\n", type ? "TCP" : "UDP", sport);

    please_shutdown = 0;
    pthread_create(&rtp_thread, NULL, type ? &rtp_tcp_receiver : &rtp_receiver,
        NULL);

    running = 1;
    return sport;
}

void rtp_shutdown(void) {
    if (!running)
        die("rtp_shutdown called without active stream!");

    debug(2, "shutting down RTP thread\n");
    please_shutdown = 1;
    pthread_kill(rtp_thread, SIGUSR1);
    void *retval;
    pthread_join(rtp_thread, &retval);
    running = 0;
}

void rtp_request_resend(seq_t first, seq_t last) {
    if (!running)
        die("rtp_request_resend called without active stream!");

    debug(1, "requesting resend on %d packets (%04X:%04X)\n",
         seq_diff(first,last) + 1, first, last);

    char req[8];    // *not* a standard RTCP NACK
    req[0] = 0x80;
    req[1] = 0x55|0x80;  // Apple 'resend'
    *(unsigned short *)(req+2) = htons(1);  // our seqnum
    *(unsigned short *)(req+4) = htons(first);  // missed seqnum
    *(unsigned short *)(req+6) = htons(last-first+1);  // count

    sendto(sock, req, sizeof(req), 0, (struct sockaddr*)&rtp_client, sizeof(rtp_client));
}
