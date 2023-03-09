#include <stdio.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>


#include "aws.h"
#include "util.h"
#include "debug.h"
#include "sock_util.h"
#include "w_epoll.h"
#include "http_parser.h"


/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

enum connection_state {
	STATE_DATA_RECEIVED,
	STATE_DATA_SENT,
	STATE_CONNECTION_CLOSED,
	STATE_DATA_SENDING,
	STATE_DATA_RECEIVING,
};

enum file_type {
	STATIC,
	DYNAMIC,
	INVALID
};

/* structure acting as a connection handler */
struct connection
{
	int sockfd;
	int fd;
	enum file_type type;

	/* buffers used for receiving messages and then echoing them back */
	char recv_buffer[BUFSIZ];
	size_t recv_len;
	char send_buffer[BUFSIZ];
	size_t send_len;
	size_t send_bytes_sent;

	/* info about sending process*/
	size_t bytes_sent;
	size_t offset;
	size_t size;

	/*dynamic file fields*/
	io_context_t ctx;
	struct iocb *iocb;
	struct iocb **piocb;
	char **buf;
	int *buf_sent_bytes;
	size_t n_files;
	int efd;
	int read_buf_ready;
	int nr_submitted_read;
	int idx_buf_tosend;
	int last_buf_size;

	enum connection_state state;
};

/*
 * Initialize connection structure on given socket.
 */
static struct connection *connection_create(int sockfd)
{
	struct connection *conn = malloc(sizeof(*conn));

	DIE(conn == NULL, "malloc");
	conn->sockfd = sockfd;
	memset(conn->recv_buffer, 0, BUFSIZ);
	conn->recv_len = 0;
	memset(conn->send_buffer, 0, BUFSIZ);
	conn->send_len = 0;
	conn->send_bytes_sent = 0;

	conn->bytes_sent = 0;
	conn->offset = 0;
	conn->size = 0;

	conn->n_files = 0;
	conn->ctx = 0;
	conn->read_buf_ready = 0;
	conn->nr_submitted_read = 0;
	conn->idx_buf_tosend = 0;
	conn->last_buf_size = BUFSIZ;

	return conn;
}

/*
 * Remove connection handler.
 */
