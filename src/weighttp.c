/*
 * weighttp - a lightweight and simple webserver benchmarking tool
 *
 * Author:
 *     Copyright (c) 2009-2011 Thomas Porzelt
 *
 * License:
 *     MIT, see COPYING file
 */
#include <limits.h>
#include <math.h>
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

	*host = NULL;
	*port = 0;

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
		len += strlen(headers[i]) + strlen("\r\n");
		if (strncmp(headers[i], "User-Agent: ", sizeof("User-Agent: ")-1) == 0)
			have_user_agent = 1;
	}

	if (!have_user_agent)
		len += strlen("User-Agent: weighttp/" VERSION "\r\n");

	req = W_MALLOC(char, len);

	strcpy(req, "GET ");
	strcat(req, url);
	strcat(req, " HTTP/1.1\r\nHost: ");
	strcat(req, *host);
	if (*port != 80)
		sprintf(req + strlen(req), ":%"PRIu16, *port);

	strcat(req, "\r\n");

	if (!have_user_agent)
		sprintf(req + strlen(req), "User-Agent: weighttp/" VERSION "\r\n");

	for (i = 0; i < headers_num; i++) {
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

static int compradre(struct Times * a, struct Times * b)
{
  if ((a->ctime) < (b->ctime))
    return -1;
  if ((a->ctime) > (b->ctime))
    return +1;
  return 0;
}

static int comprando(struct Times * a, struct Times * b)
{
  if ((a->time) < (b->time))
    return -1;
  if ((a->time) > (b->time))
    return +1;
  return 0;
}

static int compri(struct Times * a, struct Times * b)
{
  ev_tstamp p = a->time - a->ctime;
  ev_tstamp q = b->time - b->ctime;
  if (p < q)
    return -1;
  if (p > q)
    return +1;
  return 0;
}

static int compwait(struct Times * a, struct Times * b)
{
  if ((a->waittime) < (b->waittime))
    return -1;
  if ((a->waittime) > (b->waittime))
    return 1;
  return 0;
}

static void output_results(uint64_t done, struct Times * stats) {
  /* work out connection times */
  int i;
  ev_tstamp totalcon = 0, total = 0, totald = 0, totalwait = 0;
  ev_tstamp meancon, meantot, meand, meanwait;
  ev_tstamp mincon = ULONG_MAX, mintot = ULONG_MAX, mind = ULONG_MAX,
            minwait = ULONG_MAX;
  ev_tstamp maxcon = 0, maxtot = 0, maxd = 0, maxwait = 0;
  ev_tstamp mediancon = 0, mediantot = 0, mediand = 0, medianwait = 0;
  ev_tstamp sdtot = 0, sdcon = 0, sdd = 0, sdwait = 0;

  for (i = 0; i < done; i++) {
    struct Times *s = &stats[i];
    mincon = ap_min(mincon, s->ctime);
    mintot = ap_min(mintot, s->time);
    mind = ap_min(mind, s->time - s->ctime);
    minwait = ap_min(minwait, s->waittime);

    maxcon = ap_max(maxcon, s->ctime);
    maxtot = ap_max(maxtot, s->time);
    maxd = ap_max(maxd, s->time - s->ctime);
    maxwait = ap_max(maxwait, s->waittime);

    totalcon += s->ctime;
    total += s->time;
    totald += s->time - s->ctime;
    totalwait += s->waittime;
  }
  meancon = totalcon / done;
  meantot = total / done;
  meand = totald / done;
  meanwait = totalwait / done;

  /* calculating the sample variance: the sum of the squared deviations, divided by n-1 */
  for (i = 0; i < done; i++) {
    struct Times *s = &stats[i];
    ev_tstamp a;
    a = (s->time - meantot);
    sdtot += a * a;
    a = (s->ctime - meancon);
    sdcon += a * a;
    a = (s->time - s->ctime - meand);
    sdd += a * a;
    a = (s->waittime - meanwait);
    sdwait += a * a;
  }

  sdtot = (done > 1) ? sqrt(sdtot / (done - 1)) : 0;
  sdcon = (done > 1) ? sqrt(sdcon / (done - 1)) : 0;
  sdd = (done > 1) ? sqrt(sdd / (done - 1)) : 0;
  sdwait = (done > 1) ? sqrt(sdwait / (done - 1)) : 0;

  /*
   * XXX: what is better; this hideous cast of the compradre function; or
   * the four warnings during compile ? dirkx just does not know and
   * hates both/
   */
  qsort(stats, done, sizeof(struct Times),
      (int (*)(const void *, const void *))compradre);
  if ((done > 1) && (done % 2))
    mediancon = (stats[done / 2].ctime + stats[done / 2 + 1].ctime) / 2;
  else
    mediancon = stats[done / 2].ctime;

  qsort(stats, done, sizeof(struct Times),
      (int (*)(const void *, const void *))compri);
  if ((done > 1) && (done % 2))
    mediand = (stats[done / 2].time + stats[done / 2 + 1].time
        - stats[done / 2].ctime - stats[done / 2 + 1].ctime) / 2;
  else
    mediand = stats[done / 2].time - stats[done / 2].ctime;

  qsort(stats, done, sizeof(struct Times),
      (int (*)(const void *, const void *))compwait);
  if ((done > 1) && (done % 2))
    medianwait = (stats[done / 2].waittime + stats[done / 2 + 1].waittime) / 2;
  else
    medianwait = stats[done / 2].waittime;

  qsort(stats, done, sizeof(struct Times),
      (int (*)(const void *, const void *))comprando);
  if ((done > 1) && (done % 2))
    mediantot = (stats[done / 2].time + stats[done / 2 + 1].time) / 2;
  else
    mediantot = stats[done / 2].time;

  printf("\nConnection Times (ms)\n");
  /*
   * Reduce stats from apr time to milliseconds
   */
  mincon = ap_double_ms(mincon);
  mind = ap_double_ms(mind);
  minwait = ap_double_ms(minwait);
  mintot = ap_double_ms(mintot);
  meancon = ap_double_ms(meancon);
  meand = ap_double_ms(meand);
  meanwait = ap_double_ms(meanwait);
  meantot = ap_double_ms(meantot);
  mediancon = ap_double_ms(mediancon);
  mediand = ap_double_ms(mediand);
  medianwait = ap_double_ms(medianwait);
  mediantot = ap_double_ms(mediantot);
  maxcon = ap_double_ms(maxcon);
  maxd = ap_double_ms(maxd);
  maxwait = ap_double_ms(maxwait);
  maxtot = ap_double_ms(maxtot);
  sdcon = ap_double_ms(sdcon);
  sdd = ap_double_ms(sdd);
  sdwait = ap_double_ms(sdwait);
  sdtot = ap_double_ms(sdtot);

#define CONF_FMT_STRING "%5.3f %4.3f %5.1f %6.3f %7.3f\n"
#define     SANE(what,mean,median,sd) \
              { \
                double d = (double)mean - median; \
                if (d < 0) d = -d; \
                if (d > 2 * sd ) \
                    printf("ERROR: The median and mean for " what " are more than twice the standard\n" \
                           "       deviation apart. These results are NOT reliable.\n"); \
                else if (d > sd ) \
                    printf("WARNING: The median and mean for " what " are not within a normal deviation\n" \
                           "        These results are probably not that reliable.\n"); \
            }

  printf("              min  mean[+/-sd] median   max\n");
  printf("Connect:    " CONF_FMT_STRING,
      mincon, meancon, sdcon, mediancon, maxcon);
  printf("Processing: " CONF_FMT_STRING,
      mind, meand, sdd, mediand, maxd);
  printf("Waiting:    " CONF_FMT_STRING,
      minwait, meanwait, sdwait, medianwait, maxwait);
  printf("Total:      " CONF_FMT_STRING,
      mintot, meantot, sdtot, mediantot, maxtot);
  SANE("the initial connection time", meancon, mediancon, sdcon);
  SANE("the processing time", meand, mediand, sdd);
  SANE("the waiting time", meanwait, medianwait, sdwait);
  SANE("the total time", meantot, mediantot, sdtot);

  /* Sorted on total connect times */
  if ((done > 1)) {
    int percs[] = {50, 66, 75, 80, 90, 95, 98, 99, 100};
    printf("\nPercentage of the requests served within a certain time (ms)\n");
    for (i = 0; i < sizeof(percs) / sizeof(int); i++) {
      if (percs[i] <= 0)
        printf(" 0%%  <0> (never)\n");
      else if (percs[i] >= 100)
        printf(" 100%%  %5.3f (longest request)\n",
            ap_double_ms(stats[done - 1].time));
      else
        printf("  %d%%  %5.3f\n", percs[i],
            ap_double_ms(stats[(int)(done * percs[i] / 100)].time));
    }
  }
}

int main(int argc, char *argv[]) {
	Worker **workers;
	pthread_t *threads;
	int i;
	char c;
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
	struct Times * times;
	uint64_t times_offet;

	printf("weighttp - a lightweight and simple webserver benchmarking tool\n\n");

	headers = NULL;
	headers_num = 0;

	/* default settings */
	use_ipv6 = 0;
	config.thread_count = 1;
	config.concur_count = 1;
	config.req_count = 0;
	config.keep_alive = 0;

	while ((c = getopt(argc, argv, ":hv6kn:t:c:H:")) != -1) {
		switch (c) {
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
				W_ERROR("unkown option: -%c", optopt);
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

	times = W_MALLOC(struct Times, config.req_count);
	/* spawn threads */
	threads = W_MALLOC(pthread_t, config.thread_count);
	workers = W_MALLOC(Worker*, config.thread_count);

	rest_concur = config.concur_count % config.thread_count;
	rest_req = config.req_count % config.thread_count;

	printf("starting benchmark...\n");

	memset(&stats, 0, sizeof(stats));
	ts_start = ev_time();

	times_offet = 0;
	for (i = 0; i < config.thread_count; i++) {
		uint64_t reqs = config.req_count / config.thread_count;
		uint16_t concur = config.concur_count / config.thread_count;
		uint64_t diff;

		if (rest_concur) {
			diff = (i == config.thread_count) ? rest_concur : (rest_concur / config.thread_count);
			diff = diff ? diff : 1;
			concur += diff;
			rest_concur -= diff;
		}

		if (rest_req) {
			diff = (i == config.thread_count) ? rest_req : (rest_req / config.thread_count);
			diff = diff ? diff : 1;
			reqs += diff;
			rest_req -= diff;
		}
		printf("spawning thread #%d: %"PRIu16" concurrent requests, %"PRIu64" total requests\n", i+1, concur, reqs);
		workers[i] = worker_new(i+1, &config, concur, reqs, times + times_offet);
		times_offet += reqs;

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
	if (stats.req_done > 0)
	  output_results(stats.req_done, times);

	ev_default_destroy();

	free(threads);
	free(workers);
	free(config.request);
	free(host);
	free(headers);
	freeaddrinfo(config.saddr);

	return 0;
}
