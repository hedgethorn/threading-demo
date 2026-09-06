/*
Implements fork-join multi-threading, where multiple threads all execute the same code,
occasionally blocking at a barrier point until all threads reach it, and synchronizing
values to other threads with thread_sync*, and dividing work up among themselves
equally with thread_slice, or dynamically with thread_queue.
*/





/*
First some functions for storing and retrieving thread-local storage.
We'll use the gs register, since it's not used by anyone else on x86_64/linux with gcc or clang.
The group threading system only uses the THREAD_STORAGE_CONTEXT index, so all other indexes are
available to user code.
*/

#define THREAD_STORAGE_SIZE 4096

// thread local storage index of a thread's ThreadContext
#define THREAD_STORAGE_CONTEXT ((THREAD_STORAGE_SIZE / sizeof(void*)) - 1)

// Allocate memory for use with thread_storage_set_page
void* thread_storage_alloc() {
	void *tls_memory = mmap(0, THREAD_STORAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
	assert(0 != tls_memory); // can't continue without this
	return tls_memory;
}

// Set the memory that stores this thread's thread local storage
// Memory must be at least THREAD_STORAGE_SIZE
// You can use thread_storage_alloc to allocate memory for this
void thread_storage_set_page(void* tls_memory) {
	syscall(SYS_arch_prctl, ARCH_SET_GS, tls_memory);
}

// Initialize the thread-local storage system
// Must be called before using thread_storage_set or thread_storage_get
void thread_storage_init() {
	thread_storage_set_page(thread_storage_alloc());
}

// Stores a value in memory dedicated to this thread
// Each thread gets its own memory for these values
void thread_storage_set(u32 index, void* value) {
	assert(index * sizeof(void*) < THREAD_STORAGE_SIZE);
	__asm__("mov %[src], %%gs:(%[idx])" : : [src] "r"(value), [idx] "r"(index * 8));
}

// Get a value from thread-local storage
// Returns unique values for each thread.
void *thread_storage_get(u32 index) {
	assert(index * sizeof(void*) < THREAD_STORAGE_SIZE);
	void *value;
	__asm__("mov %%gs:(%[idx]), %[dest]" : [dest] "=r"(value) : [idx] "r"(index * 8));
	return value;
}




typedef struct {
	u32 thread_id; // id of the current thread
	u32 thread_count; // total number of threads participating in the group
	u32 barrier_calls; // count of number of times thread_barrier has been called
	u32 futex_sleeps; // how many times thread_barrier put the thread to sleep

	// Thread-local allocator
	Arena arena;

	// These point to memory shared across all threads in the group
	volatile u32 *join_barrier; // used in thread_barrier to block until all threads have reached the call
	volatile u32 *join_barrier_alt; // see note at end of thread_barrier() function.
	volatile u64 *broadcast_memory; // shared memory to copy data between threads

	// This data is used to setup the thread, then not used again
	u32 core; // core to pin this thread to
	void *thread_storage_memory;
	void (*entry)(void*);
	void *entry_data;
} ThreadContext;


// A context that can be used when there is only one thread and no groups.
// Makes it possible to execute multi-threaded code on a single thread.
ThreadContext STATIC_CONTEXT = { .thread_count = 1 };


// Retrieve the current thread's context
ThreadContext* thread_context() {
	ThreadContext *ctx = thread_storage_get(THREAD_STORAGE_CONTEXT);
	if (0 == ctx) return &STATIC_CONTEXT;
	else return ctx;
}


// Convenience method, retrieve the current thread's id
u32 thread_id() {
	return thread_context()->thread_id;
}


// Convenience method, retrieve the current group's thread count
u32 thread_count() {
	return thread_context()->thread_count;
}

// Convenience method, retrieve the current thread's arena
Arena * thread_arena() {
	return &thread_context()->arena;
}



// Get the number of cores on this system
u64 thread_core_count() {
	cpu_set_t set;
	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof set, &set) != 0) return -1;
	return CPU_COUNT(&set);
}


// Pin current thread to the given cpu
u64 thread_core_pin(u64 cpu) {
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof set, &set) != 0) return -1;
	return 0;
}


// Pause the current thread until all threads reach the barrier
#define thread_barrier() thread_barrier_impl(__FILE__, __LINE__, __FUNCTION__)

