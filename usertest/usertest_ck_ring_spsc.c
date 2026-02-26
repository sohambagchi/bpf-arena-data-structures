#include "usertest_common.h"

#include "ds_ck_ring_spsc.h"

#define USERTEST_NUM_PRODUCERS 1
#define USERTEST_NUM_CONSUMERS 1
#define USERTEST_ITEMS_PER_PRODUCER 2
#define USERTEST_PRODUCER_SLEEP_SEC 2
#define USERTEST_POLL_US 1000
#define USERTEST_CK_RING_CAPACITY 64u

#if USERTEST_NUM_PRODUCERS != 1
#error "CK ring SPSC requires USERTEST_NUM_PRODUCERS == 1"
#endif
#if USERTEST_NUM_CONSUMERS != 1
#error "CK ring SPSC requires USERTEST_NUM_CONSUMERS == 1"
#endif

struct ctx {
	struct ds_ck_ring_spsc_head q;
	_Atomic uint64_t produced;
	_Atomic uint64_t consumed;
	_Atomic uint64_t ordering_failures;
	uint64_t expected;
};

static void *producer_thread(void *arg)
{
	struct ctx *c = arg;

	for (int i = 0; i < USERTEST_ITEMS_PER_PRODUCER; i++) {
		uint64_t key = (uint64_t)(i + 1);
		uint64_t value = usertest_now_ns();

		for (;;) {
			int rc = ds_ck_ring_spsc_insert(&c->q, key, value);
			if (rc == DS_SUCCESS)
				break;
			if (rc != DS_ERROR_FULL) {
				fprintf(stderr, "ck_ring_spsc: insert rc=%d\n", rc);
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
	uint64_t expected_key = 1;

	while (atomic_load_explicit(&c->consumed, memory_order_relaxed) < c->expected) {
		int rc = ds_ck_ring_spsc_pop(&c->q, &out);
		if (rc == DS_SUCCESS) {
			uint64_t n = atomic_fetch_add_explicit(&c->consumed, 1, memory_order_relaxed) + 1;
			fprintf(stdout, "consumer: key=%" PRIu64 " value=%" PRIu64 " (n=%" PRIu64 ")\n",
				(uint64_t)out.key, (uint64_t)out.value, (uint64_t)n);
			if ((uint64_t)out.key != expected_key) {
				atomic_fetch_add_explicit(&c->ordering_failures, 1, memory_order_relaxed);
				fprintf(stderr,
					"ck_ring_spsc: order violation got=%" PRIu64 " expected=%" PRIu64 "\n",
					(uint64_t)out.key, expected_key);
			}
			expected_key++;
			continue;
		}
		if (rc != DS_ERROR_NOT_FOUND) {
			fprintf(stderr, "ck_ring_spsc: pop rc=%d\n", rc);
			return (void *)1;
		}
		usertest_sleep_us(USERTEST_POLL_US);
	}

	return NULL;
}

int main(void)
{
	struct ctx c = {0};
	pthread_t prod;
	pthread_t cons;
	uint64_t produced;
	uint64_t consumed;
	uint64_t ordering_failures;

	usertest_print_config("CK Ring SPSC", USERTEST_NUM_PRODUCERS,
			      USERTEST_NUM_CONSUMERS, USERTEST_ITEMS_PER_PRODUCER);

	if (ds_ck_ring_spsc_init(&c.q, USERTEST_CK_RING_CAPACITY) != DS_SUCCESS) {
		fprintf(stderr, "ck_ring_spsc: init failed\n");
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
	produced = atomic_load_explicit(&c.produced, memory_order_relaxed);
	consumed = atomic_load_explicit(&c.consumed, memory_order_relaxed);
	ordering_failures = atomic_load_explicit(&c.ordering_failures, memory_order_relaxed);

	fprintf(stdout,
		"done: produced=%" PRIu64 " consumed=%" PRIu64 " ordering_failures=%" PRIu64 "\n",
		(uint64_t)produced, (uint64_t)consumed, (uint64_t)ordering_failures);

	if (ordering_failures != 0)
		return 1;

	if (consumed != c.expected)
		return 1;

	if (produced != c.expected)
		return 1;

	return 0;
}
