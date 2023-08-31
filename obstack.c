/* obstack.c - object stack macros
   Copyright (C) 1988-2020 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <http://www.gnu.org/licenses/>.  */

/* Summary:

   All the apparent functions defined here are macros. The idea
   is that you would use these pre-tested macros to solve a
   very specific set of problems, and they would run fast.
   Caution: no side-effects in arguments please!! They may be
   evaluated MANY times!!

   These macros operate a stack of objects.  Each object starts life
   small, and may grow to maturity.  (Consider building a word syllable
   by syllable.)  An object can move while it is growing.  Once it has
   been "finished" it never changes address again.  So the "top of the
   stack" is typically an immature growing object, while the rest of the
   stack is of mature, fixed size and fixed address objects.

   These routines grab large chunks of memory, using a function you
   supply, called 'obstack_chunk_alloc'.  On occasion, they free chunks,
   by calling 'obstack_chunk_free'.  You must define them and declare
   them before using any obstack macros.

   Each independent stack is represented by a 'struct obstack'.
   Each of the obstack macros expects a pointer to such a structure
   as the first argument.

   One motivation for this package is the problem of growing char strings
   in symbol tables.  Unless you are "fascist pig with a read-only mind"
   --Gosper's immortal quote from HAKMEM item 154, out of context--you
   would not like to put any arbitrary upper limit on the length of your
   symbols.

   In practice this often means you will build many short symbols and a
   few long symbols.  At the time you are reading a symbol you don't know
   how long it is.  One traditional method is to read a symbol into a
   buffer, realloc()ating the buffer every time you try to read a symbol
   that is longer than the buffer.  This is beaut, but you still will
   want to copy the symbol from the buffer to a more permanent
   symbol-table entry say about half the time.

   With obstacks, you can work differently.  Use one obstack for all symbol
   names.  As you read a symbol, grow the name in the obstack gradually.
   When the name is complete, finalize it.  Then, if the symbol exists already,
   free the newly read name.

   The way we do this is to take a large chunk, allocating memory from
   low addresses.  When you want to build a symbol in the chunk you just
   add chars above the current "high water mark" in the chunk.  When you
   have finished adding chars, because you got to the end of the symbol,
   you know how long the chars are, and you can create a new object.
   Mostly the chars will not burst over the highest address of the chunk,
   because you would typically expect a chunk to be (say) 100 times as
   long as an average object.

   In case that isn't clear, when we have enough chars to make up
   the object, THEY ARE ALREADY CONTIGUOUS IN THE CHUNK (guaranteed)
   so we just point to it where it lies.  No moving of chars is
   needed and this is the second win: potentially long strings need
   never be explicitly shuffled. Once an object is formed, it does not
   change its address during its lifetime.

   When the chars burst over a chunk boundary, we allocate a larger
   chunk, and then copy the partly formed object from the end of the old
   chunk to the beginning of the new larger chunk.  We then carry on
   accreting characters to the end of the object as we normally would.

   A special macro is provided to add a single char at a time to a
   growing object.  This allows the use of register variables, which
   break the ordinary 'growth' macro.

   Summary:
        We allocate large chunks.
        We carve out one object at a time from the current chunk.
        Once carved, an object never moves.
        We are free to append data of any size to the currently
          growing object.
        Exactly one object is growing in an obstack at any one time.
        You can run one obstack per control block.
        You may have as many control blocks as you dare.
        Because of the way we do it, you can "unwind" an obstack
          back to a previous state. (You may remove objects much
          as you would with a stack.)
 */


/* Don't do the contents of this file more than once.  */

#ifndef _OBSTACK_H
#define _OBSTACK_H 1

#ifndef _OBSTACK_INTERFACE_VERSION
# define _OBSTACK_INTERFACE_VERSION 2
#endif

#include <stddef.h>             /* For size_t and ptrdiff_t.  */
#include <string.h>             /* For __GNU_LIBRARY__, and memcpy.  */

#if _OBSTACK_INTERFACE_VERSION == 1
/* For binary compatibility with obstack version 1, which used "int"
   and "long" for these two types.  */
# define _OBSTACK_SIZE_T unsigned int
# define _CHUNK_SIZE_T unsigned long
# define _OBSTACK_CAST(type, expr) ((type) (expr))
#else
/* Version 2 with sane types, especially for 64-bit hosts.  */
# define _OBSTACK_SIZE_T size_t
# define _CHUNK_SIZE_T size_t
# define _OBSTACK_CAST(type, expr) (expr)
#endif

