#include "usertest_common.h"

#include "ds_ck_stack_upmc.h"

#define USERTEST_NUM_PRODUCERS 3
#define USERTEST_NUM_CONSUMERS 3
#define USERTEST_ITEMS_PER_PRODUCER 8
#define USERTEST_PRODUCER_SLEEP_SEC 1
#define USERTEST_POLL_US 1000

struct ctx {
	struct ds_ck_stack_upmc_head stack;
	_Atomic uint64_t produced;
	_Atomic uint64_t consumed;
	_Atomic uint64_t duplicate_keys;
	_Atomic uint64_t out_of_range_keys;
	ds_ck_stack_upmc_entry_t entries[USERTEST_NUM_PRODUCERS][USERTEST_ITEMS_PER_PRODUCER];
	_Atomic uint8_t seen_keys[USERTEST_NUM_PRODUCERS * USERTEST_ITEMS_PER_PRODUCER];
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
		ds_ck_stack_upmc_entry_t *entry = &c->entries[pa->tid][i];
		uint64_t key = (uint64_t)pa->tid * 1000u + (uint64_t)(i + 1);
		uint64_t value = usertest_now_ns();

		while (!ds_ck_stack_upmc_trypush_upmc(&c->stack, entry, key, value)) {
			usertest_sleep_us(USERTEST_POLL_US);
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
	ds_ck_stack_upmc_entry_t *entry;

	for (;;) {
		uint64_t done = atomic_load_explicit(&c->consumed, memory_order_relaxed);
		if (done >= c->expected)
			return NULL;

		entry = ds_ck_stack_upmc_pop_upmc(&c->stack);
		if (entry) {
			uint64_t key;
			uint64_t n;
			uint64_t producer_id;
			uint64_t item_id;
			uint64_t linear_key;

			n = atomic_fetch_add_explicit(&c->consumed, 1, memory_order_relaxed) + 1;
			key = entry->data.key;
			fprintf(stdout, "consumer: key=%" PRIu64 " value=%" PRIu64 " (n=%" PRIu64 ")\n",
				(uint64_t)key, (uint64_t)entry->data.value, (uint64_t)n);

			producer_id = key / 1000u;
			item_id = key % 1000u;
			if (producer_id >= USERTEST_NUM_PRODUCERS ||
			    item_id == 0 ||
			    item_id > USERTEST_ITEMS_PER_PRODUCER) {
				atomic_fetch_add_explicit(&c->out_of_range_keys, 1, memory_order_relaxed);
				continue;
			}

			linear_key = producer_id * USERTEST_ITEMS_PER_PRODUCER + (item_id - 1);
			if (atomic_fetch_add_explicit(&c->seen_keys[linear_key], 1, memory_order_relaxed) != 0)
				atomic_fetch_add_explicit(&c->duplicate_keys, 1, memory_order_relaxed);
			continue;
		}

		usertest_sleep_us(USERTEST_POLL_US);
	}
}

int main(void)
{
	struct ctx c = {0};
	pthread_t producers[USERTEST_NUM_PRODUCERS];
	pthread_t consumers[USERTEST_NUM_CONSUMERS];
	struct prod_arg pargs[USERTEST_NUM_PRODUCERS];

	usertest_print_config("CK Treiber Stack UPMC", USERTEST_NUM_PRODUCERS,
			      USERTEST_NUM_CONSUMERS, USERTEST_ITEMS_PER_PRODUCER);

	ds_ck_stack_upmc_init(&c.stack);
	c.expected = (uint64_t)USERTEST_NUM_PRODUCERS * (uint64_t)USERTEST_ITEMS_PER_PRODUCER;

	for (int i = 0; i < USERTEST_NUM_CONSUMERS; i++) {
		if (pthread_create(&consumers[i], NULL, consumer_thread, &c) != 0) {
			perror("pthread_create consumer");
			return 1;
		}
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
	for (int i = 0; i < USERTEST_NUM_CONSUMERS; i++)
		pthread_join(consumers[i], NULL);

	fprintf(stdout, "done: produced=%" PRIu64 " consumed=%" PRIu64 "\n",
		(uint64_t)atomic_load(&c.produced), (uint64_t)atomic_load(&c.consumed));
	fprintf(stdout, "validation: duplicate_keys=%" PRIu64 " out_of_range_keys=%" PRIu64 "\n",
		(uint64_t)atomic_load(&c.duplicate_keys),
		(uint64_t)atomic_load(&c.out_of_range_keys));

	if (atomic_load(&c.duplicate_keys) != 0)
		return 1;

	if (atomic_load(&c.out_of_range_keys) != 0)
		return 1;

	return atomic_load(&c.consumed) == c.expected ? 0 : 1;
}
