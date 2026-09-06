/*
Block chaining arena allocator
Basic implementation for the purposes of the demo.
*/

typedef struct ArenaBlock ArenaBlock;

struct ArenaBlock {
	ArenaBlock *next_block;
	u64 allocation_size;
	u64 data_size;
	u8 *allocation;
	u8 *low;
	u8 *high;
};

typedef struct {
	ArenaBlock *first_block;
	ArenaBlock *block;
	u8 *low;
	u8 *high;
} Arena;

typedef struct {
	Arena *arena;
	ArenaBlock *block;
	u8 *low;
	u8 *high;
} ArenaMark;

void arena_free(Arena *arena) {
	ArenaBlock *block = arena->first_block;
	while (0 != block) {
		ArenaBlock *next = block->next_block;
		munmap(block->allocation, block->allocation_size);
		block = next;
	}

	memzero(arena, sizeof(*arena));
}

void arena_reset(Arena *arena) {
	arena->block = arena->first_block;
	if (arena->block) {
		arena->low = arena->block->low;
		arena->high = arena->block->high;
	}
}

ArenaMark arena_mark(Arena *arena) {
	return (ArenaMark) {
		.arena = arena,
		.block = arena->block,
		.low = arena->low,
		.high = arena->high,
	};
}

void arena_restore(ArenaMark mark) {
	Arena *arena = mark.arena;

	// Need special case for mark taken before any blocks were allocated
	if (0 == mark.block) {
		arena_reset(arena);
		return;
	}

	arena->block = mark.block;
	arena->low = mark.low;
	arena->high = mark.high;
}

void * arena_alloc_aligned_uninit(Arena *arena, u64 size, u64 alignment) {
	i64 result;

	assert(is_power_of_two(alignment));
	assert(size < GB(8)); // sanity check

	#ifdef LUNAR_INCLUDE_ASAN_IMPL
	if (size < 8) size = 8;
	if (alignment < 8) alignment = 8;
	#endif

	// Underflow in the case that arena->high == 0 is intentional, and results in allocation of the first arena block
	arena->high = (u8*)(((u64)arena->high - size) & ~(alignment - 1));

	if ((i64)arena->high < (i64)arena->low) {
		ArenaBlock *previous_block = arena->block;
		ArenaBlock *candidate = arena->block ? arena->block->next_block : 0;
		while (0 != candidate && candidate->data_size < size) {
			previous_block = candidate;
			candidate = candidate->next_block;
		}

		if (0 == candidate) {
			u64 page_size = sysconf(_SC_PAGESIZE);

			u64 data_size = 0;
			if (previous_block) data_size = previous_block->allocation_size * 2;
			else data_size = KB(16);
			data_size = align_to(data_size, page_size);
			while (data_size < size * 2) data_size *= 2;

			u64 allocation_size = data_size + page_size;
			candidate = mmap(0, allocation_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			assert(candidate != MAP_FAILED); // allocation failure is a crashing error
			candidate->allocation_size = allocation_size;
			candidate->data_size = data_size;
			candidate->allocation = (u8*)candidate;
			candidate->low = (u8*)candidate + page_size;
			candidate->high = candidate->low + data_size;

			// First page stores arena state - make read only to catch accidental errors
			result = mprotect(candidate, page_size, PROT_READ);
			assert(0 == result); // this should always succeed. What does it even mean to fail?

			if (0 == arena->first_block) arena->first_block = candidate;

			if (previous_block) {
				// First page is read only, need to mark it unprotected to update the block pointers
				result = mprotect(previous_block, page_size, PROT_READ | PROT_WRITE);
				assert(0 == result); // this should always succeed. What does it even mean to fail?

				previous_block->next_block = candidate;

				result = mprotect(previous_block, page_size, PROT_READ);
				assert(0 == result); // this should always succeed. What does it even mean to fail?
			}
		}

		arena->low = candidate->low;
		arena->high = candidate->high;
		arena->block = candidate;

		arena->high = (u8*)(((u64)arena->high - size) & ~(alignment - 1));
	}

	return arena->high;
}

void * arena_alloc_aligned(Arena *arena, u64 size, u64 alignment) {
	void *memory = arena_alloc_aligned_uninit(arena, size, alignment);
	memzero(memory, size);
	return memory;
}

void * arena_alloc(Arena *arena, u64 size) {
	return arena_alloc_aligned(arena, size, 16);
}

