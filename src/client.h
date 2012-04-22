/*
 * weighttp - a lightweight and simple webserver benchmarking tool
 *
 * Author:
 *     Copyright (c) 2009-2011 Thomas Porzelt
 *
 * License:
 *     MIT, see COPYING file
 */

struct Client {
	enum {
		CLIENT_START,
		CLIENT_CONNECTING,
		CLIENT_WRITING,
		CLIENT_READING,
		CLIENT_ERROR,
		CLIENT_END
	} state;

	enum {
		PARSER_START,
		PARSER_HEADER,
		PARSER_BODY
	} parser_state;

	Worker *worker;
	ev_io sock_watcher;
	uint32_t buffer_offset;
	uint32_t parser_offset;
	uint32_t request_offset;
	ev_tstamp ts_start; /* Start of connection */
	ev_tstamp ts_connect; /* Connected, start writing */
	ev_tstamp ts_endwrite; /* Request written */
	ev_tstamp ts_beginread; /* First byte of input */
	ev_tstamp ts_end; /* Connection closed */
	uint8_t keepalive;
	uint8_t success;
	uint8_t status_success;
	uint8_t chunked;
	int64_t chunk_size;
	int64_t chunk_received;
	int64_t content_length;
	uint64_t bytes_received; /* including http header */
	uint16_t header_size;

	char buffer[CLIENT_BUFFER_SIZE];
};

Client *client_new(Worker *worker);
void client_free(Client *client);
void client_state_machine(Client *client);
