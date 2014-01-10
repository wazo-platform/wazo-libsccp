#ifndef SCCP_QUEUE_H_
#define SCCP_QUEUE_H_

/*
 * Not that the sccp_queue is NOT thread safe, the lock must be
 * handled by the client using the sccp_queue.
 */

struct sccp_queue;

/*
 * create a new queue, with msg that can be sent with a maximum msg size
 * of msg_size.
 */
struct sccp_queue *sccp_queue_create(size_t msg_size);

/*
 * destroy the queue
 *
 * Note that if there's message remaining that hold resources,
 * this will cause a leak.
 *
 * If you need not to leak, you should close the queue first, then
 * get and free all the queued message.
 */
void sccp_queue_destroy(struct sccp_queue *queue);

/*
 * close the queue
 *
 * Once closed, it's not possible to put other messages, but it's
 * still possible to get the remaining messages.
 */
void sccp_queue_close(struct sccp_queue *queue);

/*
 * Return the read file descriptor of the queue.
 *
 * The file descriptor is ready when the queue is not empty, and it is
 * not ready when the queue is empty.
 *
 * No read/write/close must be done on the fd; it MUST
 * only be used in a select / poll statement to detect when there
 * is msg to be read from the queue.
 */
int sccp_queue_fd(struct sccp_queue *queue);

/*
 * msg_size byte (queue property) of msg_data will be copied.
 *
 * Note that data must be at least sizeof(msg_size of queue) bytes
 */
int sccp_queue_put(struct sccp_queue *queue, void *msg_data);

/*
 * get the next msg in the queue
 */
int sccp_queue_get(struct sccp_queue *queue, void *msg_data);

int sccp_queue_empty(struct sccp_queue *queue);

#endif /* SCCP_QUEUE_H_ */
