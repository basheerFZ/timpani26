// vim: set ts=4 sw=4 et:
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <sched.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#define NSEC_PER_SEC		1000000000
#define USEC_PER_SEC		1000000
#define NSEC_PER_USEC		1000
#define NSEC_PER_MSEC		1000000
#define USEC_PER_MSEC		1000

#define ts_ns(a)	    	((a.tv_sec * NSEC_PER_SEC) + a.tv_nsec)
#define diff(b, a)	    	(b - a)

/* Probability (0-100) of randomly injecting a deadline miss */
//#define DMISS_PROB		5
#define DMISS_PROB		0

/* Boolean macro whether to wait for timpani-n to start before running the workload loop. */
#define WAIT_FOR_TIMPANI_N	1

/* Adjusted percentage of the runtime to account for the overhead of the workload loop and other factors. */
#define RUNTIME_ADJUST_PCT	95

char pr_name[16];

uint64_t task_mit_ms;
uint64_t task_wcet_ms;

uint64_t last_wakeup_ns;

static inline uint64_t get_cpu_time(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts_ns(ts);
}

static inline uint64_t get_mono_time(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts_ns(ts);
}

static inline unsigned int get_random(unsigned int min, unsigned int max)
{
    return (rand() % (max - min + 1)) + min;
}

static inline void get_cbs_event(void)
{
    struct timespec ts;
    uint64_t random_pct = get_random(0, 50);
    uint64_t extra_ms =  task_mit_ms * random_pct / 100;
    uint64_t inter_arrival_ns = (task_mit_ms + extra_ms) * NSEC_PER_MSEC;
    uint64_t inter_arrival_adj_ns = inter_arrival_ns;

    // Adjust the inter-arrival time using last wakeup time
    if (last_wakeup_ns != 0) {
	uint64_t now_ns = get_mono_time();
	uint64_t diff_ns = now_ns - last_wakeup_ns;

	if (inter_arrival_adj_ns > diff_ns) {
		inter_arrival_adj_ns -= diff_ns;
	} else {
		inter_arrival_adj_ns = 0;
	}
    }

    ts.tv_sec = inter_arrival_adj_ns / NSEC_PER_SEC;
    ts.tv_nsec = inter_arrival_adj_ns % NSEC_PER_SEC;
    nanosleep(&ts, NULL);

    last_wakeup_ns = get_mono_time();
    printf("%s(%d) woke up after %lu ms\n", pr_name, getpid(), inter_arrival_ns/NSEC_PER_MSEC);
}

// Function for real-time workload
static void inline do_workload(void)
{
    uint64_t start_ns, runtime_ns, currt_ns, runtime_adjust_ns;
    int dmiss_prob;

    start_ns = get_cpu_time();

    runtime_ns = task_wcet_ms * NSEC_PER_MSEC;
    runtime_adjust_ns = runtime_ns * RUNTIME_ADJUST_PCT / 100;

#if DMISS_PROB
    dmiss_prob = get_random(0, 99);
    if (dmiss_prob < DMISS_PROB) {
        runtime_ns += get_random(5, 50) * NSEC_PER_MSEC;
	/* ignore the margin for this case, as we want to simulate a deadline miss */
	runtime_adjust_ns = runtime_ns;
    }
#endif

    while(1) {
        currt_ns = get_cpu_time() - start_ns;
        if (currt_ns >= runtime_adjust_ns) break;
    }

    printf("%s(%d) completed workload of %lu ms\n",
           pr_name, getpid(), currt_ns/NSEC_PER_MSEC);
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s name mit_in_ms wcet_in_ms\n", argv[0]);
        exit(1);
    }

    task_mit_ms = atol(argv[2]);
    task_wcet_ms = atol(argv[3]);

    prctl(PR_SET_NAME, (unsigned long)argv[1], 0, 0, 0);
    prctl(PR_GET_NAME, pr_name, 0, 0, 0);

    printf("%s(%d) with mit %lu ms & wcet %lu ms(adjusted %lu ms) & dmiss_prob %d %%\n",
           pr_name, getpid(), task_mit_ms, task_wcet_ms,
           task_wcet_ms * RUNTIME_ADJUST_PCT / 100, DMISS_PROB);

    srand(time(NULL));

#if WAIT_FOR_TIMPANI_N
    printf("Waiting for timpani-n to start...\n");
    while (1) {
        int fd = shm_open("/timpani_ttsched", O_RDONLY, 0666);
        if (fd >= 0) {
            close(fd);
            break;
	}
        usleep(1000);
    }
#endif

    while (1) {
        get_cbs_event();

        do_workload();
    }

    return EXIT_SUCCESS;
}
