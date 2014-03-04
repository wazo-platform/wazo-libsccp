#include <string.h>

#include <asterisk.h>
#include <asterisk/heap.h>
#include <asterisk/linkedlists.h>
#include <asterisk/time.h>
#include <asterisk/utils.h>

#include "sccp_task.h"

struct task {
	AST_LIST_ENTRY(task) list;
	struct timeval when;
	ssize_t __heap_index;

	sccp_task_cb callback;
	void *data[0];
};

struct sccp_task_runner {
	AST_LIST_HEAD_NOLOCK(, task) tasks;
	struct ast_heap *heap;
	size_t data_size;
};

static struct task *task_create(size_t data_size, sccp_task_cb callback, void *data)
{
	struct task *task;

	task = ast_calloc(1, sizeof(*task) + data_size);
	if (!task) {
		return NULL;
	}

	task->callback = callback;
	memcpy(task->data, data, data_size);

	return task;
}

static void task_destroy(struct task *task)
{
	ast_free(task);
}

static int task_is_equal(struct task *task, sccp_task_cb callback, void *data, size_t data_size)
{
	return task->callback == callback && !memcmp(task->data, data, data_size);
}

static int task_cmp(void *a, void *b)
{
	return ast_tvcmp(((struct task *) b)->when, ((struct task *) a)->when);
}

struct sccp_task_runner *sccp_task_runner_create(size_t data_size)
{
	struct sccp_task_runner *runner;

	runner = ast_calloc(1, sizeof(*runner));
	if (!runner) {
		return NULL;
	}

	runner->heap = ast_heap_create(3, task_cmp, offsetof(struct task, __heap_index));
	if (!runner->heap) {
		ast_free(runner);
		return NULL;
	}

	AST_LIST_HEAD_INIT_NOLOCK(&runner->tasks);
	runner->data_size = data_size;

	return runner;
}

void sccp_task_runner_destroy(struct sccp_task_runner *runner)
{
	struct task *task;

	ast_heap_destroy(runner->heap);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&runner->tasks, task, list) {
		AST_LIST_REMOVE_CURRENT(list);
		task_destroy(task);
	}
	AST_LIST_TRAVERSE_SAFE_END;

	ast_free(runner);
}

int sccp_task_runner_add(struct sccp_task_runner *runner, sccp_task_cb callback, void *data, int sec)
{
	struct task *task;
	size_t data_size = runner->data_size;

	/* check if the task is already known */
	AST_LIST_TRAVERSE(&runner->tasks, task, list) {
		if (task_is_equal(task, callback, data, data_size)) {
			break;
		}
	}

	if (task) {
		ast_heap_remove(runner->heap, task);
	} else {
		task = task_create(data_size, callback, data);
		if (!task) {
			return -1;
		}

		AST_LIST_INSERT_TAIL(&runner->tasks, task, list);
	}

	if (sec < 0) {
		task->when = ast_tvnow();
	} else {
		task->when = ast_tvadd(ast_tvnow(), ast_tv(sec, 0));
	}

	return ast_heap_push(runner->heap, task);
}

void sccp_task_runner_remove(struct sccp_task_runner *runner, sccp_task_cb callback, void *data)
{
	struct task *task;
	size_t data_size = runner->data_size;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&runner->tasks, task, list) {
		if (task_is_equal(task, callback, data, data_size)) {
			ast_heap_remove(runner->heap, task);
			AST_LIST_REMOVE_CURRENT(list);
			task_destroy(task);
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;
}

void sccp_task_runner_run(struct sccp_task_runner *runner, struct sccp_session *session)
{
	struct task *task;
	struct timeval when;

	when = ast_tvadd(ast_tvnow(), ast_tv(0, 1000));
	while (1) {
		task = ast_heap_peek(runner->heap, 1);
		if (!task) {
			break;
		}

		if (ast_tvcmp(task->when, when) != -1) {
			break;
		}

		ast_heap_pop(runner->heap);
		AST_LIST_REMOVE(&runner->tasks, task, list);

		task->callback(session, task->data);

		task_destroy(task);
	}
}

int sccp_task_runner_next_ms(struct sccp_task_runner *runner)
{
	struct task *task;
	int ms;

	task = ast_heap_peek(runner->heap, 1);
	if (!task) {
		return -1;
	}

	ms = ast_tvdiff_ms(task->when, ast_tvnow());
	if (ms < 0) {
		ms = 0;
	}

	return ms;
}
