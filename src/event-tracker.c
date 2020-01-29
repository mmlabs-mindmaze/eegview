/*
    Copyright (C) 2018  MindMaze SA
    Nicolas Bourdaud <nicolas.bourdaud@gmail.com>

    This program is free software: you can redistribute it and/or modify
    modify it under the terms of the version 3 of the GNU General Public
    License as published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <mmlog.h>
#include <mmsysio.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "event-tracker.h"
#include "mcpanel.h"
#include "mmpredefs.h"
#include "mmtime.h"

#define ACCEPT_TIMEOUT  500 //in ms


/**************************************************************************
 *                                                                        *
 *              Internals of event reception                              *
 *                                                                        *
 **************************************************************************/
static
int create_listening_socket(int port)
{
	int sock;
	struct addrinfo *rp, *res = NULL;
	char service[16];
	int reuse = -1;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_PASSIVE,
	};

	// Create server socket
	sock = mm_socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return -1;

	if (mm_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1)
		goto error;

	// Construct local address struct
	snprintf(service, sizeof(service), "%i", port);
	if (mm_getaddrinfo(NULL, service, &hints, &res))
		goto error;

	// Loop over the result and try to bind server socket to address
	for (rp = res; rp != NULL; rp = rp->ai_next) {
		if (mm_bind(sock, res->ai_addr, res->ai_addrlen) == 0)
			break;
	}
	mm_freeaddrinfo(res);

	// Listen for incoming clients (if sock has been bound)
	if (  rp == NULL
	   || mm_listen(sock, 1))
		goto error;

	return sock;

error:
	mm_close(sock);
	return -1;
}


/**
 * event_tracker_accept_client() - wait for next client connection
 * @trk:        initialized event tracker
 *
 * Wait for incoming client connection. Test regularly that event thread
 * has not been requested to quit. If one client is accepted, store it in
 * @trk for future reference
 *
 * Return: 0 if the connection has been accepted, 1 is event thread has
 * been requested to quit, -1 in case of error.
 */
static
int event_tracker_accept_client(struct event_tracker* trk)
{
	struct sockaddr_in client_address;
	struct sockaddr* addr;
	socklen_t addr_len;
	char ip4[16];
	int quit, client_socket, n_fd_event;
	struct mm_pollfd pfd = {.fd = trk->server_socket, .events = POLLIN};

	// Wait for incoming connection
	while (1) {
		n_fd_event = mm_poll(&pfd, 1, ACCEPT_TIMEOUT);
		if (n_fd_event < 0)
			return -1;

		// If there is a read event on fd, it means there is a
		// connection to accept
		if (n_fd_event >= 1)
			break;

		pthread_mutex_lock(&trk->mtx);
		quit = trk->quit_loop;
		pthread_mutex_unlock(&trk->mtx);
		if (quit)
			return 1;
	}

	// Accept incoming connection
	addr_len = sizeof(client_address);
	addr = (struct sockaddr*) &client_address;
	client_socket = mm_accept(trk->server_socket, addr, &addr_len);
	if (client_socket < 0)
		return -1;

	mm_getnameinfo(addr, addr_len, ip4, sizeof(ip4), NULL, 0, NI_NUMERICHOST);
	mm_log_info("Accepted client %s!", ip4);

	// Keep client socket if we are not exiting the event thread
	pthread_mutex_lock(&trk->mtx);
	if (trk->quit_loop)
		mm_close(client_socket);
	else
		trk->client_socket = client_socket;
	pthread_mutex_unlock(&trk->mtx);

	return 0;
}


/**
 * event_tracker_finish_client() - close current client connection
 * @trk:        initialized event tracker
 *
 * Return: 1 if event thread has been requested to quit, 0 otherwise
 */
static
int event_tracker_finish_client(struct event_tracker* trk)
{
	int quit;

	pthread_mutex_lock(&trk->mtx);

	mm_close(trk->client_socket);
	trk->client_socket = -1;

	quit = trk->quit_loop;

	pthread_mutex_unlock(&trk->mtx);

	mm_log_info("Client disconnected");

	return quit;
}


/**
 * event_tracker_add_event() - add event to store in tracker
 * @trk:        initialized event tracker
 * @evttype:    event code of the software event
 *
 * This function adds a new event to the event stack to write associated to code
 * @evttype. The estimation of position of event in the acquisition data stream
 * is based of wallclock time when this function is called and when was the
 * wallclock when the last call to event_tracker_update_ns_read() happened.
 *
 * Return: 1 if event thread has been requested to quit, 0 otherwise
 */