void thread_barrier_impl(const u8 *file, u32 line, const u8 *function) {
	ThreadContext *ctx = thread_context();
	ctx->barrier_calls += 1;
	if (1 == ctx->thread_count) return;

	u64 current_count = 1 + __sync_fetch_and_add(ctx->join_barrier, 1);

	// Is this the last thread to reach the barrier?
	if (current_count == ctx->thread_count) {
		// Reset the barrier
		*ctx->join_barrier = 0;

		// Wakeup other threads
		// todo: optimize wakeup - other threads can indicate whether they went to sleep,
		// and we can do the syscall only if they did.
		// For now this is the obviously-correct implementation
		__sync_synchronize();
		u64 result = syscall(SYS_futex, ctx->join_barrier, FUTEX_WAKE, ctx->thread_count - 1);
		assert(0 <= result); // If this failed, the threads may be out of sync and there's no recovery.
	}

	// Otherwise threads still need to reach the barrier
	// (and/or, this being multi-threaded code, the final thread has
	// already run, woken all the other threads, and everyone else
	// has already continued and are waiting for us at the next barrier.)
	else {
		// Should profile in a real application.
		// The Windows Win32 barrier loops 2000 times, so that's probably good as a default.
		i64 loops = 2000;

		while (1) {
			u64 current_count = *ctx->join_barrier;

			if (0 == current_count) {
				// The final thread woke everyone else up.
				break;
			} else {
				// It's possible for the kernel to wake a thread blocked on a futex without the value changing.
				if (loops < 0) {
					trace("barrier spurious wake");
				}

				// If we've spun too long, then we should sleep the thread
				if (loops <= 0) {
					ctx->futex_sleeps += 1;
					i64 result = syscall(SYS_futex, ctx->join_barrier, FUTEX_WAIT, current_count, 0);

					// Anything other than success and a spurious wake can't be recovered from.
					assert(0 == result || EAGAIN == errno);
				} else {
					__builtin_ia32_pause();
				}

				loops -= 1;
			}
		}

		// Prevent re-ordering the swaps below prior to this if branch
		__sync_synchronize();
	}

	/*
	two join barriers are required under the following timeline:
		1. Lane 1 reaches the thread_barrier first and performs the atomic increment on join_barrier
		2. Before id 1 reaches its futex_wait call, all the other threads reach the barrier.
		3. Still before id 1 reaches its futex_wait call, the final thread has now incremented and performed the futex_wake to wake all other threads
		4. Still before id 1 reaches its futex_wait call, all the other threads are not continuing.
		5. Still before id 1 reaches its futex_wait call, another thread reaches the next thread_barrier and increments the join_barrier.
		6. Finally id 1 reaches its futex_wait call and the kernel sees the join_barrier matching the value thread 1 expected it to have, and suspends thread 1.
		7. Now thread 1 is still at the first thread_barrier, but all the other threads are blocking on the *next* thread_barrier.
	To resolve this, each thread swaps join_barrier and join_barrier_alt at the end of each thread_barrier call.
	*/
	volatile u32 *temp = ctx->join_barrier;
	ctx->join_barrier = ctx->join_barrier_alt;
	ctx->join_barrier_alt = temp;
}

// Copy 64 bits of data from the given thread to all threads
void thread_sync64(u32 thread_id, u64 *value) {
	ThreadContext *tls = thread_context();
	if (tls->thread_id == thread_id) *tls->broadcast_memory = *value;
	thread_barrier();
	if (tls->thread_id != thread_id) *value = *tls->broadcast_memory;
	thread_barrier();
}

// Copy 32 bits of data from the given thread to all threads
void thread_sync32(u32 thread_id, u32 *value) {
	ThreadContext *tls = thread_context();
	if (tls->thread_id == thread_id) *tls->broadcast_memory = *value;
	thread_barrier();
	if (tls->thread_id != thread_id) *value = *tls->broadcast_memory;
	thread_barrier();
}

// Convenience wrapper for thread_sync32
void thread_sync_f32(u32 thread_id, f32 *value) {
	thread_sync32(thread_id, (u32*)value);
}

// Convenience wrapper for thread_sync64
void thread_sync_ptr(u32 thread_id, void *value) {
	thread_sync64(thread_id, (u64*)value);
}