/* If B is the base of an object addressed by P, return the result of
   aligning P to the next multiple of A + 1.  B and P must be of type
   char *.  A + 1 must be a power of 2.  */

#define __BPTR_ALIGN(B, P, A) ((B) + (((P) - (B) + (A)) & ~(A)))

/* Similar to __BPTR_ALIGN (B, P, A), except optimize the common case
   where pointers can be converted to integers, aligned as integers,
   and converted back again.  If ptrdiff_t is narrower than a
   pointer (e.g., the AS/400), play it safe and compute the alignment
   relative to B.  Otherwise, use the faster strategy of computing the
   alignment relative to 0.  */

#define __PTR_ALIGN(B, P, A)						      \
  __BPTR_ALIGN (sizeof (ptrdiff_t) < sizeof (void *) ? (B) : (char *) 0,      \
                P, A)

#ifndef __attribute_pure__
# if defined __GNUC_MINOR__ && __GNUC__ * 1000 + __GNUC_MINOR__ >= 2096
#  define __attribute_pure__ __attribute__ ((__pure__))
# else
#  define __attribute_pure__
# endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct _obstack_chunk           /* Lives at front of each chunk. */
{
  char *limit;                  /* 1 past end of this chunk */
  struct _obstack_chunk *prev;  /* address of prior chunk or NULL */
  char contents[4];             /* objects begin here */
};

struct obstack          /* control current object in current chunk */
{
  _CHUNK_SIZE_T chunk_size;     /* preferred size to allocate chunks in */
  struct _obstack_chunk *chunk; /* address of current struct obstack_chunk */
  char *object_base;            /* address of object we are building */
  char *next_free;              /* where to add next char to current object */
  char *chunk_limit;            /* address of char after current chunk */
  union
  {
    _OBSTACK_SIZE_T i;
    void *p;
  } temp;                       /* Temporary for some macros.  */
  _OBSTACK_SIZE_T alignment_mask;  /* Mask of alignment for each object. */

  /* These prototypes vary based on 'use_extra_arg'.  */
  union
  {
    void *(*plain) (size_t);
    void *(*extra) (void *, size_t);
  } chunkfun;
  union
  {
    void (*plain) (void *);
    void (*extra) (void *, void *);
  } freefun;

  void *extra_arg;              /* first arg for chunk alloc/dealloc funcs */
  unsigned use_extra_arg : 1;     /* chunk alloc/dealloc funcs take extra arg */
  unsigned maybe_empty_object : 1; /* There is a possibility that the current
                                      chunk contains a zero-length object.  This
                                      prevents freeing the chunk if we allocate
                                      a bigger chunk to replace it. */
  unsigned alloc_failed : 1;      /* No longer used, as we now call the failed
                                     handler on error, but retained for binary
                                     compatibility.  */
};

/* Declare the external functions we use; they are in obstack.c.  */

extern void _obstack_newchunk (struct obstack *, _OBSTACK_SIZE_T);
extern void _obstack_free (struct obstack *, void *);
extern int _obstack_begin (struct obstack *,
                           _OBSTACK_SIZE_T, _OBSTACK_SIZE_T,
                           void *(*) (size_t), void (*) (void *));
extern int _obstack_begin_1 (struct obstack *,
                             _OBSTACK_SIZE_T, _OBSTACK_SIZE_T,
                             void *(*) (void *, size_t),
                             void (*) (void *, void *), void *);
extern _OBSTACK_SIZE_T _obstack_memory_used (struct obstack *)
  __attribute_pure__;

/* Declare obstack_printf; it's in obstack_printf.c. */
extern int obstack_printf(struct obstack *obstack, const char *__restrict fmt, ...);


/* Error handler called when 'obstack_chunk_alloc' failed to allocate
   more memory.  This can be set to a user defined function which
   should either abort gracefully or use longjump - but shouldn't
   return.  The default action is to print a message and abort.  */
extern void (*obstack_alloc_failed_handler) (void);

/* Exit value used when 'print_and_abort' is used.  */
extern int obstack_exit_failure;

/* Pointer to beginning of object being allocated or to be allocated next.
   Note that this might not be the final address of the object
   because a new chunk might be needed to hold the final size.  */

#define obstack_base(h) ((void *) (h)->object_base)

/* Size for allocating ordinary chunks.  */

#define obstack_chunk_size(h) ((h)->chunk_size)

/* Pointer to next byte not yet allocated in current chunk.  */

#define obstack_next_free(h) ((void *) (h)->next_free)

/* Mask specifying low bits that should be clear in address of an object.  */

#define obstack_alignment_mask(h) ((h)->alignment_mask)

