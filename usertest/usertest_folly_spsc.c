#include "usertest_common.h"

#include "ds_folly_spsc.h"

/*
 * Stage 2 knobs (edit these #defines; no CLI args)
 * SPSC constraints: exactly 1 producer and 1 consumer.
 */
#define USERTEST_NUM_PRODUCERS 1
#define USERTEST_NUM_CONSUMERS 1
#define USERTEST_ITEMS_PER_PRODUCER 2
#define USERTEST_PRODUCER_SLEEP_SEC 2
#define USERTEST_POLL_US 1000
#define USERTEST_SPSC_SIZE 64u /* total slots; usable capacity is size-1 */

#if USERTEST_NUM_PRODUCERS != 1
#error "Folly SPSC requires USERTEST_NUM_PRODUCERS == 1"
#endif
#if USERTEST_NUM_CONSUMERS != 1
#error "Folly SPSC requires USERTEST_NUM_CONSUMERS == 1"
#endif

struct ctx {
	struct ds_spsc_queue_head q;
	_Atomic uint64_t produced;
	_Atomic uint64_t consumed;
	uint64_t expected;
};

static void *producer_thread(void *arg)
{
	struct ctx *c = arg;

	for (int i = 0; i < USERTEST_ITEMS_PER_PRODUCER; i++) {
		uint64_t key = (uint64_t)(i + 1);
		uint64_t value = usertest_now_ns();

		for (;;) {
			int rc = ds_spsc_insert(&c->q, key, value);
			if (rc == DS_SUCCESS)
				break;
			if (rc != DS_ERROR_FULL) {
				fprintf(stderr, "spsc: insert rc=%d\n", rc);
				return (void *)1;
			}
			usertest_sleep_us(USERTEST_POLL_US);
		}

		atomic_fetch_add_explicit(&c->produced, 1, memory_order_relaxed);
		fprintf(stdout, "producer: key=%" PRIu64 " value=%" PRIu64 "\n",
			(uint64_t)key, (uint64_t)value);

		if (i + 1 < USERTEST_ITEMS_PER_PRODUCER)
			sleep(USERTEST_PRODUCER_SLEEP_SEC);
	}

	return NULL;
}

static void *consumer_thread(void *arg)
{
	struct ctx *c = arg;
	struct ds_kv out;

	while (atomic_load_explicit(&c->consumed, memory_order_relaxed) < c->expected) {
		int rc = ds_spsc_delete(&c->q, &out);
		if (rc == DS_SUCCESS) {
			uint64_t n = atomic_fetch_add_explicit(&c->consumed, 1, memory_order_relaxed) + 1;
			fprintf(stdout, "consumer: key=%" PRIu64 " value=%" PRIu64 " (n=%" PRIu64 ")\n",
				(uint64_t)out.key, (uint64_t)out.value, (uint64_t)n);
			continue;
		}
		if (rc != DS_ERROR_NOT_FOUND) {
			fprintf(stderr, "spsc: delete rc=%d\n", rc);
			return (void *)1;
		}
		usertest_sleep_us(USERTEST_POLL_US);
	}

	return NULL;
}

int main(void)
{
	struct ctx c = {0};
	pthread_t prod, cons;

	usertest_print_config("Folly SPSC", USERTEST_NUM_PRODUCERS, USERTEST_NUM_CONSUMERS,
			      USERTEST_ITEMS_PER_PRODUCER);

	if (ds_spsc_init(&c.q, USERTEST_SPSC_SIZE) != DS_SUCCESS) {
		fprintf(stderr, "spsc: init failed\n");
		return 1;
	}

	c.expected = (uint64_t)USERTEST_NUM_PRODUCERS * (uint64_t)USERTEST_ITEMS_PER_PRODUCER;

	if (pthread_create(&cons, NULL, consumer_thread, &c) != 0) {
		perror("pthread_create consumer");
		return 1;
	}
	if (pthread_create(&prod, NULL, producer_thread, &c) != 0) {
		perror("pthread_create producer");
		return 1;
	}

	pthread_join(prod, NULL);
	pthread_join(cons, NULL);

	fprintf(stdout, "done: produced=%" PRIu64 " consumed=%" PRIu64 "\n",
		(uint64_t)atomic_load(&c.produced), (uint64_t)atomic_load(&c.consumed));
	return atomic_load(&c.consumed) == c.expected ? 0 : 1;
}
