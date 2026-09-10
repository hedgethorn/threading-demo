# Fork/join multithreading demo

Demo implementation of GPU style fork/join multi-threading on the CPU.
This is similar to the programming model that GPUs use to execute compute shaders.
(Although this demo doesn't do any SIMD, which is definitely also part of GPU execution.)

- All threads execute the same code
- Threads divide out work among themselves with thread_slice or thread_queue
- Occasionally, after handing separate parts of the task, they synchronize at an explicit barrier
- Thread_sync provides a small broadcast mechanism to exchange values between threads

The demo code performs a multi-threaded radix sort over a chunk of random 64 bit integers.

Set SORT_COUNT in main.c to a large enough number to take a noticeable amount of time to execute
on your system and change the number of threads by providing an argument to the executable.

```bash
target/main 1 # 1 thread
target/main 2 # 2 threads
target/main 3 # 3 threads
# ...etc
```

The radix implementation is not terribly good, and is severely memory bandwidth constrained on most systems. A real implementation would likely want to tile the scatter stage by the CPU's cache size to reduce the likelihood of every write touching an uncached cache line. It works well enough to demonstrate the threading code, though.

Also, I implemented the thread barrier myself instead of using pthread_barrier. There was no real reason for this, and I make no apology. It was fun.

Use "./build.sh" to build, "./build.sh run" to build and run, or "./build.sh watch" to build, run and repeat when source code is changed.


# Requirements

Platform: Linux/x86-64
Compiler: GCC/Clang
Language: C99 + compiler extensions + x86-64 assembly


# The programming model

```c
// All threads execute this entrypoint at the same time.
void thread_entry() {
	// Each thread can check its thread_id, which ranges from 0 to thread_count-1
	printf("Hello, from thread %d", thread_id());

	// All threads have their own variables
	// (But since they all execute this exact same code, their values will all be identical by default)
	u64 value_count = 1024;
	u64 *values = 0;

	// A single thread can execute some code
	if (0 == thread_id()) {
		values = arena_alloc(thread_arena(), sizeof(*values) * value_count);
	}

	// A value can be broadcast from one thread to all other threads
	thread_sync_ptr(0, &values);
	// Now all threads share the same `*values` memory, copied from thread 0.

	// thread_slice divides work evenly among all threads
	Range32 range = thread_slice(value_count);

	// Each thread receives a different min and max
	for (u32 i = range.min; i < range.max; i++) {
		// Even though the values pointer is shared memory, each thread is
		// accessing a disjoint range so no synchronization is necessary.
		values[i] = i * 12;
	}

	// Ensures all previous operations are complete and visible to all threads before continuing
	thread_barrier();

	// Since synchronization is expensive, it sometimes makes sense to
	// compute the same thing on all threads rather than narrowing to
	// a single thread and sharing the result.
	u32 total = 0;
	for (u32 i = 0; i < value_count; i++) {
		total += values[i];
	}

	// You aren't limited to the thread_* functions exclusively.
	// Atomics are also an option for when it makes sense to split out to multiple threads.
	u32 *global_total = thread_alloc_shared(4); // helper for the "0 == thread_id()" allocation above

	u32 local_total = 0;
	for (u32 i = range.min; i < range.max; i++) {
		local_total += 1;
	}

	// atomic operations provide the necessary ordering for concurrent access to the same memory without a full barrier
	__sync_fetch_and_add(global_total, local_total);

	// You still need a barrier to ensure all totals have been merged through the atomic, though.
	thread_barrier();

	// This (non atomic) read of global_total is safe. See the readme for more information.
	printf("Thread %d observes %d", thread_id(), *global_total);
}
```


# Some notes when experimenting with the code

- Most CPUs have SMT (hyperthreading), where two threads are executed on the same physical core. Since they're on the same physical core, they share the same memory bandwidth.

  As the radix sort is primarily memory bandwidth constrained, you'll observe very little performance improvement switching from one thread to two threads if those two threads are both scheduled on the same physical core, so check on that if you aren't seeing improvement when increasing the thread count.

- You're also likely to hit your CPU's total memory bandwidth before maxing out the number of cores. I have retroactively decided that this is intentional, and meant to demonstrate an issue that often comes up when multi-threading.

- The printf output includes millisecond time measurements. These depend on reading the timestamp counter frequency from CPUID leaf 15, which is only implemented on Skylake CPUs or newer.


# FAQs

## Why are there no atomics?

There are atomics, in both the job queue and the thread_barrier implementation. They use the GCC/clang built-in \__sync* functions rather than stdatomic.h because this code is intentionally written as c99 and stdatomic.h was not introduced until c11.

A potentially more interesting question is why it doesn't use the \__atomic* compiler extensions instead of \__sync*. I don't like the \__atomic* extensions, and that's the only reason.

## Why, in the radix sort, is the shared histogram or shared scratch memory, not volatile?

Both pointers are potentially aliasing pointers, since they are ultimately sourced from the arena stored in the thread_context, which itself came from an asm block. Since the arena_alloc function does not indicate that its returned pointer does *not* alias [via __attribute__((malloc))], the compiler must assume that other pointers in the program *could* alias it and thus cannot eliminate loads or stores entirely, and cannot re-order loads or stores across a memory barrier. The compiler *could* perform write combining or load elimination between memory barriers, but that's not an issue since threads do not access memory from other threads via this pointers without crossing a memory barrier.

## Why does thread_sync not take a volatile pointer to the value?

Because the value pointer is thread-local. Multiple threads never write from or read to that pointer. The only shared memory in the thread_sync operation is the region pointed to by the broadcast_memory pointer in the ThreadContext structure.

## Why is broadcast_memory not marked as volatile?

The broadcast_memory pointer is considered by the compiler a potentially aliasing pointer. The compiler is unable to eliminate loads or stores to it, or re-order loads & stores across a memory barrier. Unlike volatile, the compiler *is* able to perform write-combining optimizations that do not cross a memory barrier, which is fine because thread_sync only exchanges memory after crossing a memory barrier.

broadcast_memory is considered an escaped pointer by the thread_sync function because it is accessed via a pointer to the thread context, and the pointer to the thread context is sourced from an asm block.

## Why are there no fences in the thread_barrier implementation?

The thread_barrier implementation uses atomic operations to provide the necessary compiler and CPU ordering, and the futex semantics to provide thread synchronization. (The \__sync* family of functions establish sequential ordering in the language of the c11 memory model.)

## You assign to join_barrier without an atomic access in the thread_barrier

The assignment only occurs on the last thread to enter the thread_barrier, as tracked by the atomic increment just before that.

The assignment cannot be ordered prior to the atomic increment, as the atomic operation requires sequentially consistent ordering with respect to all other memory operations, nor can it be ordered after the futex syscall, as that is a call to an external linkage function.

The CPU also cannot reorder the assignment on x86_64, due to its strong memory ordering.

Additionally, the \__sync* primitives used in this code do not provide a plain store operation.

## This seems really dangerous

Depends on your definition of dangerous. If you're working on the basis that you need to be multithreaded *somehow* it's either this solution or a different solution. My personal experience is that, when writing in the style where multiple threads are all running the same code, it is significantly easier to code multi-threading correctly than when using other solutions like raw mutexes or thread pools.

The reliance on barriers, for example, eliminates much of the opportunity for mistakes with atomics, or volatile access, deadlocks, etc.

Not having to represent your computations as instructions that can be submitted to a thread pool is convenient too.

It's probably not the most performant solution available, since the OS de-scheduling a single thread can cause the other threads to suspend at a barrier. However, it does get you multi-threading, which is a win over *not* multi-threading, and, as a bonus, without many of the aforementioned issues with other solutions. Plus, you can always *also* have a thread pool that you submit jobs to if you find it performs better for your problem.