/* To prevent prototype warnings provide complete argument list.  */
#define obstack_init(h)							      \
  _obstack_begin ((h), 0, 0,						      \
                  _OBSTACK_CAST (void *(*) (size_t), obstack_chunk_alloc),    \
                  _OBSTACK_CAST (void (*) (void *), obstack_chunk_free))

#define obstack_begin(h, size)						      \
  _obstack_begin ((h), (size), 0,					      \
                  _OBSTACK_CAST (void *(*) (size_t), obstack_chunk_alloc), \
                  _OBSTACK_CAST (void (*) (void *), obstack_chunk_free))

#define obstack_specify_allocation(h, size, alignment, chunkfun, freefun)     \
  _obstack_begin ((h), (size), (alignment),				      \
                  _OBSTACK_CAST (void *(*) (size_t), chunkfun),		      \
                  _OBSTACK_CAST (void (*) (void *), freefun))

#define obstack_specify_allocation_with_arg(h, size, alignment, chunkfun, freefun, arg) \
  _obstack_begin_1 ((h), (size), (alignment),				      \
                    _OBSTACK_CAST (void *(*) (void *, size_t), chunkfun),     \
                    _OBSTACK_CAST (void (*) (void *, void *), freefun), arg)

#define obstack_chunkfun(h, newchunkfun)				      \
  ((void) ((h)->chunkfun.extra = (void *(*) (void *, size_t)) (newchunkfun)))

#define obstack_freefun(h, newfreefun)					      \
  ((void) ((h)->freefun.extra = (void *(*) (void *, void *)) (newfreefun)))

#define obstack_1grow_fast(h, achar) ((void) (*((h)->next_free)++ = (achar)))

#define obstack_blank_fast(h, n) ((void) ((h)->next_free += (n)))

#define obstack_memory_used(h) _obstack_memory_used (h)

#if defined __GNUC__
# if !defined __GNUC_MINOR__ || __GNUC__ * 1000 + __GNUC_MINOR__ < 2008
#  define __extension__
# endif

/* For GNU C, if not -traditional,
   we can define these macros to compute all args only once
   without using a global variable.
   Also, we can avoid using the 'temp' slot, to make faster code.  */

# define obstack_object_size(OBSTACK)					      \
  __extension__								      \
    ({ struct obstack const *__o = (OBSTACK);				      \
       (_OBSTACK_SIZE_T) (__o->next_free - __o->object_base); })

/* The local variable is named __o1 to avoid a shadowed variable
   warning when invoked from other obstack macros.  */
# define obstack_room(OBSTACK)						      \
  __extension__								      \
    ({ struct obstack const *__o1 = (OBSTACK);				      \
       (_OBSTACK_SIZE_T) (__o1->chunk_limit - __o1->next_free); })

# define obstack_make_room(OBSTACK, length)				      \
  __extension__								      \
    ({ struct obstack *__o = (OBSTACK);					      \
       _OBSTACK_SIZE_T __len = (length);				      \
       if (obstack_room (__o) < __len)					      \
         _obstack_newchunk (__o, __len);				      \
       (void) 0; })

# define obstack_empty_p(OBSTACK)					      \
  __extension__								      \
    ({ struct obstack const *__o = (OBSTACK);				      \
       (__o->chunk->prev == 0						      \
        && __o->next_free == __PTR_ALIGN ((char *) __o->chunk,		      \
                                          __o->chunk->contents,		      \
                                          __o->alignment_mask)); })

# define obstack_grow(OBSTACK, where, length)				      \
  __extension__								      \
    ({ struct obstack *__o = (OBSTACK);					      \
       _OBSTACK_SIZE_T __len = (length);				      \
       if (obstack_room (__o) < __len)					      \
         _obstack_newchunk (__o, __len);				      \
       memcpy (__o->next_free, where, __len);				      \
       __o->next_free += __len;						      \
       (void) 0; })

# define obstack_grow0(OBSTACK, where, length)				      \
  __extension__								      \
    ({ struct obstack *__o = (OBSTACK);					      \
       _OBSTACK_SIZE_T __len = (length);				      \
       if (obstack_room (__o) < __len + 1)				      \
         _obstack_newchunk (__o, __len + 1);				      \
       memcpy (__o->next_free, where, __len);				      \
       __o->next_free += __len;						      \
       *(__o->next_free)++ = 0;						      \
       (void) 0; })

# define obstack_1grow(OBSTACK, datum)					      \
  __extension__								      \
    ({ struct obstack *__o = (OBSTACK);					      \
       if (obstack_room (__o) < 1)					      \
         _obstack_newchunk (__o, 1);					      \
       obstack_1grow_fast (__o, datum); })

