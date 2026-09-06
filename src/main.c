#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <asm/prctl.h>
#include <linux/futex.h>
#include <linux/sched.h> 
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <fcntl.h>

// Number of u64s to sort - increase to make the CPU do more work
#define SORT_COUNT 200000000

// Use 0 to default to [machine's number of cores] - 2
#define THREADS_TO_SPAWN 1

typedef char i8;
typedef short i16;
typedef int i32;
typedef long int i64;
typedef unsigned char u8;
typedef unsigned short int u16;
typedef unsigned int u32;
typedef unsigned long int u64;
typedef float f32;
typedef u32 b32;

#define true 1
#define false 0

void println(u8 *text, ...);

// Initialized in main
u64 CACHE_LINE_SIZE = 0;
u64 DEFAULT_PAGE_SIZE = 0;

#include "util.c"
#include "arena.c"
#include "threads.c"
#include "println.c"





/*
Perform a parallel radix sort on the given unsorted data.
len should be the number of u64s in the buffer.
Be aware that radix sort requires scratch memory of equal size to the
input memory, which will be allocated on thread 0's arena.
*/
void radix_sort_64(volatile u64 *data, u64 len) {
	ArenaMark mark = arena_mark(thread_arena());

	/*
	Parallel radix sort requires counting the number of values with a particular
	digit, then computing the offset of each digit in the sorted data based on
	the counts of other digit frequencies from step one.

	Strategy: Give each thread a chunk of memory to count, share the counts,
	compute offsets for each digit in sorted memory, then each thread takes
	a chunk of digits to copy. Repeat for each byte in the u64.
	*/

	// Working memory, integers are copied between scratch and data as the sort proceeds.
	volatile u64 *scratch = thread_alloc_shared(sizeof(u64) * len);

	// Each thread stores its local digit frequency counts in its own memory,
	// which is shared with other threads via these shared arrays.
	u64 **wave_digit_counts = thread_alloc_shared(sizeof(u64) * thread_count());
	u64 **wave_digit_offsets = thread_alloc_shared(sizeof(u64) * thread_count());

	// Thread local memory to compute digit counts for this thread's chunk of data
	u64 *digit_counts = thread_alloc(sizeof(u64) * 256);
	u64 *digit_offsets = thread_alloc(sizeof(u64) * 256);

	// Share the local arrays
	wave_digit_counts[thread_id()] = digit_counts;
	wave_digit_offsets[thread_id()] = digit_offsets;

	for (u64 digit_idx = 0; digit_idx < 8; digit_idx++) {
		u64 shift = 8 * digit_idx;

		memset(digit_counts, 0, sizeof(u64) * 256);

		/*
		See also: https://mgarland.org/files/papers/gpusort-ipdps09.pdf
		On the GPU it is faster for each lane to pre-sort its slice
		before the scatter stage. I don't know if this would extend
		to the comparatively fewer lanes available on the CPU.
		It would make the scatter stage only scatter forward through
		memory - consecutive numbers would never scatter to a previous
		memory location in the scratch buffer. Additionally, it would
		increase the likelihood of a scatter address already being in
		cache due to an earlier write.
		On the other hand, a pre-sort would be expensive and the segments
		I'm currently using are much larger than those used in the paper.
		Maybe if we used more segments than threads and each thread handled
		multiple segments. That way the pre-sort would be on a smaller data set.
		*/

		// Count the number of occurrences of each digit
		Range32 range = thread_slice(len);
		for (u64 i = range.min; i < range.max; i++) {
			u64 number = data[i];
			digit_counts[(number >> shift) & 0xff] += 1;
		}
		thread_barrier();

		// Assign each lane a segment of the output array to write their sorted numbers to.
		// Some of this work could also be parallelized if you compute relative offsets first,
		// and then only narrow to a single thread to turn the relative offsets into absolute
		if (0 == thread_id()) {
			u64 counter = 0;
			for (u64 j = 0; j < 256; j++) {
				for (u64 i = 0; i < thread_count(); i++) {
					wave_digit_offsets[i][j] = counter;
					counter += wave_digit_counts[i][j];
				}
			}
		}
		thread_barrier();

		// Move the data to the correct bins
		for (u64 i = range.min; i < range.max; i++) {
			u64 number = data[i];
			u8 digit = (number >> shift) & 0xff;
			u64 dest_offset = digit_offsets[digit];
			scratch[dest_offset] = number;
			digit_offsets[digit] += 1;
		}
		thread_barrier();

		volatile u64 *temp = scratch;
		scratch = data;
		data = temp;
	}

	arena_restore(mark);
}






void thread_entry(void *unused) {
	u64 start = __builtin_ia32_rdtsc();
	trace("entry");


	/*
	Init a PRNG with a random seed.
	Note that each lane gets its own PRNG, which it will use to populate
	the buffer with random data to sort.
	We could just source the entire dataset from /dev/urandom, but I wanted
	a reason to multi-thread the random number generation.
	Who doesn't love a good random data sort? I can hardly sit still.
	*/
	Rand32 rand = {0};
	i64 rand_source = open("/dev/urandom", O_RDONLY);
	assert(0 <= rand_source);
	i64 result = read(rand_source, &rand, sizeof(Rand32));
	assert(sizeof(Rand32) == result);
	close(rand_source);

	// This array is shared across all threads
	u64 *data = thread_alloc_shared(sizeof(u64) * SORT_COUNT);

	// Fill memory with random data
	// thread_slice gives each thread a different segment of memory to fill
	Range32 range = thread_slice(SORT_COUNT);
	for (u32 i = range.min; i < range.max; i++) {
		data[i] = rand_u64(&rand);
	}
	thread_barrier(); // Can't start sorting until all the memory has been filled.

	radix_sort_64(data, SORT_COUNT);

	// Print the first few entries. If they're moderately low, it (probably) worked!
	if (0 == thread_id()) {
		for (u64 i = 0; i < 20; i++) {
			println("%y", data[i]);
		}
	}
	thread_barrier();

	u64 end = __builtin_ia32_rdtsc();
	u64 duration = end - start;
	u64 frequency = get_tsc_frequency();
	// no divide by zero, CPU's without support for cpuid leaf 15 return 0 from get_tsc_frequency()
	if (0 == frequency) frequency = 100000000000000;

	ThreadContext *ctx = thread_context();
	println("Total time: %lcy, %lms, Barriers: %l, Sleep: %l", duration, (1000 * duration) / frequency, ctx->barrier_calls, ctx->futex_sleeps);
}




void main() {
	CACHE_LINE_SIZE = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
	DEFAULT_PAGE_SIZE = sysconf(_SC_PAGESIZE);

	u64 cores = THREADS_TO_SPAWN;
	if (0 == cores) {
		cores = thread_core_count();
		if (2 < cores) cores -= 2;
	}

	printf("Spawning %d threads\n", cores);

	thread_storage_init();
	thread_create_group(cores, thread_entry, 0);
	syscall(SYS_exit, 0); // exit only the current thread, letting the others complete
}