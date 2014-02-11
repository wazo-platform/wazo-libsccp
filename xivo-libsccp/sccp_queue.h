#ifndef SCCP_QUEUE_H_
#define SCCP_QUEUE_H_

#include <asterisk/linkedlists.h>

struct sccp_sync_queue;

#define SCCP_QUEUE_CLOSED 1
#define SCCP_QUEUE_EMPTY 2
#define SCCP_QUEUE_INVAL 3

/* not to be used directly */
struct queue_item_container {
	AST_LIST_ENTRY(queue_item_container) list;
	void *item[0];
};

/* not to be used directly */
struct queue {
	AST_LIST_HEAD_NOLOCK(, queue_item_container) containers;
	AST_LIST_HEAD_NOLOCK(, queue_item_container) reserved;
	size_t item_size;
};

/* not to be used directly */
struct queue_reservation {
	struct queue *q;
	struct queue_item_container *container;
};

/*!
 * \brief Initialize a (FIFO) queue.
 *
 * \param q the queue to initialized
 * \param item_size the size of item
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_INVAL if item_size is zero
 */
int queue_init(struct queue *q, size_t item_size);

/*!
 * \brief Destroy the queue.
 */
void queue_destroy(struct queue *q);

/*!
 * \brief Put an item into the queue.
 *
 * \retval 0 on succes
 * \retval non-zero on failure
 */
int queue_put(struct queue *q, void *item);

/*!
 * \brief Reserve resources for a later put.
 *
 * \note If this function returns success, then you are guaranteed that the put using
 *       the reservation will not fail.
 * \note Once you get reservation, you must either call queue_reservation_put or
 *       queue_reservation_cancel, else resources will be leaked (until the queue is
 *       destroyed).
 *
 * \retval 0 on success
 * \retval non-zero on failure
 */
int queue_reserve(struct queue *q, struct queue_reservation *reservation);

/*!
 * \brief Get an item from the queue.
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_EMPTY if the queue is empty
 */
int queue_get(struct queue *q, void *item);

/*!
 * \brief Move all items from the source queue to the destination queue.
 *
 * \note The destination queue must not have been initialized.
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_INVAL if dest or src is null
 */
int queue_move(struct queue *dest, struct queue *src);

/*!
 * \brief Return non-zero if the queue is empty.
 */
int queue_empty(const struct queue *q);

/*!
 * \brief Put an item into the queue using the reservation.
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_INVAL if the reservation has already been used
 */
int queue_reservation_put(struct queue_reservation *reservation, void *item);

/*!
 * \brief Cancel a reservation.
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_INVAL if the reservation has already been used
 */
int queue_reservation_cancel(struct queue_reservation *reservation);

/*!
 * \brief Create a new synchronized (FIFO) queue.
 *
 * \param item_size size of item data
 *
 * \retval non-NULL on success
 * \retval NULL on failure
 */
struct sccp_sync_queue *sccp_sync_queue_create(size_t item_size);

/*!
 * \brief Destroy the queue.
 */
void sccp_sync_queue_destroy(struct sccp_sync_queue *sync_q);

/*!
 * \brief Return the read file descriptor of the queue.
 *
 * \note The file descriptor is ready when the queue is not empty.
 * \note You must not use the file descriptor for anything else than select / poll.
 */
int sccp_sync_queue_fd(struct sccp_sync_queue *sync_q);

/*!
 * \brief Close the queue so that no more item can be queued.
 *
 * \note This does not destroy the queue.
 */
void sccp_sync_queue_close(struct sccp_sync_queue *sync_q);

/*!
 * \brief Put an item into the queue.
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_CLOSED if the queue is closed
 * \retval SCCP_QUEUE_ERROR on other failure
 */
int sccp_sync_queue_put(struct sccp_sync_queue *sync_q, void *item);

/*!
 * \brief Get an item from the queue.
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_EMPTY if the queue is empty
 */
int sccp_sync_queue_get(struct sccp_sync_queue *sync_q, void *item);

/*!
 * \brief Get all the items from the queue.
 *
 * \note ret must not have been initialized, and if the function returns
 *       successfully, it must be destroyed after
 *
 * \retval 0 on success
 * \retval SCCP_QUEUE_INVAL if ret is null
 */
int sccp_sync_queue_get_all(struct sccp_sync_queue *sync_q, struct queue *ret);

#endif /* SCCP_QUEUE_H_ */
