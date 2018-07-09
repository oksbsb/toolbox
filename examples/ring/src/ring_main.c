#include "oryx.h"

static __oryx_always_inline__
void * ring_rp (void *r)
{
	void *data;
	int ret = 0;
	uint16_t data_size = 0;
	struct oryx_ring_t *ring = (struct oryx_ring_t *)r;

	FOREVER {
		ret = oryx_ring_get(ring, &data, &data_size);
		if(ret != 0)
			continue;
		if(data) {
			//printf ("[%p] rp-> %u\n", data, *(uint32_t *)data);
			free(data);
		}
	}
	
	oryx_task_deregistry_id(pthread_self());
	return NULL;
}

static __oryx_always_inline__
void * ring_wp (void *r)
{
	uint32_t times = 0;
	int sleeps = 0;
	void *data;
	int ret = 0;
	uint16_t data_size = 0;
	struct oryx_ring_t *ring = (struct oryx_ring_t *)r;

	FOREVER {
		data = malloc(sizeof(times));
		if(!data) continue;
		*(uint32_t *)data = times ++;
		data_size = sizeof(times);
		ret = oryx_ring_put(ring, data, data_size);
		if(ret != 0) {
			free(data);
			continue;
		}
		
		usleep(1000);

		/** dump ring desc every 3 sec */
		if(sleeps ++ == 3000) {
			sleeps = 0;
			oryx_ring_dump(ring);
			printf("times %u\n", times);
		}
	}
	
	oryx_task_deregistry_id(pthread_self());
	return NULL;
}

static struct oryx_task_t rp_task =
{
	.module = THIS,
	.sc_alias = "RP Task",
	.fn_handler = ring_rp,
	.ul_lcore = INVALID_CORE,
	.ul_prio = KERNEL_SCHED,
	.argc = 0,
	.argv = NULL,
	.ul_flags = 0,	/** Can not be recyclable. */
};


static struct oryx_task_t wp_task =
{
	.module = THIS,
	.sc_alias = "WP Task",
	.fn_handler = ring_wp,
	.ul_lcore = INVALID_CORE,
	.ul_prio = KERNEL_SCHED,
	.argc = 0,
	.argv = NULL,
	.ul_flags = 0,	/** Can not be recyclable. */
};

int main(int argc, char ** argv)
{
	int r = 0;
	struct oryx_ring_t *ring;

	oryx_initialize();
	
	r = oryx_ring_create("test_ring", 1024, 0, &ring);
	if(r != 0) return;

	oryx_ring_dump(ring);

	wp_task.argc = 1;
	wp_task.argv = ring;
	oryx_task_registry(&wp_task);


	rp_task.argc = 1;
	rp_task.argv = ring;
	oryx_task_registry(&rp_task);

	oryx_task_launch();

	FOREVER;
}
