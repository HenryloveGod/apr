/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 */

/*
 * Resource allocation code... the code here is responsible for making
 * sure that nothing leaks.
 *
 * rst --- 4/95 --- 6/95
 */

#include "apr.h"
#include "apr_private.h"

#include "apr_portable.h" /* for get_os_proc */
#include "apr_strings.h"
#include "apr_general.h"
#include "apr_pools.h"
#include "apr_lib.h"
#include "apr_lock.h"
#include "apr_hash.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_SIGNAL_H
#include <sys/signal.h>
#endif
#if APR_HAVE_SIGNAL_H
#include <signal.h>
#endif
#if APR_HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#if APR_HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if APR_HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

/* Details of the debugging options can now be found in the developer
 * section of the documentaion. */

/* magic numbers --- min free bytes to consider a free apr_pool_t block useable,
 * and the min amount to allocate if we have to go to malloc() */

#ifndef BLOCK_MINFREE
#define BLOCK_MINFREE 4096
#endif
#ifndef BLOCK_MINALLOC
#define BLOCK_MINALLOC 8192
#endif
 
#ifdef APR_POOL_DEBUG
/* first do some option checking... */
#ifdef ALLOC_USE_MALLOC
#error "sorry, no support for ALLOC_USE_MALLOC and APR_POOL_DEBUG at the same time"
#endif /* ALLOC_USE_MALLOC */

#ifdef MULTITHREAD
# error "sorry, no support for MULTITHREAD and APR_POOL_DEBUG at the same time"
#endif /* MULTITHREAD */

#endif /* APR_POOL_DEBUG */

#ifdef ALLOC_USE_MALLOC
#undef BLOCK_MINFREE
#undef BLOCK_MINALLOC
#define BLOCK_MINFREE	0
#define BLOCK_MINALLOC	0
#endif /* ALLOC_USE_MALLOC */

#define APR_SLACK_LOW    1
#define APR_SLACK_HIGH   2


/*****************************************************************
 *
 * Managing free storage blocks...
 */

union align {
    /*
     * Types which are likely to have the longest RELEVANT alignment
     * restrictions...
     */

    char *cp;
    void (*f) (void);
    long l;
    FILE *fp;
    double d;
};

#define CLICK_SZ (sizeof(union align))

union block_hdr {
    union align a;

    /* Actual header... */

    struct {
	char *endp;
	union block_hdr *next;
	char *first_avail;
#ifdef APR_POOL_DEBUG
	union block_hdr *global_next;
	apr_pool_t *owning_pool;
#endif /* APR_POOL_DEBUG */
    } h;
};

#define APR_ABORT(conditional, retcode, func, str) \
    if (conditional) { \
        if ((func) == NULL) { \
            return NULL; \
        } \
        else { \
            fprintf(stderr, "%s", str); \
            (*(func))(retcode); \
        } \
    }

/*
 * Static cells for managing our internal synchronisation.
 */
static union block_hdr *block_freelist = NULL;

#if APR_HAS_THREADS
static apr_lock_t *alloc_mutex;
static apr_lock_t *spawn_mutex;
#endif

#ifdef APR_POOL_DEBUG
static char *known_stack_point;
static int stack_direction;
static union block_hdr *global_block_list;
#define FREE_POOL	((apr_pool_t *)(-1))
#endif /* APR_POOL_DEBUG */

#ifdef ALLOC_STATS
static unsigned long long num_free_blocks_calls;
static unsigned long long num_blocks_freed;
static unsigned max_blocks_in_one_free;
static unsigned num_malloc_calls;
static unsigned num_malloc_bytes;
#endif /* ALLOC_STATS */

#ifdef ALLOC_DEBUG
#define FILL_BYTE	((char)(0xa5))
#define debug_fill(ptr,size)	((void)memset((ptr), FILL_BYTE, (size)))

static APR_INLINE void debug_verify_filled(const char *ptr, const char *endp,
					   const char *error_msg)
{
    for ( ; ptr < endp; ++ptr) {
	if (*ptr != FILL_BYTE) {
	    fputs(error_msg, stderr);
	    abort();
	    exit(1);
	}
    }
}

#else /* ALLOC_DEBUG */
#define debug_fill(a,b)
#define debug_verify_filled(a,b,c)
#endif /* ALLOC_DEBUG */

/*
 * Get a completely new block from the system pool. Note that we rely on
 * malloc() to provide aligned memory.
 */

