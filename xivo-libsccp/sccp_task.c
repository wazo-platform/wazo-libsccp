#include <asterisk.h>
#include <asterisk/heap.h>
#include <asterisk/linkedlists.h>
#include <asterisk/time.h>
#include <asterisk/utils.h>

#include "sccp_task.h"

struct sccp_runnable_task {
	struct sccp_task task;
	struct timeval when;
	ssize_t __heap_index;

	AST_LIST_ENTRY(sccp_runnable_task) list;
};

struct sccp_task_runner {
	struct ast_heap *heap;

	AST_LIST_HEAD_NOLOCK(, sccp_runnable_task) rtasks;
};

struct sccp_task sccp_task(sccp_task_cb callback, void *data)
{
	struct sccp_task task = {
		.callback = callback,
		.data = data,
	};

	return task;
}

static int sccp_task_eq(struct sccp_task a, struct sccp_task b)
{
	return a.callback == b.callback && a.data == b.data;
}

static int sccp_runnable_task_cmp(void *a, void *b)
{
	return ast_tvcmp(((struct sccp_runnable_task *) b)->when, ((struct sccp_runnable_task *) a)->when);
}

struct sccp_task_runner *sccp_task_runner_create(void)
{
	struct sccp_task_runner *runner;

	runner = ast_calloc(1, sizeof(*runner));
	if (!runner) {
		return NULL;
	}

	runner->heap = ast_heap_create(3, sccp_runnable_task_cmp, offsetof(struct sccp_runnable_task, __heap_index));
	if (!runner->heap) {
		ast_free(runner);
		return NULL;
	}

	AST_LIST_HEAD_INIT_NOLOCK(&runner->rtasks);

	return runner;
}

void sccp_task_runner_destroy(struct sccp_task_runner *runner)
{
	struct sccp_runnable_task *rtask;

	ast_heap_destroy(runner->heap);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&runner->rtasks, rtask, list) {
		AST_LIST_REMOVE_CURRENT(list);
		ast_free(rtask);
	}
	AST_LIST_TRAVERSE_SAFE_END;

	ast_free(runner);
}

int sccp_task_runner_add(struct sccp_task_runner *runner, struct sccp_task task, int sec)
{
	struct sccp_runnable_task *rtask;

	/* check if the task is already known */
	AST_LIST_TRAVERSE(&runner->rtasks, rtask, list) {
		if (sccp_task_eq(rtask->task, task)) {
			break;
		}
	}

	if (rtask) {
		ast_heap_remove(runner->heap, rtask);
	} else {
		rtask = ast_calloc(1, sizeof(*rtask));
		if (!rtask) {
			return -1;
		}

		rtask->task = task;
		AST_LIST_INSERT_TAIL(&runner->rtasks, rtask, list);
	}

	if (sec < 0) {
		rtask->when = ast_tvnow();
	} else {
		rtask->when = ast_tvadd(ast_tvnow(), ast_tv(sec, 0));
	}

	return ast_heap_push(runner->heap, rtask);
}

void sccp_task_runner_remove(struct sccp_task_runner *runner, struct sccp_task task)
{
	struct sccp_runnable_task *rtask;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&runner->rtasks, rtask, list) {
		if (sccp_task_eq(rtask->task, task)) {
			ast_heap_remove(runner->heap, rtask);
			AST_LIST_REMOVE_CURRENT(list);
			ast_free(rtask);
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;
}

void sccp_task_runner_run(struct sccp_task_runner *runner, struct sccp_session *session)
{
	struct sccp_runnable_task *rtask;
	struct timeval when;

	when = ast_tvadd(ast_tvnow(), ast_tv(0, 1000));
	while (1) {
		rtask = ast_heap_peek(runner->heap, 1);
		if (!rtask) {
			break;
		}

		if (ast_tvcmp(rtask->when, when) != -1) {
			break;
		}

		ast_heap_pop(runner->heap);
		AST_LIST_REMOVE(&runner->rtasks, rtask, list);

		rtask->task.callback(session, rtask->task.data);

		ast_free(rtask);
	}
}

int sccp_task_runner_next_ms(struct sccp_task_runner *runner)
{
	struct sccp_runnable_task *rtask;
	int ms;

	rtask = ast_heap_peek(runner->heap, 1);
	if (!rtask) {
		return -1;
	}

	ms = ast_tvdiff_ms(rtask->when, ast_tvnow());
	if (ms < 0) {
		ms = 0;
	}

	return ms;
}
