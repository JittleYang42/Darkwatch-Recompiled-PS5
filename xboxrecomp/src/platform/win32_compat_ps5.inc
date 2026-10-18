/*
 * win32_compat_ps5.inc - VirtualAlloc, file mappings and protection on the
 * PS5 (included by win32_compat.c when __PROSPERO__ is defined).
 *
 * A title's anonymous mmap and every view of a POSIX shared-memory object are
 * charged to its small flexible budget, and the Xbox memory model maps the
 * 64 MB of guest RAM 28 times over (the mirrors). So here:
 *
 *   CreateFileMapping   ps5_shm_create: one direct-memory object, mapped any
 *                       number of times at no flexible cost
 *   MapViewOfFileEx     ps5_shm_map, placed exactly; outside a reservation of
 *                       ours the range is reserved first, so an address that
 *                       is taken fails as Win32 promises instead of being
 *                       displaced
 *   VirtualAlloc        a reserved range (ps5_vrange_reserve / _at), committed
 *                       with ps5_vrange_commit (direct memory in 64 KiB units)
 *   VirtualFree         a whole reservation is released; a part of one (the
 *                       memory layout hands a slice of its span to a fixed
 *                       window) is decommitted and stays reserved, so the
 *                       window's own map lands inside it
 *   VirtualProtect      sceKernelMprotect on the 16 KiB pages that cover it
 *
 * Kernel pages are 16 KiB, so every protection change is rounded out to them.
 */

#define PS5_PAGE   ((uintptr_t)0x4000)
#define PS5_UNIT   ((uintptr_t)0x10000)

static int ps5_prot(DWORD protect)
{
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:          return 0;
    case PAGE_READONLY:          return PS5_KERNEL_PROT_CPU_READ;
    case PAGE_EXECUTE_READ:      return PS5_KERNEL_PROT_CPU_READ;
    case PAGE_READWRITE:
    case PAGE_EXECUTE_READWRITE:
    default:                     return PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE;
    }
}

/* The reservations this layer made: base, length. */
typedef struct { uintptr_t base, len; } ps5_resv;
static ps5_resv        s_resv[256];
static pthread_mutex_t s_resv_lock = PTHREAD_MUTEX_INITIALIZER;

static int resv_find(uintptr_t a, size_t n)
{
    int found = -1;
    pthread_mutex_lock(&s_resv_lock);
    for (int i = 0; i < 256; i++)
        if (s_resv[i].len && a >= s_resv[i].base && a + n <= s_resv[i].base + s_resv[i].len) {
            found = i;
            break;
        }
    pthread_mutex_unlock(&s_resv_lock);
    return found;
}

static void resv_add(uintptr_t base, size_t len)
{
    pthread_mutex_lock(&s_resv_lock);
    for (int i = 0; i < 256; i++)
        if (!s_resv[i].len) { s_resv[i].base = base; s_resv[i].len = len; break; }
    pthread_mutex_unlock(&s_resv_lock);
}

/* A reservation of exactly [base, base+len): taken out of the list. */
static int resv_take_exact(uintptr_t base, size_t len)
{
    int ok = 0;
    pthread_mutex_lock(&s_resv_lock);
    for (int i = 0; i < 256; i++)
        if (s_resv[i].len && s_resv[i].base == base && (len == 0 || s_resv[i].len == len)) {
            s_resv[i].len = 0;
            ok = 1;
            break;
        }
    pthread_mutex_unlock(&s_resv_lock);
    return ok;
}

/* Shared-memory views: a commit over one is only a protection change. */
static ps5_resv        s_shmv[512];

static int shmv_covers(uintptr_t a, size_t n)
{
    int found = 0;
    pthread_mutex_lock(&s_resv_lock);
    for (int i = 0; i < 512; i++)
        if (s_shmv[i].len && a >= s_shmv[i].base && a + n <= s_shmv[i].base + s_shmv[i].len) {
            found = 1;
            break;
        }
    pthread_mutex_unlock(&s_resv_lock);
    return found;
}

static void shmv_set(uintptr_t base, size_t len, int add)
{
    pthread_mutex_lock(&s_resv_lock);
    for (int i = 0; i < 512; i++) {
        if (add && !s_shmv[i].len) { s_shmv[i].base = base; s_shmv[i].len = len; break; }
        if (!add && s_shmv[i].len && s_shmv[i].base == base) { s_shmv[i].len = 0; break; }
    }
    pthread_mutex_unlock(&s_resv_lock);
}

static size_t round_unit(size_t n) { return (n + PS5_UNIT - 1) & ~(size_t)(PS5_UNIT - 1); }

