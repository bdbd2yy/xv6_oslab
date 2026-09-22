// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(int cpu, void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct kmem {
  struct spinlock lock;
  struct run *freelist;
};

struct kmem kmems[NCPU];

void
kinit()
{
  int sz = ((void*)PHYSTOP - (void *)end) / NCPU;
  for (int cid = 0; cid < NCPU; cid++) {
    initlock(&kmems[cid].lock, "kmem");
    freerange(cid, (void *)((uint64)end + cid * sz), (void *)((uint64)end + (cid+1) * sz));
  }
}

// A helper function for freerange to set free page for cpu of cid
void kfree_to_cpu(int cid, void *pa){
    struct run *r = (struct run*)pa;
    memset(pa, 1, PGSIZE);
    acquire(&kmems[cid].lock);
    r->next = kmems[cid].freelist;
    kmems[cid].freelist = r;
    release(&kmems[cid].lock);
}

void
freerange(int cid, void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    // to avoid passing the cid to kfree, you have to add a helper function to handle the kfree for cpu of cid
    kfree_to_cpu(cid, p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  push_off();
  int cid = cpuid();

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmems[cid].lock);
  r->next = kmems[cid].freelist;
  kmems[cid].freelist = r;
  release(&kmems[cid].lock);
  pop_off();
}

void *steal_page(int cid) {
  for (int i = 0; i < NCPU; i++) {
    if (i == cid) {
      continue;
    }
    acquire(&kmems[i].lock);
    if (kmems[i].freelist) {
      struct run *r = kmems[i].freelist;
      kmems[i].freelist = r->next;
      release(&kmems[i].lock);
      memset((char*)r, 5, PGSIZE); // fill with junk
      return r;
    }
    release(&kmems[i].lock);
  }
  return 0;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  push_off();
  int cid = cpuid();

  acquire(&kmems[cid].lock);
  r = kmems[cid].freelist;
  if(r)
    kmems[cid].freelist = r->next;
  release(&kmems[cid].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  if(r == 0) {
    r = steal_page(cid);
  }
  pop_off();
  return (void*)r;
}
