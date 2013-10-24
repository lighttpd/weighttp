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

extern int optind, optopt; /* getopt */

static void show_help(void) {
	printf("weighttp <options> <url>\n");
	printf("  -n num   number of requests    (mandatory)\n");
	printf("  -t num   threadcount           (default: 1)\n");
	printf("  -c num   concurrent clients    (default: 1)\n");
	printf("  -k       keep alive            (default: no)\n");
	printf("  -6       use ipv6              (default: no)\n");
	printf("  -H str   add header to request\n");
	printf("  -h       show help and exit\n");
	printf("  -v       show version and exit\n\n");
	printf("example: weighttpd -n 10000 -c 10 -t 2 -k -H \"User-Agent: foo\" localhost/index.html\n\n");
}

static struct addrinfo *resolve_host(char *hostname, uint16_t port, uint8_t use_ipv6) {
	int err;
	char port_str[6];
	struct addrinfo hints, *res, *res_first, *res_last;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	sprintf(port_str, "%d", port);

	err = getaddrinfo(hostname, port_str, &hints, &res_first);

	if (err) {
		W_ERROR("could not resolve hostname: %s", hostname);
		return NULL;
	}

	/* search for an ipv4 address, no ipv6 yet */
	res_last = NULL;
	for (res = res_first; res != NULL; res = res->ai_next) {
		if (res->ai_family == AF_INET && !use_ipv6)
			break;
		else if (res->ai_family == AF_INET6 && use_ipv6)
			break;

		res_last = res;
	}

	if (!res) {
		freeaddrinfo(res_first);
		W_ERROR("could not resolve hostname: %s", hostname);
		return NULL;
	}

	if (res != res_first) {
		/* unlink from list and free rest */
		res_last->ai_next = res->ai_next;
		freeaddrinfo(res_first);
		res->ai_next = NULL;
	}

	return res;
}

static char *forge_request(char *url, char keep_alive, char **host, uint16_t *port, char **headers, uint8_t headers_num) {
	char *c, *end;
	char *req;
	uint32_t len;
	uint8_t i;
	uint8_t have_user_agent;
	char *header_host;

	*host = NULL;
	*port = 0;
	header_host = NULL;

	if (strncmp(url, "http://", 7) == 0)
		url += 7;
	else if (strncmp(url, "https://", 8) == 0) {
		W_ERROR("%s", "no ssl support yet");
		url += 8;
		return NULL;
	}

	len = strlen(url);

	if ((c = strchr(url, ':'))) {
		/* found ':' => host:port */ 
		*host = W_MALLOC(char, c - url + 1);
		memcpy(*host, url, c - url);
		(*host)[c - url] = '\0';

		if ((end = strchr(c+1, '/'))) {
			*end = '\0';
			*port = atoi(c+1);
			*end = '/';
			url = end;
		} else {
			*port = atoi(c+1);
			url += len;
		}
	} else {
		*port = 80;

		if ((c = strchr(url, '/'))) {
			*host = W_MALLOC(char, c - url + 1);
			memcpy(*host, url, c - url);
			(*host)[c - url] = '\0';
			url = c;
		} else {
			*host = W_MALLOC(char, len + 1);
			memcpy(*host, url, len);
			(*host)[len] = '\0';
			url += len;
		}
	}

	if (*port == 0) {
		W_ERROR("%s", "could not parse url");
		free(*host);
		return NULL;
	}

	if (*url == '\0')
		url = "/";

	// total request size
	len = strlen("GET HTTP/1.1\r\nHost: :65536\r\nConnection: keep-alive\r\n\r\n") + 1;
	len += strlen(*host);
	len += strlen(url);

	have_user_agent = 0;
	for (i = 0; i < headers_num; i++) {
		if (strncmp(headers[i], "Host:", sizeof("Host:")-1) == 0) {
			if (header_host) {
				W_ERROR("%s", "Duplicate Host header");
				free(*host);
				return NULL;
			}
			header_host = headers[i] + 5;
			if (*header_host == ' ')
				header_host++;

			if (strlen(header_host) == 0) {
				W_ERROR("%s", "Invalid Host header");
				free(*host);
				return NULL;
			}

			len += strlen(header_host);
			continue;
		}
		len += strlen(headers[i]) + strlen("\r\n");
		if (strncmp(headers[i], "User-Agent:", sizeof("User-Agent:")-1) == 0)
			have_user_agent = 1;
	}

	if (!have_user_agent)
		len += strlen("User-Agent: weighttp/" VERSION "\r\n");

	req = W_MALLOC(char, len);

	strcpy(req, "GET ");
	strcat(req, url);
	strcat(req, " HTTP/1.1\r\nHost: ");
	if (header_host) {
		strcat(req, header_host);
	} else {
		strcat(req, *host);
		if (*port != 80)
			sprintf(req + strlen(req), ":%"PRIu16, *port);
	}

	strcat(req, "\r\n");

	if (!have_user_agent)
		sprintf(req + strlen(req), "User-Agent: weighttp/" VERSION "\r\n");

	for (i = 0; i < headers_num; i++) {
		if (strncmp(headers[i], "Host:", sizeof("Host:")-1) == 0)
			continue;
		strcat(req, headers[i]);
		strcat(req, "\r\n");
	}

	if (keep_alive)
		strcat(req, "Connection: keep-alive\r\n\r\n");
	else
		strcat(req, "Connection: close\r\n\r\n");

	return req;
}

