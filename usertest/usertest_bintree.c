#include "usertest_common.h"

#include "ds_bintree.h"

/* Stage 2 knobs (edit these #defines; no CLI args) */
#define USERTEST_NUM_PRODUCERS 1
#define USERTEST_NUM_CONSUMERS 1
#define USERTEST_ITEMS_PER_PRODUCER 2
#define USERTEST_PRODUCER_SLEEP_SEC 2
#define USERTEST_POLL_US 1000
#define USERTEST_TIMEOUT_SEC 30

struct ctx {
	struct ds_bintree_head tree;
	_Atomic uint64_t produced;
	_Atomic uint64_t consumed;
	uint64_t expected;
};

static void dump_final_kvs(struct ds_bintree_head *head)
{
	struct ds_bintree_internal *stack[BINTREE_MAX_DEPTH];
	int stack_top = 0;
	int iterations = 0;

	if (!head || !head->root)
		return;

	stack[stack_top++] = head->root;

	while (stack_top > 0 && iterations < BINTREE_MAX_DEPTH * 8) {
		struct ds_bintree_internal *node = stack[--stack_top];
		struct ds_bintree_tree_node *left_h, *right_h;

		if (!node)
			continue;

		left_h = (struct ds_bintree_tree_node *)node->pLeft;
		right_h = (struct ds_bintree_tree_node *)node->pRight;
		if (!left_h || !right_h)
			return;

		if (left_h->type == BINTREE_NODE_LEAF) {
			struct ds_bintree_leaf *leaf = (struct ds_bintree_leaf *)left_h;
			if (leaf->kv.key < BINTREE_SENTINEL_KEY1) {
				fprintf(stdout, "consumer-final: key=%" PRIu64 " value=%" PRIu64 "\n",
					(uint64_t)leaf->kv.key, (uint64_t)leaf->kv.value);
			}
		} else if (stack_top < BINTREE_MAX_DEPTH - 1) {
			stack[stack_top++] = (struct ds_bintree_internal *)left_h;
		}

		if (right_h->type == BINTREE_NODE_LEAF) {
			struct ds_bintree_leaf *leaf = (struct ds_bintree_leaf *)right_h;
			if (leaf->kv.key < BINTREE_SENTINEL_KEY1) {
				fprintf(stdout, "consumer-final: key=%" PRIu64 " value=%" PRIu64 "\n",
					(uint64_t)leaf->kv.key, (uint64_t)leaf->kv.value);
			}
		} else if (stack_top < BINTREE_MAX_DEPTH - 1) {
			stack[stack_top++] = (struct ds_bintree_internal *)right_h;
		}

		iterations++;
	}
}

struct prod_arg {
	struct ctx *c;
	int tid;
};

static void *producer_thread(void *arg)
{
	struct prod_arg *pa = arg;
	struct ctx *c = pa->c;

	for (int i = 0; i < USERTEST_ITEMS_PER_PRODUCER; i++) {
		struct ds_kv kv = {
			.key = (uint64_t)pa->tid * 1000u + (uint64_t)(i + 1),
			.value = usertest_now_ns(),
		};

		for (;;) {
			int rc = ds_bintree_insert(&c->tree, kv);
			if (rc == DS_SUCCESS)
				break;
			if (rc != DS_ERROR_BUSY) {
				fprintf(stderr, "bintree: insert rc=%d\n", rc);
				return (void *)1;
			}
			usertest_sleep_us(USERTEST_POLL_US);
		}

		atomic_fetch_add_explicit(&c->produced, 1, memory_order_relaxed);
		fprintf(stdout, "producer[%d]: key=%" PRIu64 " value=%" PRIu64 " (count=%" PRIu64 ")\n",
			pa->tid, (uint64_t)kv.key, (uint64_t)kv.value,
			(uint64_t)arena_atomic_load(&c->tree.count, ARENA_RELAXED));

		if (i + 1 < USERTEST_ITEMS_PER_PRODUCER)
			sleep(USERTEST_PRODUCER_SLEEP_SEC);
	}

	return NULL;
}

static void *consumer_thread(void *arg)
{
	struct ctx *c = arg;

	/*
	 * Poll the data structure by watching the public `head->count` field,
	 * which is updated with atomics by the implementation.
	 */
	const uint64_t start = usertest_now_ns();
	uint64_t last = 0;

	while (last < c->expected) {
		uint64_t now = usertest_now_ns();
		if (now - start > (uint64_t)USERTEST_TIMEOUT_SEC * 1000000000ull) {
			fprintf(stderr, "bintree: timeout waiting for count to reach %" PRIu64 " (last=%" PRIu64 ")\n",
				(uint64_t)c->expected, (uint64_t)last);
			return (void *)1;
		}

		uint64_t cnt = (uint64_t)arena_atomic_load(&c->tree.count, ARENA_RELAXED);
		if (cnt != last) {
			last = cnt;
			atomic_store_explicit(&c->consumed, last, memory_order_relaxed);
			fprintf(stdout, "consumer: observed count=%" PRIu64 "\n", (uint64_t)last);
		}

		usertest_sleep_us(USERTEST_POLL_US);
	}

	return NULL;
}

int main(void)
{
	struct ctx c = {0};
	pthread_t producers[USERTEST_NUM_PRODUCERS];
	pthread_t consumer;
	struct prod_arg pargs[USERTEST_NUM_PRODUCERS];

	usertest_print_config("Non-blocking BINTREE (bintree)", USERTEST_NUM_PRODUCERS, USERTEST_NUM_CONSUMERS,
			      USERTEST_ITEMS_PER_PRODUCER);

	if (ds_bintree_init(&c.tree) != DS_SUCCESS) {
		fprintf(stderr, "bintree: init failed\n");
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

	dump_final_kvs(&c.tree);

	int vrc = ds_bintree_verify(&c.tree);
	if (vrc != DS_SUCCESS) {
		fprintf(stderr, "bintree: verify rc=%d\n", vrc);
		return 1;
	}

	fprintf(stdout, "done: produced=%" PRIu64 " consumed=%" PRIu64 "\n",
		(uint64_t)atomic_load(&c.produced), (uint64_t)atomic_load(&c.consumed));
	return atomic_load(&c.consumed) == c.expected ? 0 : 1;
}
