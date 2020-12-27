// Part of readsb, a Mode-S/ADSB/TIS message decoder.
//
// net_io.c: network handling.
//
// Copyright (c) 2019 Michael Wolf <michael@mictronics.de>
//
// This code is based on a detached fork of dump1090-fa.
//
// Copyright (c) 2014-2016 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// This file incorporates work covered by the following copyright and
// license:
//
// Copyright (C) 2012 by Salvatore Sanfilippo <antirez@gmail.com>
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//  *  Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//
//  *  Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "readsb.h"

#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/sendfile.h>
//#include <brotli/encode.h>


//
// ============================= Networking =============================
//
// Note: here we disregard any kind of good coding practice in favor of
// extreme simplicity, that is:
//
// 1) We only rely on the kernel buffers for our I/O without any kind of
//    user space buffering.
// 2) We don't register any kind of event handler, from time to time a
//    function gets called and we accept new connections. All the rest is
//    handled via non-blocking I/O and manually polling clients to see if
//    they have something new to share with us when reading is needed.

static int handleApiRequest(struct client *c, char *p, int remote, uint64_t now);
static int handleBeastCommand(struct client *c, char *p, int remote, uint64_t now);
static int decodeBinMessage(struct client *c, char *p, int remote, uint64_t now);
static int decodeHexMessage(struct client *c, char *hex, int remote, uint64_t now);
static int decodeSbsLine(struct client *c, char *line, int remote, uint64_t now);
static int decodeSbsLineMlat(struct client *c, char *line, int remote, uint64_t now) {
    MODES_NOTUSED(remote);
    return decodeSbsLine(c, line, 64 + SOURCE_MLAT, now);
}
static int decodeSbsLinePrio(struct client *c, char *line, int remote, uint64_t now) {
    MODES_NOTUSED(remote);
    return decodeSbsLine(c, line, 64 + SOURCE_PRIO, now);
}
static int decodeSbsLineJaero(struct client *c, char *line, int remote, uint64_t now) {
    MODES_NOTUSED(remote);
    return decodeSbsLine(c, line, 64 + SOURCE_JAERO, now);
}

static void send_raw_heartbeat(struct net_service *service);
static void send_beast_heartbeat(struct net_service *service);
static void send_sbs_heartbeat(struct net_service *service);

static void autoset_modeac();
static int hexDigitVal(int c);
static void *pthreadGetaddrinfo(void *param);

static char *sprintAircraftObject(char *p, char *end, struct aircraft *a, uint64_t now, int printMode);
static void flushClient(struct client *c, uint64_t now);
static void read_uuid(struct client *c, char *p, char *eod);

//
//=========================================================================
//
// Networking "stack" initialization
//

// Init a service with the given read/write characteristics, return the new service.
// Doesn't arrange for the service to listen or connect
struct net_service *serviceInit(const char *descr, struct net_writer *writer, heartbeat_fn hb, read_mode_t mode, const char *sep, read_fn handler) {
    struct net_service *service;
    if (!descr) {
        fprintf(stderr, "Fatal: no service description\n");
        exit(1);
    }

    if (!(service = calloc(sizeof (*service), 1))) {
        fprintf(stderr, "Out of memory allocating service %s\n", descr);
        exit(1);
    }

    service->next = Modes.services;
    Modes.services = service;

    service->descr = descr;
    service->listener_count = 0;
    service->pusher_count = 0;
    service->connections = 0;
    service->writer = writer;
    service->read_sep = sep;
    service->read_sep_len = sep ? strlen(sep) : 0;
    service->read_mode = mode;
    service->read_handler = handler;
    service->clients = NULL;

    if (service->writer) {
        if (!service->writer->data) {
            if (!(service->writer->data = malloc(MODES_OUT_BUF_SIZE))) {
                fprintf(stderr, "Out of memory allocating output buffer for service %s\n", descr);
                exit(1);
            }
        }

        service->writer->service = service;
        service->writer->dataUsed = 0;
        service->writer->lastWrite = mstime();
        service->writer->send_heartbeat = hb;
        service->writer->lastReceiverId = 0;
    }

    return service;
}


static void setProxyString(struct client *c) {
    if (strlen(c->host) + strlen(c->port) + 2 > sizeof(c->proxy_string))
        return;
    if (!c->host[0] || !c->port[0])
        return;
    strcpy(c->proxy_string, c->host);
    uint32_t len = strlen(c->proxy_string);
    c->proxy_string[len] = ':';
    strcpy(c->proxy_string + len + 1, c->port);
    c->receiverId = fasthash64(c->proxy_string, strlen(c->proxy_string), 0x2127599bf4325c37ULL);
}

// Create a client attached to the given service using the provided socket FD
struct client *createSocketClient(struct net_service *service, int fd) {
    anetSetSendBuffer(Modes.aneterr, fd, (MODES_NET_SNDBUF_SIZE << Modes.net_sndbuf_size));
    return createGenericClient(service, fd);
}

// Create a client attached to the given service using the provided FD (might not be a socket!)

struct client *createGenericClient(struct net_service *service, int fd) {
    struct client *c;
    uint64_t now = mstime();

    anetNonBlock(Modes.aneterr, fd);

    if (!service || fd == -1) {
        fprintf(stderr, "<3> FATAL: createGenericClient called with invalid parameters!\n");
        exit(1);
    }
    if (!(c = (struct client *) calloc(1, sizeof (*c)))) {
        fprintf(stderr, "<3> FATAL: Out of memory allocating a new %s network client\n", service->descr);
        exit(1);
    }

    c->service = service;
    c->next = service->clients;
    c->fd = fd;
    c->buflen = 0;
    c->modeac_requested = 0;
    c->last_flush = now;
    c->last_send = now;
    c->sendq_len = 0;
    c->sendq_max = 0;
    c->sendq = NULL;
    c->con = NULL;
    c->last_read = now;
    c->proxy_string[0] = '\0';
    c->host[0] = '\0';
    c->port[0] = '\0';

    c->receiverId = random();
    c->receiverId <<= 22;
    c->receiverId ^= random();
    c->receiverId <<= 22;
    c->receiverId ^= random();

    c->receiverId2 = 0;

    c->receiverIdLocked = 0;

    c->connectedSince = mstime();

    //fprintf(stderr, "c->receiverId: %016"PRIx64"\n", c->receiverId);

    if (service->writer) {
        if (!(c->sendq = malloc(MODES_NET_SNDBUF_SIZE << Modes.net_sndbuf_size))) {
            fprintf(stderr, "Out of memory allocating client SendQ\n");
            exit(1);
        }
        // Have to keep track of this manually
        c->sendq_max = MODES_NET_SNDBUF_SIZE << Modes.net_sndbuf_size;
        service->writer->lastReceiverId = 0; // make sure to resend receiverId
    }
    service->clients = c;

    ++service->connections;
    if (service->writer && service->connections == 1) {
        service->writer->lastWrite = now; // suppress heartbeat initially
    }

    return c;
}

// Timer callback checking periodically whether the push service lost its server
// connection and requires a re-connect.
void serviceReconnectCallback(uint64_t now) {
    // Loop through the connectors, and
    //  - If it's not connected:
    //    - If it's "connecting", check to see if the fd is ready
    //    - Otherwise, if enough time has passed, try reconnecting

    for (int i = 0; i < Modes.net_connectors_count; i++) {
        struct net_connector *con = Modes.net_connectors[i];
        if (!con->connected) {
            if (con->connecting) {
                // Check to see...
                checkServiceConnected(con);
            } else {
                if (con->next_reconnect <= now) {
                    serviceConnect(con);
                }
            }
        }
    }
}

struct client *checkServiceConnected(struct net_connector *con) {
    int rv;

    struct pollfd pfd = {con->fd, (POLLIN | POLLOUT), 0};

    rv = poll(&pfd, 1, 0);

    if (rv == -1) {
        // select() error, just return a NULL here, but log it
        fprintf(stderr, "checkServiceConnected: select() error: %s\n", strerror(errno));
        return NULL;
    }

    if (rv == 0) {
        // If we've exceeded our connect timeout, bail but try again.
        if (mstime() >= con->connect_timeout) {
            fprintf(stderr, "%s: Connection timed out: %s:%s port %s\n",
                    con->service->descr, con->address, con->port, con->resolved_addr);
            con->connecting = 0;
            anetCloseSocket(con->fd);
        }
        return NULL;
    }

    // At this point, we need to check getsockopt() to see if we succeeded or failed...
    int optval = -1;
    socklen_t optlen = sizeof(optval);
    if (getsockopt(con->fd, SOL_SOCKET, SO_ERROR, &optval, &optlen) == -1) {
        fprintf(stderr, "getsockopt failed: %d (%s)\n", errno, strerror(errno));
        // Bad stuff going on, but clear this anyway
        con->connecting = 0;
        anetCloseSocket(con->fd);
        return NULL;
    }

    if (optval != 0) {
        // only 0 means "connection ok"
        fprintf(stderr, "%s: Connection to %s%s port %s failed: %d (%s)\n",
                con->service->descr, con->address, con->resolved_addr, con->port, optval, strerror(optval));
        con->connecting = 0;
        anetCloseSocket(con->fd);
        return NULL;
    }

    // If we're able to create this "client", save the sockaddr info and print a msg
    struct client *c;

    c = createSocketClient(con->service, con->fd);
    if (!c) {
        con->connecting = 0;
        fprintf(stderr, "createSocketClient failed on fd %d to %s%s port %s\n",
                con->fd, con->address, con->resolved_addr, con->port);
        anetCloseSocket(con->fd);
        return NULL;
    }

    strncpy(c->host, con->address, sizeof(c->host) - 1);
    strncpy(c->port, con->port, sizeof(c->port) - 1);
    setProxyString(c);

    con->connecting = 0;
    con->connected = 1;
    con->lastConnect = mstime();
    c->con = con;

    if (!Modes.interactive) {
        fprintf(stderr, "%s: Connection established: %s%s port %s\n",
                con->service->descr, con->address, con->resolved_addr, con->port);
    }

    // sending UUID if hostname matches adsbexchange
    if (c->sendq && strstr(con->address, "feed.adsbexchange.com")) {
        char buf[130];
        unsigned char *sendq = c->sendq;
        sendq[0] = 0x1A;
        sendq[1] = 0xE4;
        int fd = open(Modes.uuidFile, O_RDONLY);
        int res = (fd != -1) ? read(fd, c->sendq + 2, 128) : -1;
        if (res >= 16) {
            if (res < 130)
                buf[res] = '\0';
            else
                buf[129] = '\0';
            strncpy(buf, c->sendq + 2, res);
            fprintf(stderr, "UUID: %s\n", buf);
            c->sendq_len = res + 2;
            flushClient(c, mstime());
        } else {
            fprintf(stderr, "ERROR: Not a valid UUID: %s\n", Modes.uuidFile);
            fprintf(stderr, "Use this command to fix: sudo uuidgen > %s\n", Modes.uuidFile);
        }
        if (fd != -1) {
            close(fd);
        }
    }

    return c;
}

// Initiate an outgoing connection.
// Return the new client or NULL if the connection failed
struct client *serviceConnect(struct net_connector *con) {

    int fd;

    if (con->try_addr && con->try_addr->ai_next) {
        // iterate the address info
        con->try_addr = con->try_addr->ai_next;
    } else {
        // get the address info
        if (!con->gai_request_in_progress)  {
            // launch a pthread for async getaddrinfo
            con->try_addr = NULL;
            if (con->addr_info) {
                freeaddrinfo(con->addr_info);
                con->addr_info = NULL;
            }

            con->gai_request_in_progress = 1;
            con->gai_request_done = 0;

            if (pthread_create(&con->thread, NULL, pthreadGetaddrinfo, con)) {
                con->next_reconnect = mstime() + 15000;
                fprintf(stderr, "%s: pthread_create ERROR for %s port %s: %s\n", con->service->descr, con->address, con->port, strerror(errno));
                return NULL;
            }

            con->gai_request_in_progress = 1;
            con->next_reconnect = mstime() + 10;
            return NULL;
        } else {

            pthread_mutex_lock(&con->mutex);
            if (!con->gai_request_done) {
                con->next_reconnect = mstime() + 50;
                pthread_mutex_unlock(&con->mutex);
                return NULL;
            }
            pthread_mutex_unlock(&con->mutex);

            con->gai_request_in_progress = 0;

            if (pthread_join(con->thread, NULL)) {
                fprintf(stderr, "%s: pthread_join ERROR for %s port %s: %s\n", con->service->descr, con->address, con->port, strerror(errno));
                con->next_reconnect = mstime() + 15000;
                return NULL;
            }

            if (con->gai_error) {
                fprintf(stderr, "%s: Name resolution for %s failed: %s\n", con->service->descr, con->address, gai_strerror(con->gai_error));
                con->next_reconnect = mstime() + Modes.net_connector_delay;
                return NULL;
            }

            con->try_addr = con->addr_info;
            // SUCCESS!
        }
    }

    getnameinfo(con->try_addr->ai_addr, con->try_addr->ai_addrlen,
            con->resolved_addr, sizeof(con->resolved_addr) - 3,
            NULL, 0,
            NI_NUMERICHOST | NI_NUMERICSERV);

    if (strcmp(con->resolved_addr, con->address) == 0) {
        con->resolved_addr[0] = '\0';
    } else {
        char tmp[sizeof(con->resolved_addr)+3]; // shut up gcc
        snprintf(tmp, sizeof(tmp), " (%s)", con->resolved_addr);
        memcpy(con->resolved_addr, tmp, sizeof(con->resolved_addr));
    }

    if (!con->try_addr->ai_next) {
        con->next_reconnect = mstime() + Modes.net_connector_delay;
    } else {
        con->next_reconnect = mstime() + 100;
    }

    if (Modes.debug_net) {
        fprintf(stderr, "%s: Attempting connection to %s port %s ...\n", con->service->descr, con->address, con->port);
    }

    fd = anetTcpNonBlockConnectAddr(Modes.aneterr, con->try_addr);
    if (fd == ANET_ERR) {
        fprintf(stderr, "%s: Connection to %s%s port %s failed: %s\n",
                con->service->descr, con->address, con->resolved_addr, con->port, Modes.aneterr);
        return NULL;
    }

    con->connecting = 1;
    con->connect_timeout = mstime() + Modes.net_connector_delay / 2;
    con->fd = fd;

    if (anetTcpKeepAlive(Modes.aneterr, fd) != ANET_OK)
        fprintf(stderr, "%s: Unable to set keepalive: connection to %s port %s ...\n", con->service->descr, con->address, con->port);

    // Since this is a non-blocking connect, it will always return right away.
    // We'll need to periodically check to see if it did, in fact, connect, but do it once here.

    return checkServiceConnected(con);
}

// Set up the given service to listen on an address/port.
// _exits_ on failure!
void serviceListen(struct net_service *service, char *bind_addr, char *bind_ports) {
    int *fds = NULL;
    int n = 0;
    char *p, *end;
    char buf[128];

    if (service->listener_count > 0) {
        fprintf(stderr, "Tried to set up the service %s twice!\n", service->descr);
        exit(1);
    }

    if (!bind_ports || !strcmp(bind_ports, "") || !strcmp(bind_ports, "0"))
        return;

    p = bind_ports;
    while (p && *p) {
        int newfds[16];
        int nfds, i;

        end = strpbrk(p, ", ");
        if (!end) {
            strncpy(buf, p, sizeof (buf));
            buf[sizeof (buf) - 1] = 0;
            p = NULL;
        } else {
            size_t len = end - p;
            if (len >= sizeof (buf))
                len = sizeof (buf) - 1;
            memcpy(buf, p, len);
            buf[len] = 0;
            p = end + 1;
        }

        nfds = anetTcpServer(Modes.aneterr, buf, bind_addr, newfds, sizeof (newfds));
        if (nfds == ANET_ERR) {
            fprintf(stderr, "Error opening the listening port %s (%s): %s\n",
                    buf, service->descr, Modes.aneterr);
            exit(1);
        }
        fprintf(stderr, "%s: Listening on port %s\n", service->descr, buf);

        fds = realloc(fds, (n + nfds) * sizeof (int));
        if (!fds) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }

        for (i = 0; i < nfds; ++i) {
            anetNonBlock(Modes.aneterr, newfds[i]);
            fds[n++] = newfds[i];
        }
    }

    service->listener_count = n;
    service->listener_fds = fds;
}

