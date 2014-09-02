#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <asterisk.h>
#include <asterisk/lock.h>
#include <asterisk/utils.h>

#include "sccp_queue.h"

static struct queue_item_container *container_alloc(size_t item_size)
{
	return ast_calloc(1, sizeof(struct queue_item_container) + item_size);
}

static void container_destroy(struct queue_item_container *container)
{
	ast_free(container);
}

static void container_write_item(struct queue_item_container *container, size_t item_size, void *item)
{
	memcpy(container->item, item, item_size);
}

static void container_read_item(struct queue_item_container *container, size_t item_size, void *item)
{
	memcpy(item, container->item, item_size);
}

int sccp_queue_init(struct sccp_queue *q, size_t item_size)
{
	if (!item_size) {
		return SCCP_QUEUE_INVAL;
	}

	AST_LIST_HEAD_INIT_NOLOCK(&q->containers);
	q->item_size = item_size;

	return 0;
}

void sccp_queue_destroy(struct sccp_queue *q)
{
	struct queue_item_container *container;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&q->containers, container, list) {
		AST_LIST_REMOVE_CURRENT(list);
		container_destroy(container);
	}
	AST_LIST_TRAVERSE_SAFE_END;
}

int sccp_queue_put(struct sccp_queue *q, void *item)
{
	struct queue_item_container *container;

	container = container_alloc(q->item_size);
	if (!container) {
		return -1;
	}

	AST_LIST_INSERT_TAIL(&q->containers, container, list);

	container_write_item(container, q->item_size, item);

	return 0;
}

int sccp_queue_get(struct sccp_queue *q, void *item)
{
	struct queue_item_container *container;

	container = AST_LIST_FIRST(&q->containers);
	if (!container) {
		return -1;
	}

	AST_LIST_REMOVE_HEAD(&q->containers, list);
	container_read_item(container, q->item_size, item);
	container_destroy(container);

	return 0;
}

int sccp_queue_move(struct sccp_queue *dest, struct sccp_queue *src)
{
	if (!dest || !src) {
		return SCCP_QUEUE_INVAL;
	}

	dest->containers = src->containers;
	dest->item_size = src->item_size;

	AST_LIST_HEAD_INIT_NOLOCK(&src->containers);

	return 0;
}

int sccp_queue_empty(const struct sccp_queue *q)
{
	return AST_LIST_EMPTY(&q->containers);
}

struct sccp_sync_queue {
	ast_mutex_t lock;
	struct sccp_queue q;
	int pipefd[2];
	int closed;
};

struct sccp_sync_queue *sccp_sync_queue_create(size_t item_size)
{
	struct sccp_sync_queue *sync_q;

	sync_q = ast_calloc(1, sizeof(*sync_q));
	if (!sync_q) {
		return NULL;
	}

	if (pipe2(sync_q->pipefd, O_NONBLOCK | O_CLOEXEC) == -1) {
		ast_log(LOG_ERROR, "sccp sync queue create failed: pipe: %s\n", strerror(errno));
		ast_free(sync_q);
		return NULL;
	}

	ast_mutex_init(&sync_q->lock);
	sccp_queue_init(&sync_q->q, item_size);
	sync_q->closed = 0;

	return sync_q;
}

void sccp_sync_queue_destroy(struct sccp_sync_queue *sync_q)
{
	ast_mutex_destroy(&sync_q->lock);
	sccp_queue_destroy(&sync_q->q);
	close(sync_q->pipefd[0]);
	close(sync_q->pipefd[1]);
	ast_free(sync_q);
}

int sccp_sync_queue_fd(struct sccp_sync_queue *sync_q)
{
	return sync_q->pipefd[0];
}

void sccp_sync_queue_close(struct sccp_sync_queue *sync_q)
{
	ast_mutex_lock(&sync_q->lock);
	sync_q->closed = 1;
	ast_mutex_unlock(&sync_q->lock);
}

static int sccp_sync_queue_signal_fd(struct sccp_sync_queue *sync_q)
{
	static const char pipeval = 0xF0;
	ssize_t n;

	n = write(sync_q->pipefd[1], &pipeval, sizeof(pipeval));

	switch (n) {
	case -1:
		ast_log(LOG_ERROR, "sccp sync queue signal fd failed: write: %s\n", strerror(errno));
		return -1;
	case 0:
		ast_log(LOG_ERROR, "sccp sync queue signal fd failed: write wrote nothing\n");
		return -1;
	}

	return 0;
}

static int sccp_sync_queue_clear_fd(struct sccp_sync_queue *sync_q)
{
	ssize_t n;
	char buf[8];

	n = read(sync_q->pipefd[0], buf, sizeof(buf));

	switch (n) {
	case -1:
		ast_log(LOG_ERROR, "sccp sync queue clear fd failed: read: %s\n", strerror(errno));
		return -1;
	case 0:
		ast_log(LOG_ERROR, "sccp sync queue clear fd failed: end of file reached\n");
		return -1;
	}

	return 0;
}

static int sccp_sync_queue_put_no_lock(struct sccp_sync_queue *sync_q, void *item)
{
	if (sync_q->closed) {
		return SCCP_QUEUE_CLOSED;
	}

	if (sccp_queue_empty(&sync_q->q)) {
		if (sccp_sync_queue_signal_fd(sync_q)) {
			ast_log(LOG_ERROR, "sccp sync queue put failed: could not write to pipe\n");
			return -1;
		}
	}

	if (sccp_queue_put(&sync_q->q, item)) {
		ast_log(LOG_ERROR, "sccp sync queue put failed: could not queue item\n");
		return -1;
	}

	return 0;
}

int sccp_sync_queue_put(struct sccp_sync_queue *sync_q, void *item)
{
	int ret;

	ast_mutex_lock(&sync_q->lock);
	ret = sccp_sync_queue_put_no_lock(sync_q, item);
	ast_mutex_unlock(&sync_q->lock);

	return ret;
}

static int sccp_sync_queue_get_no_lock(struct sccp_sync_queue *sync_q, void *item)
{
	if (sccp_queue_get(&sync_q->q, item)) {
		return SCCP_QUEUE_EMPTY;
	}

	if (sccp_queue_empty(&sync_q->q)) {
		sccp_sync_queue_clear_fd(sync_q);
	}

	return 0;
}

int sccp_sync_queue_get(struct sccp_sync_queue *sync_q, void *item)
{
	int ret;

	ast_mutex_lock(&sync_q->lock);
	ret = sccp_sync_queue_get_no_lock(sync_q, item);
	ast_mutex_unlock(&sync_q->lock);

	return ret;
}

static void sccp_sync_queue_get_all_no_lock(struct sccp_sync_queue *sync_q, struct sccp_queue *ret)
{
	sccp_queue_move(ret, &sync_q->q);
	if (!sccp_queue_empty(ret)) {
		sccp_sync_queue_clear_fd(sync_q);
	}
}

int sccp_sync_queue_get_all(struct sccp_sync_queue *sync_q, struct sccp_queue *ret)
{
	if (!ret) {
		ast_log(LOG_ERROR, "sccp sync queue get all failed: ret is null\n");
		return SCCP_QUEUE_INVAL;
	}

	ast_mutex_lock(&sync_q->lock);
	sccp_sync_queue_get_all_no_lock(sync_q, ret);
	ast_mutex_unlock(&sync_q->lock);

	return 0;
}