// Sync bytes from one thread to the others
#define thread_sync_bytes(thread_id, memory, size) thread_sync_bytes_impl(thread_id, memory, size, __FILE__, __LINE__, __FUNCTION__)
void thread_sync_bytes_impl(u32 thread_id, void *memory, u64 size, const u8 *file, u64 line, const u8 *function) {
	ThreadContext *tls = thread_context();
	if (tls->thread_id == thread_id) *tls->broadcast_memory = (u64)memory;
	thread_barrier_impl(file, line, function);
	if (tls->thread_id != thread_id) memcpy(memory, (void*)*tls->broadcast_memory, size);
	thread_barrier_impl(file, line, function);
}




typedef struct { u32 min, max, len; } Range32;

// Takes a count of items and returns the slice of that count that this
// thread is responsible for performs no synchronization, but doesn't deal
// with elements taking uneven amounts of work. See thread_queue() for a
// solution that can better handle elements taking different amounts of work.
Range32 thread_slice(u32 count) {
	u32 per_thread = count / thread_count();
	u32 remainder = count % thread_count();

	Range32 result = {0};

	if (thread_id() < remainder) {
		result.min = (per_thread + 1) * thread_id();
		result.max = result.min + per_thread + 1;
	}
	else {
		result.min = (per_thread + 1) * remainder + per_thread * (thread_id() - remainder);
		result.max = result.min + per_thread;
	}

	result.len = result.max - result.min;

	return result;
}

// Same as thread_slice, but ensures that range is a multiple of the group size,
// placing the remainder in thread 0. Useful for dividing SIMD work that needs to be a multiple of the SIMD size.
// (note: might be changed to divide the remainder up among threads instead of giving it all to thread 0.)
void thread_slice_grouped(
	u32 count,
	u32 group_size,
	Range32 *range,
	Range32 *remainder
) {
	u32 clamped_count = count / group_size;
	*range = thread_slice(clamped_count);
	range->min *= group_size;
	range->max *= group_size;
	range->len *= group_size;

	if (0 == thread_id()) {
		u32 remainder_count = count - clamped_count;
		remainder->min = count - remainder_count;
		remainder->max = count;
		remainder->len = remainder->max - remainder->min;
	} else {
		memset(remainder, 0, sizeof(Range32));
	}
}


// Entrypoint for a spawned thread, setups context and calls the user provided thread entry
void thread_setup_entry(void *void_context) {
	ThreadContext *ctx = void_context;
	syscall(SYS_arch_prctl, ARCH_SET_GS, ctx->thread_storage_memory);
	thread_storage_set(THREAD_STORAGE_CONTEXT, ctx);
	thread_core_pin(ctx->core);
	thread_barrier(); // prevent threads from proceeding unless all threads in the group are ready
	ctx->entry(ctx->entry_data);
	syscall(SYS_exit, 0);
}


// Calling syscall(SYS_clone3) fails because the syscall function returns to the caller.
// Returning to the caller fails on the child as it has a fresh, empty stack with no return address on it.
extern i64 clone3_jmp(struct clone_args *clone_args, u64 clone_args_size, void(*entry)(void*), void *data);
__asm__(
	"clone3_jmp:\n"
		// the clone3 syscall
		"mov %rcx, %r8\n"
		"mov $0x1b3, %rax\n"
		"syscall\n"
		// return in parent, jmp to lx_clone3jmp_child in child
		"cmp $0, %rax\n"
		"jz clone3_jmp_child\n"
		"ret\n"
	"clone3_jmp_child:\n"
		"mov %r8, %rdi\n"
		"call *%rdx\n"
		"ud2\n" // last-ditch save in case child function ever returns
);