void modesInitNet(void) {
    if (!Modes.net)
        return;
    struct net_service *s;
    struct net_service *beast_out;
    struct net_service *beast_reduce_out;
    struct net_service *garbage_out;
    struct net_service *beast_in;
    struct net_service *raw_out;
    struct net_service *raw_in;
    struct net_service *vrs_out;
    struct net_service *json_out;
    struct net_service *sbs_out;
    struct net_service *sbs_out_replay;
    struct net_service *sbs_out_mlat;
    struct net_service *sbs_out_jaero;
    struct net_service *sbs_out_prio;
    struct net_service *sbs_in;
    struct net_service *sbs_in_mlat;
    struct net_service *sbs_in_jaero;
    struct net_service *sbs_in_prio;
    struct net_service *api_out;

    signal(SIGPIPE, SIG_IGN);
    Modes.services = NULL;


    // set up listeners
    api_out = serviceInit("API output", &Modes.api_out, NULL, READ_MODE_ASCII, "\n", handleApiRequest);
    serviceListen(api_out, Modes.net_bind_address, Modes.net_output_api_ports);

    raw_out = serviceInit("Raw TCP output", &Modes.raw_out, send_raw_heartbeat, READ_MODE_IGNORE, NULL, NULL);
    serviceListen(raw_out, Modes.net_bind_address, Modes.net_output_raw_ports);

    beast_out = serviceInit("Beast TCP output", &Modes.beast_out, send_beast_heartbeat, READ_MODE_BEAST_COMMAND, NULL, handleBeastCommand);
    serviceListen(beast_out, Modes.net_bind_address, Modes.net_output_beast_ports);

    beast_reduce_out = serviceInit("BeastReduce TCP output", &Modes.beast_reduce_out, send_beast_heartbeat, READ_MODE_IGNORE, NULL, NULL);
    serviceListen(beast_reduce_out, Modes.net_bind_address, Modes.net_output_beast_reduce_ports);

    garbage_out = serviceInit("Garbage TCP output", &Modes.garbage_out, send_beast_heartbeat, READ_MODE_IGNORE, NULL, NULL);
    serviceListen(garbage_out, Modes.net_bind_address, Modes.garbage_ports);

    vrs_out = serviceInit("VRS json output", &Modes.vrs_out, NULL, READ_MODE_IGNORE, NULL, NULL);
    serviceListen(vrs_out, Modes.net_bind_address, Modes.net_output_vrs_ports);

    json_out = serviceInit("Position json output", &Modes.json_out, NULL, READ_MODE_IGNORE, NULL, NULL);
    serviceListen(json_out, Modes.net_bind_address, Modes.net_output_json_ports);

    sbs_out = serviceInit("SBS TCP output", &Modes.sbs_out, send_sbs_heartbeat, READ_MODE_IGNORE, NULL, NULL);
    serviceListen(sbs_out, Modes.net_bind_address, Modes.net_output_sbs_ports);

    sbs_out_replay = serviceInit("SBS TCP output replay SBS IN", &Modes.sbs_out_replay, send_sbs_heartbeat, READ_MODE_IGNORE, NULL, NULL);
    sbs_out_prio = serviceInit("SBS TCP output PRIO", &Modes.sbs_out_prio, send_sbs_heartbeat, READ_MODE_IGNORE, NULL, NULL);
    sbs_out_mlat = serviceInit("SBS TCP output MLAT", &Modes.sbs_out_mlat, send_sbs_heartbeat, READ_MODE_IGNORE, NULL, NULL);
    sbs_out_jaero = serviceInit("SBS TCP output JAERO", &Modes.sbs_out_jaero, send_sbs_heartbeat, READ_MODE_IGNORE, NULL, NULL);

    serviceListen(sbs_out_jaero, Modes.net_bind_address, Modes.net_output_jaero_ports);

    if (strlen(Modes.net_output_sbs_ports) == 5 && Modes.net_output_sbs_ports[4] == '5') {

        char *replay = strdup(Modes.net_output_sbs_ports);
        replay[4] = '6';
        serviceListen(sbs_out_replay, Modes.net_bind_address, replay);

        char *mlat = strdup(Modes.net_output_sbs_ports);
        mlat[4] = '7';
        serviceListen(sbs_out_mlat, Modes.net_bind_address, mlat);

        char *prio = strdup(Modes.net_output_sbs_ports);
        prio[4] = '8';
        serviceListen(sbs_out_prio, Modes.net_bind_address, prio);

        char *jaero = strdup(Modes.net_output_sbs_ports);
        jaero[4] = '9';
        if (sbs_out_jaero->listener_count == 0)
            serviceListen(sbs_out_jaero, Modes.net_bind_address, jaero);

        free(replay);
        free(mlat);
        free(prio);
        free(jaero);
    }

    sbs_in = serviceInit("SBS TCP input", NULL, NULL, READ_MODE_ASCII, "\n",  decodeSbsLine);
    serviceListen(sbs_in, Modes.net_bind_address, Modes.net_input_sbs_ports);

    sbs_in_mlat = serviceInit("SBS TCP input MLAT", NULL, NULL, READ_MODE_ASCII, "\n",  decodeSbsLineMlat);
    sbs_in_prio = serviceInit("SBS TCP input PRIO", NULL, NULL, READ_MODE_ASCII, "\n",  decodeSbsLinePrio);
    sbs_in_jaero = serviceInit("SBS TCP input JAERO", NULL, NULL, READ_MODE_ASCII, "\n",  decodeSbsLineJaero);


    serviceListen(sbs_in_jaero, Modes.net_bind_address, Modes.net_input_jaero_ports);

    if (strlen(Modes.net_input_sbs_ports) == 5 && Modes.net_input_sbs_ports[4] == '6') {
        char *mlat = strdup(Modes.net_input_sbs_ports);
        mlat[4] = '7';
        serviceListen(sbs_in_mlat, Modes.net_bind_address, mlat);

        char *prio = strdup(Modes.net_input_sbs_ports);
        prio[4] = '8';
        serviceListen(sbs_in_prio, Modes.net_bind_address, prio);

        char *jaero = strdup(Modes.net_input_sbs_ports);
        jaero[4] = '9';
        if (sbs_in_jaero->listener_count == 0)
            serviceListen(sbs_in_jaero, Modes.net_bind_address, jaero);

        free(mlat);
        free(prio);
        free(jaero);
    }

    raw_in = serviceInit("Raw TCP input", NULL, NULL, READ_MODE_ASCII, "\n", decodeHexMessage);
    serviceListen(raw_in, Modes.net_bind_address, Modes.net_input_raw_ports);

    /* Beast input via network */
    beast_in = serviceInit("Beast TCP input", NULL, NULL, READ_MODE_BEAST, NULL, decodeBinMessage);
    serviceListen(beast_in, Modes.net_bind_address, Modes.net_input_beast_ports);

    /* Beast input from local Modes-S Beast via USB */
    if (Modes.sdr_type == SDR_MODESBEAST) {
        createGenericClient(beast_in, Modes.beast_fd);
    }
    else if (Modes.sdr_type == SDR_GNS) {
        /* Hex input from local GNS5894 via USART0 */
        s = serviceInit("Hex GNSHAT input", NULL, NULL, READ_MODE_ASCII, "\n", decodeHexMessage);
        s->serial_service = 1;
        createGenericClient(s, Modes.beast_fd);
    }

    for (int i = 0; i < Modes.net_connectors_count; i++) {
        struct net_connector *con = Modes.net_connectors[i];
        if (strcmp(con->protocol, "beast_out") == 0)
            con->service = beast_out;
        else if (strcmp(con->protocol, "beast_in") == 0)
            con->service = beast_in;
        if (strcmp(con->protocol, "beast_reduce_out") == 0)
            con->service = beast_reduce_out;
        else if (strcmp(con->protocol, "raw_out") == 0)
            con->service = raw_out;
        else if (strcmp(con->protocol, "raw_in") == 0)
            con->service = raw_in;
        else if (strcmp(con->protocol, "vrs_out") == 0)
            con->service = vrs_out;
        else if (strcmp(con->protocol, "json_out") == 0)
            con->service = json_out;
        else if (strcmp(con->protocol, "sbs_out") == 0)
            con->service = sbs_out;
        else if (strcmp(con->protocol, "sbs_in") == 0)
            con->service = sbs_in;
        else if (strcmp(con->protocol, "sbs_in_mlat") == 0)
            con->service = sbs_in_mlat;
        else if (strcmp(con->protocol, "sbs_in_jaero") == 0)
            con->service = sbs_in_jaero;
        else if (strcmp(con->protocol, "sbs_in_prio") == 0)
            con->service = sbs_in_prio;
        else if (strcmp(con->protocol, "sbs_out_mlat") == 0)
            con->service = sbs_out_mlat;
        else if (strcmp(con->protocol, "sbs_out_jaero") == 0)
            con->service = sbs_out_jaero;
        else if (strcmp(con->protocol, "sbs_out_prio") == 0)
            con->service = sbs_out_prio;
        else if (strcmp(con->protocol, "sbs_out_replay") == 0)
            con->service = sbs_out_replay;

    }
}


//
//=========================================================================
//
// This function gets called from time to time when the decoding thread is
// awakened by new data arriving. This usually happens a few times every second
//
static uint64_t modesAcceptClients(uint64_t now) {
    int fd;
    struct net_service *s;
    struct client *c;

    for (s = Modes.services; s; s = s->next) {
        int i;
        for (i = 0; i < s->listener_count; ++i) {
            struct sockaddr_storage storage;
            struct sockaddr *saddr = (struct sockaddr *) &storage;
            socklen_t slen = sizeof(storage);

            while ((fd = anetGenericAccept(Modes.aneterr, s->listener_fds[i], saddr, &slen)) >= 0) {
                c = createSocketClient(s, fd);
                if (c) {
                    // We created the client, save the sockaddr info and 'hostport'
                    getnameinfo(saddr, slen,
                            c->host, sizeof(c->host),
                            c->port, sizeof(c->port),
                            NI_NUMERICHOST | NI_NUMERICSERV);

                    setProxyString(c);
                    if (!Modes.netIngest && Modes.debug_net) {
                        fprintf(stderr, "%s: new c from %s port %s (fd %d)\n", 
                                c->service->descr, c->host, c->port, fd);
                    }
                    if (anetTcpKeepAlive(Modes.aneterr, fd) != ANET_OK)
                        fprintf(stderr, "%s: Unable to set keepalive on connection from %s port %s (fd %d)\n", c->service->descr, c->host, c->port, fd);
                } else {
                    fprintf(stderr, "%s: Fatal: createSocketClient shouldn't fail!\n", s->descr);
                    exit(1);
                }
            }

            if (errno != EMFILE && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "%s: Error accepting new connection: %s\n", s->descr, Modes.aneterr);
            }
        }
    }

    // temporarily stop trying to accept new clients if we are limited by file descriptors
    if (errno == EMFILE) {
        fprintf(stderr, "Accepting new connections suspended for 3 seconds: %s\n", Modes.aneterr);
        return (now + 3000);
    }

    // only check for new clients not sooner than 150 ms from now
    return (now + 150);
}

//
//=========================================================================
//
// On error free the client, collect the structure, adjust maxfd if needed.
//
static void modesCloseClient(struct client *c) {
    if (!c->service) {
        fprintf(stderr, "warning: double close of net client\n");
        return;
    }

    anetCloseSocket(c->fd);
    c->service->connections--;
    if (c->con) {
        // Clean this up and set the next_reconnect timer for another try.
        // If the connection had been established and the connect didn't fail,
        // only wait a short time to reconnect
        c->con->connecting = 0;
        c->con->connected = 0;
        c->con->next_reconnect = mstime() + Modes.net_connector_delay / 5;
    }

    // mark it as inactive and ready to be freed
    c->fd = -1;
    c->service = NULL;
    c->modeac_requested = 0;

    if (Modes.mode_ac_auto)
        autoset_modeac();
}

static void flushClient(struct client *c, uint64_t now) {

    int towrite = c->sendq_len;
    char *psendq = c->sendq;
    int loops = 0;
    int max_loops = 2;
    int total_nwritten = 0;
    int done = 0;

    do {
#ifndef _WIN32
        int nwritten = write(c->fd, psendq, towrite);
        int err = errno;
#else
        int nwritten = send(c->fd, psendq, towrite, 0);
        int err = WSAGetLastError();
#endif
        loops++;
        // If we get -1, it's only fatal if it's not EAGAIN/EWOULDBLOCK
        if (nwritten < 0) {
            if (err != EAGAIN && err != EWOULDBLOCK) {
                fprintf(stderr, "%s: Send Error: %s: %s port %s (fd %d, SendQ %d, RecvQ %d)\n",
                        c->service->descr, strerror(err), c->host, c->port,
                        c->fd, c->sendq_len, c->buflen);
                modesCloseClient(c);
                return;
            }
            done = 1;	// Blocking, just bail, try later.
        } else {
            if (nwritten > 0) {
                // We've written something, add it to the total
                total_nwritten += nwritten;
                // Advance buffer
                psendq += nwritten;
                towrite -= nwritten;
            }
            if (total_nwritten == c->sendq_len) {
                done = 1;
            }
        }
    } while (!done && (loops < max_loops));

    if (total_nwritten > 0) {
        c->last_send = now;	// If we wrote anything, update this.
        if (total_nwritten == c->sendq_len) {
            c->sendq_len = 0;
        } else {
            c->sendq_len -= total_nwritten;
            memmove((void*)c->sendq, c->sendq + total_nwritten, towrite);
        }
        c->last_flush = now;
    }

    // If writing has failed for 5 seconds, disconnect.
    if (c->last_flush + 5000 < now) {
        fprintf(stderr, "%s: Unable to send data, disconnecting: %s port %s (fd %d, SendQ %d)\n", c->service->descr, c->host, c->port, c->fd, c->sendq_len);
        modesCloseClient(c);
        return;
    }
}

//
//=========================================================================
//
// Send the write buffer for the specified writer to all connected clients
//
static void flushWrites(struct net_writer *writer) {
    struct client *c;
    uint64_t now = mstime();

    for (c = writer->service->clients; c; c = c->next) {
        if (!c->service)
            continue;
        if (c->service->writer == writer->service->writer) {
            uintptr_t psendq_end = (uintptr_t)c->sendq + c->sendq_len; // Pointer to end of sendq

            // Add the buffer to the client's SendQ
            if ((c->sendq_len + writer->dataUsed) >= c->sendq_max) {
                // Too much data in client SendQ.  Drop client - SendQ exceeded.
                fprintf(stderr, "%s: Dropped due to full SendQ: %s port %s (fd %d, SendQ %d, RecvQ %d)\n",
                        c->service->descr, c->host, c->port,
                        c->fd, c->sendq_len, c->buflen);
                modesCloseClient(c);
                continue;	// Go to the next client
            }
            // Append the data to the end of the queue, increment len
            memcpy((void*)psendq_end, writer->data, writer->dataUsed);
            c->sendq_len += writer->dataUsed;
            // Try flushing...
            flushClient(c, now);
        }
    }
    writer->dataUsed = 0;
    writer->lastWrite = now;
    return;
}

// Prepare to write up to 'len' bytes to the given net_writer.
// Returns a pointer to write to, or NULL to skip this write.
static void *prepareWrite(struct net_writer *writer, int len) {
    if (!writer ||
            !writer->service ||
            !writer->service->connections ||
            !writer->data)
        return NULL;

    if (len > MODES_OUT_BUF_SIZE)
        return NULL;

    if (writer->dataUsed + len >= MODES_OUT_BUF_SIZE) {
        // Flush now to free some space
        flushWrites(writer);
    }

    return writer->data + writer->dataUsed;
}

// Complete a write previously begun by prepareWrite.
// endptr should point one byte past the last byte written
// to the buffer returned from prepareWrite.
static void completeWrite(struct net_writer *writer, void *endptr) {
    writer->dataUsed = endptr - writer->data;

    if (writer->dataUsed >= Modes.net_output_flush_size) {
        flushWrites(writer);
    }
}

//
//=========================================================================
//
// Write raw output in Beast Binary format with Timestamp to TCP clients
//
static void modesSendBeastOutput(struct modesMessage *mm, struct net_writer *writer) {
    int msgLen = mm->msgbits / 8;
    char *p = prepareWrite(writer, 2 + 2 * (7 + 8 + msgLen));
    unsigned char ch;
    int j;
    int sig;
    unsigned char *msg = (Modes.net_verbatim ? mm->verbatim : mm->msg);

    if (!p)
        return;

    // receiverId, big-endian, in own message to make it backwards compatible
    // only send the receiverId when it changes
    if (Modes.netReceiverId && writer->lastReceiverId != mm->receiverId) {
        writer->lastReceiverId = mm->receiverId;
        *p++ = 0x1a;
        // other dump1090 / readsb versions or beast implementations should discard unknown message types
        *p++ = 0xe3; // good enough guess no one is using this.
        for (int i = 7; i >= 0; i--) {
            *p++ = (ch = ((mm->receiverId >> (8 * i)) & 0xFF));
            if (0x1A == ch) {
                *p++ = ch;
            }
        }
    }

    *p++ = 0x1a;
    if (msgLen == MODES_SHORT_MSG_BYTES) {
        *p++ = '2';
    } else if (msgLen == MODES_LONG_MSG_BYTES) {
        *p++ = '3';
    } else if (msgLen == MODEAC_MSG_BYTES) {
        *p++ = '1';
    } else {
        return;
    }

    /* timestamp, big-endian */
    *p++ = (ch = (mm->timestampMsg >> 40));
    if (0x1A == ch) {
        *p++ = ch;
    }
    *p++ = (ch = (mm->timestampMsg >> 32));
    if (0x1A == ch) {
        *p++ = ch;
    }
    *p++ = (ch = (mm->timestampMsg >> 24));
    if (0x1A == ch) {
        *p++ = ch;
    }
    *p++ = (ch = (mm->timestampMsg >> 16));
    if (0x1A == ch) {
        *p++ = ch;
    }
    *p++ = (ch = (mm->timestampMsg >> 8));
    if (0x1A == ch) {
        *p++ = ch;
    }
    *p++ = (ch = (mm->timestampMsg));
    if (0x1A == ch) {
        *p++ = ch;
    }

    sig = nearbyint(sqrt(mm->signalLevel) * 255);
    if (mm->signalLevel > 0 && sig < 1)
        sig = 1;
    if (sig > 255)
        sig = 255;
    *p++ = ch = (char) sig;
    if (0x1A == ch) {
        *p++ = ch;
    }

    for (j = 0; j < msgLen; j++) {
        *p++ = (ch = msg[j]);
        if (0x1A == ch) {
            *p++ = ch;
        }
    }

    completeWrite(writer, p);
}

