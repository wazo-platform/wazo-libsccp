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

int queue_init(struct queue *q, size_t item_size)
{
	if (!item_size) {
		return SCCP_QUEUE_INVAL;
	}

	AST_LIST_HEAD_INIT_NOLOCK(&q->containers);
	AST_LIST_HEAD_INIT_NOLOCK(&q->reserved);
	q->item_size = item_size;

	return 0;
}

void queue_destroy(struct queue *q)
{
	struct queue_item_container *container;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&q->containers, container, list) {
		AST_LIST_REMOVE_CURRENT(list);
		container_destroy(container);
	}
	AST_LIST_TRAVERSE_SAFE_END;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&q->reserved, container, list) {
		AST_LIST_REMOVE_CURRENT(list);
		container_destroy(container);
	}
	AST_LIST_TRAVERSE_SAFE_END;
}

int queue_put(struct queue *q, void *item)
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

static int queue_put_reserved(struct queue *q, struct queue_reservation *reservation, void *item)
{
	struct queue_item_container *container = reservation->container;

	if (!AST_LIST_REMOVE(&q->reserved, container, list)) {
		ast_log(LOG_ERROR, "queue put reserved failed: container not in reserved list\n");
		return SCCP_QUEUE_INVAL;
	}

	AST_LIST_INSERT_TAIL(&q->containers, container, list);

	container_write_item(container, q->item_size, item);

	return 0;
}

static int queue_cancel_reserved(struct queue *q, struct queue_reservation *reservation)
{
	struct queue_item_container *container = reservation->container;

	if (!AST_LIST_REMOVE(&q->reserved, container, list)) {
		ast_log(LOG_ERROR, "queue cancel reserved failed: container not in reserved list\n");
		return SCCP_QUEUE_INVAL;
	}

	container_destroy(container);

	return 0;
}

int queue_reserve(struct queue *q, struct queue_reservation *reservation)
{
	struct queue_item_container *container;

	container = container_alloc(q->item_size);
	if (!container) {
		return -1;
	}

	AST_LIST_INSERT_TAIL(&q->reserved, container, list);

	reservation->q = q;
	reservation->container = container;

	return 0;
}

int queue_get(struct queue *q, void *item)
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

int queue_move(struct queue *dest, struct queue *src)
{
	if (!dest || !src) {
		return SCCP_QUEUE_INVAL;
	}

	dest->containers = src->containers;
	AST_LIST_HEAD_INIT_NOLOCK(&dest->reserved);
	dest->item_size = src->item_size;

	AST_LIST_HEAD_INIT_NOLOCK(&src->containers);

	return 0;
}

int queue_empty(const struct queue *q)
{
	return AST_LIST_EMPTY(&q->containers);
}

int queue_reservation_put(struct queue_reservation *reservation, void *item)
{
	return queue_put_reserved(reservation->q, reservation, item);
}

int queue_reservation_cancel(struct queue_reservation *reservation)
{
	return queue_cancel_reserved(reservation->q, reservation);
}

struct sccp_queue {
	ast_mutex_t lock;
	struct queue q;
	int pipefd[2];
	int closed;
};

struct sccp_queue *sccp_queue_create(size_t item_size)
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

	ast_mutex_init(&queue->lock);
	queue_init(&queue->q, item_size);
	queue->closed = 0;

	return queue;
}

void sccp_queue_destroy(struct sccp_queue *queue)
{
	ast_mutex_destroy(&queue->lock);
	queue_destroy(&queue->q);
	close(queue->pipefd[0]);
	close(queue->pipefd[1]);
	ast_free(queue);
}

int sccp_queue_fd(struct sccp_queue *queue)
{
	return queue->pipefd[0];
}

void sccp_queue_close(struct sccp_queue *queue)
{
	ast_mutex_lock(&queue->lock);
	queue->closed = 1;
	ast_mutex_unlock(&queue->lock);
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
	ssize_t n;
	char buf[8];

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

static int sccp_queue_put_no_lock(struct sccp_queue *queue, void *item)
{
	if (queue->closed) {
		return SCCP_QUEUE_CLOSED;
	}

	if (queue_empty(&queue->q)) {
		if (sccp_queue_write_pipe(queue)) {
			ast_log(LOG_ERROR, "sccp queue put failed: could not write to pipe\n");
			return -1;
		}
	}

	if (queue_put(&queue->q, item)) {
		ast_log(LOG_ERROR, "sccp queue put failed: could not queue item\n");
		return -1;
	}

	return 0;
}

int sccp_queue_put(struct sccp_queue *queue, void *item)
{
	int ret;

	ast_mutex_lock(&queue->lock);
	ret = sccp_queue_put_no_lock(queue, item);
	ast_mutex_unlock(&queue->lock);

	return ret;
}

static int sccp_queue_get_no_lock(struct sccp_queue *queue, void *item)
{
	if (queue_get(&queue->q, item)) {
		return SCCP_QUEUE_EMPTY;
	}

	if (queue_empty(&queue->q)) {
		sccp_queue_read_pipe(queue);
	}

	return 0;
}

int sccp_queue_get(struct sccp_queue *queue, void *item)
{
	int ret;

	ast_mutex_lock(&queue->lock);
	ret = sccp_queue_get_no_lock(queue, item);
	ast_mutex_unlock(&queue->lock);

	return ret;
}

static void sccp_queue_get_all_no_lock(struct sccp_queue *queue, struct queue *ret)
{
	queue_move(ret, &queue->q);
	if (!queue_empty(ret)) {
		sccp_queue_read_pipe(queue);
	}
}

int sccp_queue_get_all(struct sccp_queue *queue, struct queue *ret)
{
	if (!ret) {
		ast_log(LOG_ERROR, "sccp queue get all failed: ret is null\n");
		return SCCP_QUEUE_INVAL;
	}

	ast_mutex_lock(&queue->lock);
	sccp_queue_get_all_no_lock(queue, ret);
	ast_mutex_unlock(&queue->lock);

	return 0;
}