static union block_hdr *malloc_block(int size, int (*apr_abort)(int retcode))
{
    union block_hdr *blok;

#ifdef ALLOC_DEBUG
    /* make some room at the end which we'll fill and expect to be
     * always filled
     */
    size += CLICK_SZ;
#endif /* ALLOC_DEBUG */

#ifdef ALLOC_STATS
    ++num_malloc_calls;
    num_malloc_bytes += size + sizeof(union block_hdr);
#endif /* ALLOC_STATS */

    blok = (union block_hdr *) malloc(size + sizeof(union block_hdr));
    APR_ABORT(blok == NULL, APR_ENOMEM, apr_abort,
              "Ouch!  malloc failed in malloc_block()\n");
    debug_fill(blok, size + sizeof(union block_hdr));

    blok->h.next = NULL;
    blok->h.first_avail = (char *) (blok + 1);
    blok->h.endp = size + blok->h.first_avail;

#ifdef ALLOC_DEBUG
    blok->h.endp -= CLICK_SZ;
#endif /* ALLOC_DEBUG */

#ifdef APR_POOL_DEBUG
    blok->h.global_next = global_block_list;
    global_block_list = blok;
    blok->h.owning_pool = NULL;
#endif /* APR_POOL_DEBUG */

    return blok;
}



#if defined(ALLOC_DEBUG) && !defined(ALLOC_USE_MALLOC)
static void chk_on_blk_list(union block_hdr *blok, union block_hdr *free_blk)
{
    debug_verify_filled(blok->h.endp, blok->h.endp + CLICK_SZ,
			"[chk_on_blk_list] Ouch!  Someone trounced the padding "
			"at the end of a block!\n");
    while (free_blk) {
	if (free_blk == blok) {
            fprintf(stderr, "Ouch!  Freeing free block\n");
	    abort();
	    exit(1);
	}
	free_blk = free_blk->h.next;
    }
}
#else /* defined(ALLOC_DEBUG) && !defined(ALLOC_USE_MALLOC) */
#define chk_on_blk_list(_x, _y)
#endif /* defined(ALLOC_DEBUG) && !defined(ALLOC_USE_MALLOC) */

/* Free a chain of blocks --- must be called with alarms blocked. */

static void free_blocks(union block_hdr *blok)
{
#ifdef ALLOC_USE_MALLOC
    union block_hdr *next;

    for ( ; blok; blok = next) {
	next = blok->h.next;
	free(blok);
    }
#else /* ALLOC_USE_MALLOC */

#ifdef ALLOC_STATS
    unsigned num_blocks;
#endif /* ALLOC_STATS */

    /*
     * First, put new blocks at the head of the free list ---
     * we'll eventually bash the 'next' pointer of the last block
     * in the chain to point to the free blocks we already had.
     */

    union block_hdr *old_free_list;

    if (blok == NULL) {
	return;			/* Sanity check --- freeing empty pool? */
    }

#if APR_HAS_THREADS
    apr_lock(alloc_mutex);
#endif
    old_free_list = block_freelist;
    block_freelist = blok;

    /*
     * Next, adjust first_avail pointers of each block --- have to do it
     * sooner or later, and it simplifies the search in new_block to do it
     * now.
     */

#ifdef ALLOC_STATS
    num_blocks = 1;
#endif /* ALLOC_STATS */

    while (blok->h.next != NULL) {

#ifdef ALLOC_STATS
	++num_blocks;
#endif /* ALLOC_STATS */

	chk_on_blk_list(blok, old_free_list);
	blok->h.first_avail = (char *) (blok + 1);
	debug_fill(blok->h.first_avail, blok->h.endp - blok->h.first_avail);
#ifdef APR_POOL_DEBUG 
	blok->h.owning_pool = FREE_POOL;
#endif /* APR_POOL_DEBUG */
	blok = blok->h.next;
    }

    chk_on_blk_list(blok, old_free_list);
    blok->h.first_avail = (char *) (blok + 1);
    debug_fill(blok->h.first_avail, blok->h.endp - blok->h.first_avail);
#ifdef APR_POOL_DEBUG
    blok->h.owning_pool = FREE_POOL;
#endif /* APR_POOL_DEBUG */

    /* Finally, reset next pointer to get the old free blocks back */

    blok->h.next = old_free_list;

#ifdef ALLOC_STATS
    if (num_blocks > max_blocks_in_one_free) {
	max_blocks_in_one_free = num_blocks;
    }
    ++num_free_blocks_calls;
    num_blocks_freed += num_blocks;
#endif /* ALLOC_STATS */

#if APR_HAS_THREADS
    apr_unlock(alloc_mutex);
#endif /* APR_HAS_THREADS */
#endif /* ALLOC_USE_MALLOC */
}