/* These assume that the obstack alignment is good enough for pointers
   or ints, and that the data added so far to the current object
   shares that much alignment.  */

# define obstack_ptr_grow(OBSTACK, datum)				      \
  __extension__								      \
    ({ struct obstack *__o = (OBSTACK);					      \
       if (obstack_room (__o) < sizeof (void *))			      \
         _obstack_newchunk (__o, sizeof (void *));			      \
       obstack_ptr_grow_fast (__o, datum); })

# define obstack_int_grow(OBSTACK, datum)				      \
  __extension__								      \
    ({ struct obstack *__o = (OBSTACK);					      \
       if (obstack_room (__o) < sizeof (int))				      \
         _obstack_newchunk (__o, sizeof (int));				      \
       obstack_int_grow_fast (__o, datum); })

# define obstack_ptr_grow_fast(OBSTACK, aptr)				      \
  __extension__								      \
    ({ struct obstack *__o1 = (OBSTACK);				      \
       void *__p1 = __o1->next_free;					      \
       *(const void **) __p1 = (aptr);					      \
       __o1->next_free += sizeof (const void *);			      \
       (void) 0; })

# define obstack_int_grow_fast(OBSTACK, aint)				      \
  __extension__								      \
    ({ struct obstack *__o1 = (OBSTACK);				      \
       void *__p1 = __o1->next_free;					      \
       *(int *) __p1 = (aint);						      \
       __o1->next_free += sizeof (int);					      \
       (void) 0; })

# define obstack_blank(OBSTACK, length)					      \
  __extension__								      \
    ({ struct obstack *__o = (OBSTACK);					      \
       _OBSTACK_SIZE_T __len = (length);				      \
       if (obstack_room (__o) < __len)					      \
         _obstack_newchunk (__o, __len);				      \
       obstack_blank_fast (__o, __len); })

# define obstack_alloc(OBSTACK, length)					      \
  __extension__								      \
    ({ struct obstack *__h = (OBSTACK);					      \
       obstack_blank (__h, (length));					      \
       obstack_finish (__h); })

# define obstack_copy(OBSTACK, where, length)				      \
  __extension__								      \
    ({ struct obstack *__h = (OBSTACK);					      \
       obstack_grow (__h, (where), (length));				      \
       obstack_finish (__h); })

# define obstack_copy0(OBSTACK, where, length)				      \
  __extension__								      \
    ({ struct obstack *__h = (OBSTACK);					      \
       obstack_grow0 (__h, (where), (length));				      \
       obstack_finish (__h); })

/* The local variable is named __o1 to avoid a shadowed variable
   warning when invoked from other obstack macros, typically obstack_free.  */
# define obstack_finish(OBSTACK)					      \
  __extension__								      \
    ({ struct obstack *__o1 = (OBSTACK);				      \
       void *__value = (void *) __o1->object_base;			      \
       if (__o1->next_free == __value)					      \
         __o1->maybe_empty_object = 1;					      \
       __o1->next_free							      \
         = __PTR_ALIGN (__o1->object_base, __o1->next_free,		      \
                        __o1->alignment_mask);				      \
       if ((size_t) (__o1->next_free - (char *) __o1->chunk)		      \
           > (size_t) (__o1->chunk_limit - (char *) __o1->chunk))	      \
         __o1->next_free = __o1->chunk_limit;				      \
       __o1->object_base = __o1->next_free;				      \
       __value; })

# define obstack_free(OBSTACK, OBJ)					      \
  __extension__								      \
    ({ struct obstack *__o = (OBSTACK);					      \
       void *__obj = (void *) (OBJ);					      \
       if (__obj > (void *) __o->chunk && __obj < (void *) __o->chunk_limit)  \
         __o->next_free = __o->object_base = (char *) __obj;		      \
       else								      \
         _obstack_free (__o, __obj); })

#else /* not __GNUC__ */

# define obstack_object_size(h)						      \
  ((_OBSTACK_SIZE_T) ((h)->next_free - (h)->object_base))

# define obstack_room(h)						      \
  ((_OBSTACK_SIZE_T) ((h)->chunk_limit - (h)->next_free))

# define obstack_empty_p(h)						      \
  ((h)->chunk->prev == 0						      \
   && (h)->next_free == __PTR_ALIGN ((char *) (h)->chunk,		      \
                                     (h)->chunk->contents,		      \
                                     (h)->alignment_mask))

