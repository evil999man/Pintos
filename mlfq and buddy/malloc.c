#include "threads/malloc.h"
#include <debug.h>
#include <list.h>
#include <round.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

/* A simple implementation of malloc().

   The size of each request, in bytes, is rounded up to a power
   of 2 and assigned to the "descriptor" that manages blocks of
   that size.  The descriptor keeps a list of free blocks.  If
   the free list is nonempty, one of its blocks is used to
   satisfy the request.

   Otherwise, a new page of memory, called an "arena", is
   obtained from the page allocator (if none is available,
   malloc() returns a null pointer).  The new arena is divided
   into blocks, all of which are added to the descriptor's free
   list.  Then we return one of the new blocks.

   When we free a block, we add it to its descriptor's free list.
   But if the arena that the block was in now has no in-use
   blocks, we remove all of the arena's blocks from the free list
   and give the arena back to the page allocator.

   We can't handle blocks bigger than 2 kB using this scheme,
   because they're too big to fit in a single page with a
   descriptor.  We handle those by allocating contiguous pages
   with the page allocator and sticking the allocation size at
   the beginning of the allocated block's arena header. */

/* Descriptor. */
struct desc
{
  size_t block_size;          /* Size of each element in bytes. */
  struct list free_list;      /* List of free blocks. */
  struct lock lock;           /* Lock. */
};

/* Magic number for detecting arena corruption. */
#define ARENA_MAGIC 0x9a548eed

/* Arena. */
struct arena
{
  unsigned magic;             /* Always set to ARENA_MAGIC. */
  struct list_elem elem;
  int arr[1 << (PGBITS - 1 - 4)];
};

struct list page_list;
/* Free block. */
struct block
{
  struct list_elem free_elem; /* Free list element. */
};

/* Our set of descriptors. */
static struct desc descs[10];   /* Descriptors. */
static size_t desc_cnt;         /* Number of descriptors. */

static struct arena *block_to_arena (struct block *);
// static struct block *arena_to_block (struct arena *, size_t idx);

/* Initializes the malloc() descriptors. */
void
malloc_init (void)
{
  size_t block_size;
  for (block_size = 16; block_size <= PGSIZE / 2; block_size *= 2)
  {
    struct desc *d = &descs[desc_cnt++];
    ASSERT (desc_cnt <= sizeof descs / sizeof * descs);
    d->block_size = block_size;
    list_init (&d->free_list);
    lock_init (&d->lock);
  }
  list_init(&page_list);
}

/* Obtains and returns a new block of at least SIZE bytes.
   Returns a null pointer if memory is not available. */
void *
malloc (size_t size)
{
  ASSERT(size <= PGSIZE / 2);
  if (size == 0 || size > PGSIZE / 2) return NULL; // not to be handled

  struct desc *d;
  struct block *b;
  struct arena *a;

  /* Find the smallest descriptor that satisfies a SIZE-byte
     request. */

  for (d = descs; d < descs + desc_cnt; d++) {
    lock_acquire(&d->lock);
    if (d->block_size >= size && !list_empty(&d->free_list)) {
      b = list_entry (list_pop_front (&d->free_list), struct block, free_elem);
      lock_release(&d->lock);
      a = block_to_arena(b);
      break;
    }
    lock_release(&d->lock);
    if (d == descs + desc_cnt - 1) {
      /* Allocate a page. */
      a = palloc_get_page (0);
      if (a == NULL)
        return NULL;
      list_push_back(&page_list, &(a->elem));
      /* Initialize arena and add its blocks to the free list. */
      a->magic = ARENA_MAGIC;
      memset(a->arr, 0, sizeof a->arr);
      lock_acquire(&d->lock);
      list_push_back(&descs[7].free_list, (struct list_elem *) ((void *) a + sizeof(*a)));
      b = list_entry (list_pop_front (&descs[7].free_list), struct block, free_elem);
      lock_release(&d->lock);
      break;
    }
  }
  while (1) {
    if (d->block_size == 16)  break;
    if (d->block_size >= 2 * size) {
      d = d - 1;
      lock_acquire(&d->lock);
      list_push_back(&(d->free_list), (struct list_elem *)(((void *)b) + d->block_size));
      lock_release(&d->lock);
    }
    else break;
  }
  a->arr[((void *)b - (void *)a - sizeof(*a)) >> 4] = d->block_size;
  return b;
}

/* Allocates and return A times B bytes initialized to zeroes.
   Returns a null pointer if memory is not available. */
void *
calloc (size_t a, size_t b)
{
  void *p;
  size_t size;

  /* Calculate block size and make sure it fits in size_t. */
  size = a * b;
  if (size < a || size < b)
    return NULL;

  /* Allocate and zero memory. */
  p = malloc (size);
  if (p != NULL)
    memset (p, 0, size);

  return p;
}