uint64_t str_to_uint64(char *str) {
	uint64_t i;

	for (i = 0; *str; str++) {
		if (*str < '0' || *str > '9')
			return UINT64_MAX;

		i *= 10;
		i += *str - '0';
	}

	return i;
}

int main(int argc, char *argv[]) {
	Worker **workers;
	pthread_t *threads;
	int i;
	int opt;
	int err;
	struct ev_loop *loop;
	ev_tstamp ts_start, ts_end;
	Config config;
	Worker *worker;
	char *host;
	uint16_t port;
	uint8_t use_ipv6;
	uint16_t rest_concur, rest_req;
	Stats stats;
	ev_tstamp duration;
	int sec, millisec, microsec;
	uint64_t rps;
	uint64_t kbps;
	char **headers;
	uint8_t headers_num;

	printf("weighttp - a lightweight and simple webserver benchmarking tool\n\n");

	headers = NULL;
	headers_num = 0;

	/* default settings */
	use_ipv6 = 0;
	config.thread_count = 1;
	config.concur_count = 1;
	config.req_count = 0;
	config.keep_alive = 0;

	while ((opt = getopt(argc, argv, ":hv6kn:t:c:H:")) != -1) {
		switch (opt) {
			case 'h':
				show_help();
				return 0;
			case 'v':
				printf("version:    " VERSION "\n");
				printf("build-date: " __DATE__ " " __TIME__ "\n\n");
				return 0;
			case '6':
				use_ipv6 = 1;
				break;
			case 'k':
				config.keep_alive = 1;
				break;
			case 'n':
				config.req_count = str_to_uint64(optarg);
				break;
			case 't':
				config.thread_count = atoi(optarg);
				break;
			case 'c':
				config.concur_count = atoi(optarg);
				break;
			case 'H':
				headers = W_REALLOC(headers, char*, headers_num+1);
				headers[headers_num] = optarg;
				headers_num++;
				break;
			case '?':
				if ('?' != optopt) W_ERROR("unkown option: -%c\n", optopt);
				show_help();
				return 1;
			case ':':
				W_ERROR("option requires an argument: -%c\n", optopt);
				show_help();
				return 1;
		}
	}

	if ((argc - optind) < 1) {
		W_ERROR("%s", "missing url argument\n");
		show_help();
		return 1;
	} else if ((argc - optind) > 1) {
		W_ERROR("%s", "too many arguments\n");
		show_help();
		return 1;
	}

	/* check for sane arguments */
	if (!config.thread_count) {
		W_ERROR("%s", "thread count has to be > 0\n");
		show_help();
		return 1;
	}
	if (!config.concur_count) {
		W_ERROR("%s", "number of concurrent clients has to be > 0\n");
		show_help();
		return 1;
	}
	if (!config.req_count) {
		W_ERROR("%s", "number of requests has to be > 0\n");
		show_help();
		return 1;
	}
	if (config.req_count == UINT64_MAX || config.thread_count > config.req_count || config.thread_count > config.concur_count || config.concur_count > config.req_count) {
		W_ERROR("%s", "insane arguments\n");
		show_help();
		return 1;
	}


	loop = ev_default_loop(0);
	if (!loop) {
		W_ERROR("%s", "could not initialize libev\n");
		return 2;
	}

	if (NULL == (config.request = forge_request(argv[optind], config.keep_alive, &host, &port, headers, headers_num))) {
		return 1;
	}

	config.request_size = strlen(config.request);
	//printf("Request (%d):\n==========\n%s==========\n", config.request_size, config.request);
	//printf("host: '%s', port: %d\n", host, port);

	/* resolve hostname */
	if(!(config.saddr = resolve_host(host, port, use_ipv6))) {
		return 1;
	}

	/* spawn threads */
	threads = W_MALLOC(pthread_t, config.thread_count);
	workers = W_MALLOC(Worker*, config.thread_count);

	rest_concur = config.concur_count % config.thread_count;
	rest_req = config.req_count % config.thread_count;

	printf("starting benchmark...\n");

	memset(&stats, 0, sizeof(stats));
	ts_start = ev_time();

	for (i = 0; i < config.thread_count; i++) {
		uint64_t reqs = config.req_count / config.thread_count;
		uint16_t concur = config.concur_count / config.thread_count;

		if (rest_concur) {
			concur += 1;
			rest_concur -= 1;
		}

		if (rest_req) {
			reqs += 1;
			rest_req -= 1;
		}
		printf("spawning thread #%d: %"PRIu16" concurrent requests, %"PRIu64" total requests\n", i+1, concur, reqs);
		workers[i] = worker_new(i+1, &config, concur, reqs);

		if (!(workers[i])) {
			W_ERROR("%s", "failed to allocate worker or client");
			return 1;
		}

		err = pthread_create(&threads[i], NULL, worker_thread, (void*)workers[i]);

		if (err != 0) {
			W_ERROR("failed spawning thread (%d)", err);
			return 2;
		}
	}

	for (i = 0; i < config.thread_count; i++) {
		err = pthread_join(threads[i], NULL);
		worker = workers[i];

		if (err != 0) {
			W_ERROR("failed joining thread (%d)", err);
			return 3;
		}

		stats.req_started += worker->stats.req_started;
		stats.req_done += worker->stats.req_done;
		stats.req_success += worker->stats.req_success;
		stats.req_failed += worker->stats.req_failed;
		stats.bytes_total += worker->stats.bytes_total;
		stats.bytes_body += worker->stats.bytes_body;
		stats.req_2xx += worker->stats.req_2xx;
		stats.req_3xx += worker->stats.req_3xx;
		stats.req_4xx += worker->stats.req_4xx;
		stats.req_5xx += worker->stats.req_5xx;

		worker_free(worker);
	}

	ts_end = ev_time();
	duration = ts_end - ts_start;
	sec = duration;
	duration -= sec;
	duration = duration * 1000;
	millisec = duration;
	duration -= millisec;
	microsec = duration * 1000;
	rps = stats.req_done / (ts_end - ts_start);
	kbps = stats.bytes_total / (ts_end - ts_start) / 1024;
	printf("\nfinished in %d sec, %d millisec and %d microsec, %"PRIu64" req/s, %"PRIu64" kbyte/s\n", sec, millisec, microsec, rps, kbps);
	printf("requests: %"PRIu64" total, %"PRIu64" started, %"PRIu64" done, %"PRIu64" succeeded, %"PRIu64" failed, %"PRIu64" errored\n",
		config.req_count, stats.req_started, stats.req_done, stats.req_success, stats.req_failed, stats.req_error
	);
	printf("status codes: %"PRIu64" 2xx, %"PRIu64" 3xx, %"PRIu64" 4xx, %"PRIu64" 5xx\n",
		stats.req_2xx, stats.req_3xx, stats.req_4xx, stats.req_5xx
	);
	printf("traffic: %"PRIu64" bytes total, %"PRIu64" bytes http, %"PRIu64" bytes data\n",
		stats.bytes_total,  stats.bytes_total - stats.bytes_body, stats.bytes_body
	);

	ev_default_destroy();

	free(threads);
	free(workers);
	free(config.request);
	free(host);
	free(headers);
	freeaddrinfo(config.saddr);

	return 0;
}
