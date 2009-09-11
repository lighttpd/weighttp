/*
 * weighttp - a lightweight and simple webserver benchmarking tool
 *
 * Author:
 *     Copyright (c) 2009 Thomas Porzelt
 *
 * License:
 *     MIT, see COPYING file
 */

#include "weighttp.h"

static uint8_t client_parse(Client *client);
static void client_io_cb(struct ev_loop *loop, ev_io *w, int revents);
static void client_set_events(Client *client, int events);
/*
static void client_add_events(Client *client, int events);
static void client_rem_events(Client *client, int events);

static void client_add_events(Client *client, int events) {
	struct ev_loop *loop = client->worker->loop;
	ev_io *watcher = &client->sock_watcher;

	if ((watcher->events & events) == events)
		return;

	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, watcher->events | events);
	ev_io_start(loop, watcher);
}

static void client_rem_events(Client *client, int events) {
	struct ev_loop *loop = client->worker->loop;
	ev_io *watcher = &client->sock_watcher;

	if (0 == (watcher->events & events))
		return;

	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, watcher->events & ~events);
	ev_io_start(loop, watcher);
}
*/

static void client_set_events(Client *client, int events) {
	struct ev_loop *loop = client->worker->loop;
	ev_io *watcher = &client->sock_watcher;

	if (events == (watcher->events & (EV_READ | EV_WRITE)))
		return;

	ev_io_stop(loop, watcher);
	ev_io_set(watcher, watcher->fd, (watcher->events & ~(EV_READ | EV_WRITE)) | events);
	ev_io_start(loop, watcher);
}

Client *client_new(Worker *worker) {
	Client *client;

	client = W_MALLOC(Client, 1);
	client->state = CLIENT_START;
	client->worker = worker;
	client->sock_watcher.fd = -1;
	client->sock_watcher.data = client;
	client->content_length = -1;
	client->buffer_offset = 0;
	client->request_offset = 0;
	client->keepalive = client->worker->config->keep_alive;

	return client;
}

void client_free(Client *client) {
	if (client->sock_watcher.fd != -1) {
		ev_io_stop(client->worker->loop, &client->sock_watcher);
		shutdown(client->sock_watcher.fd, SHUT_WR);
		close(client->sock_watcher.fd);
	}

	free(client);
}

static void client_reset(Client *client) {
	//printf("keep alive: %d\n", client->keepalive);
	if (!client->keepalive) {
		if (client->sock_watcher.fd != -1) {
			ev_io_stop(client->worker->loop, &client->sock_watcher);
			shutdown(client->sock_watcher.fd, SHUT_WR);
			close(client->sock_watcher.fd);
			client->sock_watcher.fd = -1;
		}

		client->state = CLIENT_START;
	} else {
		client_set_events(client, EV_WRITE);
		client->state = CLIENT_WRITING;
		client->worker->stats.req_started++;
	}

	client->parser_state = PARSER_START;
	client->buffer_offset = 0;
	client->parser_offset = 0;
	client->request_offset = 0;
	client->ts_start = 0;
	client->ts_end = 0;
	client->status_200 = 0;
	client->success = 0;
	client->content_length = -1;
	client->bytes_received = 0;
	client->header_size = 0;
	client->keepalive = client->worker->config->keep_alive;
}

static uint8_t client_connect(Client *client) {
	//printf("connecting...\n");
	start:

	if (-1 == connect(client->sock_watcher.fd, client->worker->config->saddr->ai_addr, client->worker->config->saddr->ai_addrlen)) {
		switch (errno) {
			case EINPROGRESS:
			case EALREADY:
				/* async connect now in progress */
				client->state = CLIENT_CONNECTING;
				return 1;
			case EISCONN:
				break;
			case EINTR:
				goto start;
			default:
			{
				strerror_r(errno, client->buffer, sizeof(client->buffer));
				W_ERROR("connect() failed: %s (%d)", client->buffer, errno);
				return 0;
			}
		}
	}

	/* successfully connected */
	client->state = CLIENT_WRITING;
	return 1;
}

