#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64 sys_exit(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  exit(n);
  return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return fork(); }

uint64 sys_wait(void) {
  uint64 p;
  int f;
  if (argaddr(0, &p) < 0) return -1;
  if (argint(1, &f) < 0) return -1;
  return wait(p, f);
}

uint64 sys_sbrk(void) {
  int addr;
  int n;

  if (argint(0, &n) < 0) return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0) return -1;
  return addr;
}

uint64 sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0) return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64 sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0) return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_rename(void) {
  char name[16];
  int len = argstr(0, name, MAXPATH);
  if (len < 0) {
    return -1;
  }
  struct proc *p = myproc();
  memmove(p->name, name, len);
  p->name[len] = '\0';
  return 0;
}

uint64 sys_yield(void) {
  struct proc *p;
  struct proc *np;
  struct proc *tp;
  p = myproc();
  printf("Save the context of the process to the memory region from address %p to %p\n", &p->context, &p->context + 1);
  printf("Current running process pid is %d and user pc is %p\n", p->pid, p->trapframe->epc);
  int found = 0;
  for (np = proc; np < &proc[NPROC]; np++) {
    acquire(&np->lock);
    if (np->state == RUNNABLE && found == 0) {
      tp = np;
      found = 1;
    }
    release(&np->lock);
  }
  if (found) {
    printf("Next runnable process pid is %d and user pc is %p\n", tp->pid, tp->trapframe->epc);
  } else {
    printf("No runnable process was found\n");
  }

  yield();
  return 0;
}

// why you need to write the declaration like uint64 func(void)? See the type of syscalls in syscall.c
// the return value will be set to a0 red in the syscall()
uint64 sys_seccomp_ctl(void) {
  int op;
  uint64 a;
  if (argint(0, &op) < 0) return -1;
  if (argaddr(1, &a) < 0) return -1;
  seccomp_ctl(op, a);
  return 0;
}

uint64 sys_seccomp_getlog(void) {
  struct proc *p = myproc();
  uint64 user_buf;
  int user_len;
  uint64 kernel_buf[32];
  int max_len;
  int actual_len;
  p = myproc();
  // get user-space addresses of two args
  if (argaddr(0, &user_buf) || argint(1, &user_len)) return -1;

  if (copyin(p->pagetable, (char *)&max_len, user_len, sizeof(max_len)) < 0) return -1;
  if (max_len < 0 || max_len > 32) {
    return -1;
  }
  actual_len = max_len;

  if (seccomp_getlog(kernel_buf, &actual_len) < 0) return -1;
  if (copyout(p->pagetable, user_buf, (char *)kernel_buf, actual_len * sizeof(uint64)) < 0) return -1;
  if (copyout(p->pagetable, user_len, (char *)&actual_len, sizeof(actual_len)) < 0) return -1;
  return 0;
}