static void send_beast_heartbeat(struct net_service *service) {
    static char heartbeat_message[] = {0x1a, '1', 0, 0, 0, 0, 0, 0, 0, 0, 0};
    char *data;

    if (!service->writer)
        return;

    data = prepareWrite(service->writer, sizeof (heartbeat_message));
    if (!data)
        return;

    memcpy(data, heartbeat_message, sizeof (heartbeat_message));
    completeWrite(service->writer, data + sizeof (heartbeat_message));
}

//
//=========================================================================
//
// Print the two hex digits to a string for a single byte.
//
static void printHexDigit(char *p, unsigned char c) {
    const char hex_lookup[] = "0123456789ABCDEF";
    p[0] = hex_lookup[(c >> 4) & 0x0F];
    p[1] = hex_lookup[c & 0x0F];
}

//
//=========================================================================
//
// Write raw output to TCP clients
//
static void modesSendRawOutput(struct modesMessage *mm) {
    int msgLen = mm->msgbits / 8;
    char *p = prepareWrite(&Modes.raw_out, msgLen * 2 + 15);
    int j;
    unsigned char *msg = (Modes.net_verbatim ? mm->verbatim : mm->msg);

    if (!p)
        return;

    if (Modes.mlat && mm->timestampMsg) {
        /* timestamp, big-endian */
        sprintf(p, "@%012" PRIX64,
                mm->timestampMsg);
        p += 13;
    } else
        *p++ = '*';

    for (j = 0; j < msgLen; j++) {
        printHexDigit(p, msg[j]);
        p += 2;
    }

    *p++ = ';';
    *p++ = '\n';

    completeWrite(&Modes.raw_out, p);
}

static void send_raw_heartbeat(struct net_service *service) {
    static char *heartbeat_message = "*0000;\n";
    char *data;
    int len = strlen(heartbeat_message);

    if (!service->writer)
        return;

    data = prepareWrite(service->writer, len);
    if (!data)
        return;

    memcpy(data, heartbeat_message, len);
    completeWrite(service->writer, data + len);
}

//
//=========================================================================
//
// Read SBS input from TCP clients
//
static int decodeSbsLine(struct client *c, char *line, int remote, uint64_t now) {
    struct modesMessage mm;
    size_t line_len = strlen(line);
    size_t max_len = 200;

    if (Modes.receiver_focus && c->receiverId != Modes.receiver_focus)
        return 0;
    if (line_len < 2) // heartbeat
        return 0;
    if (line_len < 20 || line_len >= max_len)
        goto basestation_invalid;

    memset(&mm, 0, sizeof(mm));
    mm.client = c;

    char *p = line;
    char *t[23]; // leave 0 indexed entry empty, place 22 tokens into array

    MODES_NOTUSED(c);
    if (remote >= 64)
        mm.source = remote - 64;
    else
        mm.source = SOURCE_SBS;

    char *out = NULL;

    out = prepareWrite(&Modes.sbs_out, max_len);
    if (out) {
        memcpy(out, line, line_len);
        //fprintf(stderr, "%s", out);
        out += line_len;
        out += sprintf(out, "\r\n");
        completeWrite(&Modes.sbs_out, out);
    }

    out = NULL;
    switch(mm.source) {
        case SOURCE_SBS:
            out = prepareWrite(&Modes.sbs_out_replay, max_len);
            mm.addrtype = ADDR_OTHER;
            break;
        case SOURCE_MLAT:
            out = prepareWrite(&Modes.sbs_out_mlat, max_len);
            mm.addrtype = ADDR_MLAT;
            break;
        case SOURCE_JAERO:
            out = prepareWrite(&Modes.sbs_out_jaero, max_len);
            mm.addrtype = ADDR_JAERO;
            break;
        case SOURCE_PRIO:
            out = prepareWrite(&Modes.sbs_out_prio, max_len);
            mm.addrtype = ADDR_OTHER;
            break;

        default:
            mm.addrtype = ADDR_OTHER;
    }

    if (out) {
        memcpy(out, line, line_len);
        //fprintf(stderr, "%s", out);
        out += line_len;
        *out++ = '\r';
        *out++ = '\n';


        if (mm.source == SOURCE_SBS)
            completeWrite(&Modes.sbs_out_replay, out);
        if (mm.source == SOURCE_MLAT)
            completeWrite(&Modes.sbs_out_mlat, out);
        if (mm.source == SOURCE_JAERO)
            completeWrite(&Modes.sbs_out_jaero, out);
        if (mm.source == SOURCE_PRIO)
            completeWrite(&Modes.sbs_out_prio, out);
    }


    // Mark messages received over the internet as remote so that we don't try to
    // pass them off as being received by this instance when forwarding them
    mm.remote = 1;
    mm.signalLevel = 0;
    mm.sbs_in = 1;

    // sample message from mlat-client basestation output
    //MSG,3,1,1,4AC8B3,1,2019/12/10,19:10:46.320,2019/12/10,19:10:47.789,,36017,,,51.1001,10.1915,,,,,,
    //
    for (int i = 1; i < 23; i++) {
        t[i] = strsep(&p, ",");
        if (!p && i < 22)
            goto basestation_invalid;
    }

    // check field 1
    if (!t[1] || strcmp(t[1], "MSG") != 0)
        goto basestation_invalid;

    if (!t[2] || strlen(t[2]) != 1)
        goto basestation_invalid;
    //int msg_type = atoi(t[2]);

    if (!t[5] || strlen(t[5]) != 6) // icao must be 6 characters
        goto basestation_invalid;

    char *icao = t[5];
    unsigned char *chars = (unsigned char *) &(mm.addr);
    for (int j = 0; j < 6; j += 2) {
        int high = hexDigitVal(icao[j]);
        int low = hexDigitVal(icao[j + 1]);

        if (high == -1 || low == -1)
            goto basestation_invalid;

        chars[2 - j / 2] = (high << 4) | low;
    }

    //fprintf(stderr, "%x type %s: ", mm.addr, t[2]);
    //fprintf(stderr, "%x: %d, %0.5f, %0.5f\n", mm.addr, mm.altitude_baro, mm.decoded_lat, mm.decoded_lon);
    //field 11, callsign
    if (t[11] && strlen(t[11]) > 0) {
        strncpy(mm.callsign, t[11], 9);
        mm.callsign[8] = '\0';
        mm.callsign_valid = 1;
        for (unsigned i = 0; i < 8; ++i) {
            if (mm.callsign[i] == '\0')
                mm.callsign[i] = ' ';
            if (!(mm.callsign[i] >= 'A' && mm.callsign[i] <= 'Z') &&
                    !(mm.callsign[i] >= '0' && mm.callsign[i] <= '9') &&
                    mm.callsign[i] != ' ') {
                // Bad callsign, ignore it
                mm.callsign_valid = 0;
                break;
            }
        }
        //fprintf(stderr, "call: %s, ", mm.callsign);
    }
    // field 12, altitude
    if (t[12] && strlen(t[12]) > 0) {
        mm.altitude_baro = atoi(t[12]);
        if (mm.altitude_baro > -5000 && mm.altitude_baro < 100000) {
            mm.altitude_baro_valid = 1;
            mm.altitude_baro_unit = UNIT_FEET;
        }
        //fprintf(stderr, "alt: %d, ", mm.altitude_baro);
    }
    // field 13, groundspeed
    if (t[13] && strlen(t[13]) > 0) {
        mm.gs.v0 = strtod(t[13], NULL);
        if (mm.gs.v0 > 0)
            mm.gs_valid = 1;
        //fprintf(stderr, "gs: %.1f, ", mm.gs.selected);
    }
    //field 14, heading
    if (t[14] && strlen(t[14]) > 0) {
        mm.heading_valid = 1;
        mm.heading = strtod(t[14], NULL);
        mm.heading_type = HEADING_GROUND_TRACK;
        //fprintf(stderr, "track: %.1f, ", mm.heading);
    }
    // field 15 and 16, position
    if (t[15] && strlen(t[15]) && t[16] && strlen(t[16])) {
        mm.decoded_lat = strtod(t[15], NULL);
        mm.decoded_lon = strtod(t[16], NULL);
        if (mm.decoded_lat != 0 && mm.decoded_lon != 0)
            mm.sbs_pos_valid = 1;
        //fprintf(stderr, "pos: (%.2f, %.2f)\n", mm.decoded_lat, mm.decoded_lon);
    }
    // field 17 vertical rate, assume baro
    if (t[17] && strlen(t[17]) > 0) {
        mm.baro_rate = atoi(t[17]);
        mm.baro_rate_valid = 1;
        //fprintf(stderr, "vRate: %d, ", mm.baro_rate);
    }
    // field 18 squawk
    if (t[18] && strlen(t[18]) > 0) {
        long int tmp = strtol(t[18], NULL, 10);
        if (tmp > 0) {
            mm.squawk = (tmp / 1000) * 16*16*16 + (tmp / 100 % 10) * 16*16 + (tmp / 10 % 10) * 16 + (tmp % 10);
            mm.squawk_valid = 1;
            //fprintf(stderr, "squawk: %04x %s, ", mm.squawk, t[18]);
        }
    }
    // field 19 (originally squawk change) used to indicate by some versions of mlat-server the number of receivers which contributed to the postiions
    if (mm.source == SOURCE_MLAT && t[19] && strlen(t[19]) > 0) {
        long int tmp = strtol(t[19], NULL, 10);
        if (tmp > 0) {
            mm.receiverCountMlat = tmp;
        }
    }

    // field 22 ground status
    if (t[22] && strlen(t[22]) > 0 && atoi(t[22]) > 0) {
        mm.airground = AG_GROUND;
        //fprintf(stderr, "onground, ");
    }


    // set nic / rc to 0 / unknown
    mm.decoded_nic = 0;
    mm.decoded_rc = RC_UNKNOWN;

    //fprintf(stderr, "\n");

    // record reception time as the time we read it.
    mm.sysTimestampMsg = now;

    useModesMessage(&mm);

    Modes.stats_current.remote_received_basestation_valid++;

    return 0;

basestation_invalid:

    for (size_t i = 0; i < line_len; i++)
        line[i] = (line[i] == '\0' ? ',' : line[i]);

    if (Modes.debug_garbage)
        fprintf(stderr, "SBS invalid: %.*s\n", (int) line_len, line);
    Modes.stats_current.remote_received_basestation_invalid++;
    return 0;
}
//
//=========================================================================
//
// Write SBS output to TCP clients
//
static void modesSendSBSOutput(struct modesMessage *mm, struct aircraft *a) {
    char *p;
    struct timespec now;
    struct tm stTime_receive, stTime_now;
    int msgType;

    // For now, suppress non-ICAO addresses
    if (mm->addr & MODES_NON_ICAO_ADDRESS)
        return;

    p = prepareWrite(&Modes.sbs_out, 200);
    if (!p)
        return;

    //
    // SBS BS style output checked against the following reference
    // http://www.homepages.mcb.net/bones/SBS/Article/Barebones42_Socket_Data.htm - seems comprehensive
    //

    // Decide on the basic SBS Message Type
    switch (mm->msgtype) {
        case 4:
        case 20:
            msgType = 5;
            break;
            break;

        case 5:
        case 21:
            msgType = 6;
            break;

        case 0:
        case 16:
            msgType = 7;
            break;

        case 11:
            msgType = 8;
            break;

        case 17:
        case 18:
            if (mm->metype >= 1 && mm->metype <= 4) {
                msgType = 1;
            } else if (mm->metype >= 5 && mm->metype <= 8) {
                msgType = 2;
            } else if (mm->metype >= 9 && mm->metype <= 18) {
                msgType = 3;
            } else if (mm->metype == 19) {
                msgType = 4;
            } else {
                return;
            }
            break;

        default:
            return;
    }

    // Fields 1 to 6 : SBS message type and ICAO address of the aircraft and some other stuff
    p += sprintf(p, "MSG,%d,1,1,%06X,1,", msgType, mm->addr);

    // Find current system time
    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &stTime_now);

    // Find message reception time
    time_t received = (time_t) (mm->sysTimestampMsg / 1000);
    localtime_r(&received, &stTime_receive);

    // Fields 7 & 8 are the message reception time and date
    p += sprintf(p, "%04d/%02d/%02d,", (stTime_receive.tm_year + 1900), (stTime_receive.tm_mon + 1), stTime_receive.tm_mday);
    p += sprintf(p, "%02d:%02d:%02d.%03u,", stTime_receive.tm_hour, stTime_receive.tm_min, stTime_receive.tm_sec, (unsigned) (mm->sysTimestampMsg % 1000));

    // Fields 9 & 10 are the current time and date
    p += sprintf(p, "%04d/%02d/%02d,", (stTime_now.tm_year + 1900), (stTime_now.tm_mon + 1), stTime_now.tm_mday);
    p += sprintf(p, "%02d:%02d:%02d.%03u", stTime_now.tm_hour, stTime_now.tm_min, stTime_now.tm_sec, (unsigned) (now.tv_nsec / 1000000U));

    // Field 11 is the callsign (if we have it)
    if (mm->callsign_valid) {
        p += sprintf(p, ",%s", mm->callsign);
    } else {
        p += sprintf(p, ",");
    }

    // Field 12 is the altitude (if we have it)
    if (Modes.use_gnss) {
        if (mm->altitude_geom_valid) {
            p += sprintf(p, ",%dH", mm->altitude_geom);
        } else if (mm->altitude_baro_valid && trackDataValid(&a->geom_delta_valid)) {
            p += sprintf(p, ",%dH", mm->altitude_baro + a->geom_delta);
        } else if (mm->altitude_baro_valid) {
            p += sprintf(p, ",%d", mm->altitude_baro);
        } else {
            p += sprintf(p, ",");
        }
    } else {
        if (mm->altitude_baro_valid) {
            p += sprintf(p, ",%d", mm->altitude_baro);
        } else if (mm->altitude_geom_valid && trackDataValid(&a->geom_delta_valid)) {
            p += sprintf(p, ",%d", mm->altitude_geom - a->geom_delta);
        } else {
            p += sprintf(p, ",");
        }
    }

    // Field 13 is the ground Speed (if we have it)
    if (mm->gs_valid) {
        p += sprintf(p, ",%.0f", mm->gs.selected);
    } else {
        p += sprintf(p, ",");
    }

    // Field 14 is the ground Heading (if we have it)
    if (mm->heading_valid && mm->heading_type == HEADING_GROUND_TRACK) {
        p += sprintf(p, ",%.0f", mm->heading);
    } else {
        p += sprintf(p, ",");
    }

    // Fields 15 and 16 are the Lat/Lon (if we have it)
    if (mm->cpr_decoded) {
        p += sprintf(p, ",%1.5f,%1.5f", mm->decoded_lat, mm->decoded_lon);
    } else {
        p += sprintf(p, ",,");
    }

    // Field 17 is the VerticalRate (if we have it)
    if (Modes.use_gnss) {
        if (mm->geom_rate_valid) {
            p += sprintf(p, ",%dH", mm->geom_rate);
        } else if (mm->baro_rate_valid) {
            p += sprintf(p, ",%d", mm->baro_rate);
        } else {
            p += sprintf(p, ",");
        }
    } else {
        if (mm->baro_rate_valid) {
            p += sprintf(p, ",%d", mm->baro_rate);
        } else if (mm->geom_rate_valid) {
            p += sprintf(p, ",%d", mm->geom_rate);
        } else {
            p += sprintf(p, ",");
        }
    }

    // Field 18 is  the Squawk (if we have it)
    if (mm->squawk_valid) {
        p += sprintf(p, ",%04x", mm->squawk);
    } else {
        p += sprintf(p, ",");
    }

    // Field 19 is the Squawk Changing Alert flag (if we have it)
    if (mm->alert_valid) {
        if (mm->alert) {
            p += sprintf(p, ",-1");
        } else {
            p += sprintf(p, ",0");
        }
    } else {
        p += sprintf(p, ",");
    }

    // Field 20 is the Squawk Emergency flag (if we have it)
    if (mm->squawk_valid) {
        if ((mm->squawk == 0x7500) || (mm->squawk == 0x7600) || (mm->squawk == 0x7700)) {
            p += sprintf(p, ",-1");
        } else {
            p += sprintf(p, ",0");
        }
    } else {
        p += sprintf(p, ",");
    }

    // Field 21 is the Squawk Ident flag (if we have it)
    if (mm->spi_valid) {
        if (mm->spi) {
            p += sprintf(p, ",-1");
        } else {
            p += sprintf(p, ",0");
        }
    } else {
        p += sprintf(p, ",");
    }

    // Field 22 is the OnTheGround flag (if we have it)
    switch (mm->airground) {
        case AG_GROUND:
            p += sprintf(p, ",-1");
            break;
        case AG_AIRBORNE:
            p += sprintf(p, ",0");
            break;
        default:
            p += sprintf(p, ",");
            break;
    }

    p += sprintf(p, "\r\n");

    completeWrite(&Modes.sbs_out, p);
}

