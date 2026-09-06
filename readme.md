# Fork/join multithreading demo

Demo implementation of fork/join multi-threading on the CPU. (Without a threading library.)
This is roughly the programming model that GPUs use to execute code.
Although this demo doesn't do any SIMD, which is definitely also part of GPU execution.

- All threads execute the exact same code.
- Occasionally threads synchronize by sharing memory or waiting on a barrier.
- Threads can segment up work using thread_slice (for equal distribution) or thread_queue (for unequal distribution).

The demo performs a multi-threaded radix sort over a large chunk of random 64 bit integers.

Set SORT_COUNT to a large enough number to take a noticeable amount of time to execute
on your system and change THREADS_TO_SPAWN to see if the execution time changes as more
threads are added.

The synchronization overhead appears to lead to diminishing returns as thread number increases.
Further profiling is necessary to confirm the exact source of the diminishing returns.

There also appears to be minimal improvement between 1 and 2 threads or between 1 and 3 threads,
though exact behavior requires further profiling to identify.

Use "./build.sh" to build, "./build.sh run" to build and run, or "./build.sh watch" to build, run and repeat when source code is changed.

Note: Millisecond time measurements require Skylake CPUs or newer, otherwise only cycle counts are shown. Maybe try the time command.