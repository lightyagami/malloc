typedef unsigned long size_t;
#define NULL          ((void *)0)
#define SIZE_MAX      (~(size_t)0)
#define SYS_write_NR  1
#define SYS_mmap_NR   9
#define SYS_munmap_NR 11
#define SYS_exit_NR   60
#define STDOUT_FILENO 1
#define PROT_READ     0x01
#define PROT_WRITE    0x02
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void *)-1)

typedef struct block_meta {
  size_t size;
  size_t _pad;
} block_meta;
#define META_SIZE sizeof(block_meta)

static long sys_write(int fd, const void *buf, size_t count) {
  long ret;
  __asm__ __volatile__(
    "syscall"
    : "=a"(ret)
    : "a"(SYS_write_NR), "D"(fd), "S"(buf), "d"(count)
    : "rcx", "r11", "memory"
  );
  return ret;
}

static void *sys_mmap(void *addr, size_t length, int prot, int flags, int fd, long offset) {
  long ret;
  register long r10 __asm__("r10") = flags;
  register long r8  __asm__("r8")  = fd;
  register long r9  __asm__("r9")  = offset;
  __asm__ __volatile__(
    "syscall"
    : "=a"(ret)
    : "a"(SYS_mmap_NR), "D"(addr), "S"(length), "d"(prot), "r"(r10), "r"(r8), "r"(r9)
    : "rcx", "r11", "memory"
  );
  return (void *)ret;
}

static int sys_munmap(void *addr, size_t length) {
  long ret;
  __asm__ __volatile__(
    "syscall"
    : "=a"(ret)
    : "a"(SYS_munmap_NR), "D"(addr), "S"(length)
    : "rcx", "r11", "memory"
  );
  return (int)ret;
}

static void sys_exit(int status) {
  __asm__ __volatile__(
    "syscall"
    :
    : "a"(SYS_exit_NR), "D"(status)
    : "rcx", "r11", "memory"
  );
  __builtin_unreachable();
}

static size_t my_strlen(const char *s) {
  size_t len = 0;
  while (s[len]) len++;
  return len;
}

static void print_str(const char *str) {
  sys_write(STDOUT_FILENO, str, my_strlen(str));
}

static void print_hex(size_t val) {
  char buf[19];
  buf[0] = '0';
  buf[1] = 'x';
  buf[18] = '\n';
  const char *digits = "0123456789abcdef";
  for (int i = 17; i >= 2; i--) {
    buf[i] = digits[val & 0xF];
    val >>= 4;
  }
  sys_write(STDOUT_FILENO, buf, 19);
}

void *malloc(size_t size) {
  if (size == 0 || size > SIZE_MAX - META_SIZE)
    return NULL;
  size_t total = META_SIZE + size;
  block_meta *block = (block_meta *)sys_mmap(
    NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0
  );
  if (block == MAP_FAILED)
    return NULL;
  block->size = total;
  return (void *)(block + 1);
}

void free(void *ptr) {
  if (ptr == NULL)
    return;
  block_meta *block = (block_meta *)ptr - 1;
  sys_munmap(block, block->size);
}

void _start(void) {
  print_str("128 bytes\n");
  char *ptr = (char *)malloc(128);
  if (ptr) {
    print_str("Allocated at : ");
    print_hex((size_t)ptr);
    ptr[0] = 'H';
    ptr[1] = 'i';
    ptr[2] = '\n';
    sys_write(STDOUT_FILENO, ptr, 3);
    print_str("Deallocation\n");
    free(ptr);
  } else {
    print_str("Failed to Allocate\n");
  }
  sys_exit(0);
}