static void send_sbs_heartbeat(struct net_service *service) {
    static char *heartbeat_message = "\r\n"; // is there a better one?
    char *data;
    int len = strlen(heartbeat_message);

    if (!service->writer)
        return;

    data = prepareWrite(service->writer, len);
    if (!data)
        return;

    memcpy(data, heartbeat_message, len);
    completeWrite(service->writer, data + len);
}

void jsonPositionOutput(struct modesMessage *mm, struct aircraft *a) {
    MODES_NOTUSED(mm);
    char *p;

    p = prepareWrite(&Modes.json_out, 1000);
    if (!p)
        return;
    char *end = p + 1000;

    p = sprintAircraftObject(p, end, a, mm->sysTimestampMsg, 2);
    completeWrite(&Modes.json_out, p);
}
//
//=========================================================================
//
void modesQueueOutput(struct modesMessage *mm, struct aircraft *a) {
    int is_mlat = (mm->source == SOURCE_MLAT);

    if (Modes.garbage_ports && (mm->garbage || mm->pos_bad)) {
        if (mm->garbage || !mm->pos_ignore)
            modesSendBeastOutput(mm, &Modes.garbage_out);
        return;
    }

    if (a && !is_mlat && mm->correctedbits < 2) {
        // Don't ever forward 2-bit-corrected messages via SBS output.
        // Don't ever forward mlat messages via SBS output.
        modesSendSBSOutput(mm, a);
    }

    if (!is_mlat && (Modes.net_verbatim || mm->correctedbits < 2)) {
        // Forward 2-bit-corrected messages via raw output only if --net-verbatim is set
        // Don't ever forward mlat messages via raw output.
        modesSendRawOutput(mm);
    }

    if ((!is_mlat || Modes.forward_mlat) && (Modes.net_verbatim || mm->correctedbits < 2)) {
        // Forward 2-bit-corrected messages via beast output only if --net-verbatim is set
        // Forward mlat messages via beast output only if --forward-mlat is set
        modesSendBeastOutput(mm, &Modes.beast_out);
        if (mm->reduce_forward) {
            modesSendBeastOutput(mm, &Modes.beast_reduce_out);
        }
    }
}

// Decode a little-endian IEEE754 float (binary32)
float ieee754_binary32_le_to_float(uint8_t *data) {
    double sign = (data[3] & 0x80) ? -1.0 : 1.0;
    int16_t raw_exponent = ((data[3] & 0x7f) << 1) | ((data[2] & 0x80) >> 7);
    uint32_t raw_significand = ((data[2] & 0x7f) << 16) | (data[1] << 8) | data[0];

    if (raw_exponent == 0) {
        if (raw_significand == 0) {
            /* -0 is treated like +0 */
            return 0;
        } else {
            /* denormal */
            return ldexp(sign * raw_significand, -126 - 23);
        }
    }

    if (raw_exponent == 255) {
        if (raw_significand == 0) {
            /* +/-infinity */
            return sign < 0 ? -INFINITY : INFINITY;
        } else {
            /* NaN */
#ifdef NAN
            return NAN;
#else
            return 0.0f;
#endif
        }
    }

    /* normalized value */
    return ldexp(sign * ((1 << 23) | raw_significand), raw_exponent - 127 - 23);
}

static void handle_radarcape_position(float lat, float lon, float alt) {
	// disable this
	return;
    if (!isfinite(lat) || lat < -90 || lat > 90 || !isfinite(lon) || lon < -180 || lon > 180 || !isfinite(alt))
        return;

    if (!(Modes.bUserFlags & MODES_USER_LATLON_VALID)) {
        Modes.fUserLat = lat;
        Modes.fUserLon = lon;
        Modes.bUserFlags |= MODES_USER_LATLON_VALID;
        receiverPositionChanged(lat, lon, alt);
    }
}

// recompute global Mode A/C setting
static void autoset_modeac() {
    struct net_service *s;
    struct client *c;

    if (!Modes.mode_ac_auto)
        return;

    Modes.mode_ac = 0;
    for (s = Modes.services; s; s = s->next) {
        for (c = s->clients; c; c = c->next) {
            if (c->modeac_requested) {
                Modes.mode_ac = 1;
                break;
            }
        }
    }
}

// Send some Beast settings commands to a client
void sendBeastSettings(int fd, const char *settings) {
    int len;
    char *buf, *p;

    len = strlen(settings) * 3;
    buf = p = alloca(len);

    while (*settings) {
        *p++ = 0x1a;
        *p++ = '1';
        *p++ = *settings++;
    }

    anetWrite(fd, buf, len);
}
static int handleApiRequest(struct client *c, char *p, int remote, uint64_t now) {
    MODES_NOTUSED(now);
    p = p;
    remote = remote;
    c = c;

    static uint32_t scratch[3 * API_INDEX_MAX];

    //writeJsonToNet(&Modes.api_out, generateAircraftJson(-1));
    apiReq(50, 51, 10, 11, scratch);

    return 0;
}

//
// Handle a Beast command message.
// Currently, we just look for the Mode A/C command message
// and ignore everything else.
//
static int handleBeastCommand(struct client *c, char *p, int remote, uint64_t now) {
    return 0; // disable this in this fork, no modeac unless it's enabled via config
    MODES_NOTUSED(remote);
    MODES_NOTUSED(now);
    if (p[0] != '1') {
        // huh?
        return 0;
    }

    switch (p[1]) {
        case 'j':
            c->modeac_requested = 0;
            break;
        case 'J':
            c->modeac_requested = 1;
            break;
    }

    autoset_modeac();
    return 0;
}

//
//=========================================================================
//
// This function decodes a Beast binary format message
//
// The message is passed to the higher level layers, so it feeds
// the selected screen output, the network output and so forth.
//
// If the message looks invalid it is silently discarded.
//
// The function always returns 0 (success) to the caller as there is no
// case where we want broken messages here to close the client connection.
//
static int decodeBinMessage(struct client *c, char *p, int remote, uint64_t now) {
    int msgLen = 0;
    int j;
    unsigned char ch;
    struct modesMessage mm;
    unsigned char *msg = mm.msg;
    MODES_NOTUSED(c);

    memset(&mm, 0, sizeof(mm));
    mm.client = c;

    ch = *p++; /// Get the message type

    if (ch == 0xe3 && !Modes.netIngest) {
        // Grab the receiver id (big endian format)
        uint64_t receiverId = 0;
        for (j = 0; j < 8; j++) {
            ch = *p++;
            receiverId = receiverId << 8 | (ch & 255);
            if (0x1A == ch) {
                p++;
            }
        }
        c->receiverId = receiverId;
        p++; // discard 0x1A
        ch = *p++; /// Get the message type
    }

    mm.receiverId = c->receiverId;

    if (Modes.receiver_focus && mm.receiverId != Modes.receiver_focus)
        return 0;

    if (ch == '1') {
        if (!Modes.mode_ac) {
            if (remote) {
                Modes.stats_current.remote_received_modeac++;
            } else {
                Modes.stats_current.demod_modeac++;
            }
            return 0;
        }
        msgLen = MODEAC_MSG_BYTES;
    } else if (ch == '2') {
        msgLen = MODES_SHORT_MSG_BYTES;
    } else if (ch == '3') {
        msgLen = MODES_LONG_MSG_BYTES;
    } else if (ch == '5') {
        // Special case for Radarcape position messages.
        float lat, lon, alt;

        for (j = 0; j < 21; j++) { // and the data
            msg[j] = ch = *p++;
            if (0x1A == ch) {
                p++;
            }
        }

        lat = ieee754_binary32_le_to_float(msg + 4);
        lon = ieee754_binary32_le_to_float(msg + 8);
        alt = ieee754_binary32_le_to_float(msg + 12);

        handle_radarcape_position(lat, lon, alt);
    }

    if (!msgLen)
        return 0;

    /* Beast messages are marked depending on their source. From internet they are marked
     * remote so that we don't try to pass them off as being received by this instance
     * when forwarding them.
     */
    mm.remote = remote;

    // Grab the timestamp (big endian format)
    mm.timestampMsg = 0;
    for (j = 0; j < 6; j++) {
        ch = *p++;
        mm.timestampMsg = mm.timestampMsg << 8 | (ch & 255);
        if (0x1A == ch) {
            p++;
        }
    }

    // record reception time as the time we read it.
    mm.sysTimestampMsg = now;

    ch = *p++; // Grab the signal level
    mm.signalLevel = ((unsigned char) ch / 255.0);
    mm.signalLevel = mm.signalLevel * mm.signalLevel;

    /* In case of Mode-S Beast use the signal level per message for statistics */
    if (Modes.sdr_type == SDR_MODESBEAST) {
        Modes.stats_current.signal_power_sum += mm.signalLevel;
        Modes.stats_current.signal_power_count += 1;

        if (mm.signalLevel > Modes.stats_current.peak_signal_power)
            Modes.stats_current.peak_signal_power = mm.signalLevel;
        if (mm.signalLevel > 0.50119)
            Modes.stats_current.strong_signal_count++; // signal power above -3dBFS
    }

    if (0x1A == ch) {
        p++;
    }

    for (j = 0; j < msgLen; j++) { // and the data
        msg[j] = ch = *p++;
        if (0x1A == ch) {
            p++;
        }
    }

    int result = -10;
    if (msgLen == MODEAC_MSG_BYTES) { // ModeA or ModeC
        if (remote) {
            Modes.stats_current.remote_received_modeac++;
        } else {
            Modes.stats_current.demod_modeac++;
        }
        decodeModeAMessage(&mm, ((msg[0] << 8) | msg[1]));
        result = 0;
    } else {
        if (remote) {
            Modes.stats_current.remote_received_modes++;
        } else {
            Modes.stats_current.demod_preambles++;
        }
        result = decodeModesMessage(&mm, NULL);
        if (result < 0) {
            if (result == -1) {
                if (remote) {
                    Modes.stats_current.remote_rejected_unknown_icao++;
                } else {
                    Modes.stats_current.demod_rejected_unknown_icao++;
                }
            } else {
                if (remote) {
                    Modes.stats_current.remote_rejected_bad++;
                } else {
                    Modes.stats_current.demod_rejected_bad++;
                }
            }
        } else {
            if (remote) {
                Modes.stats_current.remote_accepted[mm.correctedbits]++;
            } else {
                Modes.stats_current.demod_accepted[mm.correctedbits]++;
            }
        }
    }

    if (Modes.garbage_ports && receiverCheckBad(mm.receiverId, now)) {
        mm.garbage = 1;
    }

    useModesMessage(&mm);
    return 0;
}
//
//=========================================================================
//
// Turn an hex digit into its 4 bit decimal value.
// Returns -1 if the digit is not in the 0-F range.
//
static int hexDigitVal(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    else if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    else if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    else return -1;
}
//
//=========================================================================
//
// This function decodes a string representing message in raw hex format
// like: *8D4B969699155600E87406F5B69F; The string is null-terminated.
//
// The message is passed to the higher level layers, so it feeds
// the selected screen output, the network output and so forth.
//
// If the message looks invalid it is silently discarded.
//
// The function always returns 0 (success) to the caller as there is no
// case where we want broken messages here to close the client connection.
//
static int decodeHexMessage(struct client *c, char *hex, int remote, uint64_t now) {
    int l = strlen(hex), j;
    struct modesMessage mm;
    unsigned char *msg = mm.msg;

    MODES_NOTUSED(remote);
    MODES_NOTUSED(c);

    memset(&mm, 0, sizeof(mm));
    mm.client = c;

    // Mark messages received over the internet as remote so that we don't try to
    // pass them off as being received by this instance when forwarding them
    mm.remote = 1;
    mm.signalLevel = 0;

    // Remove spaces on the left and on the right
    while (l && isspace(hex[l - 1])) {
        hex[l - 1] = '\0';
        l--;
    }
    while (isspace(*hex)) {
        hex++;
        l--;
    }

    // Turn the message into binary.
    // Accept *-AVR raw @-AVR/BEAST timeS+raw %-AVR timeS+raw (CRC good) <-BEAST timeS+sigL+raw
    // and some AVR records that we can understand
    if (hex[l - 1] != ';') {
        return (0);
    } // not complete - abort

    switch (hex[0]) {
        case '<':
        {
            mm.signalLevel = ((hexDigitVal(hex[13]) << 4) | hexDigitVal(hex[14])) / 255.0;
            mm.signalLevel = mm.signalLevel * mm.signalLevel;
            hex += 15;
            l -= 16; // Skip <, timestamp and siglevel, and ;
            break;
        }

        case '@': // No CRC check
        case '%':
        { // CRC is OK
            hex += 13;
            l -= 14; // Skip @,%, and timestamp, and ;
            break;
        }

        case '*':
        case ':':
        {
            hex++;
            l -= 2; // Skip * and ;
            break;
        }

        default:
        {
            return (0); // We don't know what this is, so abort
            break;
        }
    }

    if ((l != (MODEAC_MSG_BYTES * 2))
            && (l != (MODES_SHORT_MSG_BYTES * 2))
            && (l != (MODES_LONG_MSG_BYTES * 2))) {
        return (0);
    } // Too short or long message... broken

    if ((0 == Modes.mode_ac)
            && (l == (MODEAC_MSG_BYTES * 2))) {
        return (0);
    } // Right length for ModeA/C, but not enabled

    for (j = 0; j < l; j += 2) {
        int high = hexDigitVal(hex[j]);
        int low = hexDigitVal(hex[j + 1]);

        if (high == -1 || low == -1) return 0;
        msg[j / 2] = (high << 4) | low;
    }

    // record reception time as the time we read it.
    mm.sysTimestampMsg = now;

    if (l == (MODEAC_MSG_BYTES * 2)) { // ModeA or ModeC
        Modes.stats_current.remote_received_modeac++;
        decodeModeAMessage(&mm, ((msg[0] << 8) | msg[1]));
    } else { // Assume ModeS
        int result;

        Modes.stats_current.remote_received_modes++;
        result = decodeModesMessage(&mm, NULL);
        if (result < 0) {
            if (result == -1)
                Modes.stats_current.remote_rejected_unknown_icao++;
            else
                Modes.stats_current.remote_rejected_bad++;
            return 0;
        } else {
            Modes.stats_current.remote_accepted[mm.correctedbits]++;
        }
    }

    useModesMessage(&mm);
    return (0);
}

/*
__attribute__ ((format(printf, 3, 0))) static char *safe_vsnprintf(char *p, char *end, const char *format, va_list ap) {
    p += vsnprintf(p < end ? p : NULL, p < end ? (size_t) (end - p) : 0, format, ap);
    return p;
}
*/

static const char *trimSpace(const char *in, char *out, int len) {

    out[len] = '\0';
    int found = 0;

    for (int i = len - 1; i >= 0; i--) {
        if (!found && in[i] == ' ') {
            out[i] = '\0';
        } else if (in[i] == '\0') {
            out[i] = '\0';
        } else {
            out[i] = in[i];
            found = 1; // found non space character
        }
    }

    return out;
}
//
//=========================================================================
//
// Return a description of planes in json. No metric conversion
//
static const char *jsonEscapeString(const char *str, char *buf, int len) {
    const char *in = str;
    char *out = buf, *end = buf + len - 10;

    for (; *in && out < end; ++in) {
        unsigned char ch = *in;
        if (ch == '"' || ch == '\\') {
            *out++ = '\\';
            *out++ = ch;
        } else if (ch < 32 || ch > 126) {
            out = safe_snprintf(out, end, "\\u%04x", ch);
        } else {
            *out++ = ch;
        }
    }

    *out++ = 0;
    return buf;
}

static const char *hexEscapeString(const char *str, char *buf, int len) {
    const char *in = str;
    char *out = buf, *end = buf + len - 10;

    for (; *in && out < end; ++in) {
        unsigned char ch = *in;
        if (ch == '"' || ch == '\\') {
            *out++ = '\\';
            *out++ = ch;
        } else if (ch < 32 || ch > 126) {
            out = safe_snprintf(out, end, ".%02x.", ch);
        } else {
            *out++ = ch;
        }
    }

    *out++ = 0;
    return buf;
}

