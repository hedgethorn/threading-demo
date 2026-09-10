#define U64C(n) n##UL

#define KB(n) (U64C(n) * U64C(1024))
#define MB(n) (U64C(n) * KB(1024))
#define GB(n) (U64C(n) * MB(1024))


/*
Print message with the source location of the message
*/
#define trace(msg) trace_impl(msg, __FILE__, __LINE__, __func__)
#define error(msg) error_impl(msg, __FILE__, __LINE__, __func__)

void trace_impl(const char *message, const char *file, const u64 line, const char *function) {
	println("\x1b[36;1mtrace\x1b[0m [\x1b[36m%s:%l\x1b[0m] \x1b[35m%s()\x1b[0m: %s", file, line, function, message);
}

void error_impl(const char *message, const char *file, const u64 line, const char *function) {
	println("\x1b[31;1merror\x1b[0m [\x1b[31;1m%s:%l\x1b[0m] \x1b[35m%s()\x1b[0m: %s", file, line, function, message);
}


/*
Memory helpers
*/
void memrev(u8 *start, u64 len) {
	u8 *end = start + len - 1;
	while (start < end) {
		u8 temp = *start;
		*start = *end;
		*end = temp;
		start += 1;
		end -= 1;
	}
}

void memzero(void *pointer, u64 len) {
	memset(pointer, 0, len);
}



/*
Integer Helpers
*/

u64 is_power_of_two(u64 value) {
	return 0 == (value & (value - 1));
}

u64 align_to(u64 value, u64 alignment) {
	if (!is_power_of_two(alignment)) {
		printf("aling_to called with non power of two\n");
		return value;
	}

	return (value + alignment - 1) & ~(alignment - 1);
}

b32 is_aligned_to(u64 value, u64 alignment) {
	assert(is_power_of_two(alignment));
	return value == align_to(value, alignment);
}



/*
Random Numbers
*/

typedef struct {
	u64 state;
	u64 inc;
} Rand32;


u32 rand_u32(Rand32 *rng) {
	/*
	*Really* minimal PCG32 code / (c) 2014 M.E. O'Neill / pcg-random.org
	Licensed under Apache License 2.0 (NO WARRANTY, etc. see website)
	*/
	u64 oldstate = rng->state;
	rng->state = oldstate * 6364136223846793005ULL + (rng->inc|1);
	u32 xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
	u32 rot = oldstate >> 59u;
	return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

// Random u64 via two random u32s
u64 rand_u64(Rand32 *rand) {
	u64 high = rand_u32(rand);
	u64 low = rand_u32(rand);
	return (high << 32) | low;
}



/*
Timestamp counter
*/


// Read a leaf from the CPU's cpuid data
void sys_cpuid(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
	__asm__ volatile (
		"cpuid"
		: "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		: "a"(leaf), "c"(subleaf)
	);
}


// Read the frequency of the timestamp counter using cpuid
// This only works on Skylake and later.
u64 get_tsc_frequency(void) {
	u32 eax, ebx, ecx, edx;

	sys_cpuid(0x15, 0, &eax, &ebx, &ecx, &edx);

	if (eax != 0 && ebx != 0) {
		u64 denominator = eax;
		u64 numerator = ebx;

		if (ecx != 0) {
			return (u64)ecx * numerator / denominator;
		}
	}

	return 0;
}
