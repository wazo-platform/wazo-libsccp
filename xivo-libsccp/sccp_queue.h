#ifndef SCCP_QUEUE_H_
#define SCCP_QUEUE_H_

struct sccp_queue;

/*!
 * \brief Function type for process callback.
 */
typedef void (*sccp_queue_process_cb)(void *msg_data, void *arg);

#define SCCP_QUEUE_CLOSED 1
#define SCCP_QUEUE_EMPTY 2
#define SCCP_QUEUE_ERROR 3

/*
 * Create a new (FIFO) queue, with msg that can be sent with a maximum msg size
 * of msg_size.
 */
struct sccp_queue *sccp_queue_create(size_t msg_size);

/*
 * destroy the queue
 *
 * Note that if there's message remaining that hold resources,
 * this will cause a leak.
 */
void sccp_queue_destroy(struct sccp_queue *queue);

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

/*!
 * \brief Close the queue so that sccp_queue_put will return an error.
 *
 * \note This does not destroy the queue.
 *
 * XXX maybe rename to sccp_queue_shutdown ?
 */
void sccp_queue_close(struct sccp_queue *queue);

/*!
 * \brief Put a message to the queue
 *
 * \note The message is copied from *msg_data.
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_CLOSED if the queue is closed
 * \retval SCCP_QUEUE_ERROR on other failure
 */
int sccp_queue_put(struct sccp_queue *queue, void *msg_data);

/*!
 * \brief Get the next message from the queue.
 *
 * \note The message is copied to *msg_data.
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_EMPTY if the queue is empty
 */
int sccp_queue_get(struct sccp_queue *queue, void *msg_data);

/*
 * \brief Process all the messages from the queue.
 *
 * \note In theory, should be more efficient than getting one message at
 *       a time.
 * \note The callback is called while the queue's lock is not held, so don't
 *       be afraid of deadlock in the callback
 */
void sccp_queue_process(struct sccp_queue *queue, sccp_queue_process_cb callback, void *arg);

#endif /* SCCP_QUEUE_H_ */