static void client_io_cb(struct ev_loop *loop, ev_io *w, int revents) {
	Client *client = w->data;

	UNUSED(loop);
	UNUSED(revents);

	client_state_machine(client);
}

void client_state_machine(Client *client) {
	int r;
	Config *config = client->worker->config;

	start:
	//printf("state: %d\n", client->state);
	switch (client->state) {
		case CLIENT_START:
			client->worker->stats.req_started++;

			do {
				r = socket(config->saddr->ai_family, config->saddr->ai_socktype, config->saddr->ai_protocol);
			} while (-1 == r && errno == EINTR);

			if (-1 == r) {
				client->state = CLIENT_ERROR;
				strerror_r(errno, client->buffer, sizeof(client->buffer));
				W_ERROR("socket() failed: %s (%d)", client->buffer, errno);
				goto start;
			}

			/* set non-blocking */
			fcntl(r, F_SETFL, O_NONBLOCK | O_RDWR);

			ev_init(&client->sock_watcher, client_io_cb);
			ev_io_set(&client->sock_watcher, r, EV_WRITE);
			ev_io_start(client->worker->loop, &client->sock_watcher);

			if (!client_connect(client)) {
				client->state = CLIENT_ERROR;
				goto start;
			} else {
				client_set_events(client, EV_WRITE);
				return;
			}
		case CLIENT_CONNECTING:
			if (!client_connect(client)) {
				client->state = CLIENT_ERROR;
				goto start;
			}
		case CLIENT_WRITING:
			while (1) {
				r = write(client->sock_watcher.fd, &config->request[client->request_offset], config->request_size - client->request_offset);
				//printf("write(%d - %d = %d): %d\n", config->request_size, client->request_offset, config->request_size - client->request_offset, r);
				if (r == -1) {
					/* error */
					if (errno == EINTR)
						continue;
					strerror_r(errno, client->buffer, sizeof(client->buffer));
					W_ERROR("write() failed: %s (%d)", client->buffer, errno);
					client->state = CLIENT_ERROR;
					goto start;
				} else if (r != 0) {
					/* success */
					client->request_offset += r;
					if (client->request_offset == config->request_size) {
						/* whole request was sent, start reading */
						client->state = CLIENT_READING;
						client_set_events(client, EV_READ);
					}

					return;
				} else {
					/* disconnect */
					client->state = CLIENT_END;
					goto start;
				}
			}
		case CLIENT_READING:
			while (1) {
				r = read(client->sock_watcher.fd, &client->buffer[client->buffer_offset], sizeof(client->buffer) - client->buffer_offset);
				//printf("read(): %d\n", r);
				if (r == -1) {
					/* error */
					if (errno == EINTR)
						continue;
					strerror_r(errno, client->buffer, sizeof(client->buffer));
					W_ERROR("read() failed: %s (%d)", client->buffer, errno);
					client->state = CLIENT_ERROR;
				} else if (r != 0) {
					/* success */
					client->bytes_received += r;
					client->buffer_offset += r;
					client->worker->stats.bytes_total += r;

					if (client->buffer_offset >= sizeof(client->buffer)) {
						/* too big response header */
						client->state = CLIENT_ERROR;
						break;
					}
					client->buffer[client->buffer_offset] = '\0';
					//printf("buffer:\n==========\n%s\n==========\n", client->buffer);
					if (!client_parse(client)) {
						client->state = CLIENT_ERROR;
						//printf("parser failed\n");
						break;
					} else {
						if (client->state == CLIENT_END)
							goto start;
						else
							break;
					}
				} else {
					/* disconnect */
					client->state = CLIENT_ERROR;
					break;
				}
			}

		case CLIENT_ERROR:
			//printf("client error\n");
			client->worker->stats.req_error++;
			client->keepalive = 0;
			client->success = 0;
			client->state = CLIENT_END;
		case CLIENT_END:
			/* update worker stats */
			client->worker->stats.req_done++;

			if (client->success) {
				client->worker->stats.req_success++;
				client->worker->stats.bytes_body += client->bytes_received - client->header_size;
			} else {
				client->worker->stats.req_failed++;
			}

			/*if (client->worker->id == 1 && (client->worker->stats.req_started % 10) == 0)
				printf("thread: %d, requests done: %"PRIu64", %"PRIu64" todo, %"PRIu64" started\n",
					client->worker->id, client->worker->stats.req_done, client->worker->stats.req_todo,
					client->worker->stats.req_started);*/

			if (client->worker->stats.req_started == client->worker->stats.req_todo) {
				/* this worker has started all requests */
				client->keepalive = 0;
				client_reset(client);

				if (client->worker->stats.req_done == client->worker->stats.req_todo) {
					/* this worker has finished all requests */
					ev_unref(client->worker->loop);
				}
			} else {
				client_reset(client);
				goto start;
			}
	}
}