static char *append_flags(char *p, char *end, struct aircraft *a, datasource_t source) {
    p = safe_snprintf(p, end, "[");

    char *start = p;
    if (a->callsign_valid.source == source)
        p = safe_snprintf(p, end, "\"callsign\",");
    if (a->altitude_baro_valid.source == source)
        p = safe_snprintf(p, end, "\"altitude\",");
    if (a->altitude_geom_valid.source == source)
        p = safe_snprintf(p, end, "\"alt_geom\",");
    if (a->gs_valid.source == source)
        p = safe_snprintf(p, end, "\"gs\",");
    if (a->ias_valid.source == source)
        p = safe_snprintf(p, end, "\"ias\",");
    if (a->tas_valid.source == source)
        p = safe_snprintf(p, end, "\"tas\",");
    if (a->mach_valid.source == source)
        p = safe_snprintf(p, end, "\"mach\",");
    if (a->track_valid.source == source)
        p = safe_snprintf(p, end, "\"track\",");
    if (a->track_rate_valid.source == source)
        p = safe_snprintf(p, end, "\"track_rate\",");
    if (a->roll_valid.source == source)
        p = safe_snprintf(p, end, "\"roll\",");
    if (a->mag_heading_valid.source == source)
        p = safe_snprintf(p, end, "\"mag_heading\",");
    if (a->true_heading_valid.source == source)
        p = safe_snprintf(p, end, "\"true_heading\",");
    if (a->baro_rate_valid.source == source)
        p = safe_snprintf(p, end, "\"baro_rate\",");
    if (a->geom_rate_valid.source == source)
        p = safe_snprintf(p, end, "\"geom_rate\",");
    if (a->squawk_valid.source == source)
        p = safe_snprintf(p, end, "\"squawk\",");
    if (a->emergency_valid.source == source)
        p = safe_snprintf(p, end, "\"emergency\",");
    if (a->nav_qnh_valid.source == source)
        p = safe_snprintf(p, end, "\"nav_qnh\",");
    if (a->nav_altitude_mcp_valid.source == source)
        p = safe_snprintf(p, end, "\"nav_altitude_mcp\",");
    if (a->nav_altitude_fms_valid.source == source)
        p = safe_snprintf(p, end, "\"nav_altitude_fms\",");
    if (a->nav_heading_valid.source == source)
        p = safe_snprintf(p, end, "\"nav_heading\",");
    if (a->nav_modes_valid.source == source)
        p = safe_snprintf(p, end, "\"nav_modes\",");
    if (a->position_valid.source == source)
        p = safe_snprintf(p, end, "\"lat\",\"lon\",\"nic\",\"rc\",");
    if (a->nic_baro_valid.source == source)
        p = safe_snprintf(p, end, "\"nic_baro\",");
    if (a->nac_p_valid.source == source)
        p = safe_snprintf(p, end, "\"nac_p\",");
    if (a->nac_v_valid.source == source)
        p = safe_snprintf(p, end, "\"nac_v\",");
    if (a->sil_valid.source == source)
        p = safe_snprintf(p, end, "\"sil\",\"sil_type\",");
    if (a->gva_valid.source == source)
        p = safe_snprintf(p, end, "\"gva\",");
    if (a->sda_valid.source == source)
        p = safe_snprintf(p, end, "\"sda\",");
    if (p != start)
        --p;
    p = safe_snprintf(p, end, "]");
    return p;
}

static struct {
    nav_modes_t flag;
    const char *name;
} nav_modes_names[] = {
    { NAV_MODE_AUTOPILOT, "autopilot"},
    { NAV_MODE_VNAV, "vnav"},
    { NAV_MODE_ALT_HOLD, "althold"},
    { NAV_MODE_APPROACH, "approach"},
    { NAV_MODE_LNAV, "lnav"},
    { NAV_MODE_TCAS, "tcas"},
    { 0, NULL}
};

static char *append_nav_modes(char *p, char *end, nav_modes_t flags, const char *quote, const char *sep) {
    int first = 1;
    for (int i = 0; nav_modes_names[i].name; ++i) {
        if (!(flags & nav_modes_names[i].flag)) {
            continue;
        }

        if (!first) {
            p = safe_snprintf(p, end, "%s", sep);
        }

        first = 0;
        p = safe_snprintf(p, end, "%s%s%s", quote, nav_modes_names[i].name, quote);
    }

    return p;
}

const char *nav_modes_flags_string(nav_modes_t flags) {
    static char buf[256];
    buf[0] = 0;
    append_nav_modes(buf, buf + sizeof (buf), flags, "", " ");
    return buf;
}

const char *addrtype_enum_string(addrtype_t type) {
    switch (type) {
        case ADDR_ADSB_ICAO:
            return "adsb_icao";
        case ADDR_ADSB_ICAO_NT:
            return "adsb_icao_nt";
        case ADDR_ADSR_ICAO:
            return "adsr_icao";
        case ADDR_TISB_ICAO:
            return "tisb_icao";

        case ADDR_JAERO:
            return "adsc";
        case ADDR_MLAT:
            return "mlat";
        case ADDR_OTHER:
            return "other";
        case ADDR_MODE_S:
            return "mode_s";

        case ADDR_ADSB_OTHER:
            return "adsb_other";
        case ADDR_ADSR_OTHER:
            return "adsr_other";
        case ADDR_TISB_TRACKFILE:
            return "tisb_trackfile";
        case ADDR_TISB_OTHER:
            return "tisb_other";


        case ADDR_MODE_A:
            return "mode_ac";

        default:
            return "unknown";
    }
}

static const char *emergency_enum_string(emergency_t emergency) {
    switch (emergency) {
        case EMERGENCY_NONE: return "none";
        case EMERGENCY_GENERAL: return "general";
        case EMERGENCY_LIFEGUARD: return "lifeguard";
        case EMERGENCY_MINFUEL: return "minfuel";
        case EMERGENCY_NORDO: return "nordo";
        case EMERGENCY_UNLAWFUL: return "unlawful";
        case EMERGENCY_DOWNED: return "downed";
        default: return "reserved";
    }
}

static const char *sil_type_enum_string(sil_type_t type) {
    switch (type) {
        case SIL_UNKNOWN: return "unknown";
        case SIL_PER_HOUR: return "perhour";
        case SIL_PER_SAMPLE: return "persample";
        default: return "invalid";
    }
}

const char *source_enum_string(datasource_t src) {
    switch (src) {
        case SOURCE_INVALID: return "SOURCE_INVALID";
        case SOURCE_INDIRECT: return "SOURCE_INDIRECT";
        case SOURCE_MODE_AC: return "SOURCE_MODE_AC";
        case SOURCE_SBS: return "SOURCE_SBS";
        case SOURCE_MLAT: return "SOURCE_MLAT";
        case SOURCE_MODE_S: return "SOURCE_MODE_S";
        case SOURCE_JAERO: return "SOURCE_JAERO";
        case SOURCE_MODE_S_CHECKED: return "SOURCE_MODE_S_CHECKED";
        case SOURCE_TISB: return "SOURCE_TISB";
        case SOURCE_ADSR: return "SOURCE_ADSR";
        case SOURCE_ADSB: return "SOURCE_ADSB";
        case SOURCE_PRIO: return "SOURCE_PRIO";
        default: return "SOURCE_WTF";
    }
}

const char *nav_altitude_source_enum_string(nav_altitude_source_t src) {
    switch (src) {
        case NAV_ALT_INVALID: return "invalid";
        case NAV_ALT_UNKNOWN: return "unknown";
        case NAV_ALT_AIRCRAFT: return "aircraft";
        case NAV_ALT_MCP: return "mcp";
        case NAV_ALT_FMS: return "fms";
        default: return "invalid";
    }
}

const char *airground_to_string(airground_t airground) {
    switch (airground) {
        case AG_GROUND:
            return "ground";
        case AG_AIRBORNE:
            return "airborne";
        case AG_INVALID:
            return "invalid";
        case AG_UNCERTAIN:
            return "airborne?";
        default:
            return "(unknown airground state)";
    }
}


/*
static void check_state_all(struct aircraft *test, uint64_t now) {
    size_t buflen = 4096;
    char buffer1[buflen];
    char buffer2[buflen];
    char *buf, *p, *end;

    struct aircraft abuf = *test;
    struct aircraft *a = &abuf;

    buf = buffer1;
    p = buf;
    end = buf + buflen;
    p = sprintAircraftObject(p, end, a, now, 1);

    buf = buffer2;
    p = buf;
    end = buf + buflen;


    struct state_all state_buf;
    memset(&state_buf, 0, sizeof(struct state_all));
    struct state_all *new_all = &state_buf;
    to_state_all(a, new_all, now);

    struct aircraft bbuf;
    memset(&bbuf, 0, sizeof(struct aircraft));
    struct aircraft *b = &bbuf;

    from_state_all(new_all, b, now);

    p = sprintAircraftObject(p, end, b, now, 1);

    if (strncmp(buffer1, buffer2, buflen)) {
        fprintf(stderr, "%s\n%s\n", buffer1, buffer2);
    }
}
*/
struct char_buffer generateGlobeBin(int globe_index, int mil) {
    struct char_buffer cb;
    uint64_t now = mstime();
    struct aircraft *a;
    size_t buflen = 1*1024*1024; // The initial buffer is resized as needed
    char *buf = (char *) malloc(buflen), *p = buf, *end = buf + buflen;

    uint32_t elementSize = sizeof(struct binCraft);
    memset(p, 0, elementSize);

#define memWrite(p, var) do { memcpy(p, &var, sizeof(var)); p += sizeof(var); } while(0)

    memWrite(p, now);

    memWrite(p, elementSize);

    uint32_t ac_count_pos = Modes.globalStatsCount.json_ac_count_pos;
    memWrite(p, ac_count_pos);

    uint32_t index = globe_index;
    memWrite(p, index);

    int16_t south, west, north, east;

    if (globe_index >= GLOBE_MIN_INDEX) {
        int grid = GLOBE_INDEX_GRID;
        south = ((globe_index - GLOBE_MIN_INDEX) / GLOBE_LAT_MULT) * grid - 90;
        west = ((globe_index - GLOBE_MIN_INDEX) % GLOBE_LAT_MULT) * grid - 180;
        north = south + grid;
        east = west + grid;
    } else {
        struct tile *tiles = Modes.json_globe_special_tiles;
        struct tile tile = tiles[globe_index];
        south = tile.south;
        west = tile.west;
        north = tile.north;
        east = tile.east;
    }

    memWrite(p, south);
    memWrite(p, west);
    memWrite(p, north);
    memWrite(p, east);

    if (p - buf > (int) elementSize)
        fprintf(stderr, "buffer overrun globeBin\n");

    p = buf + elementSize;

    struct craftArray *ca = NULL;
    int good;
    if (globe_index <= GLOBE_MAX_INDEX) {
        ca = &Modes.globeLists[globe_index];
        good = 1;
    } else {
        fprintf(stderr, "generateAircraftJson: bad globe_index: %d\n", globe_index);
        good = 0;
    }
    if (good && ca->list) {
        for (int i = 0; i < ca->len; i++) {
            a = ca->list[i];

            if (a == NULL)
                continue;
            if (mil && !(a->dbFlags & 1))
                continue;

            int use = 0;

            if (a->position_valid.source == SOURCE_JAERO)
                use = 1;
            if (now < a->seenPosReliable + 2 * MINUTES)
                use = 1;

            if (!use)
                continue;

            // check if we have enough space
            if ((p + 1000) >= end) {
                int used = p - buf;
                buflen *= 2;
                buf = (char *) realloc(buf, buflen);
                p = buf + used;
                end = buf + buflen;
            }

            struct binCraft bin;
            toBinCraft(a, &bin, now);

            memWrite(p, bin);

            if (p >= end)
                fprintf(stderr, "buffer overrun globeBin\n");
        }
    }

    cb.len = p - buf;
    cb.buffer = buf;
    return cb;

#undef memWrite
}

struct char_buffer generateGlobeJson(int globe_index){
    struct char_buffer cb;
    uint64_t now = mstime();
    struct aircraft *a;
    size_t buflen = 1*1024*1024; // The initial buffer is resized as needed
    char *buf = (char *) malloc(buflen), *p = buf, *end = buf + buflen;

    p = safe_snprintf(p, end,
            "{ \"now\" : %.1f,\n"
            "  \"messages\" : %u,\n",
            now / 1000.0,
            Modes.stats_current.messages_total + Modes.stats_alltime.messages_total);

    p = safe_snprintf(p, end,
            "  \"global_ac_count_withpos\" : %d,\n",
            Modes.globalStatsCount.json_ac_count_pos
            );

    p = safe_snprintf(p, end, "  \"globeIndex\" : %d, ", globe_index);
    if (globe_index >= GLOBE_MIN_INDEX) {
        int grid = GLOBE_INDEX_GRID;
        int lat = ((globe_index - GLOBE_MIN_INDEX) / GLOBE_LAT_MULT) * grid - 90;
        int lon = ((globe_index - GLOBE_MIN_INDEX) % GLOBE_LAT_MULT) * grid - 180;
        p = safe_snprintf(p, end,
                "\"south\" : %d, "
                "\"west\" : %d, "
                "\"north\" : %d, "
                "\"east\" : %d,\n",
                lat,
                lon,
                lat + grid,
                lon + grid);
    } else {
        struct tile *tiles = Modes.json_globe_special_tiles;
        struct tile tile = tiles[globe_index];
        p = safe_snprintf(p, end,
                "\"south\" : %d, "
                "\"west\" : %d, "
                "\"north\" : %d, "
                "\"east\" : %d,\n",
                tile.south,
                tile.west,
                tile.north,
                tile.east);
    }

    p = safe_snprintf(p, end, "  \"aircraft\" : [");

    struct craftArray *ca = NULL;
    int good;
    if (globe_index <= GLOBE_MAX_INDEX) {
        ca = &Modes.globeLists[globe_index];
        good = 1;
    } else {
        fprintf(stderr, "generateAircraftJson: bad globe_index: %d\n", globe_index);
        good = 0;
    }
    if (good && ca->list) {
        for (int i = 0; i < ca->len; i++) {
            a = ca->list[i];

            if (a == NULL)
                continue;

            int use = 0;

            if (a->position_valid.source == SOURCE_JAERO)
                use = 1;
            if (now < a->seenPosReliable + 2 * MINUTES)
                use = 1;

            if (!use)
                continue;

            // check if we have enough space
            if ((p + 1000) >= end) {
                int used = p - buf;
                buflen *= 2;
                buf = (char *) realloc(buf, buflen);
                p = buf + used;
                end = buf + buflen;
            }

            p = sprintAircraftObject(p, end, a, now, 3);

            *p++ = ',';

            if (p >= end)
                fprintf(stderr, "buffer overrun aircraft json\n");
        }
    }
    if (*(p-1) == ',')
        p--;

    p = safe_snprintf(p, end, "\n  ]\n}\n");

    cb.len = p - buf;
    cb.buffer = buf;
    return cb;
}

struct char_buffer generateAircraftJson(){
    struct char_buffer cb;
    uint64_t now = mstime();
    struct aircraft *a;
    size_t buflen = 6*1024*1024; // The initial buffer is resized as needed
    char *buf = (char *) malloc(buflen), *p = buf, *end = buf + buflen;

    p = safe_snprintf(p, end,
            "{ \"now\" : %.1f,\n"
            "  \"messages\" : %u,\n",
            now / 1000.0,
            Modes.stats_current.messages_total + Modes.stats_alltime.messages_total);

    p = safe_snprintf(p, end, "  \"aircraft\" : [");

    for (int j = 0; j < AIRCRAFT_BUCKETS; j++) {
        for (a = Modes.aircraft[j]; a; a = a->next) {
            //fprintf(stderr, "a: %05x\n", a->addr);

            // don't include stale aircraft in the JSON
            if (a->position_valid.source != SOURCE_JAERO
                    && now > a->seen + TRACK_EXPIRE / 2
                    && now > a->seenPosReliable + TRACK_EXPIRE
               ) {
                continue;
            }
            if (a->messages < 2)
                continue;

            // check if we have enough space
            if ((p + 1000) >= end) {
                int used = p - buf;
                buflen *= 2;
                buf = (char *) realloc(buf, buflen);
                p = buf + used;
                end = buf + buflen;
            }

            p = sprintAircraftObject(p, end, a, now, 0);

            *p++ = ',';

            if (p >= end)
                fprintf(stderr, "buffer overrun aircraft json\n");
        }
    }
    if (*(p-1) == ',')
        p--;

    p = safe_snprintf(p, end, "\n  ]\n}\n");

    //    fprintf(stderr, "%u\n", ac_counter);

    cb.len = p - buf;
    cb.buffer = buf;
    return cb;
}

struct char_buffer generateTraceJson(struct aircraft *a, int start, int last) {
    struct char_buffer cb;
    size_t buflen = a->trace_len * 300 + 1024;

    if (last < 0)
        last = a->trace_len - 1;

    if (!Modes.json_globe_index) {
        cb.len = 0;
        cb.buffer = NULL;
        return cb;
    }

    char *buf = (char *) malloc(buflen), *p = buf, *end = buf + buflen;

