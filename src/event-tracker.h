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
#ifndef EVENT_TRACKER_H
#define EVENT_TRACKER_H

#include <mcpanel.h>
#include <pthread.h>
#include <mmtime.h>

#define NEVENT_MAX      16

struct event_stack {
	int nevent;
	struct mcp_event events[NEVENT_MAX];
};

/**
 * struct event_tracker - data for software trigger reception
 * @thread:             event reception thread
 * @mtx:                mutex protecting the concurrent access to event stacks
 *                      and shared metadata.
 * @server_socket:      server socket listening for connection
 * @client_socket:      socket of the current client connection
 * @last_read_ts:       timestamp when data associated with @last_total_read
 *                      has been acquired
 * @last_total_read:    index of last data acquired (observable from event
 *                      thread)
 * @fs:                 sampling frequency of the acquisition data
 * @stack_idx:          index of the element in @stacks currently used for writing
 * @stacks:             array of two event stack used to transmit the event data
 *                      read from event reception thread in a double buffering scheme
 */
struct event_tracker {
	pthread_t thread;
	pthread_mutex_t mtx;
	int server_socket;
	int client_socket;
	struct mm_timespec last_read_ts;
	int last_total_read;
	float fs;
	int stack_idx;
	int quit_loop;
	struct event_stack stacks[2];
};

int event_tracker_init(struct event_tracker* trk, float fs, int port);
void event_tracker_deinit(struct event_tracker* trk);
struct event_stack* event_tracker_swap_eventstack(struct event_tracker* trk);
void event_tracker_update_ns_read(struct event_tracker* trk, int total_read);

#endif