static uint8_t client_parse(Client *client) {
	char *end, *str;

	switch (client->parser_state) {
		case PARSER_START:
			//printf("parse (START):\n%s\n", &client->buffer[client->parser_offset]);
			/* look for HTTP/1.1 200 OK */
			if (client->buffer_offset < sizeof("HTTP/1.1 200 OK\r\n"))
				return 1;

			if (strncmp(client->buffer, "HTTP/1.1 200 OK\r\n", sizeof("HTTP/1.1 200 OK\r\n")-1) == 0) {
				client->status_200 = 1;
				client->parser_offset = sizeof("HTTP/1.1 200 ok\r\n") - 1;
			} else {
				client->status_200 = 0;
				end = strchr(client->buffer, '\r');

				if (!end || *(end+1) != '\n')
					return 0;

				client->parser_offset = end + 2 - client->buffer;
			}

			client->parser_state = PARSER_HEADER;
		case PARSER_HEADER:
			//printf("parse (HEADER)\n");
			/* look for Content-Length and Connection header */
			while (NULL != (end = strchr(&client->buffer[client->parser_offset], '\r'))) {
				if (*(end+1) != '\n')
					return 0;

				if (end == &client->buffer[client->parser_offset]) {
					/* body reached */
					client->parser_state = PARSER_BODY;
					client->header_size = end + 2 - client->buffer;
					//printf("body reached\n");

					return client_parse(client);
				}

				*end = '\0';
				str = &client->buffer[client->parser_offset];
				//printf("checking header: '%s'\n", str);

				if (strncmp(str, "Content-Length: ", sizeof("Content-Length: ")-1) == 0) {
					/* content length header */
					client->content_length = atoi(str + sizeof("Content-Length: ") - 1);
				} else if (strncmp(str, "Connection: ", sizeof("Connection: ")-1) == 0) {
					/* connection header */
					str += sizeof("Connection: ") - 1;

					if (strncmp(str, "close", sizeof("close")-1) == 0)
						client->keepalive = 0;
					else if (strncmp(str, "Keep-Alive", sizeof("Keep-Alive")-1) == 0)
						client->keepalive = client->worker->config->keep_alive;
					else if (strncmp(str, "keep-alive", sizeof("keep-alive")-1) == 0)
						client->keepalive = client->worker->config->keep_alive;
					else
						return 0;
				}

				if (*(end+2) == '\r' && *(end+3) == '\n') {
					/* body reached */
					client->parser_state = PARSER_BODY;
					client->header_size = end + 4 - client->buffer;
					//printf("body reached\n");

					return client_parse(client);
				}

				client->parser_offset = end - client->buffer + 2;
			}

			return 1;
		case PARSER_BODY:
			//printf("parse (BODY)\n");
			/* do nothing, just consume the data */
			/*printf("content-l: %"PRIu64", header: %d, recevied: %"PRIu64"\n",
			client->content_length, client->header_size, client->bytes_received);
			client->buffer_offset = 0;*/

			if (client->content_length == -1)
				return 0;

			if (client->bytes_received == (uint64_t) (client->header_size + client->content_length)) {
				/* full response received */
				client->state = CLIENT_END;
				client->success = client->status_200 ? 1 : 0;
			}

			return 1;
	}

	return 1;
}