/* Note that the call to _obstack_newchunk is enclosed in (..., 0)
   so that we can avoid having void expressions
   in the arms of the conditional expression.
   Casting the third operand to void was tried before,
   but some compilers won't accept it.  */

# define obstack_make_room(h, length)					      \
  ((h)->temp.i = (length),						      \
   ((obstack_room (h) < (h)->temp.i)					      \
    ? (_obstack_newchunk (h, (h)->temp.i), 0) : 0),			      \
   (void) 0)

# define obstack_grow(h, where, length)					      \
  ((h)->temp.i = (length),						      \
   ((obstack_room (h) < (h)->temp.i)					      \
   ? (_obstack_newchunk ((h), (h)->temp.i), 0) : 0),			      \
   memcpy ((h)->next_free, where, (h)->temp.i),				      \
   (h)->next_free += (h)->temp.i,					      \
   (void) 0)

# define obstack_grow0(h, where, length)				      \
  ((h)->temp.i = (length),						      \
   ((obstack_room (h) < (h)->temp.i + 1)				      \
   ? (_obstack_newchunk ((h), (h)->temp.i + 1), 0) : 0),		      \
   memcpy ((h)->next_free, where, (h)->temp.i),				      \
   (h)->next_free += (h)->temp.i,					      \
   *((h)->next_free)++ = 0,						      \
   (void) 0)

# define obstack_1grow(h, datum)					      \
  (((obstack_room (h) < 1)						      \
    ? (_obstack_newchunk ((h), 1), 0) : 0),				      \
   obstack_1grow_fast (h, datum))

# define obstack_ptr_grow(h, datum)					      \
  (((obstack_room (h) < sizeof (char *))				      \
    ? (_obstack_newchunk ((h), sizeof (char *)), 0) : 0),		      \
   obstack_ptr_grow_fast (h, datum))

# define obstack_int_grow(h, datum)					      \
  (((obstack_room (h) < sizeof (int))					      \
    ? (_obstack_newchunk ((h), sizeof (int)), 0) : 0),			      \
   obstack_int_grow_fast (h, datum))

# define obstack_ptr_grow_fast(h, aptr)					      \
  (((const void **) ((h)->next_free += sizeof (void *)))[-1] = (aptr),	      \
   (void) 0)

# define obstack_int_grow_fast(h, aint)					      \
  (((int *) ((h)->next_free += sizeof (int)))[-1] = (aint),		      \
   (void) 0)

# define obstack_blank(h, length)					      \
  ((h)->temp.i = (length),						      \
   ((obstack_room (h) < (h)->temp.i)					      \
   ? (_obstack_newchunk ((h), (h)->temp.i), 0) : 0),			      \
   obstack_blank_fast (h, (h)->temp.i))

# define obstack_alloc(h, length)					      \
  (obstack_blank ((h), (length)), obstack_finish ((h)))

# define obstack_copy(h, where, length)					      \
  (obstack_grow ((h), (where), (length)), obstack_finish ((h)))

# define obstack_copy0(h, where, length)				      \
  (obstack_grow0 ((h), (where), (length)), obstack_finish ((h)))

# define obstack_finish(h)						      \
  (((h)->next_free == (h)->object_base					      \
    ? (((h)->maybe_empty_object = 1), 0)				      \
    : 0),								      \
   (h)->temp.p = (h)->object_base,					      \
   (h)->next_free							      \
     = __PTR_ALIGN ((h)->object_base, (h)->next_free,			      \
                    (h)->alignment_mask),				      \
   (((size_t) ((h)->next_free - (char *) (h)->chunk)			      \
     > (size_t) ((h)->chunk_limit - (char *) (h)->chunk))		      \
   ? ((h)->next_free = (h)->chunk_limit) : 0),				      \
   (h)->object_base = (h)->next_free,					      \
   (h)->temp.p)

# define obstack_free(h, obj)						      \
  ((h)->temp.p = (void *) (obj),					      \
   (((h)->temp.p > (void *) (h)->chunk					      \
     && (h)->temp.p < (void *) (h)->chunk_limit)			      \
    ? (void) ((h)->next_free = (h)->object_base = (char *) (h)->temp.p)       \
    : _obstack_free ((h), (h)->temp.p)))

#endif /* not __GNUC__ */

#ifdef __cplusplus
}       /* C++ */
#endif

#endif /* _OBSTACK_H */

/* NOTE BEFORE MODIFYING THIS FILE: _OBSTACK_INTERFACE_VERSION in
   obstack.h must be incremented whenever callers compiled using an old
   obstack.h can no longer properly call the functions in this file.  */

