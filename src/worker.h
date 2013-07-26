/*
 * weighttp - a lightweight and simple webserver benchmarking tool
 *
 * Author:
 *     Copyright (c) 2009-2011 Thomas Porzelt
 *
 * License:
 *     MIT, see COPYING file
 */

struct Times {
    ev_tstamp starttime;/* start time of connection */
    ev_tstamp waittime; /* between request and reading response */
    ev_tstamp ctime;    /* time to connect */
    ev_tstamp time;     /* time for connection */
};
#define ap_min(a,b) (((a)<(b))?(a):(b))
#define ap_max(a,b) (((a)>(b))?(a):(b))
#define ap_round_ms(a) ((uint32_t)((a) + 0.500)*1000)
#define ap_double_ms(a) ((a)*1000.0)

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
	struct Times * times;
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

#define LADDR_PORT_BEG    10000
#define LADDR_PORT_END    60000
#define LADDR_PORT_RANGE  LADDR_PORT_END - LADDR_PORT_BEG;

  uint16_t laddrs_ip_beg;
  uint16_t laddrs_ip_end;
  uint16_t current_laddr;
  uint16_t current_port;
};


Worker *worker_new(uint8_t id, Config *config, uint16_t num_clients, uint64_t num_requests, struct Times * times);
void worker_free(Worker *worker);
void *worker_thread(void* arg);
