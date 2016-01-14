#ifndef _TASK_H
#define _TASK_H

#define TASK_TIMING

#ifdef TASK_TIMING

extern void task_time_start_impl();
extern void task_time_end_impl();

#define task_time_start()		task_time_start_impl()
#define task_time_end()		task_time_end_impl(__FUNCTION__)

#else
#define task_time_start()
#define task_time_end()
#endif


#endif