/* Returns the number of bytes allocated for BLOCK. */
static size_t
block_size (void *block)
{
  struct block *b = block;
  struct arena *a = block_to_arena (b);

  return a->arr[((void *)b - (void *)a - sizeof(*a)) >> 4];
}

/* Attempts to resize OLD_BLOCK to NEW_SIZE bytes, possibly
   moving it in the process.
   If successful, returns the new block; on failure, returns a
   null pointer.
   A call with null OLD_BLOCK is equivalent to malloc(NEW_SIZE).
   A call with zero NEW_SIZE is equivalent to free(OLD_BLOCK). */
void *
realloc (void *old_block, size_t new_size)
{
  if (new_size == 0)
  {
    free (old_block);
    return NULL;
  }
  else
  {
    void *new_block = malloc (new_size);
    if (old_block != NULL && new_block != NULL)
    {
      size_t old_size = block_size (old_block);
      size_t min_size = new_size < old_size ? new_size : old_size;
      memcpy (new_block, old_block, min_size);
      free (old_block);
    }
    return new_block;
  }
}

/* Frees block P, which must have been previously allocated with
   malloc(), calloc(), or realloc(). */
void
free (void *p)
{
  if (p == NULL) return;

  struct block *b = p;
  struct arena *a = block_to_arena (b);
  size_t b_sz = block_size(b);

#ifndef NDEBUG
  /* Clear the block to help detect use-after-free bugs. */
  memset (b, 0xcc, b_sz);
#endif

  a->arr[((void *)b - (void *)a - sizeof(*a)) >> 4] = 0;  // reset to 0
  int idx = 0;
  while (b_sz > (1ul << (idx + 4))) ++idx;

  while (1) {
    if (b_sz == PGSIZE / 2) {
      list_remove(&(a->elem));
      palloc_free_page (a);
      break;
    }
    struct block *buddy = (struct block *)((((size_t)b - (size_t)a - sizeof *a) ^ b_sz) + (size_t)a + sizeof *a);
    size_t i = 0;
    for (; i < b_sz; i += 16) {
      if (a->arr[((void *)buddy - (void *)a - sizeof(*a) + i) >> 4]) {
        i = 1;
        break;
      }
    }
    if (i == 1) {
      lock_acquire(&descs[idx].lock);
      list_push_back(&descs[idx].free_list, (struct list_elem *)b);
      lock_release(&descs[idx].lock);
      break;
    }
    struct block *nb = b < buddy ? b : buddy;
    lock_acquire(&descs[idx].lock);
    list_remove((struct list_elem *)buddy);
    lock_release(&descs[idx].lock);
    idx++;
    b_sz <<= 1;
    b = nb;
  }
}

/* Returns the arena that block B is inside. */
static struct arena *
block_to_arena (struct block *b)
{
  struct arena *a = pg_round_down (b);

  /* Check that the arena is valid. */
  ASSERT (a != NULL);
  ASSERT (a->magic == ARENA_MAGIC);

  /* Check that the block is properly aligned for the arena. */
  // ASSERT (a->desc == NULL
  //         || (pg_ofs (b) - sizeof * a) % a->desc->block_size == 0);
  // ASSERT (a->desc != NULL || pg_ofs (b) == sizeof * a);

  return a;
}

/* Returns the (IDX - 1)'th block within arena A. */
// static struct block *
// arena_to_block (struct arena *a, size_t idx)
// {
//   ASSERT (a != NULL);
//   ASSERT (a->magic == ARENA_MAGIC);
// ASSERT (idx < a->desc->blocks_per_arena);
//   return (struct block *) ((uint8_t *) a
//                            + sizeof * a
//                            + idx * a->desc->block_size);
// }
bool cmp_addr(const struct list_elem *a, const struct list_elem *b, void *aux) {
  return a < b;
}

void printMemory(void) {
  struct list_elem *it, *itt;
  int n = 0;
  size_t i = 0;
  for (; i < desc_cnt; ++i) {
    list_sort(&descs[i].free_list, cmp_addr, 0);
  }
  for (it = list_begin(&page_list); it != list_end(&page_list); it = list_next(it))++n;
  printf("No. of pages allocated : %d\n", n);
  n = 1;
  for (it = list_begin(&page_list); it != list_end(&page_list); it = list_next(it), ++n) {
    printf("Page %d:\n", n);
    struct arena* a = list_entry(it, struct arena, elem);
    for (i = 0; i < desc_cnt; ++i) {
      printf("Size %d:",descs[i].block_size);
      for (itt = list_begin(&descs[i].free_list); itt != list_end(&descs[i].free_list); itt = list_next(itt)) {
        struct block* b = list_entry(itt, struct block, free_elem);
        if (a != block_to_arena(b)) continue;
        printf(" %u", (void *)b - (void *)a - sizeof(*a));
      }
      printf("\n");
    }
    printf("\n");
  }
}
