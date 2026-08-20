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

#define ALIGN         16
#define ALIGN_UP(n)   (((n) + (ALIGN - 1)) & ~(size_t)(ALIGN - 1))
#define ARENA_SIZE    (128 * 1024)
typedef struct block_meta {
    size_t              size;
    struct block_meta  *next;
    struct block_meta  *prev;
    int                 free;
    int                 is_mmap;
    size_t              _pad0;
    size_t              _pad1;
} block_meta;

#define META_SIZE sizeof(block_meta)

_Static_assert(sizeof(block_meta) % 16 == 0, "block_meta size must be a multiple of 16 for payload alignment");

static block_meta *free_list = NULL;

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

static block_meta *request_arena(size_t min_size) {
    size_t chunk = ARENA_SIZE;
    if (min_size + META_SIZE > chunk)
        chunk = ALIGN_UP(min_size + META_SIZE);

    block_meta *block = (block_meta *)sys_mmap(
        NULL, chunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0
    );
    if (block == MAP_FAILED)
        return NULL;

    block->size    = chunk - META_SIZE;
    block->next    = NULL;
    block->prev    = NULL;
    block->free    = 1;
    block->is_mmap = 0;
    return block;
}

static void split_block(block_meta *block, size_t size) {
    if (block->size >= size + META_SIZE + ALIGN) {
        block_meta *new_block = (block_meta *)((char *)(block + 1) + size);
        new_block->size    = block->size - size - META_SIZE;
        new_block->free    = 1;
        new_block->is_mmap = 0;
        new_block->next    = block->next;
        new_block->prev    = block;
        if (new_block->next)
            new_block->next->prev = new_block;

        block->size = size;
        block->next = new_block;
    }
}

void *malloc(size_t size) {
    if (size == 0 || size > SIZE_MAX - META_SIZE)
        return NULL;
    size = ALIGN_UP(size);

    if (size + META_SIZE > ARENA_SIZE) {
        block_meta *block = (block_meta *)sys_mmap(
            NULL, size + META_SIZE, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0
        );
        if (block == MAP_FAILED)
            return NULL;
        block->size    = size;
        block->free    = 0;
        block->is_mmap = 1;
        block->next    = NULL;
        block->prev    = NULL;
        return (void *)(block + 1);
    }

    block_meta *curr = free_list;
    block_meta *tail = NULL;
    while (curr) {
        if (curr->free && curr->size >= size) {
            split_block(curr, size);
            curr->free = 0;
            return (void *)(curr + 1);
        }
        tail = curr;
        curr = curr->next;
    }

    block_meta *new_arena = request_arena(size);
    if (!new_arena)
        return NULL;

    new_arena->prev = tail;
    if (tail)
        tail->next = new_arena;
    else
        free_list = new_arena;

    split_block(new_arena, size);
    new_arena->free = 0;
    return (void *)(new_arena + 1);
}

void free(void *ptr) {
    if (ptr == NULL)
        return;
    block_meta *block = (block_meta *)ptr - 1;

    if (block->is_mmap) {
        sys_munmap(block, block->size + META_SIZE);
        return;
    }

    block->free = 1;

    if (block->next && block->next->free &&
        (char *)(block + 1) + block->size == (char *)block->next) {
        block_meta *n = block->next;
    block->size += META_SIZE + n->size;
    block->next = n->next;
    if (block->next)
        block->next->prev = block;
        }

        if (block->prev && block->prev->free &&
            (char *)(block->prev + 1) + block->prev->size == (char *)block) {
            block_meta *p = block->prev;
        p->size += META_SIZE + block->size;
    p->next = block->next;
    if (p->next)
        p->next->prev = p;
            }
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