/*
 * Get a new block, from our own free list if possible, from the system
 * if necessary.  Must be called with alarms blocked.
 */

static union block_hdr *new_block(int min_size, int (*apr_abort)(int retcode))
{
    union block_hdr **lastptr = &block_freelist;
    union block_hdr *blok = block_freelist;

    /* First, see if we have anything of the required size
     * on the free list...
     */

    while (blok != NULL) {
	if (min_size + BLOCK_MINFREE <= blok->h.endp - blok->h.first_avail) {
	    *lastptr = blok->h.next;
	    blok->h.next = NULL;
	    debug_verify_filled(blok->h.first_avail, blok->h.endp,
				"[new_block] Ouch!  Someone trounced a block "
				"on the free list!\n");
	    return blok;
	}
	else {
	    lastptr = &blok->h.next;
	    blok = blok->h.next;
	}
    }

    /* Nope. */

    min_size += BLOCK_MINFREE;
    blok = malloc_block((min_size > BLOCK_MINALLOC)
			? min_size : BLOCK_MINALLOC, apr_abort);
    return blok;
}


/* Accounting */

static apr_size_t bytes_in_block_list(union block_hdr *blok)
{
    apr_size_t size = 0;

    while (blok) {
	size += blok->h.endp - (char *) (blok + 1);
	blok = blok->h.next;
    }

    return size;
}


/*****************************************************************
 *
 * Pool internals and management...
 * NB that subprocesses are not handled by the generic cleanup code,
 * basically because we don't want cleanups for multiple subprocesses
 * to result in multiple three-second pauses.
 */

struct process_chain;
struct cleanup;

static void run_cleanups(struct cleanup *c);
static void free_proc_chain(struct process_chain *p);

static apr_pool_t *permanent_pool;

/* Each pool structure is allocated in the start of its own first block,
 * so we need to know how many bytes that is (once properly aligned...).
 * This also means that when a pool's sub-pool is destroyed, the storage
 * associated with it is *completely* gone, so we have to make sure it
 * gets taken off the parent's sub-pool list...
 */

#define POOL_HDR_CLICKS (1 + ((sizeof(struct apr_pool_t) - 1) / CLICK_SZ))
#define POOL_HDR_BYTES (POOL_HDR_CLICKS * CLICK_SZ)

APR_DECLARE(apr_pool_t *) apr_make_sub_pool(apr_pool_t *p, int (*apr_abort)(int retcode))
{
    union block_hdr *blok;
    apr_pool_t *new_pool;


#if APR_HAS_THREADS
    apr_lock(alloc_mutex);
#endif

    blok = new_block(POOL_HDR_BYTES, apr_abort);
    new_pool = (apr_pool_t *) blok->h.first_avail;
    blok->h.first_avail += POOL_HDR_BYTES;
#ifdef APR_POOL_DEBUG
    blok->h.owning_pool = new_pool;
#endif

    memset((char *) new_pool, '\0', sizeof(struct apr_pool_t));
    new_pool->free_first_avail = blok->h.first_avail;
    new_pool->first = new_pool->last = blok;

    if (p) {
	new_pool->parent = p;
	new_pool->sub_next = p->sub_pools;
	if (new_pool->sub_next) {
	    new_pool->sub_next->sub_prev = new_pool;
	}
	p->sub_pools = new_pool;
    }
    else {
        permanent_pool = new_pool;
    }

#if APR_HAS_THREADS
    apr_unlock(alloc_mutex);
#endif

    return new_pool;
}

#ifdef APR_POOL_DEBUG
static void stack_var_init(char *s)
{
    char t;

    if (s < &t) {
	stack_direction = 1; /* stack grows up */
    }
    else {
	stack_direction = -1; /* stack grows down */
    }
}
#endif

#ifdef ALLOC_STATS
static void dump_stats(void)
{
    fprintf(stderr,
	    "alloc_stats: [%d] #free_blocks %llu #blocks %llu max "
	    "%u #malloc %u #bytes %u\n",
	(int) getpid(),
	num_free_blocks_calls,
	num_blocks_freed,
	max_blocks_in_one_free,
	num_malloc_calls,
	num_malloc_bytes);
}
#endif