// Spawn thread_count threads, calling the given entrypoint in each
// with the given data pointer as an argument.
// Returns true on success, false on failure.
// The calling thread does not participate in the group.
b32 thread_create_group(u32 thread_count, void(*entry)(void*), void *entry_data) {
	u64 core_count = thread_core_count();
	u64 shared_memory_size = 3 * CACHE_LINE_SIZE;
	u64 thread_context_size = thread_count * sizeof(ThreadContext);

	u64 group_memory_size = shared_memory_size + thread_context_size;
	u8 *group_memory = mmap(0, group_memory_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
	if (MAP_FAILED == group_memory) {
		error("group spawn failed");
		return false;
	}

	u32 *join_barrier       = (void*)(group_memory + CACHE_LINE_SIZE * 0);
	u32 *join_barrier_alt   = (void*)(group_memory + CACHE_LINE_SIZE * 1);
	u64 *broadcast_memory   = (void*)(group_memory + CACHE_LINE_SIZE * 2);
	ThreadContext *contexts = (void*)(group_memory + CACHE_LINE_SIZE * 3);

	for (u32 i = 0; i < thread_count; i++) {
		// Allocate stack memory
		u64 stack_size = MB(4);
		void *stack_memory_low = mmap(0, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_NORESERVE, 0, 0);
		if (MAP_FAILED == stack_memory_low) {
			error("stack alloc failed");
			munmap(group_memory, group_memory_size);
			// todo: kill threads that have already started
			// todo: place a thread barrier in each thread before entry is called
			// so that none begin executing until all threads have been created
			return false;
		}

		// Guard the low page to trigger a fault if the stack grows too far
		if (0 != mprotect(stack_memory_low, DEFAULT_PAGE_SIZE, PROT_NONE)) {
			error("failed to protect low stack page");
			munmap(group_memory, group_memory_size);
			munmap(stack_memory_low, stack_size);
			return false;
		}

		// Allocate thread local-storage

		ThreadContext *ctx = contexts + i;
		ctx->thread_id = i;
		ctx->thread_count = thread_count;
		ctx->join_barrier     = (u32*)(group_memory + CACHE_LINE_SIZE * 0);
		ctx->join_barrier_alt = (u32*)(group_memory + CACHE_LINE_SIZE * 1);
		ctx->broadcast_memory = (u64*)(group_memory + CACHE_LINE_SIZE * 2);
		ctx->thread_storage_memory = thread_storage_alloc();
		ctx->entry = entry;
		ctx->entry_data = entry_data;
		ctx->core = i % core_count;

		struct clone_args clone_args = {0};
		clone_args.flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
		clone_args.stack = (u64)stack_memory_low;
		clone_args.stack_size = stack_size;
		assert(0 != clone_args.stack);

		i64 result = clone3_jmp(&clone_args, sizeof(clone_args), thread_setup_entry, ctx);
		assert(0 < result);
	}
}


// Convenience method, allocate on the current thread's arena
void * thread_alloc(u64 size) {
	return arena_alloc(thread_arena(), size);
}


// Allocate memory accessible to all threads in the group
// Allocates on thread 0 and syncs the address to all other threads
void* thread_alloc_shared(u64 size) {
	void *data = 0;
	if (0 == thread_id()) data = arena_alloc(thread_arena(), size);
	thread_sync_ptr(0, &data);
	return data;
}


// Gather different values from each thread and make all values accessible to all threads
// Allocates on thread 0 and shares the buffer with all other threads for the copy,
// then returns that buffer in all threads.
u64* thread_gather64(u64 value, Arena *arena) {
	u64 *data = 0;
	if (0 == thread_id()) {
		data = arena_alloc(arena, sizeof(u64) * thread_count());
	}
	thread_sync_ptr(0, &data);

	data[thread_id()] = value;

	thread_barrier();

	return data;
}


struct JobQueue {
	volatile i32 *remaining;
	i32 allocated_from;
	i32 allocated_to;
};

// Setup a work-stealing job queue with the given number of elements
// todo: could take an initial 'take' amount to avoid a barrier
// on the first call to thread_queue_next, and shortcut the entire synchronization
// mechanism if the request amount is less than the total.
struct JobQueue thread_queue(u32 count, Arena *arena) {
	volatile i32 *remaining = 0;
	if (0 == thread_id()) {
		remaining = arena_alloc(arena, CACHE_LINE_SIZE);
		*remaining = count;
	}
	thread_sync_ptr(0, &remaining);

	return (struct JobQueue) {
		.remaining = remaining,
		.allocated_from = 0,
		.allocated_to = 0,
	};
}

// Get the next job from the job queue
// Uses the already allocated range or allocates a new range from the shared set of jobs
// Returns false when there are no more jobs
b32 thread_queue_next(struct JobQueue *job, u32 size, u32 *next) {
	if (job->allocated_from < job->allocated_to) {
		*next = job->allocated_from++;
		return true;
	}

	i32 old_value = __sync_fetch_and_sub(job->remaining, size);
	if (old_value <= 0) return false;

	job->allocated_to = old_value;
	job->allocated_from = old_value - size;
	if (job->allocated_from < 0) job->allocated_from = 0;
	*next = job->allocated_from++;
	return true;
}