/* Comment out all this code if we are using the GNU C Library, and are not
   actually compiling the library itself, and the installed library
   supports the same library interface we do.  This code is part of the GNU
   C Library, but also included in many other GNU distributions.  Compiling
   and linking in this code is a waste when using the GNU C library
   (especially if it is a shared library).  Rather than having every GNU
   program understand 'configure --with-gnu-libc' and omit the object
   files, it is simpler to just do this in the source for each such file.  */
#if !defined _LIBC && defined __GNU_LIBRARY__ && __GNU_LIBRARY__ > 1
# include <gnu-versions.h>
# if (_GNU_OBSTACK_INTERFACE_VERSION == _OBSTACK_INTERFACE_VERSION	      \
      || (_GNU_OBSTACK_INTERFACE_VERSION == 1				      \
          && _OBSTACK_INTERFACE_VERSION == 2				      \
          && defined SIZEOF_INT && defined SIZEOF_SIZE_T		      \
          && SIZEOF_INT == SIZEOF_SIZE_T))
#  define _OBSTACK_ELIDE_CODE
# endif
#endif

#ifndef _OBSTACK_ELIDE_CODE
/* If GCC, or if an oddball (testing?) host that #defines __alignof__,
   use the already-supplied __alignof__.  Otherwise, this must be Gnulib
   (as glibc assumes GCC); defer to Gnulib's alignof_type.  */
# if !defined __GNUC__ && !defined __IBM__ALIGNOF__ && !defined __alignof__
#  if defined __cplusplus
template <class type> struct alignof_helper { char __slot1; type __slot2; };
#   define __alignof__(type) offsetof (alignof_helper<type>, __slot2)
#  else
#   define __alignof__(type)						      \
  offsetof (struct { char __slot1; type __slot2; }, __slot2)
#  endif
# endif
# include <stdlib.h>
# include <stdint.h>

# ifndef MAX
#  define MAX(a,b) ((a) > (b) ? (a) : (b))
# endif

/* Determine default alignment.  */

/* If malloc were really smart, it would round addresses to DEFAULT_ALIGNMENT.
   But in fact it might be less smart and round addresses to as much as
   DEFAULT_ROUNDING.  So we prepare for it to do that.

   DEFAULT_ALIGNMENT cannot be an enum constant; see gnulib's alignof.h.  */
#define DEFAULT_ALIGNMENT MAX (__alignof__ (long double),		      \
                               MAX (__alignof__ (uintmax_t),		      \
                                    __alignof__ (void *)))
#define DEFAULT_ROUNDING MAX (sizeof (long double),			      \
                               MAX (sizeof (uintmax_t),			      \
                                    sizeof (void *)))

/* Call functions with either the traditional malloc/free calling
   interface, or the mmalloc/mfree interface (that adds an extra first
   argument), based on the value of use_extra_arg.  */

static void *
call_chunkfun (struct obstack *h, size_t size)
{
  if (h->use_extra_arg)
    return h->chunkfun.extra (h->extra_arg, size);
  else
    return h->chunkfun.plain (size);
}

static void
call_freefun (struct obstack *h, void *old_chunk)
{
  if (h->use_extra_arg)
    h->freefun.extra (h->extra_arg, old_chunk);
  else
    h->freefun.plain (old_chunk);
}


/* Initialize an obstack H for use.  Specify chunk size SIZE (0 means default).
   Objects start on multiples of ALIGNMENT (0 means use default).

   Return nonzero if successful, calls obstack_alloc_failed_handler if
   allocation fails.  */

static int
_obstack_begin_worker (struct obstack *h,
                       _OBSTACK_SIZE_T size, _OBSTACK_SIZE_T alignment)
{
  struct _obstack_chunk *chunk; /* points to new chunk */

  if (alignment == 0)
    alignment = DEFAULT_ALIGNMENT;
  if (size == 0)
    /* Default size is what GNU malloc can fit in a 4096-byte block.  */
    {
      /* 12 is sizeof (mhead) and 4 is EXTRA from GNU malloc.
         Use the values for range checking, because if range checking is off,
         the extra bytes won't be missed terribly, but if range checking is on
         and we used a larger request, a whole extra 4096 bytes would be
         allocated.

         These number are irrelevant to the new GNU malloc.  I suspect it is
         less sensitive to the size of the request.  */
      int extra = ((((12 + DEFAULT_ROUNDING - 1) & ~(DEFAULT_ROUNDING - 1))
                    + 4 + DEFAULT_ROUNDING - 1)
                   & ~(DEFAULT_ROUNDING - 1));
      size = 4096 - extra;
    }

  h->chunk_size = size;
  h->alignment_mask = alignment - 1;

  chunk = (struct _obstack_chunk *) call_chunkfun (h, h->chunk_size);
  if (!chunk)
    (*obstack_alloc_failed_handler) ();
  h->chunk = chunk;
  h->next_free = h->object_base = __PTR_ALIGN ((char *) chunk, chunk->contents,
                                               alignment - 1);
  h->chunk_limit = chunk->limit = (char *) chunk + h->chunk_size;
  chunk->prev = 0;
  /* The initial chunk now contains no empty object.  */
  h->maybe_empty_object = 0;
  h->alloc_failed = 0;
  return 1;
}