/* ### why do we have this, in addition to apr_make_sub_pool? */
apr_status_t apr_create_pool(apr_pool_t **newcont, apr_pool_t *cont)
{
    apr_pool_t *newpool;

    if (cont) {
        newpool = apr_make_sub_pool(cont, cont->apr_abort);
    }
    else {
        newpool = apr_make_sub_pool(NULL, NULL);
    }
        
    if (newpool == NULL) {
        return APR_ENOPOOL;
    }   

    newpool->prog_data = NULL;
    if (cont) {
        newpool->apr_abort = cont->apr_abort;
    }
    else {
        newpool->apr_abort = NULL;
    }
    *newcont = newpool;
    return APR_SUCCESS;
}

/*****************************************************************
 *
 * Managing generic cleanups.  
 */

struct cleanup {
    const void *data;
    apr_status_t (*plain_cleanup) (void *);
    apr_status_t (*child_cleanup) (void *);
    struct cleanup *next;
};

APR_DECLARE(void) apr_register_cleanup(apr_pool_t *p, const void *data,
				      apr_status_t (*plain_cleanup) (void *),
				      apr_status_t (*child_cleanup) (void *))
{
    struct cleanup *c;

    if (p != NULL) {
        c = (struct cleanup *) apr_palloc(p, sizeof(struct cleanup));
        c->data = data;
        c->plain_cleanup = plain_cleanup;
        c->child_cleanup = child_cleanup;
        c->next = p->cleanups;
        p->cleanups = c;
    }
}

APR_DECLARE(void) apr_kill_cleanup(apr_pool_t *p, const void *data,
                                 apr_status_t (*cleanup) (void *))
{
    struct cleanup *c;
    struct cleanup **lastp;

    if (p == NULL)
        return;
    c = p->cleanups;
    lastp = &p->cleanups;
    while (c) {
        if (c->data == data && c->plain_cleanup == cleanup) {
            *lastp = c->next;
            break;
        }

        lastp = &c->next;
        c = c->next;
    }
}

APR_DECLARE(apr_status_t) apr_run_cleanup(apr_pool_t *p, void *data,
                                       apr_status_t (*cleanup) (void *))
{
    apr_kill_cleanup(p, data, cleanup);
    return (*cleanup) (data);
}

static void run_cleanups(struct cleanup *c)
{
    while (c) {
	(*c->plain_cleanup) ((void *)c->data);
	c = c->next;
    }
}

static void run_child_cleanups(struct cleanup *c)
{
    while (c) {
	(*c->child_cleanup) ((void *)c->data);
	c = c->next;
    }
}

static void cleanup_pool_for_exec(apr_pool_t *p)
{
    run_child_cleanups(p->cleanups);
    p->cleanups = NULL;

    for (p = p->sub_pools; p; p = p->sub_next) {
	cleanup_pool_for_exec(p);
    }
}

APR_DECLARE(void) apr_cleanup_for_exec(void)
{
#if !defined(WIN32) && !defined(OS2)
    /*
     * Don't need to do anything on NT or OS/2, because I
     * am actually going to spawn the new process - not
     * exec it. All handles that are not inheritable, will
     * be automajically closed. The only problem is with
     * file handles that are open, but there isn't much
     * I can do about that (except if the child decides
     * to go out and close them
     */
    cleanup_pool_for_exec(permanent_pool);
#endif /* ndef WIN32 */
}

APR_DECLARE_NONSTD(apr_status_t) apr_null_cleanup(void *data)
{
    /* do nothing cleanup routine */
    return APR_SUCCESS;
}

apr_status_t apr_init_alloc(void)
{
#if APR_HAS_THREADS
    apr_status_t status;
#endif
#ifdef APR_POOL_DEBUG
    char s;

    known_stack_point = &s;
    stack_var_init(&s);
#endif
#if APR_HAS_THREADS
    status = apr_create_lock(&alloc_mutex, APR_MUTEX, APR_INTRAPROCESS,
                   NULL, NULL);
    if (status != APR_SUCCESS) {
        apr_destroy_lock(alloc_mutex); 
        return status;
    }
    status = apr_create_lock(&spawn_mutex, APR_MUTEX, APR_INTRAPROCESS,
                   NULL, NULL);
    if (status != APR_SUCCESS) {
        apr_destroy_lock(spawn_mutex); 
        return status;
    }
#endif

#ifdef ALLOC_STATS
    atexit(dump_stats);
#endif

    return APR_SUCCESS;
}