    p = safe_snprintf(p, end, "{\"icao\":\"%s%06x\"", (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : "", a->addr & 0xFFFFFF);

    if (Modes.db) {
        char *regInfo = p;
        if (a->registration[0])
            p = safe_snprintf(p, end, ",\n\"r\":\"%.*s\"", (int) sizeof(a->registration), a->registration);
        if (a->typeCode[0])
            p = safe_snprintf(p, end, ",\n\"t\":\"%.*s\"", (int) sizeof(a->typeCode), a->typeCode);
        if (a->typeLong[0])
            p = safe_snprintf(p, end, ",\n\"desc\":\"%.*s\"", (int) sizeof(a->typeLong), a->typeLong);
        if (a->dbFlags)
            p = safe_snprintf(p, end, ",\n\"dbFlags\":%u", a->dbFlags);
        if (p == regInfo)
            p = safe_snprintf(p, end, ",\n\"noRegData\":true");
    }

    if (start <= last && last < a->trace_len) {
        p = safe_snprintf(p, end, ",\n\"timestamp\": %.3f", (a->trace + start)->timestamp / 1000.0);

        p = safe_snprintf(p, end, ",\n\"trace\":[ ");

        for (int i = start; i <= last; i++) {
            struct state *trace = &a->trace[i];

            int32_t altitude = trace->altitude * 25;
            int32_t rate = trace->rate * 32;
            int rate_valid = trace->flags.rate_valid;
            int rate_geom = trace->flags.rate_geom;
            int stale = trace->flags.stale;
            int on_ground = trace->flags.on_ground;
            int altitude_valid = trace->flags.altitude_valid;
            int gs_valid = trace->flags.gs_valid;
            int track_valid = trace->flags.track_valid;
            int leg_marker = trace->flags.leg_marker;
            int altitude_geom = trace->flags.altitude_geom;

                // in the air
                p = safe_snprintf(p, end, "\n[%.1f,%f,%f",
                        (trace->timestamp - (a->trace + start)->timestamp) / 1000.0, trace->lat / 1E6, trace->lon / 1E6);

                if (on_ground)
                    p = safe_snprintf(p, end, ",\"ground\"");
                else if (altitude_valid)
                    p = safe_snprintf(p, end, ",%d", altitude);
                else
                    p = safe_snprintf(p, end, ",null");

                if (gs_valid)
                    p = safe_snprintf(p, end, ",%.1f", trace->gs / 10.0);
                else
                    p = safe_snprintf(p, end, ",null");

                if (track_valid)
                    p = safe_snprintf(p, end, ",%.1f", trace->track / 10.0);
                else
                    p = safe_snprintf(p, end, ",null");

                int bitfield = (altitude_geom << 3) | (rate_geom << 2) | (leg_marker << 1) | (stale << 0);
                p = safe_snprintf(p, end, ",%d", bitfield);

                if (rate_valid)
                    p = safe_snprintf(p, end, ",%d", rate);
                else
                    p = safe_snprintf(p, end, ",null");

                if (i % 4 == 0) {
                    uint64_t now = trace->timestamp;
                    struct state_all *all = &(a->trace_all[i/4]);
                    struct aircraft b;
                    memset(&b, 0, sizeof(struct aircraft));
                    struct aircraft *ac = &b;
                    from_state_all(all, ac, now);

                    p = safe_snprintf(p, end, ",");
                    p = sprintAircraftObject(p, end, ac, now, 1);
                } else {
                    p = safe_snprintf(p, end, ",null");
                }
                p = safe_snprintf(p, end, "],");
        }

        p--; // remove last comma

        p = safe_snprintf(p, end, " ]\n");
    }

    p = safe_snprintf(p, end, " }\n");

    cb.len = p - buf;
    cb.buffer = buf;

    if (p >= end) {
        fprintf(stderr, "buffer overrun trace json %zu %zu\n", cb.len, buflen);
    }

    return cb;
}


//
// Return a description of the receiver in json.
//
struct char_buffer generateReceiverJson() {
    struct char_buffer cb;
    size_t buflen = 8192;
    char *buf = (char *) malloc(buflen), *p = buf, *end = buf + buflen;

    p = safe_snprintf(p, end, "{ "
            "\"refresh\": %.0f, "
            "\"history\": %d",
            1.0 * Modes.json_interval, Modes.json_aircraft_history_next + 1);


    if (Modes.json_location_accuracy && (Modes.fUserLat != 0.0 || Modes.fUserLon != 0.0)) {
        if (Modes.json_location_accuracy == 1) {
            p = safe_snprintf(p, end, ", "
                    "\"lat\": %.2f, "
                    "\"lon\": %.2f",
                    Modes.fUserLat, Modes.fUserLon); // round to 2dp - about 0.5-1km accuracy - for privacy reasons
        } else {
            p = safe_snprintf(p, end, ", "
                    "\"lat\": %.6f, "
                    "\"lon\": %.6f",
                    Modes.fUserLat, Modes.fUserLon); // exact location
        }
    }

    if (Modes.db || Modes.db2)
        p = safe_snprintf(p, end, ", \"dbServer\": true");


    p = safe_snprintf(p, end, ", \"jaeroTimeout\": %.1f", ((double) Modes.trackExpireJaero) / (60 * SECONDS));

    if (Modes.json_globe_index) {
        p = safe_snprintf(p, end, ", \"binCraft\": true");
        p = safe_snprintf(p, end, ", \"globeIndexGrid\": %d", GLOBE_INDEX_GRID);

        p = safe_snprintf(p, end, ", \"globeIndexSpecialTiles\": [ ");
        struct tile *tiles = Modes.json_globe_special_tiles;

        for (int i = 0; tiles[i].south != 0 || tiles[i].north != 0; i++) {
            struct tile tile = tiles[i];
            p = safe_snprintf(p, end, "{ \"south\": %d, ", tile.south);
            p = safe_snprintf(p, end, "\"east\": %d, ", tile.east);
            p = safe_snprintf(p, end, "\"north\": %d, ", tile.north);
            p = safe_snprintf(p, end, "\"west\": %d }, ", tile.west);
        }
        p -= 2; // get rid of comma and space at the end
        p = safe_snprintf(p, end, " ]");
    }

    p = safe_snprintf(p, end, ", \"version\": \"%s\" }\n", MODES_READSB_VERSION);

    if (p >= end)
        fprintf(stderr, "buffer overrun receiver json\n");

