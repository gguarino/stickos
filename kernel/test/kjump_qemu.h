/* kjump on QEMU vexpress-a9 (kernel/test/run.sh): the test kernel is built with
 * STRICT_DEVMEM and STRICT_KERNEL_RWX off; TABLE and NI come from its System.map */
#define MEMDEV  "/dev/mem"
#define AREA    0x6f400000ul
#define AREAOFF 0x6f400000ll
#define AREALEN 0xc00000ul
#define SHOWN   0x400000ul
#define PL310   0x1e00a000ul
#define FBSHOW  AREA