void apr_term_alloc(void)
{
#if APR_HAS_THREADS
    apr_destroy_lock(alloc_mutex);
    apr_destroy_lock(spawn_mutex);
#endif
}

/* We only want to lock the mutex if we are being called from apr_clear_pool.
 * This is because if we also call this function from apr_destroy_real_pool,
 * which also locks the same mutex, and recursive locks aren't portable.  
 * This way, we are garaunteed that we only lock this mutex once when calling
 * either one of these functions.
 */
APR_DECLARE(void) apr_clear_pool(apr_pool_t *a)
{
    while (a->sub_pools) {
	apr_destroy_pool(a->sub_pools);
    }
    /*
     * Don't hold the mutex during cleanups.
     */
    run_cleanups(a->cleanups);
    a->cleanups = NULL;
    free_proc_chain(a->subprocesses);
    a->subprocesses = NULL;
    free_blocks(a->first->h.next);
    a->first->h.next = NULL;

    a->prog_data = NULL;

    a->last = a->first;
    a->first->h.first_avail = a->free_first_avail;
    debug_fill(a->first->h.first_avail,
	       a->first->h.endp - a->first->h.first_avail);

#ifdef ALLOC_USE_MALLOC
    {
	void *c, *n;

	for (c = a->allocation_list; c; c = n) {
	    n = *(void **)c;
	    free(c);
	}
	a->allocation_list = NULL;
    }
#endif
}

APR_DECLARE(void) apr_destroy_pool(apr_pool_t *a)
{
    apr_clear_pool(a);
#if APR_HAS_THREADS
    apr_lock(alloc_mutex);
#endif

    if (a->parent) {
	if (a->parent->sub_pools == a) {
	    a->parent->sub_pools = a->sub_next;
	}
	if (a->sub_prev) {
	    a->sub_prev->sub_next = a->sub_next;
	}
	if (a->sub_next) {
	    a->sub_next->sub_prev = a->sub_prev;
	}
    }
#if APR_HAS_THREADS
    apr_unlock(alloc_mutex);
#endif
    free_blocks(a->first);
}

APR_DECLARE(apr_size_t) apr_bytes_in_pool(apr_pool_t *p)
{
    return bytes_in_block_list(p->first);
}
APR_DECLARE(apr_size_t) apr_bytes_in_free_blocks(void)
{
    return bytes_in_block_list(block_freelist);
}

/*****************************************************************
 * APR_POOL_DEBUG support
 */
#ifdef APR_POOL_DEBUG

/* the unix linker defines this symbol as the last byte + 1 of
 * the executable... so it includes TEXT, BSS, and DATA
 */
extern char _end;

/* is ptr in the range [lo,hi) */
#define is_ptr_in_range(ptr, lo, hi) \
    (((unsigned long)(ptr) - (unsigned long)(lo)) \
     < (unsigned long)(hi) - (unsigned long)(lo))

/* Find the pool that ts belongs to, return NULL if it doesn't
 * belong to any pool.
 */
APR_DECLARE(apr_pool_t *) apr_find_pool(const void *ts)
{
    const char *s = ts;
    union block_hdr **pb;
    union block_hdr *b;

    /* short-circuit stuff which is in TEXT, BSS, or DATA */
    if (is_ptr_in_range(s, 0, &_end)) {
	return NULL;
    }
    /* consider stuff on the stack to also be in the NULL pool...
     * XXX: there's cases where we don't want to assume this
     */
    if ((stack_direction == -1 && is_ptr_in_range(s, &ts, known_stack_point))
	|| (stack_direction == 1 && is_ptr_in_range(s, known_stack_point, &ts))) {
	abort();
	return NULL;
    }
    /* search the global_block_list */
    for (pb = &global_block_list; *pb; pb = &b->h.global_next) {
	b = *pb;
	if (is_ptr_in_range(s, b, b->h.endp)) {
	    if (b->h.owning_pool == FREE_POOL) {
		fprintf(stderr,
		    "Ouch!  find_pool() called on pointer in a free block\n");
		abort();
		exit(1);
	    }
	    if (b != global_block_list) {
		/*
		 * promote b to front of list, this is a hack to speed
		 * up the lookup
		 */
		*pb = b->h.global_next;
		b->h.global_next = global_block_list;
		global_block_list = b;
	    }
	    return b->h.owning_pool;
	}
    }
    return NULL;
}

/* return TRUE iff a is an ancestor of b
 * NULL is considered an ancestor of all pools
 */