    cb.len = p - buf;
    cb.buffer = buf;
    return cb;
}

// Write JSON to file
static inline void writeJsonTo (const char* dir, const char *file, struct char_buffer cb, int gzip) {
#ifndef _WIN32

    char pathbuf[PATH_MAX];
    char tmppath[PATH_MAX];
    int fd;
    int len = cb.len;
    char *content = cb.buffer;

    if (!dir)
        snprintf(tmppath, PATH_MAX, "%s.%lx", file, random());
    else
        snprintf(tmppath, PATH_MAX, "%s/%s.%lx", dir, file, random());

    tmppath[PATH_MAX - 1] = 0;
    fd = open(tmppath, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        fprintf(stderr, "writeJsonTo open(): ");
        perror(tmppath);
        if (!gzip)
            free(content);
        return;
    }

    if (!dir)
        snprintf(pathbuf, PATH_MAX, "%s", file);
    else
        snprintf(pathbuf, PATH_MAX, "%s/%s", dir, file);

    pathbuf[PATH_MAX - 1] = 0;

    if (gzip < 0) {
        /*
        int brotliLvl = -gzip;
        size_t outSize = len + 4096;
        char *outBuf = malloc(outSize);
        // BROTLI_MODE_TEXT  Compression mode for UTF-8 formatted text input. 
        // BROTLI_MODE_GENERIC
        int rc = BrotliEncoderCompress(
                brotliLvl, 22, BROTLI_DEFAULT_MODE,
                len, (uint8_t *) content, &outSize, (uint8_t *) outBuf);

        if (rc == BROTLI_FALSE) {
            goto error_1;
        }

        if (write(fd, outBuf, outSize) != (ssize_t) outSize)
            goto error_1;

        if (close(fd) < 0)
            goto error_2;
        */
    } else if (gzip > 0) {
        gzFile gzfp = gzdopen(fd, "wb");
        if (!gzfp)
            goto error_1;

        gzbuffer(gzfp, 1024 * 1024);
        gzsetparams(gzfp, gzip, Z_DEFAULT_STRATEGY);

        int res = gzwrite(gzfp, content, len);
        if (res != len) {
            int error;
            fprintf(stderr, "%s: gzwrite of length %d failed: %s (res == %d)\n", pathbuf, len, gzerror(gzfp, &error), res);
        }

        if (gzclose(gzfp) < 0)
            goto error_2;
    } else {
        if (write(fd, content, len) != len) {
            fprintf(stderr, "writeJsonTo write(): ");
            perror(tmppath);
            goto error_1;
        }

        if (close(fd) < 0)
            goto error_2;
    }

    if (rename(tmppath, pathbuf) == -1) {
        fprintf(stderr, "writeJsonTo rename(): %s -> %s", tmppath, pathbuf);
        perror("");
        goto error_2;
    }
    if (!gzip)
        free(content);
    return;

error_1:
    close(fd);
error_2:
    unlink(tmppath);
    if (!gzip)
        free(content);
    return;
#endif
}

void writeJsonToFile (const char* dir, const char *file, struct char_buffer cb) {
    writeJsonTo(dir, file, cb, 0);
}

void writeJsonToGzip (const char* dir, const char *file, struct char_buffer cb, int gzip) {
    writeJsonTo(dir, file, cb, gzip);
}

static void periodicReadFromClient(struct client *c) {
    int nread, err;
    char buf[512];

    /* FIXME:  Not Win32 safe networking */
    nread = read(c->fd, buf, sizeof(buf));
    err = errno;

    if (nread < 0 && (err == EAGAIN || err == EWOULDBLOCK)) {
	return;
    }
    if (nread <= 0) { // Other errors, or EOF
        fprintf(stderr, "%s: Socket Error: %s: %s port %s (fd %d)\n",
                c->service->descr, nread < 0 ? strerror(err) : "EOF", c->host, c->port,
                c->fd);
        modesCloseClient(c);
        return;
    }
}

//
//=========================================================================
//
// This function polls the clients using read() in order to receive new
// messages from the net.
//
// The message is supposed to be separated from the next message by the
// separator 'sep', which is a null-terminated C string.
//
// Every full message received is decoded and passed to the higher layers
// calling the function's 'handler'.
//
// The handler returns 0 on success, or 1 to signal this function we should
// close the connection with the client in case of non-recoverable errors.
//
static void modesReadFromClient(struct client *c) {
    int left;
    int nread;
    int bContinue = 1;
    int discard = 0;

    uint64_t start = mstime();
    uint64_t now = start;

    for (int loop = 0; bContinue && loop < 32; loop++, now = mstime()) {

        if (!discard && now > start + 200) {
            discard = 1;
            static uint64_t antiSpam;
            if (now > antiSpam + 30 * SECONDS) {
                antiSpam = now;
                if (c->proxy_string[0] != '\0')
                    fprintf(stderr, "<3>ERROR, not enough CPU: Discarding data from: %s (suppressing for 30 seconds)\n", c->proxy_string);
                else
                    fprintf(stderr, "<3>%s: ERROR, not enough CPU: Discarding data from: %s port %s (fd %d) (suppressing for 30 seconds)\n",
                            c->service->descr, c->host, c->port, c->fd);
            }
        }
        if (discard)
            c->buflen = 0;

        left = MODES_CLIENT_BUF_SIZE - c->buflen - 1; // leave 1 extra byte for NUL termination in the ASCII case

        // If our buffer is full discard it, this is some badly formatted shit
        if (left <= 0) {
            c->garbage += c->buflen;
            Modes.stats_current.remote_malformed_beast += c->buflen;
            c->buflen = 0;
            left = MODES_CLIENT_BUF_SIZE - c->buflen - 1; // leave 1 extra byte for NUL termination in the ASCII case
            // If there is garbage, read more to discard it ASAP
        }
#ifndef _WIN32
        nread = read(c->fd, c->buf + c->buflen, left);
        int err = errno;
#else
        nread = recv(c->fd, c->buf + c->buflen, left, 0);
        if (nread < 0) {
            errno = WSAGetLastError();
        }
#endif

        // If we didn't get all the data we asked for, then return once we've processed what we did get.
        if (nread != left) {
            bContinue = 0;
        }

        // check for idle connection, this server version requires data
        // or a heartbeat, otherwise it will force a reconnect
        if (Modes.net_heartbeat_interval && c->con && c->last_read + Modes.net_heartbeat_interval + 5 * SECONDS <= now
                && c->service->read_mode != READ_MODE_IGNORE
                && c->service->read_mode != READ_MODE_BEAST_COMMAND
           ) {
            fprintf(stderr, "%s: No data received for %.0f seconds, reconnecting: %s port %s\n",
                    c->service->descr, (double)(Modes.net_heartbeat_interval + 5 * SECONDS),c->host, c->port);
            modesCloseClient(c);
            return;
        }

#ifndef _WIN32
        if (nread < 0 && (err == EAGAIN || err == EWOULDBLOCK)) // No data available (not really an error)
#else
        if (nread < 0 && errno == EWOULDBLOCK) // No data available (not really an error)
#endif
        {
            return;
        }

        if (nread < 0) { // Other errors
                if (c->proxy_string[0] != '\0') {
                    double elapsed = (now - c->connectedSince) / 1000.0;
                    fprintf(stderr, "disc: %56s rId %016"PRIx64"%016"PRIx64" %6.2f kbit/s for %6.1f s\n",
                            c->proxy_string, c->receiverId, c->receiverId2,
                            c->bytesReceived / 128.0 / elapsed, elapsed);
                } else {
                    fprintf(stderr, "%s: Receive Error: %s: %s port %s (fd %d, SendQ %d, RecvQ %d)\n",
                            c->service->descr, strerror(err), c->host, c->port,
                            c->fd, c->sendq_len, c->buflen);
                }
            modesCloseClient(c);
            return;
        }

        if (nread == 0) { // End of file
            if (c->con) {
                fprintf(stderr, "%s: Remote server disconnected: %s port %s (fd %d, SendQ %d, RecvQ %d)\n",
                        c->service->descr, c->con->address, c->con->port, c->fd, c->sendq_len, c->buflen);
            } else if (Modes.debug_net) {

                if (c->proxy_string[0] != '\0') {
                    double elapsed = (now - c->connectedSince) / 1000.0;
                    fprintf(stderr, "disc: %56s rId %016"PRIx64"%016"PRIx64" %6.2f kbit/s for %6.1f s\n",
                            c->proxy_string, c->receiverId, c->receiverId2,
                            c->bytesReceived / 128.0 / elapsed, elapsed);
                } else {
                    fprintf(stderr, "%s: Listen client disconnected: %s port %s (fd %d, SendQ %d, RecvQ %d)\n",
                            c->service->descr, c->host, c->port, c->fd, c->sendq_len, c->buflen);
                }
            }
            modesCloseClient(c);
            return;
        }

        if (discard)
            continue;

        c->buflen += nread;
        c->bytesReceived += nread;

        char *som = c->buf; // first byte of next message
        char *eod = som + c->buflen; // one byte past end of data
        char *p;
        int remote = 1; // Messages will be marked remote by default
        if ((c->fd == Modes.beast_fd) && (Modes.sdr_type == SDR_MODESBEAST || Modes.sdr_type == SDR_GNS)) {
            /* Message from a local connected Modes-S beast or GNS5894 are passed off the internet */
            remote = 0;
        }


        if (nread > 0)
            c->last_read = now;

        // check for PROXY v1 header if connection is new / low bytes received
        if (Modes.netIngest && c->bytesReceived <= MODES_CLIENT_BUF_SIZE && c->buflen > 5 && som[0] == 'P' && som[1] == 'R') {
            // NUL-terminate so we are free to use strstr()
            // nb: we never fill the last byte of the buffer with read data (see above) so this is safe
            *eod = '\0';
            char *proxy = strstr(som, "PROXY ");
            char *eop = strstr(som, "\r\n");
            if (proxy && proxy == som) {
                if (!eop) // incomplete proxy string (shouldn't happen but let's check anyhow)
                    break;
                *eop = '\0';
                strncpy(c->proxy_string, proxy, sizeof(c->proxy_string));
                c->proxy_string[sizeof(c->proxy_string) - 1] = '\0'; // make sure it's null terminated
                //fprintf(stderr, "%s\n", c->proxy_string);
                *eop = '\r';

                // expected string example: "PROXY TCP4 172.12.2.132 172.191.123.45 40223 30005"

                char *space = proxy;
                space = memchr(space + 1, ' ', eop - space - 1);
                space = memchr(space + 1, ' ', eop - space - 1);
                space = memchr(space + 1, ' ', eop - space - 1);
                // hash up to 3rd space
                if (eop - proxy > 10) {
                    //fprintf(stderr, "%ld %ld %s\n", eop - proxy, space - proxy, space);
                    c->receiverId = fasthash64(proxy, space - proxy, 0x2127599bf4325c37ULL);
                }

                som = eop + 2;
            }
        }

        switch (c->service->read_mode) {
            case READ_MODE_IGNORE:
                // drop the bytes on the floor
                som = eod;
                break;

            case READ_MODE_BEAST:
                // This is the Beast Binary scanning case.
                // If there is a complete message still in the buffer, there must be the separator 'sep'
                // in the buffer, note that we full-scan the buffer at every read for simplicity.


                // disconnect garbage feeds
                if (c->garbage > 512) {
                    if (!Modes.netIngest || Modes.debug_receiver) {
                        *eod = '\0';
                        char sample[64];
                        hexEscapeString(som, sample, sizeof(sample));
                        sample[sizeof(sample) - 1] = '\0';
                        if (c->proxy_string[0] != '\0')
                            fprintf(stderr, "Garbage: Close: %s sample: %s\n", c->proxy_string, sample);
                        else
                            fprintf(stderr, "Garbage: Close: %s port %s sample: %s\n", c->host, c->port, sample);
                    }
                    modesCloseClient(c);
                    return;
                }
                while (som < eod && ((p = memchr(som, (char) 0x1a, eod - som)) != NULL)) { // The first byte of buffer 'should' be 0x1a

                    c->garbage += p - som;
                    Modes.stats_current.remote_malformed_beast += p - som;

                    som = p; // consume garbage up to the 0x1a
                    ++p; // skip 0x1a

                    if (p >= eod) {
                        // Incomplete message in buffer, retry later
                        break;
                    }

                    char *eom; // one byte past end of message
                    unsigned char ch;

                    int invalid = 0;


                    // Check for message with receiverId prepended
                    ch = *p;
                    if (ch == 0xe3) {
                        eom = p + 9;
                        // we need to be careful of double escape characters in the receiverId
                        for (; p < eod && p < eom; p++) {
                            if (0x1A == *p) {
                                p++;
                                eom++;
                                if (p < eod && 0x1A != *p) { // check that it's indeed a double escape
                                    // might be start of message rather than double escape.
                                    c->garbage += p - 1 - som;
                                    Modes.stats_current.remote_malformed_beast += p - 1 - som;
                                    som = p - 1;
                                    invalid = 1;
                                    break;
                                }
                            }
                        }
                        if (invalid) // skip to potential start of a message
                            continue;
                        if (eom + 2 > eod)// Incomplete message in buffer, retry later
                            break;
                        p++; // skip 0x1a
                    }

                    ch = *p;
                    if (ch == '1') {
                        eom = p + MODEAC_MSG_BYTES + 8; // point past remainder of message
                    } else if (ch == '2') {
                        eom = p + MODES_SHORT_MSG_BYTES + 8;
                    } else if (ch == '3') {
                        eom = p + MODES_LONG_MSG_BYTES + 8;
                    } else if (ch == '4') {
                        eom = p + MODES_LONG_MSG_BYTES + 8;
                    } else if (ch == '5') {
                        eom = p + MODES_LONG_MSG_BYTES + 8;
                    } else if (ch == 0xe4) {
                        // read UUID and continue with next message
                        p++;
                        read_uuid(c, p, eod);
                        ++som;
                        continue;
                    } else {
                        // Not a valid beast message, skip 0x1a and try again
                        ++som;
                        continue;
                    }

                    // we need to be careful of double escape characters in the message body
                    for (; p < eod && p < eom; p++) {
                        if (0x1A == *p) {
                            p++;
                            eom++;
                            if (p < eod && 0x1A != *p) { // check that it's indeed a double escape
                                // might be start of message rather than double escape.
                                c->garbage += p - 1 - som;
                                Modes.stats_current.remote_malformed_beast += p - 1 - som;
                                som = p - 1;
                                invalid = 1;
                                break;
                            }
                        }
                    }
                    if (invalid) // skip to potential start of a message
                        continue;
                    if (eom > eod) // Incomplete message in buffer, retry later
                        break;


                    // Have a 0x1a followed by 1/2/3/4/5 - pass message to handler.
                    if (c->service->read_handler(c, som + 1, remote, now)) {
                        modesCloseClient(c);
                        return;
                    }

                    // if we get some valid data, reduce the garbage counter.
                    if (c->garbage > 128)
                        c->garbage -= 128;

                    // advance to next message
                    som = eom;
                }

                if (eod - som > 256) {
                    c->garbage += eod - som;
                    Modes.stats_current.remote_malformed_beast += eod - som;
                    som = eod;
                }
                break;

            case READ_MODE_BEAST_COMMAND:
                while (som < eod && ((p = memchr(som, (char) 0x1a, eod - som)) != NULL)) { // The first byte of buffer 'should' be 0x1a
                    char *eom; // one byte past end of message

                    som = p; // consume garbage up to the 0x1a
                    ++p; // skip 0x1a

                    if (p >= eod) {
                        // Incomplete message in buffer, retry later
                        break;
                    }

                    if (*p == '1') {
                        eom = p + 2;
                    } else {
                        // Not a valid beast command, skip 0x1a and try again
                        ++som;
                        continue;
                    }

                    // we need to be careful of double escape characters in the message body
                    for (p = som + 1; p < eod && p < eom; p++) {
                        if (0x1A == *p) {
                            p++;
                            eom++;
                        }
                    }

                    if (eom > eod) { // Incomplete message in buffer, retry later
                        break;
                    }

                    // Have a 0x1a followed by 1 - pass message to handler.
                    if (c->service->read_handler(c, som + 1, remote, now)) {
                        modesCloseClient(c);
                        return;
                    }

                    // advance to next message
                    som = eom;
                }
                break;

            case READ_MODE_ASCII:
                //
                // This is the ASCII scanning case, AVR RAW or HTTP at present
                // If there is a complete message still in the buffer, there must be the separator 'sep'
                // in the buffer, note that we full-scan the buffer at every read for simplicity.

                // Always NUL-terminate so we are free to use strstr()
                // nb: we never fill the last byte of the buffer with read data (see above) so this is safe
                *eod = '\0';

                while (som < eod && (p = strstr(som, c->service->read_sep)) != NULL) { // end of first message if found
                    *p = '\0'; // The handler expects null terminated strings
                    if (c->service->read_handler(c, som, remote, now)) { // Pass message to handler.
                        if (Modes.debug_net) {
                            fprintf(stderr, "%s: Closing connection from %s port %s\n", c->service->descr, c->host, c->port);
                        }
                        modesCloseClient(c); // Handler returns 1 on error to signal we .
                        return; // should close the client connection
                    }
                    som = p + c->service->read_sep_len; // Move to start of next message
                }

                break;
        }

        if (!c->receiverIdLocked && (c->bytesReceived > 512 || now > c->connectedSince + 10000)) {
            c->receiverIdLocked = 1;
            if (Modes.netIngest && (Modes.debug_net)) {
                if (c->proxy_string[0] != '\0')
                    fprintf(stderr, "new c %56s rId %016"PRIx64"%016"PRIx64"\n", 
                            c->proxy_string, c->receiverId, c->receiverId2);
                else
                    fprintf(stderr, "%s: new c from %s port %s rId %016"PRIx64"%016"PRIx64"\n",
                            c->service->descr, c->host, c->port, c->receiverId, c->receiverId2);
            }
        }

        if (som > c->buf) { // We processed something - so
            c->buflen = eod - som; //     Update the unprocessed buffer length
            memmove(c->buf, som, c->buflen); //     Move what's remaining to the start of the buffer
        } else { // If no message was decoded process the next client
            return;
        }
    }
}

static inline unsigned unsigned_difference(unsigned v1, unsigned v2) {
    return (v1 > v2) ? (v1 - v2) : (v2 - v1);
}

static inline float heading_difference(float h1, float h2) {
    float d = fabs(h1 - h2);
    return (d < 180) ? d : (360 - d);
}

const char *airground_enum_string(airground_t ag) {
    switch (ag) {
        case AG_AIRBORNE:
            return "A+";
        case AG_GROUND:
            return "G+";
        default:
            return "?";
    }
}

void modesNetSecondWork(void) {
    struct client *c;
    struct net_service *s;
    uint64_t now = mstime();

    for (s = Modes.services; s; s = s->next) {
        if (s->read_handler)
            continue;
        for (c = s->clients; c; c = c->next) {
            if (!c->service)
                continue;
            if (c->last_read + 30000 < now) {
                // This is called if there is no read handler - we just read and discard to try to trigger socket errors
                // (if 30 sec have passed)
                periodicReadFromClient(c);
                c->last_read = now;
            }
        }
    }

    // If we have generated no messages for a while, send
    // a heartbeat
    if (Modes.net_heartbeat_interval) {
        for (s = Modes.services; s; s = s->next) {
            if (s->writer &&
                    s->connections &&
                    s->writer->send_heartbeat &&
                    (s->writer->lastWrite + Modes.net_heartbeat_interval) <= now) {
                s->writer->send_heartbeat(s);
            }
        }
    }
}

// Unlink and free closed clients
void netFreeClients() {
    struct client *c, **prev;
    struct net_service *s;

    for (s = Modes.services; s; s = s->next) {
        for (prev = &s->clients, c = *prev; c; c = *prev) {
            if (c->fd == -1) {
                // Recently closed, prune from list
                *prev = c->next;
                free(c->sendq);
                free(c);
            } else {
                prev = &c->next;
            }
        }
    }
}
static void readClients() {
    uint64_t now = mstime();
    for (struct net_service *s = Modes.services; s; s = s->next) {
        for (struct client *c = s->clients; c; c = c->next) {
            if (!c->service)
                continue;

            if (s->read_handler) {
                modesReadFromClient(c);
            }

            // If there is a sendq, try to flush it
            if (s->writer) {
                if (c->sendq_len == 0) {
                    c->last_flush = now;
                    continue;
                }
                flushClient(c, now);
            }
        }
    }
}
//
// Perform periodic network work
//
void modesNetPeriodicWork(void) {
    uint64_t now = mstime();
    static uint64_t next_tcp_json;
    static uint64_t next_accept;

    // Accept new connections
    if (now > next_accept) {
        next_accept = modesAcceptClients(now);
    }

    // Read from clients, and if any need flushing, do so.
    readClients();

    // supply JSON to vrs_out writer
    if (Modes.vrs_out.service && Modes.vrs_out.service->connections && now >= next_tcp_json) {
        static uint32_t part;
        static uint32_t count;
        uint32_t n_parts = 16; // must be 16 :)

        next_tcp_json = now + Modes.net_output_vrs_interval / n_parts;

        writeJsonToNet(&Modes.vrs_out, generateVRS(part, n_parts, (count % n_parts / 2 != part % 8)));
        if (++part == n_parts) {
            part = 0;
            count += 2;
        }
    }

    // If we have data that has been waiting to be written for a while,
    // write it now.
    for (struct net_service *s = Modes.services; s; s = s->next) {
        if (s->writer &&
                s->writer->dataUsed &&
                ((s->writer->lastWrite + Modes.net_output_flush_interval) <= now)) {
            flushWrites(s->writer);
        }
    }

    serviceReconnectCallback(now);
}

/**
 * Reads data from serial client (GNS5894) via SignalIO trigger and
 * writes output. Speed up data handling since we have no influence on
 * flow control in that case.
 * Other periodic work is still done in function above and triggered from
 * backgroundTasks().
 */
void modesReadSerialClient(void) {
    struct net_service *s;
    struct client *c;

    // Search and read from marked serial client only
    for (s = Modes.services; s; s = s->next) {
        if (s->read_handler && s->serial_service) {
            for (c = s->clients; c; c = c->next) {
                if (!c->service)
                    continue;
                modesReadFromClient(c);
            }
        }
    }
}

void writeJsonToNet(struct net_writer *writer, struct char_buffer cb) {
    int len = cb.len;
    int written = 0;
    char *content = cb.buffer;
    char *pos;
    int bytes = MODES_OUT_BUF_SIZE / 2;

    char *p = prepareWrite(writer, bytes);
    if (!p) {
        free(content);
        return;
    }

    pos = content;

    while (p && written < len) {
        if (bytes > len - written) {
            bytes = len - written;
        }
        memcpy(p, pos, bytes);
        p += bytes;
        pos += bytes;
        written += bytes;
        completeWrite(writer, p);

        p = prepareWrite(writer, bytes);
    }

    flushWrites(writer);
    free(content);
}

struct char_buffer generateVRS(int part, int n_parts, int reduced_data) {
    struct char_buffer cb;
    uint64_t now = mstime();
    struct aircraft *a;
    size_t buflen = 256*1024; // The initial buffer is resized as needed
    char *buf = (char *) malloc(buflen), *p = buf, *end = buf + buflen;
    char *line_start;
    int first = 1;
    int part_len = AIRCRAFT_BUCKETS / n_parts;
    int part_start = part * part_len;

    //fprintf(stderr, "%02d/%02d reduced_data: %d\n", part, n_parts, reduced_data);

    p = safe_snprintf(p, end,
            "{\"acList\":[");