int
_obstack_begin (struct obstack *h,
                _OBSTACK_SIZE_T size, _OBSTACK_SIZE_T alignment,
                void *(*chunkfun) (size_t),
                void (*freefun) (void *))
{
  h->chunkfun.plain = chunkfun;
  h->freefun.plain = freefun;
  h->use_extra_arg = 0;
  return _obstack_begin_worker (h, size, alignment);
}

int
_obstack_begin_1 (struct obstack *h,
                  _OBSTACK_SIZE_T size, _OBSTACK_SIZE_T alignment,
                  void *(*chunkfun) (void *, size_t),
                  void (*freefun) (void *, void *),
                  void *arg)
{
  h->chunkfun.extra = chunkfun;
  h->freefun.extra = freefun;
  h->extra_arg = arg;
  h->use_extra_arg = 1;
  return _obstack_begin_worker (h, size, alignment);
}

/* Allocate a new current chunk for the obstack *H
   on the assumption that LENGTH bytes need to be added
   to the current object, or a new object of length LENGTH allocated.
   Copies any partial object from the end of the old chunk
   to the beginning of the new one.  */

void
_obstack_newchunk (struct obstack *h, _OBSTACK_SIZE_T length)
{
  struct _obstack_chunk *old_chunk = h->chunk;
  struct _obstack_chunk *new_chunk = 0;
  size_t obj_size = h->next_free - h->object_base;
  char *object_base;

  /* Compute size for new chunk.  */
  size_t sum1 = obj_size + length;
  size_t sum2 = sum1 + h->alignment_mask;
  size_t new_size = sum2 + (obj_size >> 3) + 100;
  if (new_size < sum2)
    new_size = sum2;
  if (new_size < h->chunk_size)
    new_size = h->chunk_size;

  /* Allocate and initialize the new chunk.  */
  if (obj_size <= sum1 && sum1 <= sum2)
    new_chunk = (struct _obstack_chunk *) call_chunkfun (h, new_size);
  if (!new_chunk)
    (*obstack_alloc_failed_handler)();
  h->chunk = new_chunk;
  new_chunk->prev = old_chunk;
  new_chunk->limit = h->chunk_limit = (char *) new_chunk + new_size;

  /* Compute an aligned object_base in the new chunk */
  object_base =
    __PTR_ALIGN ((char *) new_chunk, new_chunk->contents, h->alignment_mask);

  /* Move the existing object to the new chunk.  */
  memcpy (object_base, h->object_base, obj_size);

  /* If the object just copied was the only data in OLD_CHUNK,
     free that chunk and remove it from the chain.
     But not if that chunk might contain an empty object.  */
  if (!h->maybe_empty_object
      && (h->object_base
          == __PTR_ALIGN ((char *) old_chunk, old_chunk->contents,
                          h->alignment_mask)))
    {
      new_chunk->prev = old_chunk->prev;
      call_freefun (h, old_chunk);
    }

  h->object_base = object_base;
  h->next_free = h->object_base + obj_size;
  /* The new chunk certainly contains no empty object yet.  */
  h->maybe_empty_object = 0;
}

/* Return nonzero if object OBJ has been allocated from obstack H.
   This is here for debugging.
   If you use it in a program, you are probably losing.  */

/* Suppress -Wmissing-prototypes warning.  We don't want to declare this in
   obstack.h because it is just for debugging.  */
int _obstack_allocated_p (struct obstack *h, void *obj) __attribute_pure__;

int
_obstack_allocated_p (struct obstack *h, void *obj)
{
  struct _obstack_chunk *lp;    /* below addr of any objects in this chunk */
  struct _obstack_chunk *plp;   /* point to previous chunk if any */

  lp = (h)->chunk;
  /* We use >= rather than > since the object cannot be exactly at
     the beginning of the chunk but might be an empty object exactly
     at the end of an adjacent chunk.  */
  while (lp != 0 && ((void *) lp >= obj || (void *) (lp)->limit < obj))
    {
      plp = lp->prev;
      lp = plp;
    }
  return lp != 0;
}