APR_DECLARE(int) apr_pool_is_ancestor(apr_pool_t *a, apr_pool_t *b)
{
    if (a == NULL) {
	return 1;
    }
    while (a->joined) {
	a = a->joined;
    }
    while (b) {
	if (a == b) {
	    return 1;
	}
	b = b->parent;
    }
    return 0;
}

/*
 * All blocks belonging to sub will be changed to point to p
 * instead.  This is a guarantee by the caller that sub will not
 * be destroyed before p is.
 */
APR_DECLARE(void) apr_pool_join(apr_pool_t *p, apr_pool_t *sub)
{
    union block_hdr *b;

    /* We could handle more general cases... but this is it for now. */
    if (sub->parent != p) {
	fprintf(stderr, "pool_join: p is not parent of sub\n");
	abort();
    }
    while (p->joined) {
	p = p->joined;
    }
    sub->joined = p;
    for (b = global_block_list; b; b = b->h.global_next) {
	if (b->h.owning_pool == sub) {
	    b->h.owning_pool = p;
	}
    }
    return 0;
}
#endif

/*****************************************************************
 *
 * Allocating stuff...
 */

APR_DECLARE(void*) apr_palloc(apr_pool_t *a, apr_size_t reqsize)
{
#ifdef ALLOC_USE_MALLOC
    apr_size_t size = reqsize + CLICK_SZ;
    void *ptr;

    if (a == NULL) {
        return malloc(reqsize);
    }
    if (c == NULL) {
        return malloc(reqsize);
    }
    ptr = malloc(size);
    if (ptr == NULL) {
	fputs("Ouch!  Out of memory!\n", stderr);
	exit(1);
    }
    debug_fill(ptr, size); /* might as well get uninitialized protection */
    *(void **)ptr = a->allocation_list;
    a->allocation_list = ptr;
    return (char *)ptr + CLICK_SZ;
#else

    /*
     * Round up requested size to an even number of alignment units
     * (core clicks)
     */
    apr_size_t nclicks;
    apr_size_t size;

    /* First, see if we have space in the block most recently
     * allocated to this pool
     */

    union block_hdr *blok;
    char *first_avail;
    char *new_first_avail;


    if (a == NULL) {
        return malloc(reqsize);
    }


    nclicks = 1 + ((reqsize - 1) / CLICK_SZ);
    size = nclicks * CLICK_SZ;

    /* First, see if we have space in the block most recently
     * allocated to this pool
     */

    blok = a->last;
    first_avail = blok->h.first_avail;

    if (reqsize <= 0) {
	return NULL;
    }

    new_first_avail = first_avail + size;

    if (new_first_avail <= blok->h.endp) {
	debug_verify_filled(first_avail, blok->h.endp,
			    "[apr_palloc] Ouch!  Someone trounced past the end "
			    "of their allocation!\n");
	blok->h.first_avail = new_first_avail;
	return (void *) first_avail;
    }

    /* Nope --- get a new one that's guaranteed to be big enough */

#if APR_HAS_THREADS
    apr_lock(alloc_mutex);
#endif

    blok = new_block(size, a->apr_abort);
    a->last->h.next = blok;
    a->last = blok;
#ifdef APR_POOL_DEBUG
    blok->h.owning_pool = a;
#endif

#if APR_HAS_THREADS
    apr_unlock(alloc_mutex);
#endif

    first_avail = blok->h.first_avail;
    blok->h.first_avail += size;

    return (void *) first_avail;
#endif
}

APR_DECLARE(void *) apr_pcalloc(apr_pool_t *a, apr_size_t size)
{
    void *res = apr_palloc(a, size);
    memset(res, '\0', size);
    return res;
}

/*****************************************************************
 *
 * User data management functions
 */

apr_status_t apr_set_userdata(const void *data, const char *key,
			      apr_status_t (*cleanup) (void *),
			      apr_pool_t *cont)
{
    int keylen = strlen(key);

    if (cont->prog_data == NULL)
        cont->prog_data = apr_make_hash(cont);

    if (apr_hash_get(cont->prog_data, key, keylen) == NULL){
        char *new_key = apr_pstrdup(cont, key);
        apr_hash_set(cont->prog_data, new_key, keylen, data);
    } 
    else {
        apr_hash_set(cont->prog_data, key, keylen, data);
    }

    apr_register_cleanup(cont, data, cleanup, cleanup);
    return APR_SUCCESS;
}

