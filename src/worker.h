/*
 * weighttp - a lightweight and simple webserver benchmarking tool
 *
 * Author:
 *     Copyright (c) 2009-2011 Thomas Porzelt
 *
 * License:
 *     MIT, see COPYING file
 */

struct Stats {
	ev_tstamp req_ts_min;	/* minimum time taken for a request */
	ev_tstamp req_ts_max;	/* maximum time taken for a request */
	ev_tstamp req_ts_total;	/* total time taken for all requests (this is not ts_end - ts_start!) */
	uint64_t req_todo;		/* total number of requests to do */
	uint64_t req_started;	/* total number of requests started */
	uint64_t req_done;		/* total number of requests done */
	uint64_t req_success;	/* total number of successful requests */
	uint64_t req_failed;	/* total number of failed requests */
	uint64_t req_error;		/* total number of error'd requests */
	uint64_t bytes_total;	/* total number of bytes received (html+body) */
	uint64_t bytes_body;	/* total number of bytes received (body) */
	uint64_t req_1xx;
	uint64_t req_2xx;
	uint64_t req_3xx;
	uint64_t req_4xx;
	uint64_t req_5xx;
};

struct Worker {
	uint8_t id;
	Config *config;
	struct ev_loop *loop;
	char *request;
	Client **clients;
	uint16_t num_clients;
	Stats stats;
	uint64_t progress_interval;
};


Worker *worker_new(uint8_t id, Config *config, uint16_t num_clients, uint64_t num_requests);
void worker_free(Worker *worker);
void *worker_thread(void* arg);