static void connection_remove(struct connection *conn)
{
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

/*
 * Handle a new connection request on the server socket.
 */
static void handle_new_connection(void)
{
	static int sockfd;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;
	int rc;

	/* accept new connection */
	sockfd = accept(listenfd, (SSA *)&addr, &addrlen);
	DIE(sockfd < 0, "accept");

	int flags = fcntl(sockfd, F_GETFL);
	fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

	/* instantiate new connection handler */
	conn = connection_create(sockfd);

	/* add socket to epoll */
	rc = w_epoll_add_ptr_in(epollfd, sockfd, conn);
	DIE(rc < 0, "w_epoll_add_in");
}

/*
 * Send message on socket.
 * Store message in send_buffer in struct connection.
 */
static enum connection_state send_message(struct connection *conn)
{
	ssize_t bytes_sent;
	int rc;
	char abuffer[64];

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	/* send data to socket until all bytes are sent*/
	if (conn->send_bytes_sent < conn->send_len) {

		/* trying to send header to socket*/
		bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_bytes_sent,
							conn->send_len - conn->send_bytes_sent, 0);
		if (bytes_sent < 0) { 
			/* error in communication */
			goto remove_connection;
		}
		conn->send_bytes_sent += bytes_sent;
			conn->state = STATE_DATA_SENDING;
			return STATE_DATA_SENDING;
	}

	/* Send file until all bytes are sent*/
	if (conn->type == STATIC) {

		rc = sendfile(conn->sockfd, conn->fd, (off_t *) &conn->offset,
					 conn->size - conn->offset <= BUFSIZ ? conn->size - conn->offset : BUFSIZ);
		DIE(rc < 0, "sendfile");
		conn->bytes_sent += rc;

		if (conn->size > conn->bytes_sent) {
			/*Still need to send data*/
			conn->state = STATE_DATA_SENDING;
			return STATE_DATA_SENDING;
		}
	} else if (conn->type == DYNAMIC) {
		rc = do_io_async(conn);	
		if (rc == 1)
			return STATE_DATA_SENDING;

 	} else if (conn->type == INVALID) {
		/* remove out notification of the socket from epoll*/
		rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
		DIE(rc < 0, "w_epoll_update_ptr_in");

		conn->state = STATE_DATA_SENT;
		return STATE_DATA_SENT;

	}

	/* remove out notification of the socket from epoll*/
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_update_ptr_in");

	conn->state = STATE_DATA_SENT;
	return STATE_DATA_SENT;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

static http_parser request_parser;
static char request_path[BUFSIZ - 5];	/* storage for request_path */

/*
 * Callback is invoked by HTTP request parser when parsing request path.
 * Request path is stored in global request_path variable.
 */
static int on_path_cb(http_parser *p, const char *buf, size_t len)
{
	assert(p == &request_parser);
	memcpy(request_path, buf, len);

	return 0;
}

/* Use mostly null settings except for on_path callback. */
static http_parser_settings settings_on_path = {
	/* on_message_begin */ 0,
	/* on_header_field */ 0,
	/* on_header_value */ 0,
	/* on_path */ on_path_cb,
	/* on_url */ 0,
	/* on_fragment */ 0,
	/* on_query_string */ 0,
	/* on_body */ 0,
	/* on_headers_complete */ 0,
	/* on_message_complete */ 0
};

/*
 * Handle a client request on a client connection.
 */
static void handle_client_request(struct connection *conn)
{

	int rc;
	enum connection_state ret_state;
	char pathname[BUFSIZ];

	ret_state = receive_message(conn);

	if (ret_state == STATE_CONNECTION_CLOSED || 
		ret_state == STATE_DATA_RECEIVING)
		return;

	http_parser_init(&request_parser, HTTP_REQUEST);
	memset(request_path, 0, BUFSIZ);
	http_parser_execute(&request_parser, &settings_on_path,
	conn->recv_buffer, conn->recv_len);

	/* get file path and open file */
	memset(pathname, 0, BUFSIZ);
	sprintf(pathname, "%s%s", AWS_DOCUMENT_ROOT, request_path + 1);
	conn->fd = open(pathname, O_RDONLY);

	struct stat buf;
	fstat(conn->fd, &buf);
	conn->size = buf.st_size;

	if (conn->fd >= 0) {
		char buff[BUFSIZ] = "HTTP/1.0 200 OK\r\n\r\n";
		conn->send_len = strlen(buff);
		memcpy(conn->send_buffer, buff, strlen(buff));
	} else {
		char buff[BUFSIZ] = "HTTP/1.0 404 Not Found\r\n\r\n";
		conn->send_len = strlen(buff);
		memcpy(conn->send_buffer, buff, strlen(buff));
	}

	
	/* find file type*/
	if (strstr(pathname, "static") != NULL) {
		conn->type = STATIC;

	} else if (strstr(pathname, "dynamic") != NULL) {
		conn->type = DYNAMIC;

	} else if (conn->fd < 0) {
		conn->type = INVALID;
	}

	/* add socket to epoll for out events */
	rc = w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_inout");
}

/*
 * Receive (HTTP) request. Don't parse it, just read data in buffer
 */
static enum connection_state receive_message(struct connection *conn)
{
	ssize_t bytes_recv;
	char abuffer[64];
	int rc;

	rc = get_peer_address(conn->sockfd, abuffer, 64);
	if (rc < 0) {
		ERR("get_peer_address");
		goto remove_connection;
	}

	bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->recv_len,
					 BUFSIZ - conn->recv_len, 0);
	if (bytes_recv < 0) {
		/* error in communication */
		goto remove_connection;
	}
	if (bytes_recv == 0) {	
		/* connection closed */
		goto remove_connection;
	}
	conn->recv_len += bytes_recv;

	/* check if all the bytes were received*/
	char *endptr = conn->recv_buffer + conn->recv_len;
	if (strcmp(endptr - 4, "\r\n\r\n") != 0) {
		conn->state = STATE_DATA_RECEIVING;
		return STATE_DATA_RECEIVING;
	}

	conn->state = STATE_DATA_RECEIVED;
	return STATE_DATA_RECEIVED;

remove_connection:
	rc = w_epoll_remove_ptr(epollfd, conn->sockfd, conn);
	DIE(rc < 0, "w_epoll_remove_ptr");

	/* remove current connection */
	connection_remove(conn);

	return STATE_CONNECTION_CLOSED;
}