apr_status_t apr_get_userdata(void **data, const char *key, apr_pool_t *cont)
{
    if (cont->prog_data == NULL)
        *data = NULL;
    else
        *data = apr_hash_get(cont->prog_data, key, strlen(key));
    return APR_SUCCESS;
}

/*****************************************************************
 *
 * "Print" functions
 */

/*
 * apr_psprintf is implemented by writing directly into the current
 * block of the pool, starting right at first_avail.  If there's
 * insufficient room, then a new block is allocated and the earlier
 * output is copied over.  The new block isn't linked into the pool
 * until all the output is done.
 *
 * Note that this is completely safe because nothing else can
 * allocate in this apr_pool_t while apr_psprintf is running.  alarms are
 * blocked, and the only thing outside of alloc.c that's invoked
 * is apr_vformatter -- which was purposefully written to be
 * self-contained with no callouts.
 */

struct psprintf_data {
    apr_vformatter_buff_t vbuff;
#ifdef ALLOC_USE_MALLOC
    char *base;
#else
    union block_hdr *blok;
    int got_a_new_block;
#endif
};

static int psprintf_flush(apr_vformatter_buff_t *vbuff)
{
    struct psprintf_data *ps = (struct psprintf_data *)vbuff;
#ifdef ALLOC_USE_MALLOC
    apr_size_t size;
    char *ptr;

    size = (char *)ps->vbuff.curpos - ps->base;
    ptr = realloc(ps->base, 2*size);
    if (ptr == NULL) {
	fputs("Ouch!  Out of memory!\n", stderr);
	exit(1);
    }
    ps->base = ptr;
    ps->vbuff.curpos = ptr + size;
    ps->vbuff.endpos = ptr + 2*size - 1;
    return 0;
#else
    union block_hdr *blok;
    union block_hdr *nblok;
    apr_size_t cur_len;
    char *strp;

    blok = ps->blok;
    strp = ps->vbuff.curpos;
    cur_len = strp - blok->h.first_avail;

    /* must try another blok */
#if APR_HAS_THREADS
    apr_lock(alloc_mutex);
#endif
    nblok = new_block(2 * cur_len, NULL);
#if APR_HAS_THREADS
    apr_unlock(alloc_mutex);
#endif
    memcpy(nblok->h.first_avail, blok->h.first_avail, cur_len);
    ps->vbuff.curpos = nblok->h.first_avail + cur_len;
    /* save a byte for the NUL terminator */
    ps->vbuff.endpos = nblok->h.endp - 1;

    /* did we allocate the current blok? if so free it up */
    if (ps->got_a_new_block) {
	debug_fill(blok->h.first_avail, blok->h.endp - blok->h.first_avail);
#if APR_HAS_THREADS
        apr_lock(alloc_mutex);
#endif
	blok->h.next = block_freelist;
	block_freelist = blok;
#if APR_HAS_THREADS
        apr_unlock(alloc_mutex);
#endif
    }
    ps->blok = nblok;
    ps->got_a_new_block = 1;
    /* note that we've deliberately not linked the new block onto
     * the pool yet... because we may need to flush again later, and
     * we'd have to spend more effort trying to unlink the block.
     */
    return 0;
#endif
}

APR_DECLARE(char *) apr_pvsprintf(apr_pool_t *p, const char *fmt, va_list ap)
{
#ifdef ALLOC_USE_MALLOC
    struct psprintf_data ps;
    void *ptr;

    ps.base = malloc(512);
    if (ps.base == NULL) {
	fputs("Ouch!  Out of memory!\n", stderr);
	exit(1);
    }
    /* need room at beginning for allocation_list */
    ps.vbuff.curpos = ps.base + CLICK_SZ;
    ps.vbuff.endpos = ps.base + 511;
    apr_vformatter(psprintf_flush, &ps.vbuff, fmt, ap);
    *ps.vbuff.curpos++ = '\0';
    ptr = ps.base;
    /* shrink */
    ptr = realloc(ptr, (char *)ps.vbuff.curpos - (char *)ptr);
    if (ptr == NULL) {
	fputs("Ouch!  Out of memory!\n", stderr);
	exit(1);
    }
    *(void **)ptr = p->allocation_list;
    p->allocation_list = ptr;
    return (char *)ptr + CLICK_SZ;
#else
    struct psprintf_data ps;
    char *strp;
    apr_size_t size;

    ps.blok = p->last;
    ps.vbuff.curpos = ps.blok->h.first_avail;
    ps.vbuff.endpos = ps.blok->h.endp - 1;	/* save one for NUL */
    ps.got_a_new_block = 0;

    apr_vformatter(psprintf_flush, &ps.vbuff, fmt, ap);

    strp = ps.vbuff.curpos;
    *strp++ = '\0';

    size = strp - ps.blok->h.first_avail;
    size = (1 + ((size - 1) / CLICK_SZ)) * CLICK_SZ;
    strp = ps.blok->h.first_avail;	/* save away result pointer */
    ps.blok->h.first_avail += size;

    /* have to link the block in if it's a new one */
    if (ps.got_a_new_block) {
	p->last->h.next = ps.blok;
	p->last = ps.blok;
#ifdef APR_POOL_DEBUG
	ps.blok->h.owning_pool = p;
#endif
    }

    return strp;
#endif
}

