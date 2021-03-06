#ifndef ORYX_TASK_H
#define ORYX_TASK_H

#include "task.h"

ORYX_DECLARE (
	struct oryx_task_t * oryx_task_spawn (
		IN const char		__oryx_unused__*alias, 
		IN const uint32_t	__oryx_unused__ ul_prio,
		IN void	__oryx_unused__*attr,
		IN void *(*handler)(void *),
		IN void	*argv
	)
);

ORYX_DECLARE (
	void oryx_task_registry (
		IN struct oryx_task_t *task
	)
);

ORYX_DECLARE (
	void oryx_task_deregistry_id (
		IN oryx_os_thread_t pid
	)
);

ORYX_DECLARE (
	void oryx_task_launch (
		void
	)
);

ORYX_DECLARE (
	void oryx_task_initialize (
		void
	)
);

#endif
