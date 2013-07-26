/*
 * weighttp - a lightweight and simple webserver benchmarking tool
 *
 * Author:
 *     Copyright (c) 2009-2011 Thomas Porzelt
 *
 * License:
 *     MIT, see COPYING file
 */

#include "weighttp.h"
#include <arpa/inet.h>

Worker *worker_new(uint8_t id, Config *config, uint16_t num_clients, uint64_t num_requests, struct Times * times) {
	Worker *worker;
	uint16_t i;

	worker = W_MALLOC(Worker, 1);
	worker->id = id;
	worker->loop = ev_loop_new(0);
	ev_ref(worker->loop);
	worker->config = config;
	worker->num_clients = num_clients;
	worker->stats.req_todo = num_requests;
	worker->stats.times = times;
	worker->progress_interval = num_requests / 10;
	if(0 < config->laddr_size) {
	  uint16_t num_laddrs = config->laddr_size / config->thread_count;
	  worker->laddrs_ip_beg = (id - 1) * num_laddrs;
	  worker->laddrs_ip_end = worker->laddrs_ip_beg + num_laddrs;
	  printf("worker-%u will use local addresses:\n", id);
	  for(i = worker->laddrs_ip_beg; i < worker->laddrs_ip_end; ++i) {
	    printf("\t%s\n", inet_ntoa(config->laddres[i].sin_addr));
	  }
	  worker->current_laddr = worker->laddrs_ip_beg;
	  worker->current_port = LADDR_PORT_BEG;
	}

	if (worker->progress_interval == 0)
		worker->progress_interval = 1;

	worker->clients = W_MALLOC(Client*, num_clients);

	for (i = 0; i < num_clients; i++) {
		if (NULL == (worker->clients[i] = client_new(worker)))
			return NULL;
	}

	return worker;
}

void worker_free(Worker *worker) {
	uint16_t i;

	for (i = 0; i < worker->num_clients; i++)
		client_free(worker->clients[i]);

	free(worker->clients);
	free(worker);
}

void *worker_thread(void* arg) {
	uint16_t i;
	Worker *worker = (Worker*)arg;

	/* start all clients */
	for (i = 0; i < worker->num_clients; i++) {
		if (worker->stats.req_started < worker->stats.req_todo)
			client_state_machine(worker->clients[i]);
	}

	ev_loop(worker->loop, 0);

	ev_loop_destroy(worker->loop);

	return NULL;
}