APR_DECLARE_NONSTD(char *) apr_psprintf(apr_pool_t *p, const char *fmt, ...)
{
    va_list ap;
    char *res;

    va_start(ap, fmt);
    res = apr_pvsprintf(p, fmt, ap);
    va_end(ap);
    return res;
}


/*****************************************************************
 *
 * More grotty system stuff... subprocesses.  Frump.  These don't use
 * the generic cleanup interface because I don't want multiple
 * subprocesses to result in multiple three-second pauses; the
 * subprocesses have to be "freed" all at once.  If someone comes
 * along with another resource they want to allocate which has the
 * same property, we might want to fold support for that into the
 * generic interface, but for now, it's a special case
 */

APR_DECLARE(void) apr_note_subprocess(apr_pool_t *a, apr_proc_t *pid,
                                    enum kill_conditions how)
{
    struct process_chain *new =
    (struct process_chain *) apr_palloc(a, sizeof(struct process_chain));

    new->pid = pid;
    new->kill_how = how;
    new->next = a->subprocesses;
    a->subprocesses = new;
}

static void free_proc_chain(struct process_chain *procs)
{
    /* Dispose of the subprocesses we've spawned off in the course of
     * whatever it was we're cleaning up now.  This may involve killing
     * some of them off...
     */
    struct process_chain *p;
    int need_timeout = 0;

    if (procs == NULL) {
	return;			/* No work.  Whew! */
    }

    /* First, check to see if we need to do the SIGTERM, sleep, SIGKILL
     * dance with any of the processes we're cleaning up.  If we've got
     * any kill-on-sight subprocesses, ditch them now as well, so they
     * don't waste any more cycles doing whatever it is that they shouldn't
     * be doing anymore.
     */

#ifndef NEED_WAITPID
    /* Pick up all defunct processes */
    for (p = procs; p; p = p->next) {
        if (apr_wait_proc(p->pid, APR_NOWAIT) == APR_CHILD_DONE) {
            p->kill_how = kill_never;
        }
    }
#endif

    for (p = procs; p; p = p->next) {
        if ((p->kill_how == kill_after_timeout)
            || (p->kill_how == kill_only_once)) {
            /*
             * Subprocess may be dead already.  Only need the timeout if not.
             * Note: apr_kill on Windows is TerminateProcess(), which is 
             * similar to a SIGKILL, so always give the process a timeout
             * under Windows before killing it.
             */
#ifdef WIN32
            need_timeout = 1;
#else
	    if (apr_kill(p->pid, APR_SIGTERM) == APR_SUCCESS) {
		need_timeout = 1;
	    }
#endif
	}
	else if (p->kill_how == kill_always) {
	    apr_kill(p->pid, APR_SIGKILL);
	}
    }

    /* Sleep only if we have to... */
    if (need_timeout) {
	sleep(3);
    }

    /* OK, the scripts we just timed out for have had a chance to clean up
     * --- now, just get rid of them, and also clean up the system accounting
     * goop...
     */
    for (p = procs; p; p = p->next) {
	if (p->kill_how == kill_after_timeout) {
	    apr_kill(p->pid, APR_SIGKILL);
	}
    }
#ifdef WIN32
    /* 
     * Do we need an APR function to clean-up a proc_t?
     */
    {
        for (p = procs; p; p = p->next) {
            CloseHandle((HANDLE)p->pid->pid);
        }
    }
#endif /* WIN32 */

    /* Now wait for all the signaled processes to die */
    for (p = procs; p; p = p->next) {
	if (p->kill_how != kill_never) {
	    (void) apr_wait_proc(p->pid, APR_WAIT);
	}
    }
}