/**
 * write data asynchronously (using io_setup(2), io_sumbit(2),
 *	io_getevents(2), io_destroy(2))
 */
static int do_io_async(struct connection *conn)
{
	conn->n_files = conn->size / BUFSIZ;
	if (conn->size % BUFSIZ != 0) {
		conn->n_files++;
		conn->last_buf_size = conn->size % BUFSIZ;
	}

	int n_aio_ops = 0;
	int rc = 0;

	if (!conn->read_buf_ready) {

		conn->efd = eventfd(0, 0);
		DIE(conn->efd < 0, "eventfd");

		rc = io_setup(conn->n_files, &conn->ctx);
		DIE(rc < 0, "io_setup");

		conn->iocb = (struct iocb *) malloc(conn->n_files * sizeof(*(conn->iocb)));
		DIE(conn->iocb == NULL, "malloc");

		conn->piocb = (struct iocb **) malloc(conn->n_files * sizeof(*(conn->piocb)));
		DIE(conn->piocb == NULL, "malloc");

		conn->buf = calloc(conn->n_files, sizeof(char *));
		conn->buf_sent_bytes = calloc(conn->n_files, sizeof(int));

		for (int i = 0; i < conn->n_files; i++) {
			conn->buf[i] = calloc(BUFSIZ, sizeof(char));

			if (i != conn->n_files - 1) {
				io_prep_pread(&conn->iocb[i], conn->fd, conn->buf[i], BUFSIZ, i * BUFSIZ);
			} else {
				io_prep_pread(&conn->iocb[i], conn->fd, conn->buf[i], conn->last_buf_size, i * BUFSIZ);
			}
			io_set_eventfd(&conn->iocb[i], conn->efd);

			conn->piocb[i] = &conn->iocb[i];
		}

		conn->read_buf_ready = 1;
	}

	/* submit all buffers */
	if (conn->nr_submitted_read < conn->n_files) {

		n_aio_ops = io_submit(conn->ctx, conn->n_files, conn->piocb);
		DIE(n_aio_ops < 0, "io_submit");

		conn->nr_submitted_read += n_aio_ops;
		rc = wait_aio(conn, conn->nr_submitted_read);
		return 1;
	}


	if (conn->idx_buf_tosend != conn->n_files) {

		int buf_size;

		if (conn->idx_buf_tosend == conn->n_files - 1) {
			buf_size = conn->last_buf_size;
		} else {
			buf_size = BUFSIZ;
		}

		if (conn->buf_sent_bytes[conn->idx_buf_tosend] < buf_size) {

			ssize_t bytes_sent = send(conn->sockfd,
			conn->buf[conn->idx_buf_tosend] + conn->buf_sent_bytes[conn->idx_buf_tosend],
			buf_size -conn->buf_sent_bytes[conn->idx_buf_tosend], 0);

			conn->buf_sent_bytes[conn->idx_buf_tosend] += bytes_sent;
		} else {
			conn->idx_buf_tosend++;
		}
		return 1;
	}
	
	io_destroy(conn->ctx);

	for (int i = 0; i < conn->n_files; i++) {
		free(conn->buf[i]);
	}
	free(conn->buf_sent_bytes);
	free(conn->buf);
	free(conn->iocb);
	free(conn->piocb);

	return 0;

}

/**
 * Wait for asynchronous I/O operations
 * (eventfd or io_getevents)
 */
static int wait_aio(struct connection *conn, int nops)
{
	struct io_event *events;
	int rc;

	events = (struct io_event *) malloc(nops * sizeof(struct io_event));
	DIE(events == NULL, "malloc");

	rc = io_getevents(conn->ctx, nops, nops, events, NULL);
	if (rc < 0) {
		ERR("io_getevents");
	}
	free(events);

	return rc;
}


int main(void)
{

	int rc;

	/* init multiplexing */
	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	/* create server socket */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT,
								   DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	/* add server socket to poll*/
	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* wait for events */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");

		/*
		 * switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
		struct connection *conn = (struct connection *) rev.data.ptr;
		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN)
				handle_new_connection();
		}
		else {
			if (rev.events & EPOLLIN) {
				handle_client_request(conn);
			}
			if (rev.events & EPOLLOUT) {
				send_message(conn);
				if (conn->state == STATE_DATA_SENT) {
					close(conn->fd);
					connection_remove(conn);
				}
			}
		}
	}
	return 0;
}