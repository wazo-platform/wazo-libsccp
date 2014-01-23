#ifndef SCCP_QUEUE_H_
#define SCCP_QUEUE_H_

struct sccp_queue;

/*!
 * \brief Function type for process callback.
 */
typedef void (*sccp_queue_process_cb)(void *msg_data, void *arg);

#define SCCP_QUEUE_ERROR -1
#define SCCP_QUEUE_CLOSED 1
#define SCCP_QUEUE_EMPTY 2

/*!
 * \brief Create a new (FIFO) queue.
 *
 * \param msg_size size of message data
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_queue *sccp_queue_create(size_t msg_size);

/*!
 * \brief Destroy the queue.
 */
void sccp_queue_destroy(struct sccp_queue *queue);

/*!
 * \brief Return the read file descriptor of the queue.
 *
 * \note The file descriptor is ready when the queue is not empty.
 * \note You must not use the file descriptor for anything else than select / poll.
 */
int sccp_queue_fd(struct sccp_queue *queue);

/*!
 * \brief Close the queue so that no more message can be queued.
 *
 * \note This does not destroy the queue.
 */
void sccp_queue_close(struct sccp_queue *queue);

/*!
 * \brief Put a message into the queue.
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_CLOSED if the queue is closed
 * \retval SCCP_QUEUE_ERROR on other failure
 */
int sccp_queue_put(struct sccp_queue *queue, void *msg_data);

/*!
 * \brief Get the next message from the queue.
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_EMPTY if the queue is empty
 */
int sccp_queue_get(struct sccp_queue *queue, void *msg_data);

/*
 * \brief Process all the messages from the queue.
 *
 * \note In theory, is a bit more efficient than getting one message at a time.
 * \note No lock is held while the callback is called.
 */
void sccp_queue_process(struct sccp_queue *queue, sccp_queue_process_cb callback, void *arg);

#endif /* SCCP_QUEUE_H_ */
