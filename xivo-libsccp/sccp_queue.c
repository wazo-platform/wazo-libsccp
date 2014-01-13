#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <asterisk.h>
#include <asterisk/linkedlists.h>
#include <asterisk/utils.h>

#include "sccp_queue.h"

struct sccp_queue {
	AST_LIST_HEAD_NOLOCK(, msg) msgs;
	size_t msg_size;
	int pipefd[2];
};

struct msg {
	AST_LIST_ENTRY(msg) list;
	void *data[0];
};

static struct msg *msg_create(size_t data_size, void *data)
{
	struct msg *msg;

	msg = ast_calloc(1, sizeof(*msg) + data_size);
	if (!msg) {
		return NULL;
	}

	memcpy(msg->data, data, data_size);

	return msg;
}

static void msg_extract_data(struct msg *msg, size_t data_size, void *data)
{
	memcpy(data, msg->data, data_size);
}

static void msg_free(struct msg *msg) {
	ast_free(msg);
}

struct sccp_queue *sccp_queue_create(size_t msg_size)
{
	struct sccp_queue *queue;

	queue = ast_calloc(1, sizeof(*queue));
	if (!queue) {
		return NULL;
	}

	if (pipe2(queue->pipefd, O_NONBLOCK) == -1) {
		ast_log(LOG_ERROR, "sccp queue create failed: pipe: %s\n", strerror(errno));
		ast_free(queue);
		return NULL;
	}

	queue->msg_size = msg_size;

	return queue;
}

void sccp_queue_destroy(struct sccp_queue *queue)
{
	struct msg *msg;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&queue->msgs, msg, list) {
		AST_LIST_REMOVE_CURRENT(list);
		msg_free(msg);
	}
	AST_LIST_TRAVERSE_SAFE_END;

	close(queue->pipefd[0]);
	close(queue->pipefd[1]);
	ast_free(queue);
}

int sccp_queue_fd(struct sccp_queue *queue)
{
	return queue->pipefd[0];
}

static int sccp_queue_write_pipe(struct sccp_queue *queue)
{
	static const char pipeval = 0xF0;
	ssize_t n;

	n = write(queue->pipefd[1], &pipeval, sizeof(pipeval));

	switch (n) {
	case -1:
		ast_log(LOG_ERROR, "sccp queue write pipe failed: write: %s\n", strerror(errno));
		return -1;
	case 0:
		ast_log(LOG_ERROR, "sccp queue write pipe failed: write wrote nothing\n");
		return -1;
	}

	return 0;
}

static int sccp_queue_read_pipe(struct sccp_queue *queue)
{
	char buf[8];
	ssize_t n;

	n = read(queue->pipefd[0], buf, sizeof(buf));

	switch (n) {
	case -1:
		ast_log(LOG_ERROR, "sccp queue read pipe failed: read: %s\n", strerror(errno));
		return -1;
	case 0:
		ast_log(LOG_ERROR, "sccp queue read pipe failed: end of file reached\n");
		return -1;
	}

	return 0;
}

int sccp_queue_put(struct sccp_queue *queue, void *msg_data)
{
	struct msg *msg;

	msg = msg_create(queue->msg_size, msg_data);
	if (!msg) {
		return -1;
	}

	if (AST_LIST_EMPTY(&queue->msgs)) {
		if (sccp_queue_write_pipe(queue)) {
			msg_free(msg);
			return -1;
		}
	}

	AST_LIST_INSERT_TAIL(&queue->msgs, msg, list);

	return 0;
}

int sccp_queue_get(struct sccp_queue *queue, void *msg_data)
{
	struct msg *msg;

	msg = AST_LIST_FIRST(&queue->msgs);
	if (!msg) {
		ast_log(LOG_WARNING, "sccp queue get failed: queue is empty\n");
		return -1;
	}

	AST_LIST_REMOVE_HEAD(&queue->msgs, list);
	msg_extract_data(msg, queue->msg_size, msg_data);
	msg_free(msg);

	if (AST_LIST_EMPTY(&queue->msgs)) {
		sccp_queue_read_pipe(queue);
	}

	return 0;
}

int sccp_queue_is_empty(struct sccp_queue *queue)
{
	return AST_LIST_EMPTY(&queue->msgs);
}