/* mprotect for the rest of the runtime (xbox_memory_layout's trap pages). */
int ps5_compat_mprotect(void *addr, size_t len, int prot)
{
    uintptr_t lo = (uintptr_t)addr & ~(PS5_PAGE - 1);
    uintptr_t hi = ((uintptr_t)addr + len + PS5_PAGE - 1) & ~(PS5_PAGE - 1);
    int p = 0;
    if (prot & PROT_READ)  p |= PS5_KERNEL_PROT_CPU_READ;
    if (prot & PROT_WRITE) p |= PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_CPU_READ;
    return sceKernelMprotect((void *)lo, hi - lo, p) == 0 ? 0 : -1;
}

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect)
{
    int prot = ps5_prot(protect);
    uintptr_t a = (uintptr_t)address;
    void *base = NULL;
    int fresh = 0;

    if (size == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    if (address && resv_find(a, size) >= 0) {
        /* Inside our own reservation: a commit, or a reservation already held. */
        base = address;
    } else {
        size_t len = round_unit(size + (address ? (a & (PS5_UNIT - 1)) : 0));
        void *at = NULL;
        int r;
        if (address) {
            at = (void *)(a & ~(PS5_UNIT - 1));
            r = ps5_vrange_reserve_at(at, len);
        } else {
            r = ps5_vrange_reserve(len, NULL, PS5_UNIT, &at);
        }
        if (r != 0) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        resv_add((uintptr_t)at, len);
        base = address ? address : at;
        fresh = 1;
    }

    if ((allocationType & MEM_COMMIT) && shmv_covers((uintptr_t)base, size)) {
        ps5_compat_mprotect(base, size, prot ? (PROT_READ | ((prot & PS5_KERNEL_PROT_CPU_WRITE) ? PROT_WRITE : 0)) : PROT_NONE);
    } else if (allocationType & MEM_COMMIT) {
        if (ps5_vrange_commit(base, size, prot ? prot : PS5_KERNEL_PROT_CPU_READ) != 0) {
            if (fresh) {
                uintptr_t at = (uintptr_t)base & ~(PS5_UNIT - 1);
                size_t len = round_unit(size + ((uintptr_t)base & (PS5_UNIT - 1)));
                resv_take_exact(at, len);
                ps5_vrange_release((void *)at, len);
            }
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        if (!prot)  /* committed with no access: a guard */
            ps5_compat_mprotect(base, size, PROT_NONE);
    }
    if (fresh || (allocationType & MEM_RESERVE))
        view_register(base, size);
    return base;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD freeType)
{
    uintptr_t a = (uintptr_t)address;
    if (freeType & MEM_RELEASE) {
        void *vbase = NULL;
        size_t vlen = 0;
        int whole;
        /* Win32 releases a whole allocation with size 0. A size that covers
         * the allocation is the same; anything smaller is a slice of it (the
         * memory layout hands slices of its span to fixed windows), which is
         * given back and stays reserved so the window maps inside it. */
        if (view_lookup(address, &vbase, &vlen) && vbase == address)
            whole = size == 0 || size >= vlen;
        else
            whole = 0;
        if (whole) {
            size_t len = view_take(address);
            shmv_set(a, len, 0);
            if (resv_take_exact(a, round_unit(len)) || resv_take_exact(a, 0))
                return ps5_vrange_release(address, round_unit(len)) == 0;
            /* A block VirtualAlloc placed inside a bigger reservation. */
            return ps5_vrange_decommit(address, len) == 0;
        }
        if (size && resv_find(a, size) >= 0) {
            /* As munmap of a slice does on Linux: a slice at the head of a
             * registered region ends the registry's record of it. */
            if (vbase == address)
                view_take(address);
            return ps5_vrange_decommit(address, size) == 0;
        }
        return FALSE;
    }
    if (freeType & MEM_DECOMMIT)
        return ps5_vrange_decommit(address, size) == 0;
    return TRUE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    int p = ps5_prot(newProtect);
    if (oldProtect) *oldProtect = PAGE_READWRITE;
    uintptr_t lo = (uintptr_t)address & ~(PS5_PAGE - 1);
    uintptr_t hi = ((uintptr_t)address + size + PS5_PAGE - 1) & ~(PS5_PAGE - 1);
    return sceKernelMprotect((void *)lo, hi - lo, p) == 0;
}

HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCSTR name)
{
    (void)file; (void)sa; (void)protect; (void)name;
    SIZE_T size = ((SIZE_T)maxSizeHigh << 32) | maxSizeLow;
    if (size == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    w32_object *o = obj_alloc(K_FILEMAP);
    if (ps5_shm_create(size, &o->shm) != 0) {
        free(o);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    o->fd       = -1;
    o->map_size = size;
    return (HANDLE)o;
}

HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCWSTR name)
{
    (void)name;
    return CreateFileMappingA(file, sa, protect, maxSizeHigh, maxSizeLow, NULL);
}

LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow,
                       SIZE_T count, LPVOID baseAddr)
{
    w32_object *o = (w32_object *)mapping;
    if (!o || o->kind != K_FILEMAP) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }

    size_t off = ((size_t)offHigh << 32) | offLow;
    SIZE_T len = count ? count : (o->map_size - (SIZE_T)off);
    int prot   = PS5_SHM_READ | ((access != FILE_MAP_READ) ? PS5_SHM_WRITE : 0);
    void *view = NULL;
    int reserved_here = 0;

    if (baseAddr && resv_find((uintptr_t)baseAddr, len) < 0) {
        /* Not ours: claim it first, which fails when anything is there. */
        if (ps5_vrange_reserve_at(baseAddr, round_unit(len)) != 0) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return NULL;
        }
        resv_add((uintptr_t)baseAddr, round_unit(len));
        reserved_here = 1;
    }
    if (ps5_shm_map(&o->shm, off, len, baseAddr, prot,
                    baseAddr ? PS5_SHM_FIXED : 0u, &view) != 0 || !view) {
        if (reserved_here) {
            resv_take_exact((uintptr_t)baseAddr, round_unit(len));
            ps5_vrange_release(baseAddr, round_unit(len));
        }
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    view_register(view, len);
    shmv_set((uintptr_t)view, len, 1);
    return view;
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T count)
{
    return MapViewOfFileEx(mapping, access, offHigh, offLow, count, NULL);
}

BOOL UnmapViewOfFile(LPCVOID baseAddr)
{
    size_t len = view_take(baseAddr);
    if (len == 0) return FALSE;
    shmv_set((uintptr_t)baseAddr, len, 0);
    unsigned flags = resv_find((uintptr_t)baseAddr, len) >= 0 ? PS5_SHM_KEEP_RESERVED : 0u;
    return ps5_shm_unmap((void *)baseAddr, len, flags) == 0;
}