    for (int j = part_start; j < part_start + part_len; j++) {
        for (a = Modes.aircraft[j]; a; a = a->next) {
            if (a->messages < 2) { // basic filter for bad decodes
                continue;
            }
            if (now > a->seen + 10 * SECONDS) // don't include stale aircraft in the JSON
                continue;

            // For now, suppress non-ICAO addresses
            if (a->addr & MODES_NON_ICAO_ADDRESS)
                continue;

            if (first)
                first = 0;
            else
                *p++ = ',';

retry:
            line_start = p;

            p = safe_snprintf(p, end, "{\"Icao\":\"%s%06X\"", (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : "", a->addr & 0xFFFFFF);


            if (trackDataValid(&a->position_valid)) {
                p = safe_snprintf(p, end, ",\"Lat\":%f,\"Long\":%f", a->lat, a->lon);
                //p = safe_snprintf(p, end, ",\"PosTime\":%"PRIu64, a->position_valid.updated);
            }

            if (altReliable(a))
                p = safe_snprintf(p, end, ",\"Alt\":%d", a->altitude_baro);

            if (trackDataValid(&a->geom_rate_valid)) {
                p = safe_snprintf(p, end, ",\"Vsi\":%d", a->geom_rate);
            } else if (trackDataValid(&a->baro_rate_valid)) {
                p = safe_snprintf(p, end, ",\"Vsi\":%d", a->baro_rate);
            }

            if (trackDataValid(&a->track_valid)) {
                p = safe_snprintf(p, end, ",\"Trak\":%.1f", a->track);
            } else if (trackDataValid(&a->mag_heading_valid)) {
                p = safe_snprintf(p, end, ",\"Trak\":%.1f", a->mag_heading);
            } else if (trackDataValid(&a->true_heading_valid)) {
                p = safe_snprintf(p, end, ",\"Trak\":%.1f", a->true_heading);
            }

            if (trackDataValid(&a->gs_valid)) {
                p = safe_snprintf(p, end, ",\"Spd\":%.1f", a->gs);
            } else if (trackDataValid(&a->ias_valid)) {
                p = safe_snprintf(p, end, ",\"Spd\":%u", a->ias);
            } else if (trackDataValid(&a->tas_valid)) {
                p = safe_snprintf(p, end, ",\"Spd\":%u", a->tas);
            }

            if (trackDataValid(&a->altitude_geom_valid))
                p = safe_snprintf(p, end, ",\"GAlt\":%d", a->altitude_geom);

            if (trackDataValid(&a->airground_valid) && a->airground == AG_GROUND)
                p = safe_snprintf(p, end, ",\"Gnd\":true");
            else
                p = safe_snprintf(p, end, ",\"Gnd\":false");

            if (trackDataValid(&a->squawk_valid))
                p = safe_snprintf(p, end, ",\"Sqk\":\"%04x\"", a->squawk);

            if (trackDataValid(&a->nav_altitude_mcp_valid)) {
                p = safe_snprintf(p, end, ",\"TAlt\":%d", a->nav_altitude_mcp);
            } else if (trackDataValid(&a->nav_altitude_fms_valid)) {
                p = safe_snprintf(p, end, ",\"TAlt\":%d", a->nav_altitude_fms);
            }

            if (a->position_valid.source != SOURCE_INVALID) {
                if (a->position_valid.source == SOURCE_MLAT)
                    p = safe_snprintf(p, end, ",\"Mlat\":true");
                else if (a->position_valid.source == SOURCE_TISB)
                    p = safe_snprintf(p, end, ",\"Tisb\":true");
                else if (a->position_valid.source == SOURCE_JAERO)
                    p = safe_snprintf(p, end, ",\"Sat\":true");
            }

            if (reduced_data && a->addrtype != ADDR_JAERO && a->position_valid.source != SOURCE_JAERO)
                goto skip_fields;

            if (trackDataAge(now, &a->callsign_valid) < 5 * MINUTES
                    || (a->position_valid.source == SOURCE_JAERO && trackDataAge(now, &a->callsign_valid) < 8 * HOURS)
               ) {
                char buf[128];
                char buf2[16];
                const char *trimmed = trimSpace(a->callsign, buf2, 8);
                if (trimmed[0] != 0) {
                    p = safe_snprintf(p, end, ",\"Call\":\"%s\"", jsonEscapeString(trimmed, buf, sizeof(buf)));
                    p = safe_snprintf(p, end, ",\"CallSus\":false");
                }
            }

            if (trackDataValid(&a->nav_heading_valid))
                p = safe_snprintf(p, end, ",\"TTrk\":%.1f", a->nav_heading);


            if (trackDataValid(&a->geom_rate_valid)) {
                p = safe_snprintf(p, end, ",\"VsiT\":1");
            } else if (trackDataValid(&a->baro_rate_valid)) {
                p = safe_snprintf(p, end, ",\"VsiT\":0");
            }


            if (trackDataValid(&a->track_valid)) {
                p = safe_snprintf(p, end, ",\"TrkH\":false");
            } else if (trackDataValid(&a->mag_heading_valid)) {
                p = safe_snprintf(p, end, ",\"TrkH\":true");
            } else if (trackDataValid(&a->true_heading_valid)) {
                p = safe_snprintf(p, end, ",\"TrkH\":true");
            }

            p = safe_snprintf(p, end, ",\"Sig\":%d", get8bitSignal(a));

            if (trackDataValid(&a->nav_qnh_valid))
                p = safe_snprintf(p, end, ",\"InHg\":%.2f", a->nav_qnh * 0.02952998307);

            p = safe_snprintf(p, end, ",\"AltT\":%d", 0);


            if (a->position_valid.source != SOURCE_INVALID) {
                if (a->position_valid.source != SOURCE_MLAT)
                    p = safe_snprintf(p, end, ",\"Mlat\":false");
                if (a->position_valid.source != SOURCE_TISB)
                    p = safe_snprintf(p, end, ",\"Tisb\":false");
                if (a->position_valid.source != SOURCE_JAERO)
                    p = safe_snprintf(p, end, ",\"Sat\":false");
            }


            if (trackDataValid(&a->gs_valid)) {
                p = safe_snprintf(p, end, ",\"SpdTyp\":0");
            } else if (trackDataValid(&a->ias_valid)) {
                p = safe_snprintf(p, end, ",\"SpdTyp\":2");
            } else if (trackDataValid(&a->tas_valid)) {
                p = safe_snprintf(p, end, ",\"SpdTyp\":3");
            }

            if (a->adsb_version >= 0)
                p = safe_snprintf(p, end, ",\"Trt\":%d", a->adsb_version + 3);
            else
                p = safe_snprintf(p, end, ",\"Trt\":%d", 1);


            //p = safe_snprintf(p, end, ",\"Cmsgs\":%ld", a->messages);


skip_fields:

            p = safe_snprintf(p, end, "}");

            if ((p + 10) >= end) { // +10 to leave some space for the final line
                // overran the buffer
                int used = line_start - buf;
                buflen *= 2;
                buf = (char *) realloc(buf, buflen);
                p = buf + used;
                end = buf + buflen;
                goto retry;
            }
        }
    }

    p = safe_snprintf(p, end, "]}\n");

    if (p >= end)
        fprintf(stderr, "buffer overrun vrs json\n");

    cb.len = p - buf;
    cb.buffer = buf;
    return cb;
}

//
// =============================== Network IO ===========================
//

static void *pthreadGetaddrinfo(void *param) {
    struct net_connector *con = (struct net_connector *) param;

    struct addrinfo gai_hints;

    gai_hints.ai_family = AF_UNSPEC;
    gai_hints.ai_socktype = SOCK_STREAM;
    gai_hints.ai_protocol = 0;
    gai_hints.ai_flags = 0;
    gai_hints.ai_addrlen = 0;
    gai_hints.ai_addr = NULL;
    gai_hints.ai_canonname = NULL;
    gai_hints.ai_next = NULL;

    if (con->use_addr && con->address1) {
        con->address = con->address1;
        if (con->port1)
            con->port = con->port1;
        con->use_addr = 0;
    } else {
        con->address = con->address0;
        con->port = con->port0;
        con->use_addr = 1;
    }
    con->gai_error = getaddrinfo(con->address, con->port, &gai_hints, &con->addr_info);

    pthread_mutex_lock(&con->mutex);
    con->gai_request_done = 1;
    pthread_mutex_unlock(&con->mutex);
    return NULL;
}

static char *sprintAircraftObject(char *p, char *end, struct aircraft *a, uint64_t now, int printMode) {

    // printMode == 0: aircraft.json
    // printMode == 1: trace.json
    // printMode == 2: jsonPositionOutput
    // printMode == 3: globe.json

    p = safe_snprintf(p, end, "\n{");
    if (printMode == 2)
        p = safe_snprintf(p, end, "\"now\" : %.1f,", now / 1000.0);
    if (printMode != 1)
        p = safe_snprintf(p, end, "\"hex\":\"%s%06x\",", (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : "", a->addr & 0xFFFFFF);
    p = safe_snprintf(p, end, "\"type\":\"%s\"", addrtype_enum_string(a->addrtype));
    if (trackDataValid(&a->callsign_valid)) {
        char buf[128];
        p = safe_snprintf(p, end, ",\"flight\":\"%s\"", jsonEscapeString(a->callsign, buf, sizeof(buf)));
    }
    if (Modes.db) {
        if (printMode != 1) {
            if (a->registration[0])
                p = safe_snprintf(p, end, ",\"r\":\"%.*s\"", (int) sizeof(a->registration), a->registration);
            if (a->typeCode[0])
                p = safe_snprintf(p, end, ",\"t\":\"%.*s\"", (int) sizeof(a->typeCode), a->typeCode);
            if (a->dbFlags)
                p = safe_snprintf(p, end, ",\"dbFlags\":%u", a->dbFlags);
        }
        if ((printMode == 0 || printMode == 2)&& !Modes.dbExchange) {
            if (a->typeLong[0])
                p = safe_snprintf(p, end, ",\"desc\":\"%.*s\"", (int) sizeof(a->typeLong), a->typeLong);
        }
    }
    if (printMode != 1) {
        if (trackDataValid(&a->airground_valid) && a->airground == AG_GROUND)
            if (printMode == 2)
                p = safe_snprintf(p, end, ",\"ground\":true");
            else
                p = safe_snprintf(p, end, ",\"alt_baro\":\"ground\"");
        else {
            if (altReliable(a))
                p = safe_snprintf(p, end, ",\"alt_baro\":%d", a->altitude_baro);
            if (printMode == 2)
                p = safe_snprintf(p, end, ",\"ground\":false");
        }
    }
    if (trackDataValid(&a->altitude_geom_valid))
        p = safe_snprintf(p, end, ",\"alt_geom\":%d", a->altitude_geom);
    if (printMode != 1 && trackDataValid(&a->gs_valid))
        p = safe_snprintf(p, end, ",\"gs\":%.1f", a->gs);
    if (trackDataValid(&a->ias_valid))
        p = safe_snprintf(p, end, ",\"ias\":%u", a->ias);
    if (trackDataValid(&a->tas_valid))
        p = safe_snprintf(p, end, ",\"tas\":%u", a->tas);
    if (trackDataValid(&a->mach_valid))
        p = safe_snprintf(p, end, ",\"mach\":%.3f", a->mach);
    if (now < a->wind_updated + TRACK_EXPIRE && abs(a->wind_altitude - a->altitude_baro) < 500) {
        p = safe_snprintf(p, end, ",\"wd\":%.0f", a->wind_direction);
        p = safe_snprintf(p, end, ",\"ws\":%.0f", a->wind_speed);
    }
    if (now < a->oat_updated + TRACK_EXPIRE) {
        p = safe_snprintf(p, end, ",\"oat\":%.0f", a->oat);
        p = safe_snprintf(p, end, ",\"tat\":%.0f", a->tat);
    }

    if (trackDataValid(&a->track_valid))
        p = safe_snprintf(p, end, ",\"track\":%.2f", a->track);
    else if (printMode != 1 && trackDataValid(&a->position_valid) &&
        !(trackDataValid(&a->airground_valid) && a->airground == AG_GROUND))
        p = safe_snprintf(p, end, ",\"calc_track\":%.0f", a->calc_track);

    if (trackDataValid(&a->track_rate_valid))
        p = safe_snprintf(p, end, ",\"track_rate\":%.2f", a->track_rate);
    if (trackDataValid(&a->roll_valid))
        p = safe_snprintf(p, end, ",\"roll\":%.2f", a->roll);
    if (trackDataValid(&a->mag_heading_valid))
        p = safe_snprintf(p, end, ",\"mag_heading\":%.2f", a->mag_heading);
    if (trackDataValid(&a->true_heading_valid))
        p = safe_snprintf(p, end, ",\"true_heading\":%.2f", a->true_heading);
    if (trackDataValid(&a->baro_rate_valid))
        p = safe_snprintf(p, end, ",\"baro_rate\":%d", a->baro_rate);
    if (trackDataValid(&a->geom_rate_valid))
        p = safe_snprintf(p, end, ",\"geom_rate\":%d", a->geom_rate);
    if (trackDataValid(&a->squawk_valid))
        p = safe_snprintf(p, end, ",\"squawk\":\"%04x\"", a->squawk);
    if (trackDataValid(&a->emergency_valid))
        p = safe_snprintf(p, end, ",\"emergency\":\"%s\"", emergency_enum_string(a->emergency));
    if (a->category != 0)
        p = safe_snprintf(p, end, ",\"category\":\"%02X\"", a->category);
    if (trackDataValid(&a->nav_qnh_valid))
        p = safe_snprintf(p, end, ",\"nav_qnh\":%.1f", a->nav_qnh);
    if (trackDataValid(&a->nav_altitude_mcp_valid))
        p = safe_snprintf(p, end, ",\"nav_altitude_mcp\":%d", a->nav_altitude_mcp);
    if (trackDataValid(&a->nav_altitude_fms_valid))
        p = safe_snprintf(p, end, ",\"nav_altitude_fms\":%d", a->nav_altitude_fms);
    if (trackDataValid(&a->nav_heading_valid))
        p = safe_snprintf(p, end, ",\"nav_heading\":%.2f", a->nav_heading);
    if (trackDataValid(&a->nav_modes_valid)) {
        p = safe_snprintf(p, end, ",\"nav_modes\":[");
        p = append_nav_modes(p, end, a->nav_modes, "\"", ",");
        p = safe_snprintf(p, end, "]");
    }
    if (printMode != 1) {
        if (posReliable(a)) {
            p = safe_snprintf(p, end, ",\"lat\":%f,\"lon\":%f,\"nic\":%u,\"rc\":%u,\"seen_pos\":%.1f",
                    a->lat, a->lon, a->pos_nic, a->pos_rc,
                    (now < a->position_valid.updated) ? 0 : ((now - a->position_valid.updated) / 1000.0));
        } else if (now < a->rr_seen + 2 * MINUTES) {
            p = safe_snprintf(p, end, ",\"rr_lat\":%.1f,\"rr_lon\":%.1f", a->rr_lat, a->rr_lon);
        }
    }

    if (printMode == 1 && trackDataValid(&a->position_valid)) {
        p = safe_snprintf(p, end, ",\"nic\":%u,\"rc\":%u",
                a->pos_nic, a->pos_rc);
    }
    if (a->adsb_version >= 0)
        p = safe_snprintf(p, end, ",\"version\":%d", a->adsb_version);
    if (trackDataValid(&a->nic_baro_valid))
        p = safe_snprintf(p, end, ",\"nic_baro\":%u", a->nic_baro);
    if (trackDataValid(&a->nac_p_valid))
        p = safe_snprintf(p, end, ",\"nac_p\":%u", a->nac_p);
    if (trackDataValid(&a->nac_v_valid))
        p = safe_snprintf(p, end, ",\"nac_v\":%u", a->nac_v);
    if (trackDataValid(&a->sil_valid))
        p = safe_snprintf(p, end, ",\"sil\":%u", a->sil);
    if (a->sil_type != SIL_INVALID)
        p = safe_snprintf(p, end, ",\"sil_type\":\"%s\"", sil_type_enum_string(a->sil_type));
    if (trackDataValid(&a->gva_valid))
        p = safe_snprintf(p, end, ",\"gva\":%u", a->gva);
    if (trackDataValid(&a->sda_valid))
        p = safe_snprintf(p, end, ",\"sda\":%u", a->sda);
    if (trackDataValid(&a->alert_valid))
        p = safe_snprintf(p, end, ",\"alert\":%u", a->alert);
    if (trackDataValid(&a->spi_valid))
        p = safe_snprintf(p, end, ",\"spi\":%u", a->spi);

    /*
    if (a->position_valid.source == SOURCE_JAERO)
        p = safe_snprintf(p, end, ",\"jaero\": true");
    if (a->position_valid.source == SOURCE_SBS)
        p = safe_snprintf(p, end, ",\"sbs_other\": true");
    */
    if (Modes.netReceiverIdPrint) {
        p = safe_snprintf(p, end, ",\"rId\":%016"PRIx64"", a->lastPosReceiverId);
    }

    if (printMode != 1) {
        p = safe_snprintf(p, end, ",\"mlat\":");
        p = append_flags(p, end, a, SOURCE_MLAT);
        p = safe_snprintf(p, end, ",\"tisb\":");
        p = append_flags(p, end, a, SOURCE_TISB);

        p = safe_snprintf(p, end, ",\"messages\":%u,\"seen\":%.1f,\"rssi\":%.1f}",
                a->messages, (now < a->seen) ? 0 : ((now - a->seen) / 1000.0),
                10 * log10((a->signalLevel[0] + a->signalLevel[1] + a->signalLevel[2] + a->signalLevel[3] +
                        a->signalLevel[4] + a->signalLevel[5] + a->signalLevel[6] + a->signalLevel[7]) / 8 + 1.125e-5));
    } else {
        p = safe_snprintf(p, end, "}");
    }

    return p;
}

void cleanupNetwork(void) {
    for (struct net_service *s = Modes.services; s; s = s->next) {
        struct client *c = s->clients, *nc;
        while (c) {
            nc = c->next;

            anetCloseSocket(c->fd);
            c->sendq_len = 0;
            if (c->sendq) {
                free(c->sendq);
                c->sendq = NULL;
            }
            free(c);

            c = nc;
        }
    }

    struct net_service *s = Modes.services, *ns;
    while (s) {
        ns = s->next;
        free(s->listener_fds);
        if (s->writer && s->writer->data) {
            free(s->writer->data);
            s->writer->data = NULL;
        }
        if (s) free(s);
        s = ns;
    }

    for (int i = 0; i < Modes.net_connectors_count; i++) {
        struct net_connector *con = Modes.net_connectors[i];
        if (con->gai_request_in_progress) {
            pthread_join(con->thread, NULL);
        }
        free(con->address0);
        if (con->addr_info) {
            freeaddrinfo(con->addr_info);
            con->addr_info = NULL;
        }
        pthread_mutex_destroy(&con->mutex);
        free(con);
    }
    free(Modes.net_connectors);

    Modes.net_connectors_count = 0;

}

static void read_uuid(struct client *c, char *p, char *eod) {
    if (c->receiverIdLocked) { // only allow the receiverId to be set once
        return;
    }

    unsigned char ch;
    char *start = p;
    uint64_t receiverId = 0;
    uint64_t receiverId2 = 0;
    // read ascii to binary
    int j = 0;
    for (int i = 0; i < 128 && j < 32; i++) {
        ch = *p++;

        //fprintf(stderr, "%c", ch);
        if (p >= eod)
            break;
        if (0x1A == ch) {
            break;
        }
        if ('-' == ch || ' ' == ch) {
            continue;
        }

        unsigned char x = 0xff;

        if (ch <= 'f' && ch >= 'a')
            x = ch - 'a' + 10;
        else if (ch <= '9' && ch >= '0')
            x = ch - '0';
        else if (ch <= 'F' && ch >= 'A')
            x = ch - 'A' + 10;
        else
            break;

        if (j < 16)
            receiverId = receiverId << 4 | x; // set 4 bits and shift them up
        else if (j < 32)
            receiverId2 = receiverId2 << 4 | x; // set 4 bits and shift them up
        j++;
    }

    if (j >= 16) {
        c->receiverId = receiverId;
        c->receiverId2 = receiverId2;

        if (0) {
            fprintf(stderr, "ADDR %s,%s rId %016"PRIx64" UUID %.*s\n",
                    c->host, c->port, c->receiverId,
                    min(eod - start, 36), start);
        }
    }
    return;
}


struct char_buffer generateClientsJson() {
    struct char_buffer cb;
    uint64_t now = mstime();

    size_t buflen = 1*1024*1024; // The initial buffer is resized as needed
    char *buf = (char *) malloc(buflen), *p = buf, *end = buf + buflen;

    p = safe_snprintf(p, end, "{ \"now\" : %.1f,\n", now / 1000.0);
    p = safe_snprintf(p, end, "  \"format\" : "
            "[ \"receiverId\", \"host:port\", \"avg. kbit/s\", \"conn time(s)\", \"messageCounter\", \"positionCounter\" ],\n");

    p = safe_snprintf(p, end, "  \"clients\" : [\n");

    for (struct net_service *s = Modes.services; s; s = s->next) {
        for (struct client *c = s->clients; c; c = c->next) {
            if (!c->service)
                continue;
            if (!s->read_handler)
                continue;

            // check if we have enough space
            if ((p + 1000) >= end) {
                int used = p - buf;
                buflen *= 2;
                buf = (char *) realloc(buf, buflen);
                p = buf + used;
                end = buf + buflen;
            }

            double elapsed = (now - c->connectedSince) / 1000.0;
            p = safe_snprintf(p, end, "[ \"%016"PRIx64"%016"PRIx64"\", \"%s\", %6.2f, %6.1f, %9.0f, %9.0f ],\n",
                    c->receiverId,
                    c->receiverId2,
                    c->proxy_string,
                    c->bytesReceived / 128.0 / elapsed,
                    elapsed,
                    (double) c->messageCounter,
                    (double) c->positionCounter);


            if (p >= end)
                fprintf(stderr, "buffer overrun client json\n");
        }
    }

    if (*(p-2) == ',')
        *(p-2) = ' ';

    p = safe_snprintf(p, end, "\n  ]\n}\n");

    cb.len = p - buf;
    cb.buffer = buf;
    return cb;
}
