/* Stack overrides for gateway build: Main/HTTP stacks are too small in on-chip
 * SRAM once Micro800/gateway pages and larger CPPCALL frames are linked.
 * Keep stacks in SDRAM so we can grow them without exhausting the 64KB SRAM. */

#define MAIN_TASK_STK_SIZE (8192)
#define HTTP_STK_SIZE (8192)
#define NO_FAST_MAIN_STACK
#define NO_FAST_HTTP_STACK
