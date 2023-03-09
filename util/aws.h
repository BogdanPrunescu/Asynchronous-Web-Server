/*
 * Asynchronous Web Server - header file (macros and structures)
 *
 * 2011-2017, Operating Systems
 */

#ifndef AWS_H_
#define AWS_H_		1

#ifdef __cplusplus
extern "C" {
#endif

#define AWS_LISTEN_PORT		8888
#define AWS_DOCUMENT_ROOT	"./"
#define AWS_REL_STATIC_FOLDER	"static/"
#define AWS_REL_DYNAMIC_FOLDER	"dynamic/"
#define AWS_ABS_STATIC_FOLDER	(AWS_DOCUMENT_ROOT AWS_REL_STATIC_FOLDER)
#define AWS_ABS_DYNAMIC_FOLDER	(AWS_DOCUMENT_ROOT AWS_REL_DYNAMIC_FOLDER)

static struct connection *connection_create(int sockfd);
static void connection_remove(struct connection *conn);
static void handle_new_connection(void);
static enum connection_state send_message(struct connection *conn);
static void handle_client_request(struct connection *conn);
static enum connection_state receive_message(struct connection *conn);
static int do_io_async(struct connection *conn);
static int wait_aio(struct connection *conn, int nops);

#ifdef __cplusplus
}
#endif

#endif /* AWS_H_ */
