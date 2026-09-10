/*
Crappy printf that prefixes messages with the thread number
and ensures the entire message is printed at once without
interleaving characters from other lanes.

libc's printf, or at least the implementation I'm using, is not
thread safe if you are not using the pthread library.
I believe there's some flag you need to set to indicate you're
running multi-threaded and enable locks, but haven't investigated enough.

Supported format specifiers:
	%d - u32
	%l - u64
	%x - u32 in base 16
	%y - u64 in base 16
	%b - b32 as "true" or "false"
	%s - zero terminated string
*/



#define PRINTLN_BUFFER_SIZE 256


u64 u64_to_ascii(u64 value, u8 buffer[20], u64 radix) {
	u8 digits[] = "0123456789abcdef";

	if (0 == radix) radix = 1;
	if (sizeof(digits) < radix) radix = sizeof(digits);

	u64 length = 0;

	if (value == 0) {
		buffer[0] = '0';
		length += 1;
	}

	while (0 < value) {
		buffer[length] = digits[value % radix];
		value /= radix;
		length += 1;
	}

	memrev(buffer, length);

	return length;
}


void println(char *text, ...) {
	ThreadContext *ctx = thread_context();

	va_list args;
	va_start(args, text);

	u8 output[PRINTLN_BUFFER_SIZE] = {0};
	u64 output_len = 0;

	if (1 < ctx->thread_count) {
		output[output_len++] = '\x1b';
		output[output_len++] = '[';
		output[output_len++] = '2';
		output[output_len++] = 'm';
		output[output_len++] = '[';
		if (ctx->thread_id < 10) output[output_len++] = ' ';
		output_len += u64_to_ascii(ctx->thread_id, output + output_len, 10);
		output[output_len++] = ']';
		output[output_len++] = ':';
		output[output_len++] = '\x1b';
		output[output_len++] = '[';
		output[output_len++] = '0';
		output[output_len++] = 'm';
		output[output_len++] = ' ';
	}

	u64 len;
	while (0 != *text) {
		len = 0;

		while (0 != text[len] && '%' != text[len]) {
			len += 1;
		}

		assert(output_len + len <= PRINTLN_BUFFER_SIZE);

		memcpy(output + output_len, text, len);
		output_len += len;
		text += len;
		if (0 == *text) break;
		text += 1;

		switch (*text) {
			case 'd': {
				assert(output_len + 20 <= PRINTLN_BUFFER_SIZE);
				u32 value = va_arg(args, u32);
				u64 len = u64_to_ascii(value, output + output_len, 10);
				output_len += len;
				text += 1;
			} break;

			case 'l': {
				assert(output_len + 20 <= PRINTLN_BUFFER_SIZE);
				u64 value = va_arg(args, u64);
				u64 len = u64_to_ascii(value, output + output_len, 10);
				output_len += len;
				text += 1;
			} break;

			case 'x': {
				assert(output_len + 18 <= PRINTLN_BUFFER_SIZE);
				output[output_len++] = '0';
				output[output_len++] = 'x';
				u32 value = va_arg(args, u32);
				u64 len = u64_to_ascii(value, output + output_len, 16);
				output_len += len;
				text += 1;
			} break;

			case 'y': {
				assert(output_len + 18 <= PRINTLN_BUFFER_SIZE);
				output[output_len++] = '0';
				output[output_len++] = 'x';
				u64 value = va_arg(args, u64);
				u64 len = u64_to_ascii(value, output + output_len, 16);
				output_len += len;
				text += 1;
			} break;

			case 'b': {
				assert(output_len + 5 <= PRINTLN_BUFFER_SIZE);
				if (va_arg(args, b32)) {
					memcpy(output + output_len, "true", 4);
					output_len += 4;
				}
				else {
					memcpy(output + output_len, "false", 5);
					output_len += 5;
				}

				text += 1;
			} break;

			case 's':  {
				char *string = va_arg(args, char*);
				u64 len = strlen(string);
				assert(output_len + len <= PRINTLN_BUFFER_SIZE);
				memcpy(output + output_len, string, len);
				output_len += len;
				text += 1;
			} break;

			default: {
				assert(false);
			} break;
		}
	}

	assert(output_len + 1 < PRINTLN_BUFFER_SIZE);
	output[output_len++] = '\n';

	/*
	Linux guarantees a write syscall is atomic with respect to other write
	syscalls in the same thread group.
	*/
	assert(output_len <= PRINTLN_BUFFER_SIZE);
	write(1, output, output_len);

	va_end(args);
}
