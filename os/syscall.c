#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"

// included to help with helper function errors
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc);

uint64 sys_write(int fd, char *str, uint len)
{
    debugf("sys_write fd = %d str = %x, len = %d", fd, str, len);
    if (fd != STDOUT)
        return -1;

    struct proc *p = curr_proc();
	// changed this to work with addresses instead of direct declaration
    for (uint i = 0; i < len; i++) {
        uint64 kva = useraddr(p->pagetable, (uint64)(str + i));
        if (kva == 0)
            return -1;
        console_putchar(*(char *)kva);
    }

    return len;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(TimeVal *uval, int _tz)
{
    struct proc *p = curr_proc();
    uint64 kva = useraddr(p->pagetable, (uint64)uval);
    if (kva == 0)
        return -1;

    TimeVal *val = (TimeVal *)kva;
    uint64 cycle = get_cycle();
    val->sec = cycle / CPU_FREQ;
    val->usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
    return 0;
}

// TODO: add support for mmap and munmap syscall.
// hint: read through docstrings in vm.c. Watching CH4 video may also help.
// Note the return value and PTE flags (especially U,X,W,R)
/*
* LAB1: you may need to define sys_task_info here
*/
uint64 sys_getpid(void)
{
    return curr_proc()->pid;
}


uint64 sys_task_info(struct TaskInfo *uinfo)
{
    struct proc *p = curr_proc();
    uint64 kva = useraddr(p->pagetable, (uint64)uinfo);
    if (kva == 0)
        return -1;

    struct TaskInfo *info = (struct TaskInfo *)kva;

    p->task_info.status = Running;

    uint64 now = get_cycle();
    uint64 elapsed_cycles = (now > p->start_time) ? (now - p->start_time) : 0;
    p->task_info.time = (int)(elapsed_cycles * 1000 / CPU_FREQ);

    *info = p->task_info;
    return 0;
}

static int page_mapped(pagetable_t pagetable, uint64 va)
{
    pte_t *pte = walk(pagetable, va, 0);
    if (pte == 0)
        return 0;
    if ((*pte & PTE_V) == 0)
        return 0;
    return 1;
}
uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
    struct proc *p = curr_proc();
    (void)flag;
    (void)fd;

    if (len == 0) {
        return 0;
    }

    if (len > (1UL << 30)) {
        return -1;
    }

    if (!PGALIGNED(start)) {
        return -1;
    }

    if (start + len < start) {
        return -1;
    }

    if ((port & ~0x7) != 0) {
        return -1;
    }

    if ((port & 0x7) == 0) {
        return -1;
    }

    uint64 va_start = start;
    uint64 va_end = PGROUNDUP(start + len);

    for (uint64 va = va_start; va < va_end; va += PAGE_SIZE) {
        if (page_mapped(p->pagetable, va)) {
            return -1;
        }
    }

    int perm = PTE_U;
    if (port & 0x1) perm |= PTE_R;
    if (port & 0x2) perm |= PTE_W;
    if (port & 0x4) perm |= PTE_X;

    for (uint64 va = va_start; va < va_end; va += PAGE_SIZE) {
        void *pa = kalloc();
        if (pa == 0) {
            for (uint64 x = va_start; x < va; x += PAGE_SIZE) {
                uvmunmap(p->pagetable, x, 1, 1);
            }
            return -1;
        }

        memset(pa, 0, PAGE_SIZE);

        if (mappages(p->pagetable, va, PAGE_SIZE, (uint64)pa, perm) != 0) {
            kfree(pa);
            for (uint64 x = va_start; x < va; x += PAGE_SIZE) {
                uvmunmap(p->pagetable, x, 1, 1);
            }
            return -1;
        }
    }

    uint64 new_max = PGROUNDUP(va_end - 1) / PAGE_SIZE;
    if (new_max > p->max_page) {
        p->max_page = new_max;
    }

    return 0;
}


uint64 sys_munmap(uint64 start, uint64 len)
{
    struct proc *p = curr_proc();

    if (len == 0) {
        return 0;
    }

    if (!PGALIGNED(start)) {
        return -1;
    }

    if (start + len < start) {
        return -1;
    }

    uint64 va_start = start;
    uint64 va_end = PGROUNDUP(start + len);

    for (uint64 va = va_start; va < va_end; va += PAGE_SIZE) {
        if (!page_mapped(p->pagetable, va)) {
            return -1;
        }
    }

    for (uint64 va = va_start; va < va_end; va += PAGE_SIZE) {
        uvmunmap(p->pagetable, va, 1, 1);
    }

    return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	if (id >= 0 && id < MAX_SYSCALL_NUM) {
		curr_proc()->task_info.syscall_times[id]++;
	}

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], (char *)args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;
	/*
	* LAB1: you may need to add SYS_taskinfo case here
	*/
	case SYS_getpid:
		ret = sys_getpid();
		break;

	case SYS_task_info:
		ret = sys_task_info((struct TaskInfo *)args[0]);
		break;

	/*
	* Proj 2
	* added cases for SYS_mmap and SYS_munmap
	*/
	case SYS_mmap:
    	ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
    	break;

	case SYS_munmap:
    	ret = sys_munmap(args[0], args[1]);
    	break;


	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}