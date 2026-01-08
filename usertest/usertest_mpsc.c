#include "usertest_common.h"

#include "ds_mpsc.h"

/*
 * Stage 2 knobs (edit these #defines; no CLI args)
 * MPSC constraints: multiple producers, single consumer.
 */
#define USERTEST_NUM_PRODUCERS 3
#define USERTEST_NUM_CONSUMERS 1
#define USERTEST_ITEMS_PER_PRODUCER 2
#define USERTEST_PRODUCER_SLEEP_SEC 2
#define USERTEST_POLL_US 1000

#if USERTEST_NUM_CONSUMERS != 1
#error "MPSC requires USERTEST_NUM_CONSUMERS == 1"
#endif

struct ctx {
	struct ds_mpsc_head q;
	_Atomic uint64_t produced;
	_Atomic uint64_t consumed;
	uint64_t expected;
};

struct prod_arg {
	struct ctx *c;
	int tid;
};

static void *producer_thread(void *arg)
{
	struct prod_arg *pa = arg;
	struct ctx *c = pa->c;

	for (int i = 0; i < USERTEST_ITEMS_PER_PRODUCER; i++) {
		uint64_t key = (uint64_t)pa->tid * 1000u + (uint64_t)(i + 1);
		uint64_t value = usertest_now_ns();

		int rc = ds_mpsc_insert(&c->q, key, value);
		if (rc != DS_SUCCESS) {
			fprintf(stderr, "mpsc: insert rc=%d\n", rc);
			return (void *)1;
		}

		atomic_fetch_add_explicit(&c->produced, 1, memory_order_relaxed);
		fprintf(stdout, "producer[%d]: key=%" PRIu64 " value=%" PRIu64 "\n",
			pa->tid, (uint64_t)key, (uint64_t)value);

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
		int rc = ds_mpsc_delete(&c->q, &out);
		if (rc == DS_SUCCESS) {
			uint64_t n = atomic_fetch_add_explicit(&c->consumed, 1, memory_order_relaxed) + 1;
			fprintf(stdout, "consumer: key=%" PRIu64 " value=%" PRIu64 " (n=%" PRIu64 ")\n",
				(uint64_t)out.key, (uint64_t)out.value, (uint64_t)n);
			continue;
		}
		if (rc == DS_ERROR_NOT_FOUND || rc == DS_ERROR_BUSY) {
			usertest_sleep_us(USERTEST_POLL_US);
			continue;
		}
		fprintf(stderr, "mpsc: delete rc=%d\n", rc);
		return (void *)1;
	}

	return NULL;
}

int main(void)
{
	struct ctx c = {0};
	pthread_t producers[USERTEST_NUM_PRODUCERS];
	pthread_t consumer;
	struct prod_arg pargs[USERTEST_NUM_PRODUCERS];

	usertest_print_config("Vyukhov MPSC", USERTEST_NUM_PRODUCERS, USERTEST_NUM_CONSUMERS,
			      USERTEST_ITEMS_PER_PRODUCER);

	if (ds_mpsc_init(&c.q) != DS_SUCCESS) {
		fprintf(stderr, "mpsc: init failed\n");
		return 1;
	}

	c.expected = (uint64_t)USERTEST_NUM_PRODUCERS * (uint64_t)USERTEST_ITEMS_PER_PRODUCER;

	if (pthread_create(&consumer, NULL, consumer_thread, &c) != 0) {
		perror("pthread_create consumer");
		return 1;
	}

	for (int i = 0; i < USERTEST_NUM_PRODUCERS; i++) {
		pargs[i] = (struct prod_arg){ .c = &c, .tid = i };
		if (pthread_create(&producers[i], NULL, producer_thread, &pargs[i]) != 0) {
			perror("pthread_create producer");
			return 1;
		}
	}

	for (int i = 0; i < USERTEST_NUM_PRODUCERS; i++)
		pthread_join(producers[i], NULL);
	pthread_join(consumer, NULL);

	fprintf(stdout, "done: produced=%" PRIu64 " consumed=%" PRIu64 "\n",
		(uint64_t)atomic_load(&c.produced), (uint64_t)atomic_load(&c.consumed));
	return atomic_load(&c.consumed) == c.expected ? 0 : 1;
}