/* Free objects in obstack H, including OBJ and everything allocate
   more recently than OBJ.  If OBJ is zero, free everything in H.  */

void
_obstack_free (struct obstack *h, void *obj)
{
  struct _obstack_chunk *lp;    /* below addr of any objects in this chunk */
  struct _obstack_chunk *plp;   /* point to previous chunk if any */

  lp = h->chunk;
  /* We use >= because there cannot be an object at the beginning of a chunk.
     But there can be an empty object at that address
     at the end of another chunk.  */
  while (lp != 0 && ((void *) lp >= obj || (void *) (lp)->limit < obj))
    {
      plp = lp->prev;
      call_freefun (h, lp);
      lp = plp;
      /* If we switch chunks, we can't tell whether the new current
         chunk contains an empty object, so assume that it may.  */
      h->maybe_empty_object = 1;
    }
  if (lp)
    {
      h->object_base = h->next_free = (char *) (obj);
      h->chunk_limit = lp->limit;
      h->chunk = lp;
    }
  else if (obj != 0)
    /* obj is not in any of the chunks! */
    abort ();
}

extern __typeof(_obstack_free) obstack_free __attribute__((alias("_obstack_free")));

_OBSTACK_SIZE_T
_obstack_memory_used (struct obstack *h)
{
  struct _obstack_chunk *lp;
  _OBSTACK_SIZE_T nbytes = 0;

  for (lp = h->chunk; lp != 0; lp = lp->prev)
    {
      nbytes += lp->limit - (char *) lp;
    }
  return nbytes;
}

# ifndef _OBSTACK_NO_ERROR_HANDLER
/* Define the error handler.  */
#  include <stdio.h>

/* Exit value used when 'print_and_abort' is used.  */
#  ifdef _LIBC
int obstack_exit_failure = EXIT_FAILURE;
#  else
#   ifndef EXIT_FAILURE
#    define EXIT_FAILURE 1
#   endif
#   define obstack_exit_failure EXIT_FAILURE
#  endif

#  if defined _LIBC || (HAVE_LIBINTL_H && ENABLE_NLS)
#   include <libintl.h>
#   ifndef _
#    define _(msgid) gettext (msgid)
#   endif
#  else
#   ifndef _
#    define _(msgid) (msgid)
#   endif
#  endif

#  if !(defined _Noreturn						      \
        || (defined __STDC_VERSION__ && __STDC_VERSION__ >= 201112))
#   if ((defined __GNUC__						      \
	 && (__GNUC__ >= 3 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 8)))	      \
	|| (defined __SUNPRO_C && __SUNPRO_C >= 0x5110))
#    define _Noreturn __attribute__ ((__noreturn__))
#   elif defined _MSC_VER && _MSC_VER >= 1200
#    define _Noreturn __declspec (noreturn)
#   else
#    define _Noreturn
#   endif
#  endif

#  ifdef _LIBC
#   include <libio/iolibio.h>
#  endif

static _Noreturn void
print_and_abort (void)
{
  /* Don't change any of these strings.  Yes, it would be possible to add
     the newline to the string and use fputs or so.  But this must not
     happen because the "memory exhausted" message appears in other places
     like this and the translation should be reused instead of creating
     a very similar string which requires a separate translation.  */
#  ifdef _LIBC
  (void) __fxprintf (NULL, "%s\n", _("memory exhausted"));
#  else
  fprintf (stderr, "%s\n", _("memory exhausted"));
#  endif
  exit (obstack_exit_failure);
}

/* The functions allocating more room by calling 'obstack_chunk_alloc'
   jump to the handler pointed to by 'obstack_alloc_failed_handler'.
   This can be set to a user defined function which should either
   abort gracefully or use longjump - but shouldn't return.  This
   variable by default points to the internal function
   'print_and_abort'.  */
void (*obstack_alloc_failed_handler) (void) = print_and_abort;
# endif /* !_OBSTACK_NO_ERROR_HANDLER */
#endif /* !_OBSTACK_ELIDE_CODE */

#include <stdarg.h>
#include <stdio.h>
#include <strings.h>

int obstack_printf(struct obstack *obstack, const char *__restrict fmt, ...)
{
	char buf[1024];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	obstack_grow(obstack, buf, len);
	va_end(ap);

	return len;
}


int obstack_vprintf(struct obstack *obstack, const char *__restrict fmt, va_list ap)
{
	char buf[1024];
	int len;

	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	obstack_grow(obstack, buf, len);

	return len;
}