static
int event_tracker_add_event(struct event_tracker* trk, uint32_t evttype)
{
	struct event_stack* evtstack;
	struct mcp_event* evt;
	int64_t dt;
	int quit, pos;
	struct mm_timespec ts;

	mm_gettime(CLOCK_REALTIME, &ts);

	pthread_mutex_lock(&trk->mtx);

	// Compute number of sample passed being acquired given ts relative to
	// last update
	dt = mm_timediff_ns(&ts, &trk->last_read_ts);
	pos = dt * 1e-9f * trk->fs;
	pos += trk->last_total_read;

	// Get event struct for storing (from event stack used for writing)
	// and store event info in it. If the event stack is full, just drop
	// the software event
	evtstack = trk->stacks + trk->stack_idx;
	if (evtstack->nevent < NEVENT_MAX) {
		evt = &evtstack->events[evtstack->nevent++];
		evt->pos = pos;
		evt->type = evttype;
	}

	quit = trk->quit_loop;
	pthread_mutex_unlock(&trk->mtx);

	return quit;
}


/**
 * event_tracker_handle_client_connection() - store event coming for client
 * @trk:        initialized event tracker with established client
 */
static
void event_tracker_handle_client_connection(struct event_tracker* trk)
{
	uint32_t evttype;
	ssize_t rsz;
	int quit, sock;

	sock = trk->client_socket;
	quit = 0;
	while (!quit) {
		rsz = mm_recv(sock, &evttype, sizeof(evttype), 0);
		if (rsz <= 0)
			break;

		quit = event_tracker_add_event(trk, evttype);
	}
}


static
void* event_thread(void* arg)
{
	struct event_tracker* trk = arg;
	int quit = 0;

	while (!quit) {
		quit = event_tracker_accept_client(trk);
		if (quit)
			break;

		event_tracker_handle_client_connection(trk);
		quit = event_tracker_finish_client(trk);
	}

	return NULL;
}

/**************************************************************************
 *                                                                        *
 *                       API of event tracker                             *
 *                                                                        *
 **************************************************************************/

/**
 * event_tracker_swap_eventstack() - get last event stack for reading
 * @trk:        initialized event tracker
 *
 * Return: the pointer to event stack containing the software event. The
 * content of the pointed data is valid until the next call to
 * event_tracker_swap_eventstack().
 */
struct event_stack* event_tracker_swap_eventstack(struct event_tracker* trk)
{
	struct event_stack* evt_stk;

	pthread_mutex_lock(&trk->mtx);

	// Get pointer of event stack meant for reading
	evt_stk = trk->stacks + trk->stack_idx;

	// update stack index and reset the number of event written
	trk->stack_idx = (trk->stack_idx + 1) % MM_NELEM(trk->stacks);
	trk->stacks[trk->stack_idx].nevent = 0;

	pthread_mutex_unlock(&trk->mtx);

	return evt_stk;
}


/**
 * event_tracker_update_ns_read() - inform tracker about number of sample acquired
 * @trk:        initialized event tracker
 * @total_read: number of sample acquired since beginning of acquisition
 *
 * NOTE: For better estimation of software event timing, it is better to call
 * this function close to the moment when acquisition function returned
 */
void event_tracker_update_ns_read(struct event_tracker* trk, int total_read)
{
	struct mm_timespec ts;

	mm_gettime(CLOCK_REALTIME, &ts);

	pthread_mutex_lock(&trk->mtx);
	trk->last_total_read = total_read;
	trk->last_read_ts = ts;
	pthread_mutex_unlock(&trk->mtx);
}


int event_tracker_init(struct event_tracker* trk, float fs, int port)
{
	*trk = (struct event_tracker) {
		.client_socket = -1,
		.server_socket = -1,
		.fs = fs,
	};

	pthread_mutex_init(&trk->mtx, NULL);
	trk->server_socket = create_listening_socket(port);
	if (trk->server_socket == -1) {
		trk->quit_loop = 1;
		return -1;
	}

	mm_gettime(CLOCK_REALTIME, &trk->last_read_ts);
	pthread_create(&trk->thread, NULL, event_thread, trk);
	return 0;
}


void event_tracker_deinit(struct event_tracker* trk)
{
	pthread_mutex_lock(&trk->mtx);
	trk->quit_loop = 1;
	if (trk->client_socket >= 0)
		mm_shutdown(trk->client_socket, SHUT_RDWR);
	pthread_mutex_unlock(&trk->mtx);

	pthread_join(trk->thread, NULL);
	pthread_mutex_destroy(&trk->mtx);

	mm_close(trk->server_socket);
}


