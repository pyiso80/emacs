#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "blockinput.h"
#include "buffer.h"
#include "charset.h"
#include "coding.h"
#include "fingerprint.h"
#include "frame.h"
#include "getpagesize.h"
#include "intervals.h"
#include "lisp.h"
#include "pdumper.h"
#include "window.h"


/*
  TODO:

  - Make sure global finalizer list makes it across the dump.

  - Respect dump_object_contents.

  - Two-pass dumping: first assemble object list, then write all.

  - Don't emit relocations that happen to set Emacs memory locations
    to values they will already have.

  - Check at dump time that relocations are properly aligned.

  - Nullify frame_and_buffer_state.

  - Preferred base address for relocation-free non-PIC startup.

  - Compressed dump support.

  - Automate detection of struct layout changes.

*/


#ifdef HAVE_PDUMPER

#ifdef __GNUC__
# pragma GCC diagnostic error "-Wconversion"
# pragma GCC diagnostic error "-Wshadow"
# define ALLOW_IMPLICIT_CONVERSION                       \
  _Pragma ("GCC diagnostic push")                        \
  _Pragma ("GCC diagnostic ignored \"-Wconversion\"")
  _Pragma ("GCC diagnostic ignored \"-Wsign-conversion\"")
# define DISALLOW_IMPLICIT_CONVERSION \
  _Pragma ("GCC diagnostic pop")
#else
# define ALLOW_IMPLICIT_CONVERSION ((void)0)
# define DISALLOW_IMPLICIT_CONVERSION ((void)0)
#endif

#define VM_POSIX 1
#define VM_MS_WINDOWS 2

#if defined (HAVE_MMAP) && defined (MAP_FIXED)
# define VM_SUPPORTED VM_POSIX
# if !defined (MAP_POPULATE) && defined (MAP_PREFAULT_READ)
#  define MAP_POPULATE MAP_PREFAULT_READ
# elif !defined (MAP_POPULATE)
#  define MAP_POPULATE 0
# endif
#elif defined (WINDOWSNT)
  /* Use a float infinity, to avoid compiler warnings in comparing vs
     candidates' score.  */
# undef INFINITY
# define INFINITY __builtin_inff ()
# include <windows.h>
# define VM_SUPPORTED VM_MS_WINDOWS
#else
# define VM_SUPPORTED 0
#endif

#define DANGEROUS 0

/* PDUMPER_CHECK_REHASHING being true causes the portable dumper to
   check, for each hash table it dumps, that the hash table means the
   same thing after rehashing.  */
#ifndef PDUMPER_CHECK_REHASHING
# if ENABLE_CHECKING
#  define PDUMPER_CHECK_REHASHING 1
# else
#  define PDUMPER_CHECK_REHASHING 0
# endif
#endif

/* We require an architecture in which all pointers are the same size
   and have the same layout, where pointers are either 32 or 64 bits
   long, and where bytes have eight bits --- that is, a
   general-purpose computer made after 1990.  */
verify (sizeof (ptrdiff_t) == sizeof (void*));
verify (sizeof (intptr_t) == sizeof (ptrdiff_t));
verify (sizeof (void (*)(void)) == sizeof (void*));
verify (sizeof (ptrdiff_t) <= sizeof (Lisp_Object));
verify (sizeof (ptrdiff_t) <= sizeof (EMACS_INT));
verify (sizeof (off_t) == sizeof (int32_t) ||
        sizeof (off_t) == sizeof (int64_t));
verify (CHAR_BIT == 8);

#ifndef MAX_OFF_T
# define MAX_OFF_T ((sizeof (off_t) == sizeof (int32_t)) ?      \
                    INT32_MAX : INT64_MAX)
#endif

#define DIVIDE_ROUND_UP(x, y) (((x) + (y) - 1) / (y))

static const char dump_magic[16] = {
  'D', 'U', 'M', 'P', 'E', 'D',
  'G', 'N', 'U',
  'E', 'M', 'A', 'C', 'S'
};

static pdumper_hook dump_hooks[24];
static int nr_dump_hooks = 0;

static struct
{
  void *mem;
  int sz;
} remembered_data[32];
static int nr_remembered_data = 0;

typedef int32_t dump_off;
#define DUMP_OFF_T_MIN INT32_MIN
#define DUMP_OFF_T_MAX INT32_MAX

__attribute__((format (printf,1,2)))
static void
dump_trace (const char *fmt, ...)
{
  if (0)
    {
      va_list args;
      va_start (args, fmt);
      vfprintf (stderr, fmt, args);
      va_end (args);
    }
}

static ssize_t dump_read_all (int fd, void *buf, size_t bytes_to_read);

static dump_off
ptrdiff_t_to_dump_off (ptrdiff_t value)
{
  eassert (DUMP_OFF_T_MIN <= value);
  eassert (value <= DUMP_OFF_T_MAX);
  return (dump_off) value;
}

/* Worst-case allocation granularity on any system that might load
   this dump.  */
static int
dump_get_page_size (void)
{
#ifdef WINDOWSNT
  return 64 * 1024;  /* Worst-case allocation granularity.  */
#else
  return getpagesize ();
#endif
}

#define dump_offsetof(type, member)                             \
  (ptrdiff_t_to_dump_off (offsetof (type, member)))

enum dump_reloc_type
  {
    /* dump_ptr = dump_ptr + emacs_basis() */
    RELOC_DUMP_TO_EMACS_PTR_RAW,
    /* dump_ptr = dump_ptr + dump_base */
    RELOC_DUMP_TO_DUMP_PTR_RAW,
    /* dump_lv = make_lisp_ptr (
         dump_lv + dump_base,
         type - RELOC_DUMP_TO_DUMP_LV)
       (Special case for symbols: make_lisp_symbol)
       Must be second-last.  */
    RELOC_DUMP_TO_DUMP_LV,
    /* dump_lv = make_lisp_ptr (
         dump_lv + emacs_basis(),
         type - RELOC_DUMP_TO_DUMP_LV)
       (Special case for symbols: make_lisp_symbol.)
       Must be last.  */
    RELOC_DUMP_TO_EMACS_LV = RELOC_DUMP_TO_DUMP_LV + 8,
  };

enum emacs_reloc_type
  {
    /* Copy raw bytes from the dump into Emacs.  */
    RELOC_EMACS_COPY_FROM_DUMP,
    /* Set a piece of memory in Emacs to a value we store directly in
       this relocation.  The length field contains the number of bytes
       we actually copy into Emacs.  */
    RELOC_EMACS_IMMEDIATE,
    /* Set an aligned pointer-sized object in Emacs to a dump offset.  */
    RELOC_EMACS_DUMP_PTR_RAW,
    /* Set an aligned pointer-sized object in Emacs to point to
       something also in Emacs.  */
    RELOC_EMACS_EMACS_PTR_RAW,
    /* Set an aligned Lisp_Object in Emacs to point to a value in the
       dump.  Must be last.  */
    RELOC_EMACS_DUMP_LV,
  };

#define EMACS_RELOC_TYPE_BITS 3
#define EMACS_RELOC_LENGTH_BITS                         \
  (sizeof (dump_off) * CHAR_BIT - EMACS_RELOC_TYPE_BITS)

struct emacs_reloc
{
  ENUM_BF (emacs_reloc_type) type : EMACS_RELOC_TYPE_BITS;
  dump_off length : EMACS_RELOC_LENGTH_BITS;
  dump_off emacs_offset;
  union
  {
    dump_off dump_offset;
    dump_off emacs_offset2;
    intmax_t immediate;
    int8_t immediate_i8;
    int16_t immediate_i16;
    int32_t immediate_i32;
  } u;
};

/* Set the type of an Emacs relocation.

   Also make sure that the type fits in the bitfield.  */
static void
emacs_reloc_set_type (struct emacs_reloc *reloc,
                      enum emacs_reloc_type type)
{
  reloc->type = type;
  eassert (reloc->type == type);
}

struct dump_table_locator
{
  /* Offset in dump, in bytes, of the first entry in the dump
     table.  */
  dump_off offset;
  /* Number of entries in the dump table.  We need an explicit end
     indicator (as opposed to a special sentinel) so we can efficiency
     binary search over the relocation entries.  */
  dump_off nr_entries;
};

#define DUMP_RELOC_TYPE_BITS 4
#define DUMP_RELOC_ALIGNMENT_BITS 2
#define DUMP_RELOC_OFFSET_BITS                          \
  (sizeof (dump_off) * CHAR_BIT - DUMP_RELOC_TYPE_BITS)

struct dump_reloc
{
  uint32_t raw_offset : DUMP_RELOC_OFFSET_BITS;
  ENUM_BF (dump_reloc_type) type : DUMP_RELOC_TYPE_BITS;
};

/* Set the type of a dump relocation.

   Also assert that the type fits in the bitfield.  */
static void
dump_reloc_set_type (struct dump_reloc *reloc, enum dump_reloc_type type)
{
  reloc->type = type;
  eassert (reloc->type == type);
}

static dump_off
dump_reloc_get_offset (struct dump_reloc reloc)
{
  return reloc.raw_offset << DUMP_RELOC_ALIGNMENT_BITS;
}

static void
dump_reloc_set_offset (struct dump_reloc *reloc, dump_off offset)
{
  eassert (offset >= 0);
  ALLOW_IMPLICIT_CONVERSION;
  reloc->raw_offset = offset >> DUMP_RELOC_ALIGNMENT_BITS;
  DISALLOW_IMPLICIT_CONVERSION;
  if (dump_reloc_get_offset (*reloc) != offset)
    error ("dump relocation out of range");
}

static void dump_fingerprint (const char* label, const uint8_t* xfingerprint) {
  fprintf (stderr, "%s: ", label);
  for (int i = 0; i <32; ++i) {
    fprintf (stderr, "%02x", (unsigned) xfingerprint[i]);
  }
  fprintf (stderr, "\n");
}

/* Format of an Emacs portable dump file.  All offsets are relative to
   the beginning of the file.  An Emacs portable dump file is coupled
   to exactly the Emacs binary that produced it, so details of
   alignment and endianness are unimportant.

   An Emacs dump file contains the contents of the Lisp heap.
   On startup, Emacs can start faster by mapping a dump file into
   memory and using the objects contained inside it instead of
   performing initialization from scratch.

   The dump file can be loaded at arbitrary locations in memory, so it
   includes a table of relocations that let Emacs adjust the pointers
   embedded in the dump file to account for the location where it was
   actually loaded.

   Dump files can contain pointers to other objects in the dump file
   or to parts of the Emacs binary.  */
struct dump_header
{
  /* File type magic.  */
  char magic[sizeof (dump_magic)];

  /* Associated Emacs binary.  */
  uint8_t fingerprint[32];

  /* Relocation table for the dump file; each entry is a
     struct dump_reloc.  */
  struct dump_table_locator dump_relocs;

  /* "Relocation" table we abuse to hold information about the
     location and type of each lisp object in the dump.  We need for
     pdumper_object_type and ultimately for conservative GC
     correctness.  */
  struct dump_table_locator object_starts;

  /* Relocation table for Emacs; each entry is a struct
     emacs_reloc.  */
  struct dump_table_locator emacs_relocs;

  /* Start of sub-region of hot region that we can discard after load
     completes.  The discardable region ends at cold_start.

     This region contains objects that we copy into the Emacs image at
     dump-load time.  */
  dump_off discardable_start;

  /* Start of the region that does not require relocations and that we
     expect never to be modified.  This region can be memory-mapped
     directly from the backing dump file with the reasonable
     expectation of taking few copy-on-write faults.

     For correctness, however, this region must be modifible, since in
     rare cases it is possible to see modifications to these bytes.
     For example, this region contains string data, and it's
     technically possible for someone to ASET a string character
     (although nobody tends to do that).

     The start of the cold region is always aligned on a page
     boundary.  */
  dump_off cold_start;
};

/* Double-ended singly linked list.  */
struct dump_tailq
{
  Lisp_Object head;
  Lisp_Object tail;
  intptr_t length;
};

/* Queue of objects to dump.  */
struct dump_queue
{
  /* Objects with no link weights at all.  Kept in dump order.  */
  struct dump_tailq zero_weight_objects;
  /* Objects with simple link weight: just one entry of type
     WEIGHT_NORMAL.  Score in this special case is non-decreasing as
     position increases, so we can avoid the need to rescan a big list
     for each object by storing these objects in order.  */
  struct dump_tailq one_weight_normal_objects;
  /* Likewise, for objects with one WEIGHT_STRONG weight.  */
  struct dump_tailq one_weight_strong_objects;
  /* List of objects with complex link weights --- i.e., not one of
     the above cases.  Order is irrelevant, since we scan the whole
     list every time.  Relatively few objects end up here.  */
  struct dump_tailq fancy_weight_objects;
  /* Hash table of link weights: maps an object to a list of zero or
     more (BASIS . WEIGHT) pairs.  As a special case, an object with
     zero weight is marked by Qt in the hash table --- this way, we
     can distinguish objects we've seen but that have no weight from
     ones that we haven't seen at all.  */
  Lisp_Object link_weights;
  /* Hash table mapping object to a sequence number --- used to
     resolve ties.  */
  Lisp_Object sequence_numbers;
  dump_off next_sequence_number;
};

enum cold_op
  {
    COLD_OP_OBJECT,
    COLD_OP_STRING,
    COLD_OP_CHARSET,
    COLD_OP_BUFFER,
  };

/* This structure controls what operations we perform inside
   dump_object.  */
struct dump_flags
{
  /* Actually write object contents to the dump.  Without this flag
     set, we still scan objects and enqueue pointed-to objects; making
     this flag false is useful when we want to process an object's
     referents normally, but dump an object itself separately,
     later.  */
  bool_bf dump_object_contents : 1;
  /* Record object starts. We turn this flag off when writing to the
     discardable section so that we don't trick conservative GC into
     thinking we have objects there.  Ignored (we never record object
     starts) if dump_object_contents is false.  */
  bool_bf dump_object_starts : 1;
  /* Pack objects tighter than GC memory alignment would normally
     require.  Useful for objects copied into the Emacs image instead
     of used directly from the loaded dump.

     XXX: actually use

     */
  bool_bf pack_objects : 1;
  /* Sometimes we dump objects that we've already scanned for outbound
     references to other objects.  These objects should not cause new
     objects to enter the object dumping queue.  This flag causes Emacs
     to assert that no new objects are enqueued while dumping.  */
  bool_bf assert_already_seen : 1;
  /* Punt on unstable hash tables: defer them to ctx->deferred_hash_tables.  */
  bool_bf defer_hash_tables : 1;
  /* Punt on symbols: defer them to ctx->deferred_symbols.  */
  bool_bf defer_symbols : 1;
};

/* Information we use while we dump.  Note that we're not the garbage
   collector and can operate under looser constraints: specifically,
   we allocate memory during the dumping process.  */
struct dump_context
{
  /* Header we'll write to the dump file when done.  */
  struct dump_header header;

  Lisp_Object old_purify_flag;
  Lisp_Object old_post_gc_hook;

#ifdef REL_ALLOC
  bool blocked_ralloc;
#endif

  /* File descriptor for dumpfile; < 0 if closed.  */
  int fd;
  /* Name of dump file --- used for error reporting.  */
  Lisp_Object dump_filename;
  /* Current offset in dump file.  */
  dump_off offset;

  /* Starting offset of current object.  */
  dump_off obj_offset;

  /* Flags currently in effect for dumping.  */
  struct dump_flags flags;

  dump_off end_heap;

  /* Hash mapping objects we've already dumped to their offsets.  */
  Lisp_Object objects_dumped;

  /* Hash mapping objects to where we got them.  Used for debugging.  */
  Lisp_Object referrers;
  Lisp_Object current_referrer;
  bool have_current_referrer;

  /* Queue of objects to dump.  */
  struct dump_queue dump_queue;

  /* Deferred object lists.  */
  Lisp_Object deferred_hash_tables;
  Lisp_Object deferred_symbols;

  /* Fixups in the dump file.  */
  Lisp_Object fixups;
  /* Hash table mapping symbols to their pre-copy-queue fwd or blv
     structures (which we dump immediately before the start of the
     discardable section). */
  Lisp_Object symbol_aux;
  /* Queue of copied objects for special treatment.  */
  Lisp_Object copied_queue;
  /* Queue of cold objects to dump.  */
  Lisp_Object cold_queue;

  /* Relocations in the dump.  */
  Lisp_Object dump_relocs;
  /* Object starts.  */
  Lisp_Object object_starts;
  /* Relocations in Emacs.  */
  Lisp_Object emacs_relocs;

  unsigned number_hot_relocations;
  unsigned number_discardable_relocations;
};

/* These special values for use as offsets in dump_remember_object and
   dump_recall_object indicate that the corresponding object isn't in
   the dump yet (and so it has no valid offset), but that it's on one
   of our to-be-dumped-later object queues (or that we haven't seen it
   at all).  All values must be greater than or equal to zero. */
enum dump_object_special_offset {
  DUMP_OBJECT_DEFERRED = -2,
  DUMP_OBJECT_ON_NORMAL_QUEUE = -1,
  DUMP_OBJECT_NOT_SEEN = 0,
};

/* Weights for score scores for object non-locality.  */
enum link_weight_enum
  {
    WEIGHT_NONE_VALUE = 0,
    WEIGHT_NORMAL_VALUE = 1000,
    WEIGHT_STRONG_VALUE = 1200,
  };

struct link_weight
{
  enum link_weight_enum value;
};

#define LINK_WEIGHT_LITERAL(x) ((struct link_weight){.value=(x)})
#define WEIGHT_NONE LINK_WEIGHT_LITERAL (WEIGHT_NONE_VALUE)
#define WEIGHT_NORMAL LINK_WEIGHT_LITERAL (WEIGHT_NORMAL_VALUE)
#define WEIGHT_STRONG LINK_WEIGHT_LITERAL (WEIGHT_STRONG_VALUE)


/* Dump file creation */

static dump_off dump_object (struct dump_context *ctx, Lisp_Object object);
static dump_off dump_object_for_offset (
  struct dump_context *ctx, Lisp_Object object);

static void
dump_push (Lisp_Object *where, Lisp_Object newelt)
{
  *where = Fcons (newelt, *where);
}

static Lisp_Object
dump_pop (Lisp_Object *where)
{
  Lisp_Object ret = XCAR (*where);
  *where = XCDR (*where);
  return ret;
}

static bool
dump_tracking_referrers_p (struct dump_context *ctx)
{
  return !NILP (ctx->referrers);
}

static void
dump_set_have_current_referrer (struct dump_context *ctx, bool have)
{
#ifdef ENABLE_CHECKING
  ctx->have_current_referrer = have;
#endif
}

/* Remember the reason objects are enqueued.

   Until DUMP_CLEAR_REFERRER is called, any objects enqueued are being
   enqueued because OBJECT refers to them.  It is not legal to enqueue
   objects without a referer set.  We check this constraint
   at runtime.

   It is illegal to call DUMP_SET_REFERRER twice without an
   intervening call to DUMP_CLEAR_REFERRER.

   Define as a macro so we can avoid evaluating OBJECT
   if we dont want referrer tracking.  */
#define DUMP_SET_REFERRER(ctx, object)                   \
  do                                                     \
    {                                                    \
      struct dump_context *_ctx = (ctx);                 \
      eassert (!_ctx->have_current_referrer);            \
      dump_set_have_current_referrer (_ctx, true);       \
      if (dump_tracking_referrers_p (_ctx))              \
        ctx->current_referrer = (object);                \
    }                                                    \
  while (0)

/* Unset the referer that DUMP_SET_REFERRER set.

   Named with upper-case letters for symmetry with
   DUMP_SET_REFERRER.  */
static void
DUMP_CLEAR_REFERRER (struct dump_context *ctx)
{
  eassert (ctx->have_current_referrer);
  dump_set_have_current_referrer (ctx, false);
  if (dump_tracking_referrers_p (ctx))
    ctx->current_referrer = Qnil;
}

static Lisp_Object
dump_ptr_referrer (const char *label, void *address)
{
  char buf[128];
  buf[0] = '\0';
  sprintf (buf, "%s @ %p", label, address);
  return build_string (buf);
}

static void
print_paths_to_root (struct dump_context *ctx, Lisp_Object object);

static void dump_remember_cold_op (struct dump_context *ctx,
                                   enum cold_op op,
                                   Lisp_Object arg);

_Noreturn
static void
error_unsupported_dump_object (struct dump_context *ctx,
                               Lisp_Object object,
                               const char* msg)
{
  if (dump_tracking_referrers_p (ctx))
    print_paths_to_root (ctx, object);
  error ("unsupported object type in dump: %s", msg);
}

static uintptr_t
emacs_basis (void)
{
  return (uintptr_t) &Vpurify_flag;
}

static dump_off
emacs_offset (const void *emacs_ptr)
{
  /* TODO: assert that emacs_ptr is actually in emacs */
  eassert (emacs_ptr != NULL);
  intptr_t emacs_ptr_value = (intptr_t) emacs_ptr;
  ptrdiff_t emacs_ptr_relative = emacs_ptr_value - (intptr_t) emacs_basis ();
  return ptrdiff_t_to_dump_off (emacs_ptr_relative);
}

/* Return whether OBJECT is a symbol the storage of which is built
   into Emacs (and so is invariant across ASLR).  */
static bool
dump_builtin_symbol_p (Lisp_Object object)
{
  if (!SYMBOLP (object))
    return false;
  char* bp = (char*) lispsym;
  struct Lisp_Symbol *s = XSYMBOL (object);
  char* sp = (char*) s;
  return bp <= sp && sp < bp + sizeof (lispsym);
}

/* Return whether OBJECT has the same bit pattern in all Emacs
   invocations --- i.e., is invariant across a dump.  */
static bool
dump_object_self_representing_p (Lisp_Object object)
{
  bool result;
  ALLOW_IMPLICIT_CONVERSION;
  result =  INTEGERP (object) || dump_builtin_symbol_p (object);
  DISALLOW_IMPLICIT_CONVERSION;
  return result;
}

#define DEFINE_FROMLISP_FUNC(fn, type)          \
  static type                                   \
  fn (Lisp_Object value)                        \
  {                                             \
    type result;                                \
    ALLOW_IMPLICIT_CONVERSION;                  \
    CONS_TO_INTEGER (value, type, result);      \
    DISALLOW_IMPLICIT_CONVERSION;               \
    return result;                              \
  }

#define DEFINE_TOLISP_FUNC(fn, type) \
  static Lisp_Object                 \
  fn (type value)                    \
  {                                  \
    return INTEGER_TO_CONS (value);  \
  }

DEFINE_FROMLISP_FUNC (intmax_t_from_lisp, intmax_t);
DEFINE_TOLISP_FUNC (intmax_t_to_lisp, intmax_t);
DEFINE_FROMLISP_FUNC (dump_off_from_lisp, dump_off);
DEFINE_TOLISP_FUNC (dump_off_to_lisp, dump_off);

static void
dump_write (struct dump_context *ctx, const void *buf, dump_off nbyte)
{
  eassert (nbyte == 0 || buf != NULL);
  eassert (ctx->obj_offset == 0);
  eassert (ctx->flags.dump_object_contents);
  if (emacs_write (ctx->fd, buf, nbyte) < nbyte)
    report_file_error ("Could not write to dump file", ctx->dump_filename);
  ctx->offset += nbyte;
}

static Lisp_Object
make_eq_hash_table (void)
{
  return CALLN (Fmake_hash_table, QCtest, Qeq);
}

static void
dump_tailq_init (struct dump_tailq *tailq)
{
  tailq->head = tailq->tail = Qnil;
  tailq->length = 0;
}

static intptr_t
dump_tailq_length (const struct dump_tailq *tailq)
{
  return tailq->length;
}

__attribute__((unused))
static void
dump_tailq_prepend (struct dump_tailq *tailq, Lisp_Object value)
{
  Lisp_Object link = Fcons (value, tailq->head);
  tailq->head = link;
  if (NILP (tailq->tail))
    tailq->tail = link;
  tailq->length += 1;
}

__attribute__((unused))
static void
dump_tailq_append (struct dump_tailq *tailq, Lisp_Object value)
{
  Lisp_Object link = Fcons (value, Qnil);
  if (NILP (tailq->head))
    {
      eassert (NILP (tailq->tail));
      tailq->head = tailq->tail = link;
    }
  else
    {
      eassert (!NILP (tailq->tail));
      XSETCDR (tailq->tail, link);
      tailq->tail = link;
    }
  tailq->length += 1;
}

static bool
dump_tailq_empty_p (struct dump_tailq *tailq)
{
  return NILP (tailq->head);
}

static Lisp_Object
dump_tailq_peek (struct dump_tailq *tailq)
{
  eassert (!dump_tailq_empty_p (tailq));
  return XCAR (tailq->head);
}

static Lisp_Object
dump_tailq_pop (struct dump_tailq *tailq)
{
  eassert (!dump_tailq_empty_p (tailq));
  eassert (tailq->length > 0);
  tailq->length -= 1;
  Lisp_Object value = XCAR (tailq->head);
  tailq->head = XCDR (tailq->head);
  if (NILP (tailq->head))
    tailq->tail = Qnil;
  return value;
}

static void
dump_seek (struct dump_context *ctx, dump_off offset)
{
  eassert (ctx->obj_offset == 0);
  if (lseek (ctx->fd, offset, SEEK_SET) < 0)
    report_file_error ("Setting file position",
                       ctx->dump_filename);
  ctx->offset = offset;
}

static void
dump_write_zero (struct dump_context *ctx, dump_off nbytes)
{
  while (nbytes > 0)
    {
      uintmax_t zero = 0;
      dump_off to_write = sizeof (zero);
      if (to_write > nbytes)
        to_write = nbytes;
      dump_write (ctx, &zero, to_write);
      nbytes -= to_write;
    }
}

static void
dump_align_output (struct dump_context *ctx, int alignment)
{
  if (ctx->offset % alignment != 0)
    dump_write_zero (ctx, alignment - (ctx->offset % alignment));
}

static dump_off
dump_object_start (struct dump_context *ctx,
                   int alignment,
                   void *out,
                   dump_off outsz)
{
  /* We dump only one object at a time, so obj_offset should be
     invalid.  */
  eassert (ctx->obj_offset == 0);
  if (ctx->flags.pack_objects)
    alignment = 1;
  if (ctx->flags.dump_object_contents)
    dump_align_output (ctx, alignment);
  ctx->obj_offset = ctx->offset;
  memset (out, 0, outsz);
  return ctx->offset;
}

static dump_off
dump_object_finish (struct dump_context *ctx,
                    const void *out,
                    dump_off sz)
{
  dump_off offset = ctx->obj_offset;
  eassert (offset > 0);
  eassert (offset == ctx->offset); /* No intervening writes.  */
  ctx->obj_offset = 0;
  if (ctx->flags.dump_object_contents)
    dump_write (ctx, out, sz);
  return offset;
}

/* Return offset at which OBJECT has been dumped, or 0 if OBJECT has
   not been dumped.  */
static dump_off
dump_recall_object (struct dump_context *ctx, Lisp_Object object)
{
  Lisp_Object dumped = ctx->objects_dumped;
  return dump_off_from_lisp (Fgethash (object, dumped, make_number (0)));
}

static void
dump_remember_object (struct dump_context *ctx,
                      Lisp_Object object,
                      dump_off offset)
{
  Fputhash (object,
            dump_off_to_lisp (offset),
            ctx->objects_dumped);
}

static void
dump_note_reachable (struct dump_context *ctx, Lisp_Object object)
{
  eassert (ctx->have_current_referrer);
  if (!dump_tracking_referrers_p (ctx))
    return;
  Lisp_Object referrer = ctx->current_referrer;
  Lisp_Object obj_referrers = Fgethash (object, ctx->referrers, Qnil);
  if (NILP (Fmemq (referrer, obj_referrers)))
    Fputhash (object, Fcons (referrer, obj_referrers), ctx->referrers);
}

/* If this object lives in the Emacs image and not on the heap, return
   a pointer to the object data.  Otherwise, return NULL.  */
static void*
dump_object_emacs_ptr (Lisp_Object lv)
{
  if (SUBRP (lv))
    return XSUBR (lv);
  if (dump_builtin_symbol_p (lv))
    return XSYMBOL (lv);
  return NULL;
}

static void
dump_queue_init (struct dump_queue *dump_queue)
{
  dump_tailq_init (&dump_queue->zero_weight_objects);
  dump_tailq_init (&dump_queue->one_weight_normal_objects);
  dump_tailq_init (&dump_queue->one_weight_strong_objects);
  dump_tailq_init (&dump_queue->fancy_weight_objects);
  dump_queue->link_weights = make_eq_hash_table ();
  dump_queue->sequence_numbers = make_eq_hash_table ();
  dump_queue->next_sequence_number = 1;
}

static bool
dump_queue_empty_p (struct dump_queue *dump_queue)
{
  bool is_empty =
    EQ (Fhash_table_count (dump_queue->sequence_numbers),
        make_number (0));
  eassert (EQ (Fhash_table_count (dump_queue->sequence_numbers),
               Fhash_table_count (dump_queue->link_weights)));
  if (!is_empty)
    {
      eassert (
        !dump_tailq_empty_p (&dump_queue->zero_weight_objects) ||
        !dump_tailq_empty_p (&dump_queue->one_weight_normal_objects) ||
        !dump_tailq_empty_p (&dump_queue->one_weight_strong_objects) ||
        !dump_tailq_empty_p (&dump_queue->fancy_weight_objects));
    }
  else
    {
      /* If we're empty, we can still have a few stragglers on one of
         the above queues.  */
    }

  return is_empty;
}

static void
dump_queue_push_weight (Lisp_Object *weight_list,
                        dump_off basis,
                        struct link_weight weight)
{
  if (EQ (*weight_list, Qt))
    *weight_list = Qnil;
  dump_push (weight_list, Fcons (dump_off_to_lisp (basis),
                                 dump_off_to_lisp (weight.value)));
}

static void
dump_queue_enqueue (struct dump_queue *dump_queue,
                    Lisp_Object object,
                    dump_off basis,
                    struct link_weight weight)
{
  Lisp_Object weights = Fgethash (object, dump_queue->link_weights, Qnil);
  Lisp_Object orig_weights = weights;
  // N.B. want to find the last item of a given weight in each queue
  // due to prepend use.
  bool use_single_queues = true;
  if (NILP (weights))
    {
      /* Object is new.  */
      dump_trace ("new object %016x weight=%u\n",
                  (unsigned) XLI (object),
                  (unsigned) weight.value);

      if (weight.value == WEIGHT_NONE.value)
        {
          eassert (weight.value == 0);
          dump_tailq_prepend (&dump_queue->zero_weight_objects, object);
          weights = Qt;
        }
      else if (!use_single_queues)
        {
          dump_tailq_prepend (&dump_queue->fancy_weight_objects, object);
          dump_queue_push_weight (&weights, basis, weight);
        }
      else if (weight.value == WEIGHT_NORMAL.value)
        {
          dump_tailq_prepend (&dump_queue->one_weight_normal_objects, object);
          dump_queue_push_weight (&weights, basis, weight);
        }
      else if (weight.value == WEIGHT_STRONG.value)
        {
          dump_tailq_prepend (&dump_queue->one_weight_strong_objects, object);
          dump_queue_push_weight (&weights, basis, weight);
        }
      else
        {
          emacs_abort ();
        }

      Fputhash (object,
                dump_off_to_lisp(dump_queue->next_sequence_number++),
                dump_queue->sequence_numbers);
    }
  else
    {
      /* Object was already on the queue.  It's okay for an object to
         be on multiple queues so long as we maintain order
         invariants: attempting to dump an object multiple times is
         harmless, and most of the time, an object is only referenced
         once before being dumped, making this code path uncommon.  */
      if (weight.value != WEIGHT_NONE.value)
        {
          if (EQ (weights, Qt))
            {
              /* Object previously had a zero weight.  Once we
                 incorporate the link weight attached to this call,
                 the object will have a single weight.  Put the object
                 on the appropriate single-weight queue.  */
              weights = Qnil;
              if (!use_single_queues)
                dump_tailq_prepend (&dump_queue->fancy_weight_objects, object);
              else if (weight.value == WEIGHT_NORMAL.value)
                dump_tailq_prepend (
                  &dump_queue->one_weight_normal_objects, object);
              else if (weight.value == WEIGHT_STRONG.value)
                dump_tailq_prepend (
                  &dump_queue->one_weight_strong_objects, object);
              else
                emacs_abort ();
            }
          else if (use_single_queues && NILP (XCDR (weights)))
            dump_tailq_prepend (&dump_queue->fancy_weight_objects, object);
          dump_queue_push_weight (&weights, basis, weight);
        }
    }

  if (!EQ (weights, orig_weights))
    Fputhash (object, weights, dump_queue->link_weights);
}

static float
dump_calc_link_score (dump_off basis,
                      dump_off link_basis,
                      dump_off link_weight)
{
  float distance = (float)(basis - link_basis);
  eassert (distance >= 0);
  float link_score = powf (distance, -0.2f);
  return powf (link_score, (float) link_weight / 1000.0f);
}

/* Compute the score score for a queued object.

   OBJECT is the object to query, which must currently be queued for
   dumping.  BASIS is the offset at which we would be
   dumping the object; score is computed relative to BASIS and the
   various BASIS values supplied to dump_add_link_weight --- the
   further an object is from its referrers, the greater the
   score.  */
static float
dump_queue_compute_score (struct dump_queue *dump_queue,
                          Lisp_Object object,
                          dump_off basis)
{
  float score = 0;
  Lisp_Object object_link_weights =
    Fgethash (object, dump_queue->link_weights, Qnil);
  if (EQ (object_link_weights, Qt))
    object_link_weights = Qnil;
  while (!NILP (object_link_weights))
    {
      Lisp_Object basis_weight_pair = dump_pop (&object_link_weights);
      dump_off link_basis = dump_off_from_lisp (XCAR (basis_weight_pair));
      dump_off link_weight = dump_off_from_lisp (XCDR (basis_weight_pair));
      score += dump_calc_link_score (basis, link_basis, link_weight);
    }
  return score;
}

/* Scan the fancy part of the dump queue.

   BASIS is the position at which to evaluate the score function,
   usually ctx->offset.

   If we have at least one entry in the queue, return the pointer (in
   the singly-linked list) to the cons containing the object via
   *OUT_HIGHEST_SCORE_CONS_PTR and return its score.

   If the queue is empty, set *OUT_HIGHEST_SCORE_CONS_PTR to NULL
   and return negative infinity.  */
static float
dump_queue_scan_fancy (struct dump_queue *dump_queue,
                       dump_off basis,
                       Lisp_Object **out_highest_score_cons_ptr)
{
  Lisp_Object *cons_ptr = &dump_queue->fancy_weight_objects.head;
  Lisp_Object *highest_score_cons_ptr = NULL;
  float highest_score = -INFINITY;
  bool first = true;

  while (!NILP (*cons_ptr))
    {
      Lisp_Object queued_object = XCAR (*cons_ptr);
      float score = dump_queue_compute_score (
        dump_queue, queued_object, basis);
      if (first || score >= highest_score)
        {
          highest_score_cons_ptr = cons_ptr;
          highest_score = score;
          if (first)
            first = false;
        }
      cons_ptr = &XCONS (*cons_ptr)->u.s.u.cdr;
    }

  *out_highest_score_cons_ptr = highest_score_cons_ptr;
  return highest_score;
}

/* Return the sequence number of OBJECT.

   Return -1 if object doesn't have a sequence number.  This situation
   can occur when we've double-queued an object.  If this happens, we
   discard the errant object and try again.  */
static dump_off
dump_queue_sequence (struct dump_queue *dump_queue,
                     Lisp_Object object)
{
  Lisp_Object n = Fgethash (object, dump_queue->sequence_numbers, Qnil);
  return NILP (n) ? -1 : dump_off_from_lisp (n);
}

/* Find score and sequence at head of a one-weight object queue.

   Transparently discard stale objects from head of queue.  BASIS
   is the baseness for score computation.

   We organize these queues so that score is strictly decreasing, so
   examining the head is sufficient.  */
static void
dump_queue_find_score_of_one_weight_queue (
  struct dump_queue *dump_queue,
  dump_off basis,
  struct dump_tailq *one_weight_queue,
  float *out_score,
  int *out_sequence)
{
  /* Transparently discard stale objects from the head of this queue.  */
  do
    {
      if (dump_tailq_empty_p (one_weight_queue))
        {
          *out_score = -INFINITY;
          *out_sequence = 0;
        }
      else
        {
          Lisp_Object head = dump_tailq_peek (one_weight_queue);
          *out_sequence = dump_queue_sequence (dump_queue, head);
          if (*out_sequence < 0)
            dump_tailq_pop (one_weight_queue);
          else
            *out_score =
              dump_queue_compute_score (dump_queue, head, basis);
        }
    }
  while (*out_sequence < 0);
}

/* Pop the next object to dump from the dump queue.

   BASIS is the dump offset at which to evaluate score.

   The object returned is the queued object with the greatest score;
   by side effect, the object is removed from the dump queue.
   The dump queue must not be empty.  */
static Lisp_Object
dump_queue_dequeue (struct dump_queue *dump_queue, dump_off basis)
{
  eassert (EQ (Fhash_table_count (dump_queue->sequence_numbers),
               Fhash_table_count (dump_queue->link_weights)));

  eassert (
    XFASTINT (Fhash_table_count (dump_queue->sequence_numbers))
    <= (dump_tailq_length (&dump_queue->fancy_weight_objects) +
        dump_tailq_length (&dump_queue->zero_weight_objects) +
        dump_tailq_length (&dump_queue->one_weight_normal_objects) +
        dump_tailq_length (&dump_queue->one_weight_strong_objects)));

  bool dump_object_counts = true;
  if (dump_object_counts)
    dump_trace (
      "dump_queue_dequeue basis=%d fancy=%u zero=%u "
      "normal=%u strong=%u hash=%u\n",
      basis,
      (unsigned) dump_tailq_length (&dump_queue->fancy_weight_objects),
      (unsigned) dump_tailq_length (&dump_queue->zero_weight_objects),
      (unsigned) dump_tailq_length (&dump_queue->one_weight_normal_objects),
      (unsigned) dump_tailq_length (&dump_queue->one_weight_strong_objects),
      (unsigned) XFASTINT (Fhash_table_count (dump_queue->link_weights)));

  static const int nr_candidates = 3;
  struct candidate {
    float score;
    dump_off sequence;
  } candidates[nr_candidates];

  Lisp_Object *fancy_cons = NULL;
  candidates[0].sequence = 0;
  do
    {
      if (candidates[0].sequence < 0)
        *fancy_cons = XCDR (*fancy_cons);  /* Discard stale object.  */
      candidates[0].score = dump_queue_scan_fancy (
        dump_queue,
        basis,
        &fancy_cons);
      candidates[0].sequence =
        candidates[0].score > -INFINITY
        ? dump_queue_sequence (dump_queue, XCAR (*fancy_cons))
        : 0;
    }
  while (candidates[0].sequence < 0);

  dump_queue_find_score_of_one_weight_queue (
    dump_queue,
    basis,
    &dump_queue->one_weight_normal_objects,
    &candidates[1].score,
    &candidates[1].sequence);

  dump_queue_find_score_of_one_weight_queue (
    dump_queue,
    basis,
    &dump_queue->one_weight_strong_objects,
    &candidates[2].score,
    &candidates[2].sequence);

  int best = -1;
  for (int i = 0; i < nr_candidates; ++i)
    {
      eassert (candidates[i].sequence >= 0);
      if (candidates[i].score > -INFINITY &&
          (best < 0 ||
           candidates[i].score > candidates[best].score ||
           (candidates[i].score == candidates[best].score
            && candidates[i].sequence < candidates[best].sequence)))
        best = i;
    }

  Lisp_Object result;
  const char *src;
  if (best < 0)
    {
      src = "zero";
      result = dump_tailq_pop (&dump_queue->zero_weight_objects);
    }
  else if (best == 0)
    {
      src = "fancy";
      result = dump_tailq_pop (&dump_queue->fancy_weight_objects);
    }
  else if (best == 1)
    {
      src = "normal";
      result = dump_tailq_pop (&dump_queue->one_weight_normal_objects);
    }
  else if (best == 2)
    {
      src = "strong";
      result = dump_tailq_pop (&dump_queue->one_weight_strong_objects);
    }
  else
    emacs_abort ();

  dump_trace ("  result score=%f src=%s object=%016x\n",
              best < 0 ? -1.0 : (double) candidates[best].score,
              src,
              (unsigned) XLI (result));

  {
    Lisp_Object weights = Fgethash (result, dump_queue->link_weights, Qnil);
    while (!NILP (weights) && CONSP (weights))
      {
        Lisp_Object basis_weight_pair = dump_pop (&weights);
        dump_off link_basis =
          dump_off_from_lisp (XCAR (basis_weight_pair));
        dump_off link_weight =
          dump_off_from_lisp (XCDR (basis_weight_pair));
        dump_trace (
          "    link_basis=%d distance=%d weight=%d contrib=%f\n",
          link_basis,
          basis - link_basis,
          link_weight,
          (double) dump_calc_link_score (
            basis, link_basis, link_weight));
      }
  }

  Fremhash (result, dump_queue->link_weights);
  Fremhash (result, dump_queue->sequence_numbers);
  return result;
}

static void
dump_enqueue_object (struct dump_context *ctx,
                     Lisp_Object object,
                     struct link_weight weight)
{
  if (!dump_object_self_representing_p (object) ||
       dump_object_emacs_ptr (object))
    {
      dump_off state = dump_recall_object (ctx, object);
      bool already_dumped_object = state > DUMP_OBJECT_NOT_SEEN;
      if (ctx->flags.assert_already_seen)
        eassert (already_dumped_object);
      if (!already_dumped_object)
        {
          bool cold = BOOL_VECTOR_P (object) || FLOATP (object);
          if (state == DUMP_OBJECT_NOT_SEEN)
            {
              dump_remember_object (ctx, object, DUMP_OBJECT_ON_NORMAL_QUEUE);
              if (cold)
                dump_remember_cold_op (ctx, COLD_OP_OBJECT, object);
            }
          if (!cold &&
              state <= DUMP_OBJECT_NOT_SEEN &&
              state != DUMP_OBJECT_DEFERRED)
            dump_queue_enqueue (&ctx->dump_queue,
                                object,
                                ctx->offset,
                                weight);
        }
    }
  /* Always make sure that we have a referrer.  */
  dump_note_reachable (ctx, object);
}

static void
print_paths_to_root_1 (struct dump_context *ctx,
                       Lisp_Object object,
                       int level)
{
  Lisp_Object referrers = Fgethash (object, ctx->referrers, Qnil);
  while (!NILP (referrers))
    {
      Lisp_Object referrer = XCAR (referrers);
      referrers = XCDR (referrers);
      Lisp_Object repr = Fprin1_to_string (referrer, Qnil);
      for (int i = 0; i < level; ++i)
        fputc (' ', stderr);
      fprintf (stderr, "%s\n", SDATA (repr));
      print_paths_to_root_1 (ctx, referrer, level + 1);
    }
}

static void
print_paths_to_root (struct dump_context *ctx, Lisp_Object object)
{
  print_paths_to_root_1 (ctx, object, 0);
}

static void
dump_remember_cold_op (struct dump_context *ctx,
                       enum cold_op op,
                       Lisp_Object arg)
{
  if (ctx->flags.dump_object_contents)
    dump_push (&ctx->cold_queue, Fcons (make_number (op), arg));
}

/* Add a dump relocation that points into Emacs.

   Add a relocation that updates the pointer stored at DUMP_OFFSET to
   point into the Emacs binary upon dump load.  The pointer-sized
   value at DUMP_OFFSET in the dump file should contain a number
   relative to emacs_basis().  */
static void
dump_reloc_dump_to_emacs_ptr_raw (struct dump_context *ctx,
                                  dump_off dump_offset)
{
  if (ctx->flags.dump_object_contents)
    dump_push (&ctx->dump_relocs,
               list2 (dump_off_to_lisp (RELOC_DUMP_TO_EMACS_PTR_RAW),
                      dump_off_to_lisp (dump_offset)));
}

/* Add a dump relocation that points a Lisp_Object back at the dump.

   Add a relocation that updates the Lisp_Object at DUMP_OFFSET in the
   dump to point to another object in the dump.  The Lisp_Object-sized
   value at DUMP_OFFSET in the dump file should contain the offset of
   the target object relative to the start of the dump.  */
static void
dump_reloc_dump_to_dump_lv (struct dump_context *ctx,
                            dump_off dump_offset,
                            enum Lisp_Type type)
{
  if (!ctx->flags.dump_object_contents)
    return;

  int reloc_type;
  switch (type)
    {
    case Lisp_Symbol:
    case Lisp_Misc:
    case Lisp_String:
    case Lisp_Vectorlike:
    case Lisp_Cons:
    case Lisp_Float:
      reloc_type = RELOC_DUMP_TO_DUMP_LV + type;
      break;
    default:
      emacs_abort ();
    }

  dump_push (&ctx->dump_relocs,
             list2 (dump_off_to_lisp (reloc_type),
                    dump_off_to_lisp (dump_offset)));
}

/* Add a dump relocation that points a raw pointer back at the dump.

   Add a relocation that updates the raw pointer at DUMP_OFFSET in the
   dump to point to another object in the dump.  The pointer-sized
   value at DUMP_OFFSET in the dump file should contain the offset of
   the target object relative to the start of the dump.  */
static void
dump_reloc_dump_to_dump_ptr_raw (struct dump_context *ctx,
                                 dump_off dump_offset)
{
  if (ctx->flags.dump_object_contents)
    dump_push (&ctx->dump_relocs,
               list2 (dump_off_to_lisp (RELOC_DUMP_TO_DUMP_PTR_RAW),
                      dump_off_to_lisp (dump_offset)));
}

/* Add a dump relocation that points to a Lisp object in Emacs.

   Add a relocation that updates the Lisp_Object at DUMP_OFFSET in the
   dump to point to a lisp object in Emacs.  The Lisp_Object-sized
   value at DUMP_OFFSET in the dump file should contain the offset of
   the target object relative to emacs_basis().  TYPE is the type of
   Lisp value.  */
static void
dump_reloc_dump_to_emacs_lv (struct dump_context *ctx,
                             dump_off dump_offset,
                             enum Lisp_Type type)
{
  if (!ctx->flags.dump_object_contents)
    return;

  int reloc_type;
  switch (type)
    {
    case Lisp_Misc:
    case Lisp_String:
    case Lisp_Vectorlike:
    case Lisp_Cons:
    case Lisp_Float:
      reloc_type = RELOC_DUMP_TO_EMACS_LV + type;
      break;
    default:
      emacs_abort ();
    }

  dump_push (&ctx->dump_relocs,
             list2 (dump_off_to_lisp (reloc_type),
                    dump_off_to_lisp (dump_offset)));
}

/* Add an Emacs relocation that copies arbitrary bytes from the dump.

   When the dump is loaded, Emacs copies SIZE bytes from OFFSET in
   dump to LOCATION in the Emacs data section.  This copying happens
   after other relocations, so it's all right to, say, copy a
   Lisp_Object (since by the time we copy the Lisp_Object, it'll have
   been adjusted to account for the location of the running Emacs and
   dump file).  */
static void
dump_emacs_reloc_copy_from_dump (struct dump_context *ctx,
                                 dump_off dump_offset,
                                 void* emacs_ptr,
                                 dump_off size)
{
  eassert (size >= 0);
  eassert (size < (1 << EMACS_RELOC_LENGTH_BITS));

  if (!ctx->flags.dump_object_contents)
    return;

  if (size == 0)
    return;

  dump_push (&ctx->emacs_relocs,
             list4 (make_number (RELOC_EMACS_COPY_FROM_DUMP),
                    dump_off_to_lisp (emacs_offset (emacs_ptr)),
                    dump_off_to_lisp (dump_offset),
                    dump_off_to_lisp (size)));
}

/* Add an Emacs relocation that sets values to arbitrary bytes.

   When the dump is loaded, Emacs copies SIZE bytes from the
   relocation itself to the adjusted location inside Emacs EMACS_PTR.
   SIZE is the number of bytes to copy.  See struct emacs_reloc for
   the maximum size that this mechanism can support.  The value comes
   from VALUE_PTR.
 */
static void
dump_emacs_reloc_immediate (struct dump_context *ctx,
                            const void *emacs_ptr,
                            const void *value_ptr,
                            dump_off size)
{
  if (!ctx->flags.dump_object_contents)
    return;

  intmax_t value = 0;
  eassert (size <= sizeof (value));
  memcpy (&value, value_ptr, size);
  dump_push (&ctx->emacs_relocs,
             list4 (make_number (RELOC_EMACS_IMMEDIATE),
                    dump_off_to_lisp (emacs_offset (emacs_ptr)),
                    intmax_t_to_lisp (value),
                    dump_off_to_lisp (size)));
}

#define DEFINE_EMACS_IMMEDIATE_FN(fnname, type)                         \
  static void                                                           \
  fnname (struct dump_context *ctx,                                     \
          const type *emacs_ptr,                                        \
          type value)                                                   \
  {                                                                     \
    dump_emacs_reloc_immediate (                                        \
      ctx, emacs_ptr, &value, sizeof (value));                          \
  }

DEFINE_EMACS_IMMEDIATE_FN (dump_emacs_reloc_immediate_lv, Lisp_Object);
DEFINE_EMACS_IMMEDIATE_FN (dump_emacs_reloc_immediate_ptrdiff_t, ptrdiff_t);
DEFINE_EMACS_IMMEDIATE_FN (dump_emacs_reloc_immediate_emacs_int, EMACS_INT);
DEFINE_EMACS_IMMEDIATE_FN (dump_emacs_reloc_immediate_int, int);
DEFINE_EMACS_IMMEDIATE_FN (dump_emacs_reloc_immediate_bool, bool);

/* Add an emacs relocation that makes a raw pointer in Emacs point
   into the dump.  */
static void
dump_emacs_reloc_to_dump_ptr_raw (struct dump_context *ctx,
                                  const void* emacs_ptr,
                                  dump_off dump_offset)
{
  if (!ctx->flags.dump_object_contents)
    return;

  dump_push (&ctx->emacs_relocs,
             list3 (make_number (RELOC_EMACS_DUMP_PTR_RAW),
                    dump_off_to_lisp (emacs_offset (emacs_ptr)),
                    dump_off_to_lisp (dump_offset)));
}

/* Add an emacs relocation that points into the dump.

   When the dump is loaded, the Lisp_Object at EMACS_ROOT in Emacs to
   point to VALUE.  VALUE can be any Lisp value; this function
   automatically queues the value for dumping if necessary.  */
static void
dump_emacs_reloc_to_dump_lv (struct dump_context *ctx,
                             Lisp_Object *emacs_ptr,
                             Lisp_Object value)
{
  if (dump_object_self_representing_p (value))
    dump_emacs_reloc_immediate_lv (ctx, emacs_ptr, value);
  else
    {
      if (ctx->flags.dump_object_contents)
        dump_push (
          &ctx->emacs_relocs,
          list3 (dump_off_to_lisp (RELOC_EMACS_DUMP_LV + XTYPE (value)),
                 dump_off_to_lisp (emacs_offset (emacs_ptr)),
                 value));
      dump_enqueue_object (ctx, value, WEIGHT_NONE);
    }
}

/* Add an emacs relocation that makes a raw pointer in Emacs point
   back into the Emacs image.  */
static void
dump_emacs_reloc_to_emacs_ptr_raw (struct dump_context *ctx,
                                   void* emacs_ptr,
                                   void *target_emacs_ptr)
{
  if (!ctx->flags.dump_object_contents)
    return;

  dump_push (&ctx->emacs_relocs,
             list3 (make_number (RELOC_EMACS_EMACS_PTR_RAW),
                    dump_off_to_lisp (emacs_offset (emacs_ptr)),
                    dump_off_to_lisp (emacs_offset (target_emacs_ptr))));
}

/* Add an Emacs relocation that makes a raw pointer in Emacs point to
   a different part of Emacs.  */

enum dump_fixup_type
  {
    DUMP_FIXUP_LISP_OBJECT,
    DUMP_FIXUP_LISP_OBJECT_RAW,
    DUMP_FIXUP_PTR_DUMP_RAW,
  };

enum dump_lv_fixup_type
  {
    LV_FIXUP_LISP_OBJECT,
    LV_FIXUP_RAW_POINTER,
  };

/* Make something in the dump point to a lisp object.

   CTX is a dump context.  DUMP_OFFSET is the location in the dump to
   fix.  VALUE is the object to which the location in the dump
   should point.

   If FIXUP_SUBTYPE is LV_FIXUP_LISP_OBJECT, we expect a Lisp_Object
   at DUMP_OFFSET.  If it's LV_FIXUP_RAW_POINTER, we expect a pointer.
 */
static void
dump_remember_fixup_lv (struct dump_context *ctx,
                        dump_off dump_offset,
                        Lisp_Object value,
                        enum dump_lv_fixup_type fixup_subtype)
{
  if (!ctx->flags.dump_object_contents)
    return;

  dump_push (&ctx->fixups,
             list3 (
               make_number (fixup_subtype == LV_FIXUP_LISP_OBJECT
                            ? DUMP_FIXUP_LISP_OBJECT
                            : DUMP_FIXUP_LISP_OBJECT_RAW),
               dump_off_to_lisp (dump_offset),
               value));
}

/* Remember to fix up the dump file such that the pointer-sized value
   at DUMP_OFFSET points to NEW_DUMP_OFFSET in the dump file and to
   its absolute address at runtime.  */
static void
dump_remember_fixup_ptr_raw (struct dump_context *ctx,
                             dump_off dump_offset,
                             dump_off new_dump_offset)
{
  if (!ctx->flags.dump_object_contents)
    return;

  /* We should not be generating relocations into the
     to-be-copied-into-Emacs dump region.  */
  eassert (ctx->header.discardable_start == 0 ||
           new_dump_offset < ctx->header.discardable_start ||
           (ctx->header.cold_start != 0 &&
            new_dump_offset >= ctx->header.cold_start));

  dump_push (&ctx->fixups,
             list3 (
               make_number (DUMP_FIXUP_PTR_DUMP_RAW),
               dump_off_to_lisp (dump_offset),
               dump_off_to_lisp (new_dump_offset)));
}

static void
dump_root_visitor (Lisp_Object *root_ptr, enum gc_root_type type, void *data)
{
  struct dump_context *ctx = data;
  Lisp_Object value = *root_ptr;
  if (type == GC_ROOT_C_SYMBOL)
    {
      eassert (dump_builtin_symbol_p (value));
      /* Remember to dump the object itself later along with all the
         rest of the copied-to-Emacs objects.  */
      DUMP_SET_REFERRER (ctx, build_string ("built-in symbol list"));
      dump_enqueue_object (ctx, value, WEIGHT_NONE);
      DUMP_CLEAR_REFERRER (ctx);
    }
  else
    {
      if (root_ptr != &Vinternal_interpreter_environment)
        {
          DUMP_SET_REFERRER (ctx, dump_ptr_referrer ("emacs root", root_ptr));
          dump_emacs_reloc_to_dump_lv (ctx, root_ptr, *root_ptr);
          DUMP_CLEAR_REFERRER (ctx);
        }
    }
}

/* Kick off the dump process by queuing up the static GC roots.  */
static void
dump_roots (struct dump_context *ctx)
{
  struct gc_root_visitor visitor;
  memset (&visitor, 0, sizeof (visitor));
  visitor.visit = dump_root_visitor;
  visitor.data = ctx;
  visit_static_gc_roots (visitor);
}

static dump_off
field_relpos (const void *in_start, const void *in_field)
{
  ptrdiff_t in_start_val = (ptrdiff_t) in_start;
  ptrdiff_t in_field_val = (ptrdiff_t) in_field;
  eassert (in_start_val <= in_field_val);
  ptrdiff_t relpos = in_field_val - in_start_val;
  eassert (relpos < 1024); /* Sanity check.  */
  return (dump_off) relpos;
}

static void
cpyptr (void *out, const void *in)
{
  memcpy (out, in, sizeof (void *));
}

/* Convenience macro for regular assignment.  */
#define DUMP_FIELD_COPY(out, in, name) \
  do                                   \
    {                                  \
      (out)->name = (in)->name;        \
    }                                  \
  while (0)

static void
dump_field_lv_or_rawptr (struct dump_context *ctx,
                         void *out,
                         const void *in_start,
                         const void *in_field,
                         /* opt */ const enum Lisp_Type *ptr_raw_type,
                         struct link_weight weight)
{
  eassert (ctx->obj_offset > 0);

  Lisp_Object value;
  dump_off relpos = field_relpos (in_start, in_field);
  void *out_field = (char *) out + relpos;
  if (ptr_raw_type == NULL)
    {
      memcpy (&value, in_field, sizeof (value));
      if (dump_object_self_representing_p (value))
        {
          memcpy (out_field, &value, sizeof (value));
          return;
        }
    }
  else
    {
      void *ptrval;
      cpyptr (&ptrval, in_field);
      if (ptrval == NULL)
        return; /* Nothing to do.  */
      switch (*ptr_raw_type)
        {
        case Lisp_Symbol:
          value = make_lisp_symbol (ptrval);
          break;
        case Lisp_Misc:
        case Lisp_String:
        case Lisp_Vectorlike:
        case Lisp_Cons:
        case Lisp_Float:
          value = make_lisp_ptr (ptrval, *ptr_raw_type);
          break;
        default:
          emacs_abort ();
        }
    }

  bool is_ptr_raw = (ptr_raw_type != NULL);

  /* Now value is the Lisp_Object to which we want to point whether or
     not the field is a raw pointer (in which case we just synthesized
     the Lisp_Object outselves) or a Lisp_Object (in which case we
     just copied the thing).  Add a fixup or relocation.  */

  intptr_t out_value;
  dump_off out_field_offset = ctx->obj_offset + relpos;
  dump_off target_offset = dump_recall_object (ctx, value);
  if (DANGEROUS &&
      target_offset > 0 && dump_object_emacs_ptr (value) == NULL)
    {
      /* We've already dumped the referenced object, so we can emit
         the value and a relocation directly instead of indirecting
         through a fixup.  */
      out_value = target_offset;
      if (is_ptr_raw)
        dump_reloc_dump_to_dump_ptr_raw (ctx, out_field_offset);
      else
        dump_reloc_dump_to_dump_lv (ctx, out_field_offset, XTYPE (value));
    }
  else
    {
      /* We don't know about the target object yet, so add a fixup.
         When we process the fixup, we'll have dumped the target
         object.  */
      out_value = (intptr_t) 0xDEADF00D;
      dump_remember_fixup_lv (ctx,
                              out_field_offset,
                              value,
                              ( is_ptr_raw
                                ? LV_FIXUP_RAW_POINTER
                                : LV_FIXUP_LISP_OBJECT ));
      dump_enqueue_object (ctx, value, weight);
    }

  memcpy (out_field, &out_value, sizeof (out_value));
}

/* Set a pointer field on an output object during dump.

   CTX is the dump context.  OFFSET is the offset at which the current
   object starts.  OUT is a pointer to the dump output object.
   IN_START is the start of the current Emacs object.  IN_FIELD is a
   pointer to the field in that object.  TYPE is the type of pointer
   to which IN_FIELD points.
 */
static void
dump_field_lv_rawptr (struct dump_context *ctx,
                      void *out,
                      const void *in_start,
                      const void *in_field,
                      enum Lisp_Type type,
                      struct link_weight weight)
{
  dump_field_lv_or_rawptr (ctx, out, in_start, in_field, &type, weight);
}

/* Set a Lisp_Object field on an output object during dump.

   CTX is a dump context.  OFFSET is the offset at which the current
   object starts.  OUT is a pointer to the dump output object.
   IN_START is the start of the current Emacs object.  IN_FIELD is a
   pointer to a Lisp_Object field in that object.

   Arrange for the dump to contain fixups and relocations such that,
   at load time, the given field of the output object contains a valid
   Lisp_Object pointing to the same notional object that *IN_FIELD
   contains now.

   See idomatic usage below.  */
static void
dump_field_lv (struct dump_context *ctx,
               void *out,
               const void *in_start,
               const Lisp_Object *in_field,
               struct link_weight weight)
{
  dump_field_lv_or_rawptr (ctx, out, in_start, in_field, NULL, weight);
}

/* Note that we're going to add a manual fixup for the given field
   later.  */
static void
dump_field_fixup_later (struct dump_context *ctx,
                        void *out,
                        const void *in_start,
                        const void *in_field)
{
  // TODO: more error checking
  (void) field_relpos (in_start, in_field);
}

/* Mark an output object field, which is as wide as a poiner, as being
   fixed up to point to a specific offset in the dump.  */
static void
dump_field_ptr_to_dump_offset (struct dump_context *ctx,
                               void *out,
                               const void *in_start,
                               const void *in_field,
                               dump_off target_dump_offset)
{
  eassert (ctx->obj_offset > 0);
  if (!ctx->flags.dump_object_contents)
    return;

  dump_off relpos = field_relpos (in_start, in_field);
  dump_reloc_dump_to_dump_ptr_raw (ctx, ctx->obj_offset + relpos);
  intptr_t outval = target_dump_offset;
  memcpy ((char*) out + relpos, &outval, sizeof (outval));
}

/* Mark a field as pointing to a place inside Emacs.

   CTX is the dump context.  OUT points to the out-object for the
   current dump function.  IN_START points to the start of the object
   being dumped.  IN_FIELD points to the field inside the object being
   dumped that we're dumping.  The contents of this field (which
   should be as wide as a pointer) are the Emacs pointer to dump.

 */
static void
dump_field_emacs_ptr (struct dump_context *ctx,
                      void *out,
                      const void *in_start,
                      const void *in_field)
{
  eassert (ctx->obj_offset > 0);
  if (!ctx->flags.dump_object_contents)
    return;

  intptr_t abs_emacs_ptr;
  cpyptr (&abs_emacs_ptr, in_field);
  ptrdiff_t rel_emacs_ptr = abs_emacs_ptr - (intptr_t) emacs_basis ();
  dump_off relpos = field_relpos (in_start, in_field);
  cpyptr ((char*) out + relpos, &rel_emacs_ptr);
  dump_reloc_dump_to_emacs_ptr_raw (ctx, ctx->obj_offset + relpos);
}

static dump_off
dump_cons (struct dump_context *ctx, const struct Lisp_Cons *cons)
{
  struct Lisp_Cons out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  dump_field_lv (ctx, &out, cons, &cons->u.s.car, WEIGHT_STRONG);
  dump_field_lv (ctx, &out, cons, &cons->u.s.u.cdr, WEIGHT_NORMAL);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_interval_tree (struct dump_context *ctx,
                    INTERVAL tree,
                    dump_off parent_offset)
{
  // TODO: output tree breadth-first?
  struct interval out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, tree, total_length);
  DUMP_FIELD_COPY (&out, tree, position);
  if (tree->left)
    dump_field_fixup_later (ctx, &out, tree, &tree->left);
  if (tree->right)
    dump_field_fixup_later (ctx, &out, tree, &tree->right);
  if (!tree->up_obj)
    {
      eassert (parent_offset != 0);
      dump_field_ptr_to_dump_offset (
        ctx, &out,
        tree, &tree->up.interval,
        parent_offset);
    }
  else
    dump_field_lv (ctx, &out, tree, &tree->up.obj, WEIGHT_STRONG);
  DUMP_FIELD_COPY (&out, tree, up_obj);
  eassert (tree->gcmarkbit == 0);
  DUMP_FIELD_COPY (&out, tree, write_protect);
  DUMP_FIELD_COPY (&out, tree, visible);
  DUMP_FIELD_COPY (&out, tree, front_sticky);
  DUMP_FIELD_COPY (&out, tree, rear_sticky);
  dump_field_lv (ctx, &out, tree, &tree->plist, WEIGHT_STRONG);
  dump_off offset = dump_object_finish (ctx, &out, sizeof (out));
  if (tree->left)
      dump_remember_fixup_ptr_raw (
        ctx,
        offset + dump_offsetof (struct interval, left),
        dump_interval_tree (ctx, tree->left, offset));
  if (tree->right)
      dump_remember_fixup_ptr_raw (
        ctx,
        offset + dump_offsetof (struct interval, right),
        dump_interval_tree (ctx, tree->right, offset));
  return offset;
}

static dump_off
dump_string (struct dump_context *ctx, const struct Lisp_String *string)
{
  /* If we have text properties, write them _after_ the string so that
     at runtime, the prefetcher and cache will DTRT. (We access the
     string before its properties.).

     There's special code to dump string data contiguously later on.
     we seldom write to string data and never relocate it, so lumping
     it together at the end of the dump saves on COW faults.

     If, however, the string's size_byte field is -1, the string data
     is actually a pointer to Emacs data segment, so we can do even
     better by emitting a relocation instead of bothering to copy the
     string data.  */
  struct Lisp_String out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, string, u.s.size);
  DUMP_FIELD_COPY (&out, string, u.s.size_byte);
  if (string->u.s.intervals)
    dump_field_fixup_later (ctx, &out, string, &string->u.s.intervals);

  if (string->u.s.size_byte == -2)
    /* String literal in Emacs rodata.  */
    dump_field_emacs_ptr (ctx, &out, string, &string->u.s.data);
  else
    {
      dump_field_fixup_later (ctx, &out, string, &string->u.s.data);
      dump_remember_cold_op (ctx,
                             COLD_OP_STRING,
                             make_lisp_ptr ((void*) string, Lisp_String));
    }

  dump_off offset = dump_object_finish (ctx, &out, sizeof (out));
  if (string->u.s.intervals)
    dump_remember_fixup_ptr_raw (
      ctx,
      offset + dump_offsetof (struct Lisp_String, u.s.intervals),
      dump_interval_tree (ctx, string->u.s.intervals, 0));

  return offset;
}

static dump_off
dump_marker (struct dump_context *ctx, const struct Lisp_Marker *marker)
{
  struct Lisp_Marker out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, marker, type);
  eassert (marker->gcmarkbit == 0);
  (void) marker->spacer; /* Do not write padding.  */
  DUMP_FIELD_COPY (&out, marker, need_adjustment);
  DUMP_FIELD_COPY (&out, marker, insertion_type);
  if (marker->buffer)
    {
      dump_field_lv_rawptr (
        ctx, &out,
        marker, &marker->buffer,
        Lisp_Vectorlike,
        WEIGHT_NORMAL);
      dump_field_lv_rawptr (
        ctx, &out,
        marker, &marker->next,
        Lisp_Misc,
        WEIGHT_STRONG);
      DUMP_FIELD_COPY (&out, marker, charpos);
      DUMP_FIELD_COPY (&out, marker, bytepos);
    }
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_overlay (struct dump_context *ctx, const struct Lisp_Overlay *overlay)
{
  struct Lisp_Overlay out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, overlay, type);
  eassert (overlay->gcmarkbit == 0);
  (void) overlay->spacer; /* Do not write padding.  */
  dump_field_lv_rawptr (ctx, &out, overlay, &overlay->next, Lisp_Misc,
                        WEIGHT_STRONG);
  dump_field_lv (ctx, &out, overlay, &overlay->start, WEIGHT_STRONG);
  dump_field_lv (ctx, &out, overlay, &overlay->end, WEIGHT_STRONG);
  dump_field_lv (ctx, &out, overlay, &overlay->plist, WEIGHT_STRONG);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_save_value (struct dump_context *ctx,
                 const struct Lisp_Save_Value *ptr)
{
  struct Lisp_Save_Value out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, ptr, type);
  eassert(ptr->gcmarkbit == 0);
  (void) ptr->spacer; /* Do not write padding.  */
  DUMP_FIELD_COPY (&out, ptr, save_type);
  for (int i = 0; i < SAVE_VALUE_SLOTS; i++)
    {
      switch (save_type (&out, i))
        {
        case SAVE_UNUSED:
          break;
        case SAVE_INTEGER:
          DUMP_FIELD_COPY (&out, ptr, data[i].integer);
          break;
        case SAVE_FUNCPOINTER:
          dump_field_emacs_ptr (ctx, &out, ptr, &ptr->data[i].funcpointer);
          break;
        case SAVE_OBJECT:
          dump_field_lv (ctx, &out, ptr, &ptr->data[i].object, WEIGHT_STRONG);
          break;
        case SAVE_POINTER:
          error_unsupported_dump_object(
            ctx, make_lisp_ptr ((void *) ptr, Lisp_Misc), "SAVE_POINTER");
        default:
          emacs_abort ();
        }
    }
  return dump_object_finish (ctx, &out, sizeof (out));
}

static void
dump_field_finalizer_ref (struct dump_context *ctx,
                          void *out,
                          const struct Lisp_Finalizer *finalizer,
                          struct Lisp_Finalizer *const *field)
{
  if (*field == &finalizers || *field == &doomed_finalizers)
    dump_field_emacs_ptr (ctx, out, finalizer, field);
  else
    dump_field_lv_rawptr (ctx, out, finalizer, field, Lisp_Misc,
                          WEIGHT_NORMAL);
}

static dump_off
dump_finalizer (struct dump_context *ctx,
                const struct Lisp_Finalizer *finalizer)
{
  struct Lisp_Finalizer out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, finalizer, base.type);
  eassert (finalizer->base.gcmarkbit == 0);
  (void) finalizer->base.spacer; /* Do not write padding.  */
  dump_field_finalizer_ref (ctx, &out, finalizer, &finalizer->prev);
  dump_field_finalizer_ref (ctx, &out, finalizer, &finalizer->next);
  dump_field_lv (ctx, &out, finalizer, &finalizer->function,
                 WEIGHT_NORMAL);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_misc_any (struct dump_context *ctx, struct Lisp_Misc_Any *misc_any)
{
  dump_off result;

  switch (misc_any->type)
    {
    case Lisp_Misc_Marker:
      result = dump_marker (ctx, (struct Lisp_Marker *) misc_any);
      break;

    case Lisp_Misc_Overlay:
      result = dump_overlay (ctx, (struct Lisp_Overlay *) misc_any);
      break;

    case Lisp_Misc_Save_Value:
      result = dump_save_value (ctx, (struct Lisp_Save_Value *) misc_any);
      break;

    case Lisp_Misc_Finalizer:
      result = dump_finalizer (ctx, (struct Lisp_Finalizer *) misc_any);
      break;

#ifdef HAVE_MODULES
    case Lisp_Misc_User_Ptr:
      error_unsupported_dump_object(
        ctx,
        make_lisp_ptr (misc_any, Lisp_Misc),
        "module user ptr");
      break;
#endif

    default:
      emacs_abort ();
    }

  return result;
}

static dump_off
dump_float (struct dump_context *ctx, const struct Lisp_Float *lfloat)
{
  eassert (ctx->header.cold_start);
  struct Lisp_Float out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, lfloat, u.data);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_fwd_int (struct dump_context *ctx, const struct Lisp_Intfwd *intfwd)
{
  dump_emacs_reloc_immediate_emacs_int (ctx, intfwd->intvar, *intfwd->intvar);
  struct Lisp_Intfwd out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, intfwd, type);
  dump_field_emacs_ptr (ctx, &out, intfwd, &intfwd->intvar);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_fwd_bool (struct dump_context *ctx, const struct Lisp_Boolfwd *boolfwd)
{
  dump_emacs_reloc_immediate_bool (ctx, boolfwd->boolvar, *boolfwd->boolvar);
  struct Lisp_Boolfwd out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, boolfwd, type);
  dump_field_emacs_ptr (ctx, &out, boolfwd, &boolfwd->boolvar);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_fwd_obj (struct dump_context *ctx, const struct Lisp_Objfwd *objfwd)
{
  dump_emacs_reloc_to_dump_lv (ctx, objfwd->objvar, *objfwd->objvar);
  struct Lisp_Objfwd out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, objfwd, type);
  dump_field_emacs_ptr (ctx, &out, objfwd, &objfwd->objvar);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_fwd_buffer_obj (struct dump_context *ctx,
                     const struct Lisp_Buffer_Objfwd *buffer_objfwd)
{
  struct Lisp_Buffer_Objfwd out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, buffer_objfwd, type);
  DUMP_FIELD_COPY (&out, buffer_objfwd, offset);
  dump_field_lv (ctx, &out, buffer_objfwd, &buffer_objfwd->predicate,
                 WEIGHT_NORMAL);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_fwd_kboard_obj (struct dump_context *ctx,
                     const struct Lisp_Kboard_Objfwd *kboard_objfwd)
{
  struct Lisp_Kboard_Objfwd out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, kboard_objfwd, type);
  DUMP_FIELD_COPY (&out, kboard_objfwd, offset);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_fwd (struct dump_context *ctx, union Lisp_Fwd *fwd)
{
  dump_off offset;

  switch (XFWDTYPE (fwd))
    {
    case Lisp_Fwd_Int:
      offset = dump_fwd_int (ctx, &fwd->u_intfwd);
      break;
    case Lisp_Fwd_Bool:
      offset = dump_fwd_bool (ctx, &fwd->u_boolfwd);
      break;
    case Lisp_Fwd_Obj:
      offset = dump_fwd_obj (ctx, &fwd->u_objfwd);
      break;
    case Lisp_Fwd_Buffer_Obj:
      offset = dump_fwd_buffer_obj (ctx, &fwd->u_buffer_objfwd);
      break;
    case Lisp_Fwd_Kboard_Obj:
      offset = dump_fwd_kboard_obj (ctx, &fwd->u_kboard_objfwd);
      break;
    default:
      emacs_abort ();
    }

  return offset;
}

static dump_off
dump_blv (struct dump_context *ctx,
          const struct Lisp_Buffer_Local_Value *blv)
{
  struct Lisp_Buffer_Local_Value out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, blv, local_if_set);
  DUMP_FIELD_COPY (&out, blv, found);
  if (blv->fwd)
    dump_field_fixup_later (ctx, &out, blv, &blv->fwd);
  dump_field_lv (ctx, &out, blv, &blv->where, WEIGHT_NORMAL);
  dump_field_lv (ctx, &out, blv, &blv->defcell, WEIGHT_STRONG);
  dump_field_lv (ctx, &out, blv, &blv->valcell, WEIGHT_STRONG);
  dump_off offset = dump_object_finish (ctx, &out, sizeof (out));
  if (blv->fwd)
    dump_remember_fixup_ptr_raw (
      ctx,
      offset + dump_offsetof (struct Lisp_Buffer_Local_Value, fwd),
      dump_fwd (ctx, blv->fwd));
  return offset;
}

static dump_off
dump_recall_symbol_aux (struct dump_context *ctx, Lisp_Object symbol)
{
  Lisp_Object symbol_aux = ctx->symbol_aux;
  if (NILP (symbol_aux))
    return 0;
  return dump_off_from_lisp (
    Fgethash (symbol, symbol_aux, make_number (0)));
}

static void
dump_remember_symbol_aux (struct dump_context *ctx,
                          Lisp_Object symbol,
                          dump_off offset)
{
  Fputhash (symbol, dump_off_to_lisp (offset), ctx->symbol_aux);
}

static void
dump_pre_dump_symbol (
  struct dump_context *ctx,
  struct Lisp_Symbol *symbol)
{
  Lisp_Object symbol_lv = make_lisp_symbol (symbol);
  eassert (!dump_recall_symbol_aux (ctx, symbol_lv));
  DUMP_SET_REFERRER (ctx, symbol_lv);
  switch (symbol->u.s.redirect)
    {
    case SYMBOL_LOCALIZED:
      dump_remember_symbol_aux (
        ctx,
        symbol_lv,
        dump_blv (ctx, symbol->u.s.val.blv));
      break;
    case SYMBOL_FORWARDED:
      dump_remember_symbol_aux (
        ctx,
        symbol_lv,
        dump_fwd (ctx, symbol->u.s.val.fwd));
      break;
    default:
      break;
    }
  DUMP_CLEAR_REFERRER (ctx);
}

static dump_off
dump_symbol (struct dump_context *ctx, struct Lisp_Symbol *symbol)
{
  if (ctx->flags.defer_symbols)
    {
      /* Scan everything to which this symbol refers.  */
      struct dump_flags old_flags = ctx->flags;
      ctx->flags.dump_object_contents = false;
      ctx->flags.defer_symbols = false;
      dump_symbol (ctx, symbol);
      ctx->flags = old_flags;
      return DUMP_OBJECT_DEFERRED;
    }

  struct Lisp_Symbol out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  eassert (symbol->u.s.gcmarkbit == 0);
  DUMP_FIELD_COPY (&out, symbol, u.s.redirect);
  DUMP_FIELD_COPY (&out, symbol, u.s.trapped_write);
  DUMP_FIELD_COPY (&out, symbol, u.s.interned);
  DUMP_FIELD_COPY (&out, symbol, u.s.declared_special);
  DUMP_FIELD_COPY (&out, symbol, u.s.pinned);
  dump_field_lv (ctx, &out, symbol, &symbol->u.s.name, WEIGHT_STRONG);
  switch (symbol->u.s.redirect)
    {
    case SYMBOL_PLAINVAL:
      dump_field_lv (ctx, &out, symbol, &symbol->u.s.val.value,
                     WEIGHT_NORMAL);
      break;
    case SYMBOL_VARALIAS:
      dump_field_lv_rawptr (ctx, &out, symbol,
                            &symbol->u.s.val.alias, Lisp_Symbol,
                            WEIGHT_NORMAL);
      break;
    case SYMBOL_LOCALIZED:
      dump_field_fixup_later (ctx, &out, symbol, &symbol->u.s.val.blv);
      break;
    case SYMBOL_FORWARDED:
      dump_field_fixup_later (ctx, &out, symbol, &symbol->u.s.val.fwd);
      break;
    default:
      emacs_abort ();
    }
  dump_field_lv (ctx, &out, symbol, &symbol->u.s.function, WEIGHT_NORMAL);
  dump_field_lv (ctx, &out, symbol, &symbol->u.s.plist, WEIGHT_NORMAL);
  dump_field_lv_rawptr (ctx, &out, symbol, &symbol->u.s.next, Lisp_Symbol,
                        WEIGHT_STRONG);

  dump_off offset = dump_object_finish (ctx, &out, sizeof (out));
  dump_off aux_offset;

  switch (symbol->u.s.redirect)
    {
    case SYMBOL_LOCALIZED:
      aux_offset = dump_recall_symbol_aux (ctx, make_lisp_symbol (symbol));
      dump_remember_fixup_ptr_raw (
        ctx,
        offset + dump_offsetof (struct Lisp_Symbol, u.s.val.blv),
        (aux_offset
         ? aux_offset
         : dump_blv (ctx, symbol->u.s.val.blv)));
      break;
    case SYMBOL_FORWARDED:
      aux_offset = dump_recall_symbol_aux (ctx, make_lisp_symbol (symbol));
      dump_remember_fixup_ptr_raw (
        ctx,
        offset + dump_offsetof (struct Lisp_Symbol, u.s.val.fwd),
        (aux_offset
         ? aux_offset
         : dump_fwd (ctx, symbol->u.s.val.fwd)));
      break;
    default:
      break;
    }
  return offset;
}

static dump_off
dump_vectorlike_generic (
  struct dump_context *ctx,
  const union vectorlike_header *header)
{
  const struct Lisp_Vector *v = (const struct Lisp_Vector *) header;
  ptrdiff_t size = header->size;
  enum pvec_type pvectype = PSEUDOVECTOR_TYPE (v);
  dump_off offset;

  if (size & PSEUDOVECTOR_FLAG)
    {
      /* Assert that the pseudovector contains only Lisp values ---
         but see the PVEC_SUB_CHAR_TABLE special case below.  We allow
         one extra word of non-lisp data when Lisp_Object is shorter
         than GCALIGN (e.g., on 32-bit builds) to account for
         GCALIGN-enforcing struct padding.  We can't distinguish
         between padding and some undumpable data member this way, but
         we'll count on sizeof(Lisp_Object) >= GCALIGN builds to catch
         this class of problem.
         */
      eassert (
        ((size & PSEUDOVECTOR_REST_MASK) >> PSEUDOVECTOR_REST_BITS)
        <= (sizeof (Lisp_Object) < GCALIGNMENT) ? 1 : 0);
      size &= PSEUDOVECTOR_SIZE_MASK;
    }

  dump_align_output (ctx, GCALIGNMENT);
  dump_off prefix_start_offset = ctx->offset;

  dump_off skip;
  if (pvectype == PVEC_SUB_CHAR_TABLE)
    {
      /* PVEC_SUB_CHAR_TABLE has a special case because it's a
         variable-length vector (unlike other pseudovectors) and has
         its non-Lisp data _before_ the variable-length Lisp part.  */
      const struct Lisp_Sub_Char_Table *sct =
        (const struct Lisp_Sub_Char_Table *) header;
      struct Lisp_Sub_Char_Table out;
      dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
      DUMP_FIELD_COPY (&out, sct, header.size);
      DUMP_FIELD_COPY (&out, sct, depth);
      DUMP_FIELD_COPY (&out, sct, min_char);
      offset = dump_object_finish (ctx, &out, sizeof (out));
      skip = SUB_CHAR_TABLE_OFFSET;
    }
  else
    {
      union vectorlike_header out;
      dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
      DUMP_FIELD_COPY (&out, header, size);
      offset = dump_object_finish (ctx, &out, sizeof (out));
      skip = 0;
    }

  /* dump_object_start isn't what records conservative-GC object
     starts --- dump_object_1 does --- so the hack below of using
     dump_object_start for each vector word doesn't cause GC problems
     at runtime.  */

  dump_off prefix_size = ctx->offset - prefix_start_offset;
  eassert (prefix_size > 0);
  dump_off skip_start = ptrdiff_t_to_dump_off (
    (char*) &v->contents[skip] - (char*) v);
  eassert (skip_start >= prefix_size);
  dump_write_zero (ctx, skip_start - prefix_size);
  for (dump_off i = skip; i < size; ++i)
    {
      Lisp_Object out;
      const Lisp_Object *vslot = &v->contents[i];
      eassert (ctx->offset % sizeof (out) == 0);
      dump_object_start (ctx, 1, &out, sizeof (out));
      dump_field_lv (ctx, &out, vslot, vslot, WEIGHT_STRONG);
      dump_object_finish (ctx, &out, sizeof (out));
    }

  if (sizeof (Lisp_Object) < GCALIGNMENT)
    dump_write_zero (ctx, GCALIGNMENT - (ctx->offset % GCALIGNMENT));

  return offset;
}

static void
dump_object_start_pseudovector (
  struct dump_context *ctx,
  union vectorlike_header *out_hdr,
  dump_off out_size,
  const union vectorlike_header *in_hdr)
{
  const struct Lisp_Vector *in = (const struct Lisp_Vector *) in_hdr;
  struct Lisp_Vector *out = (struct Lisp_Vector *) out_hdr;

  eassert (vector_nbytes ((struct Lisp_Vector *) in) == out_size);

  dump_object_start (ctx, GCALIGNMENT, out, out_size);
  DUMP_FIELD_COPY (out, in, header);
  ptrdiff_t size = in->header.size;
  eassert (size & PSEUDOVECTOR_FLAG);
  size &= PSEUDOVECTOR_SIZE_MASK;
  for (ptrdiff_t i = 0; i < size; ++i)
    dump_field_lv (ctx, out, in, &in->contents[i], WEIGHT_STRONG);
}

/* Determine whether the hash table's hash order is stable
   across dump and load.  If it is, we don't have to trigger
   a rehash on access.  */
static bool
dump_hash_table_stable_p (const struct Lisp_Hash_Table *hash)
{
  bool is_eql = hash->test.hashfn == hashfn_eql;
  bool is_equal = hash->test.hashfn == hashfn_equal;
  ptrdiff_t size = HASH_TABLE_SIZE (hash);
  for (ptrdiff_t i = 0; i < size; ++i)
    if (!NILP (HASH_HASH (hash, i)))
      {
        Lisp_Object key =  HASH_KEY (hash, i);
        bool key_stable = (dump_builtin_symbol_p (key) ||
                           INTEGERP (key) ||
                           (is_equal && STRINGP (key)) ||
                           ((is_equal || is_eql) && FLOATP (key)));
        if (!key_stable)
          return false;
      }

  return true;
}

/* Return a list of (KEY . VALUE) pairs in the given hash table.  */
static Lisp_Object
hash_table_contents (Lisp_Object table)
{
  Lisp_Object contents = Qnil;
  struct Lisp_Hash_Table *h = XHASH_TABLE (table);
  for (ptrdiff_t i = 0; i < HASH_TABLE_SIZE (h); ++i)
    if (!NILP (HASH_HASH (h, i)))
      dump_push (&contents, Fcons (HASH_KEY (h, i), HASH_VALUE (h, i)));
  return Fnreverse (contents);
}

/* Copy the given hash table, rehash it, and make sure that we can
   look up all the values in the original.  */
static void
check_hash_table_rehash (Lisp_Object table_orig)
{
  Lisp_Object table_rehashed = Fcopy_hash_table (table_orig);
  eassert (XHASH_TABLE (table_rehashed)->count >= 0);
  XHASH_TABLE (table_rehashed)->count *= -1;
  eassert (XHASH_TABLE (table_rehashed)->count <= 0);
  hash_rehash_if_needed (XHASH_TABLE (table_rehashed));
  eassert (XHASH_TABLE (table_rehashed)->count >= 0);
  Lisp_Object expected_contents = hash_table_contents (table_orig);
  while (!NILP (expected_contents))
    {
      Lisp_Object key_value_pair = dump_pop (&expected_contents);
      Lisp_Object key = XCAR (key_value_pair);
      Lisp_Object expected_value = XCDR (key_value_pair);
      Lisp_Object found_value = Fgethash (
        key,
        table_rehashed,
        Qdump_emacs_portable__sort_predicate_copied /* arbitrary */);
      eassert (EQ (expected_value, found_value));
      Fremhash (key, table_rehashed);
    }

  eassert (EQ (Fhash_table_count (table_rehashed),
               make_number (0)));
}

static dump_off
dump_hash_table (struct dump_context *ctx,
                 const struct Lisp_Hash_Table *hash_in)
{
  bool is_stable = dump_hash_table_stable_p (hash_in);
  /* If the hash table is likely to be modified in memory (either
     because we need to rehash, and thus toggle hash->count, or
     because we need to assemble a list of weak tables) punt the hash
     table to the end of the dump, where we can lump all such hash
     tables together.  */
  if (!(is_stable || !NILP (hash_in->weak)) &&
      ctx->flags.defer_hash_tables)
    {
      /* We still want to dump the actual keys and values now.  */
      dump_enqueue_object (ctx, hash_in->key_and_value, WEIGHT_NONE);
      /* We'll get to the rest later.  */
      dump_push (&ctx->deferred_hash_tables,
                 make_lisp_ptr ((void*)hash_in, Lisp_Vectorlike));
      return DUMP_OBJECT_DEFERRED;
    }

  if (PDUMPER_CHECK_REHASHING)
    check_hash_table_rehash (make_lisp_ptr ((void*)hash_in, Lisp_Vectorlike));

  struct Lisp_Hash_Table hash_munged = *hash_in;
  struct Lisp_Hash_Table *hash = &hash_munged;

  /* Remember to rehash this hash table on first access.  After a
     dump reload, the hash table values will have changed, so we'll
     need to rebuild the index.

     TODO: for EQ and EQL hash tables, it should be possible to rehash
     here using the preferred load address of the dump, eliminating
     the need to rehash-on-access if we can load the dump where we
     want.  */
  if (hash->count > 0 && !is_stable)
    hash->count = -hash->count;

  struct Lisp_Hash_Table out;
  dump_object_start_pseudovector (
    ctx, &out.header, sizeof (out), &hash->header);
  DUMP_FIELD_COPY (&out, hash, count);
  DUMP_FIELD_COPY (&out, hash, next_free);
  DUMP_FIELD_COPY (&out, hash, pure);
  DUMP_FIELD_COPY (&out, hash, rehash_threshold);
  DUMP_FIELD_COPY (&out, hash, rehash_size);
  dump_field_lv (ctx, &out, hash, &hash->key_and_value, WEIGHT_STRONG);
  dump_field_lv (ctx, &out, hash, &hash->test.name, WEIGHT_STRONG);
  dump_field_lv (ctx, &out, hash, &hash->test.user_hash_function,
                 WEIGHT_STRONG);
  dump_field_lv (ctx, &out, hash, &hash->test.user_cmp_function,
                 WEIGHT_STRONG);
  dump_field_emacs_ptr (ctx, &out, hash, &hash->test.cmpfn);
  dump_field_emacs_ptr (ctx, &out, hash, &hash->test.hashfn);
  eassert (hash->next_weak == NULL);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static dump_off
dump_buffer (struct dump_context *ctx, const struct buffer *in_buffer)
{
  struct buffer munged_buffer = *in_buffer;
  struct buffer *buffer = &munged_buffer;

  /* Clear some buffer state for correctness upon load.  */
  if (buffer->base_buffer == NULL)
    buffer->window_count = 0;
  else
    eassert (buffer->window_count == -1);
  buffer->last_selected_window_ = Qnil;
  buffer->display_count_ = make_number (0);
  buffer->clip_changed = 0;
  buffer->last_window_start = -1;
  buffer->point_before_scroll_ = Qnil;

  dump_off base_offset = 0;
  if (buffer->base_buffer)
    {
      eassert (buffer->base_buffer->base_buffer == NULL);
      base_offset = dump_object_for_offset (
        ctx,
        make_lisp_ptr (buffer->base_buffer, Lisp_Vectorlike));
    }

  eassert ((base_offset == 0 && buffer->text == &in_buffer->own_text) ||
           (base_offset > 0 && buffer->text != &in_buffer->own_text));

  struct buffer out;
  dump_object_start_pseudovector (
    ctx, &out.header, sizeof (out), &buffer->header);
  if (base_offset == 0)
    base_offset = ctx->obj_offset;
  eassert (base_offset > 0);
  if (buffer->base_buffer == NULL)
    {
      eassert (base_offset == ctx->obj_offset);

      if (BUFFER_LIVE_P (buffer))
        {
          dump_field_fixup_later (ctx, &out, buffer, &buffer->own_text.beg);
          dump_remember_cold_op (
            ctx,
            COLD_OP_BUFFER,
            make_lisp_ptr ((void*) in_buffer, Lisp_Vectorlike));
        }
      else
        eassert (buffer->own_text.beg == NULL);

      DUMP_FIELD_COPY (&out, buffer, own_text.gpt);
      DUMP_FIELD_COPY (&out, buffer, own_text.z);
      DUMP_FIELD_COPY (&out, buffer, own_text.gpt_byte);
      DUMP_FIELD_COPY (&out, buffer, own_text.z_byte);
      DUMP_FIELD_COPY (&out, buffer, own_text.gap_size);
      DUMP_FIELD_COPY (&out, buffer, own_text.modiff);
      DUMP_FIELD_COPY (&out, buffer, own_text.chars_modiff);
      DUMP_FIELD_COPY (&out, buffer, own_text.save_modiff);
      DUMP_FIELD_COPY (&out, buffer, own_text.overlay_modiff);
      DUMP_FIELD_COPY (&out, buffer, own_text.compact);
      DUMP_FIELD_COPY (&out, buffer, own_text.beg_unchanged);
      DUMP_FIELD_COPY (&out, buffer, own_text.end_unchanged);
      DUMP_FIELD_COPY (&out, buffer, own_text.unchanged_modified);
      DUMP_FIELD_COPY (&out, buffer, own_text.overlay_unchanged_modified);
      if (buffer->own_text.intervals)
        dump_field_fixup_later (ctx, &out, buffer, &buffer->own_text.intervals);
      dump_field_lv_rawptr (ctx, &out, buffer, &buffer->own_text.markers,
                            Lisp_Misc, WEIGHT_NORMAL);
      DUMP_FIELD_COPY (&out, buffer, own_text.inhibit_shrinking);
      DUMP_FIELD_COPY (&out, buffer, own_text.redisplay);
    }

  eassert (ctx->obj_offset > 0);
  dump_remember_fixup_ptr_raw (
    ctx,
    ctx->obj_offset + dump_offsetof (struct buffer, text),
    base_offset + dump_offsetof (struct buffer, own_text));

  dump_field_lv_rawptr (ctx, &out, buffer, &buffer->next,
                        Lisp_Vectorlike, WEIGHT_NORMAL);
  DUMP_FIELD_COPY (&out, buffer, pt);
  DUMP_FIELD_COPY (&out, buffer, pt_byte);
  DUMP_FIELD_COPY (&out, buffer, begv);
  DUMP_FIELD_COPY (&out, buffer, begv_byte);
  DUMP_FIELD_COPY (&out, buffer, zv);
  DUMP_FIELD_COPY (&out, buffer, zv_byte);

  if (buffer->base_buffer)
    {
      eassert (ctx->obj_offset != base_offset);
      dump_field_ptr_to_dump_offset (
        ctx, &out, buffer, &buffer->base_buffer,
        base_offset);
    }

  DUMP_FIELD_COPY (&out, buffer, indirections);
  DUMP_FIELD_COPY (&out, buffer, window_count);

  memcpy (&out.local_flags,
          &buffer->local_flags,
          sizeof (out.local_flags));
  DUMP_FIELD_COPY (&out, buffer, modtime);
  DUMP_FIELD_COPY (&out, buffer, modtime_size);
  DUMP_FIELD_COPY (&out, buffer, auto_save_modified);
  DUMP_FIELD_COPY (&out, buffer, display_error_modiff);
  DUMP_FIELD_COPY (&out, buffer, auto_save_failure_time);
  DUMP_FIELD_COPY (&out, buffer, last_window_start);

  /* Not worth serializing these caches.  TODO: really? */
  out.newline_cache = NULL;
  out.width_run_cache = NULL;
  out.bidi_paragraph_cache = NULL;

  DUMP_FIELD_COPY (&out, buffer, prevent_redisplay_optimizations_p);
  DUMP_FIELD_COPY (&out, buffer, clip_changed);

  dump_field_lv_rawptr (ctx, &out, buffer, &buffer->overlays_before,
                        Lisp_Misc, WEIGHT_NORMAL);

  dump_field_lv_rawptr (ctx, &out, buffer, &buffer->overlays_after,
                        Lisp_Misc, WEIGHT_NORMAL);

  DUMP_FIELD_COPY (&out, buffer, overlay_center);
  dump_field_lv (ctx, &out, buffer, &buffer->undo_list_,
                 WEIGHT_STRONG);
  dump_off offset = dump_object_finish (ctx, &out, sizeof (out));
  if (!buffer->base_buffer && buffer->own_text.intervals)
    dump_remember_fixup_ptr_raw (
      ctx,
      offset + dump_offsetof (struct buffer, own_text.intervals),
      dump_interval_tree (ctx, buffer->own_text.intervals, 0));

  return offset;
}

static dump_off
dump_bool_vector (struct dump_context *ctx, const struct Lisp_Vector *v)
{
  /* No relocation needed, so we don't need dump_object_start.  */
  dump_align_output (ctx, GCALIGNMENT);
  eassert (ctx->offset >= ctx->header.cold_start);
  dump_off offset = ctx->offset;
  ptrdiff_t nbytes = vector_nbytes ((struct Lisp_Vector *) v);
  if (nbytes > DUMP_OFF_T_MAX)
    error ("vector too large");
  dump_write (ctx, v, ptrdiff_t_to_dump_off (nbytes));
  return offset;
}

static dump_off
dump_subr (struct dump_context *ctx, const struct Lisp_Subr *subr)
{
  struct Lisp_Subr out;
  dump_object_start (ctx, GCALIGNMENT, &out, sizeof (out));
  DUMP_FIELD_COPY (&out, subr, header.size);
  dump_field_emacs_ptr (ctx, &out, subr, &subr->function.a0);
  DUMP_FIELD_COPY (&out, subr, min_args);
  DUMP_FIELD_COPY (&out, subr, max_args);
  dump_field_emacs_ptr (ctx, &out, subr, &subr->symbol_name);
  dump_field_emacs_ptr (ctx, &out, subr, &subr->intspec);
  DUMP_FIELD_COPY (&out, subr, doc);
  return dump_object_finish (ctx, &out, sizeof (out));
}

static void
fill_pseudovec (union vectorlike_header *header, Lisp_Object item)
{
  struct Lisp_Vector *v = (struct Lisp_Vector *) header;
  eassert (v->header.size & PSEUDOVECTOR_FLAG);
  ptrdiff_t size = v->header.size & PSEUDOVECTOR_SIZE_MASK;
  for (ptrdiff_t idx = 0; idx < size; idx++)
    v->contents[idx] = item;
}

static dump_off
dump_nilled_pseudovec (struct dump_context *ctx,
                       const union vectorlike_header *in)
{
  if (vector_nbytes ((struct Lisp_Vector *) in) > DUMP_OFF_T_MAX)
    error ("pseudovector too large");
  dump_off nbytes = ptrdiff_t_to_dump_off (
    vector_nbytes ((struct Lisp_Vector *) in));
  union vectorlike_header *in_nilled = alloca (nbytes);
  memset (in_nilled, 0, nbytes);
  in_nilled->size = in->size;
  fill_pseudovec (in_nilled, Qnil);
  union vectorlike_header *out = alloca (nbytes);
  memset (out, 0, nbytes);
  dump_object_start_pseudovector (ctx, out, nbytes, in_nilled);
  return dump_object_finish (ctx, out, nbytes);
}

static dump_off
dump_vectorlike (struct dump_context *ctx, const struct Lisp_Vector *v)
{
  dump_off offset;
  Lisp_Object lv = make_lisp_ptr ((void *) v, Lisp_Vectorlike);
  switch (PSEUDOVECTOR_TYPE (v))
    {
    case PVEC_FONT:
      /* There are three kinds of font objects that all use PVEC_FONT,
         distinguished by their size.  Font specs and entities are
         harmless data carriers that we can dump like other Lisp
         objects.  Fonts themselves are window-system-specific and
         need to be recreated on each startup.  */
      if ((v->header.size & PSEUDOVECTOR_SIZE_MASK) != FONT_SPEC_MAX &&
          (v->header.size & PSEUDOVECTOR_SIZE_MASK) != FONT_ENTITY_MAX)
        error_unsupported_dump_object(ctx, lv, "font");
      /* Fall through */
    case PVEC_NORMAL_VECTOR:
    case PVEC_COMPILED:
    case PVEC_CHAR_TABLE:
    case PVEC_SUB_CHAR_TABLE:
    case PVEC_RECORD:
      offset = dump_vectorlike_generic (ctx, &v->header);
      break;
    case PVEC_BOOL_VECTOR:
      offset = dump_bool_vector(ctx, v);
      break;
    case PVEC_HASH_TABLE:
      offset = dump_hash_table (ctx, (struct Lisp_Hash_Table *) v);
      break;
    case PVEC_BUFFER:
      offset = dump_buffer (ctx, (struct buffer *) v);
      break;
    case PVEC_SUBR:
      offset = dump_subr (ctx, (const struct Lisp_Subr *) v);
      break;
    case PVEC_FRAME:
    case PVEC_WINDOW:
    case PVEC_PROCESS:
    case PVEC_TERMINAL:
      offset = dump_nilled_pseudovec (ctx, &v->header);
      break;
    case PVEC_WINDOW_CONFIGURATION:
      error_unsupported_dump_object (ctx, lv, "window configuration");
    case PVEC_OTHER:
      error_unsupported_dump_object (ctx, lv, "other?!");
    case PVEC_XWIDGET:
      error_unsupported_dump_object (ctx, lv, "xwidget");
    case PVEC_XWIDGET_VIEW:
      error_unsupported_dump_object (ctx, lv, "xwidget view");
    case PVEC_THREAD:
    case PVEC_MUTEX:
    case PVEC_CONDVAR:
      error_unsupported_dump_object (ctx, lv, "threading object");
    case PVEC_MODULE_FUNCTION:
      error_unsupported_dump_object (ctx, lv, "module function");
    default:
      error_unsupported_dump_object(ctx, lv, "weird pseudovector");
    }

  return offset;
}

/* Internal guts of dump_object().

   This function has the same contract as dump_object(), except that
   it doesn't defer copying dumped objects (instead, dumping them
   immediately) and always returns a valid offset.

   Called directly by dump_copied_objects() to bypass dump_object()'s
   check for copied objects.
 */
static dump_off
dump_object_1 (struct dump_context *ctx, Lisp_Object object)
{
#ifdef ENABLE_CHECKING
  /* Vdead is extern only when ENABLE_CHECKING.  */
  eassert (!EQ (object, Vdead));
#endif

  dump_off offset = dump_recall_object (ctx, object);
  if (offset > 0)
    {
      /* Object already dumped.  */
    }
  else
    {
      /* Object needs to be dumped.  */
      DUMP_SET_REFERRER (ctx, object);
      switch (XTYPE (object))
        {
        case Lisp_String:
          offset = dump_string (ctx, XSTRING (object));
          break;
        case Lisp_Vectorlike:
          offset = dump_vectorlike (ctx, XVECTOR (object));
          break;
        case Lisp_Symbol:
          offset = dump_symbol (ctx, XSYMBOL (object));
          break;
        case Lisp_Misc:
          offset = dump_misc_any (ctx, XMISCANY (object));
          break;
        case Lisp_Cons:
          offset = dump_cons (ctx, XCONS (object));
          break;
        case Lisp_Float:
          offset = dump_float (ctx, XFLOAT (object));
          break;
        case_Lisp_Int:
          eassert (("should not be dumping int: is self-representing", 0));
        default:
          emacs_abort ();
        }
      DUMP_CLEAR_REFERRER (ctx);

      /* offset can be < 0 if we've deferred an object --- e.g., a
         hash table.  */
      if (ctx->flags.dump_object_contents && offset > 0)
        {
          eassert (offset % (1<<DUMP_RELOC_ALIGNMENT_BITS) == 0);
          dump_remember_object (ctx, object, offset);
          if (ctx->flags.dump_object_starts)
            dump_push (&ctx->object_starts,
                       list2 (dump_off_to_lisp (XTYPE (object)),
                              dump_off_to_lisp (offset)));
        }
    }

  return offset;
}

/* Add an object to the dump.

   CTX is the dump context; OBJECT is the object to add.  Normally,
   return OFFSET, the location (in bytes, from the start of the dump
   file) where we wrote the object.  Valid OFFSETs are always greater
   than zero.

   If we've already dumped an object, return the location where we put
   it: dump_object is idempotent.

   The object may not be self-representing.  Self-representing objects
   are immediate values rather than tagged pointers to Lisp heap
   structures and so have no individual representation in the Lisp
   heap dump.

   May also return DUMP_OBJECT_DEFERRED if we "dumped" the
   object by remembering to process it specially later.  In this case,
   we don't have a valid offset.  Call dump_object_for_offset if you
   need a valid offset for an object.
 */
static dump_off
dump_object (struct dump_context *ctx, Lisp_Object object)
{
  dump_off result;

  if (dump_object_emacs_ptr (object) == NULL)
    {
      eassert (!dump_object_self_representing_p (object));
      result = dump_object_1 (ctx, object);
    }
  else
    {
      /* Objects that are part of the Emacs image need to be copied
         into that image from the dump image, so handle them
         specially.  */
      result = dump_recall_object (ctx, object);

      /* We should not have written a copied object normally due to
         the above constraint.  This object must either be on some
         queue or not yet seen.  */
      eassert (result == DUMP_OBJECT_NOT_SEEN ||
               result == DUMP_OBJECT_ON_NORMAL_QUEUE ||
               result == DUMP_OBJECT_DEFERRED);
      if (result != DUMP_OBJECT_DEFERRED)
        {
          /* Remember to dump this object in the special copied
             objects section.  */
          dump_push (&ctx->copied_queue, object);
          result = DUMP_OBJECT_DEFERRED;
          dump_remember_object (ctx, object, result);

          /* But scan the object for objects to which it refers.  */
          struct dump_flags old_flags = ctx->flags;
          ctx->flags.dump_object_contents = false;
          dump_object_1 (ctx, object);
          ctx->flags = old_flags;
        }
    }

  return result;
}

/* Like dump_object(), but assert that we get a valid offset.  */
static dump_off
dump_object_for_offset (struct dump_context *ctx, Lisp_Object object)
{
  dump_off offset = dump_object (ctx, object);
  eassert (offset > 0);
  return offset;
}

static dump_off
dump_charset (struct dump_context *ctx, int cs_i)
{
  const struct charset *cs = charset_table + cs_i;
  struct charset out;
  dump_object_start (ctx, sizeof (int), &out, sizeof (out));
  DUMP_FIELD_COPY (&out, cs, id);
  DUMP_FIELD_COPY (&out, cs, hash_index);
  DUMP_FIELD_COPY (&out, cs, dimension);
  memcpy (out.code_space, &cs->code_space, sizeof (cs->code_space));
  if (cs->code_space_mask)
    dump_field_fixup_later (ctx, &out, cs, &cs->code_space_mask);
  DUMP_FIELD_COPY (&out, cs, code_linear_p);
  DUMP_FIELD_COPY (&out, cs, iso_chars_96);
  DUMP_FIELD_COPY (&out, cs, ascii_compatible_p);
  DUMP_FIELD_COPY (&out, cs, supplementary_p);
  DUMP_FIELD_COPY (&out, cs, compact_codes_p);
  DUMP_FIELD_COPY (&out, cs, unified_p);
  DUMP_FIELD_COPY (&out, cs, iso_final);
  DUMP_FIELD_COPY (&out, cs, iso_revision);
  DUMP_FIELD_COPY (&out, cs, emacs_mule_id);
  DUMP_FIELD_COPY (&out, cs, method);
  DUMP_FIELD_COPY (&out, cs, min_code);
  DUMP_FIELD_COPY (&out, cs, max_code);
  DUMP_FIELD_COPY (&out, cs, char_index_offset);
  DUMP_FIELD_COPY (&out, cs, min_char);
  DUMP_FIELD_COPY (&out, cs, max_char);
  DUMP_FIELD_COPY (&out, cs, invalid_code);
  memcpy (out.fast_map, &cs->fast_map, sizeof (cs->fast_map));
  DUMP_FIELD_COPY (&out, cs, code_offset);
  dump_off offset = dump_object_finish (ctx, &out, sizeof (out));
  if (cs->code_space_mask)
    dump_remember_cold_op (ctx, COLD_OP_CHARSET,
                           Fcons (dump_off_to_lisp (cs_i),
                                  dump_off_to_lisp (offset)));
  return offset;
}

static dump_off
dump_charset_table (struct dump_context *ctx)
{
  dump_align_output (ctx, GCALIGNMENT);
  dump_off offset = ctx->offset;
  for (int i = 0; i < charset_table_used; ++i)
    dump_charset (ctx, i);
  dump_emacs_reloc_to_dump_ptr_raw (ctx, &charset_table, offset);
  dump_emacs_reloc_immediate_int (
    ctx, &charset_table_used, charset_table_used);
  dump_emacs_reloc_immediate_ptrdiff_t (
    ctx, &charset_table_size, charset_table_used);
  return offset;
}

static void
dump_finalizer_list_head_ptr (struct dump_context *ctx,
                              struct Lisp_Finalizer **ptr)
{
  struct Lisp_Finalizer *value = *ptr;
  if (value != &finalizers && value != &doomed_finalizers)
    dump_emacs_reloc_to_dump_ptr_raw (
      ctx, ptr,
      dump_object_for_offset (ctx, make_lisp_ptr (value, Lisp_Misc)));
}

static void
dump_metadata_for_pdumper (struct dump_context *ctx)
{
  for (int i = 0; i < nr_dump_hooks; ++i)
    dump_emacs_reloc_to_emacs_ptr_raw (ctx, &dump_hooks[i], dump_hooks[i]);
  dump_emacs_reloc_immediate_int (ctx, &nr_dump_hooks, nr_dump_hooks);

  for (int i = 0; i < nr_remembered_data; ++i)
    {
      dump_emacs_reloc_to_emacs_ptr_raw (
        ctx,
        &remembered_data[i].mem,
        remembered_data[i].mem);
      dump_emacs_reloc_immediate_int (
        ctx,
        &remembered_data[i].sz,
        remembered_data[i].sz);
    }
  dump_emacs_reloc_immediate_int (
    ctx,
    &nr_remembered_data,
    nr_remembered_data);
}

/* Sort the list of copied objects in CTX.  */
static void
dump_sort_copied_objects (struct dump_context *ctx)
{
  /* Sort the objects into the order in which they'll appear in the
     Emacs: this way, on startup, we'll do both the IO from the dump
     file and the copy into Emacs in-order, where prefetch will be
     most effective.  */
  ctx->copied_queue =
    Fsort (Fnreverse (
             ctx->copied_queue),
           Qdump_emacs_portable__sort_predicate_copied);
}

/* Dump parts of copied objects we need at runtime.  */
static void
dump_hot_parts_of_discardable_objects (struct dump_context *ctx)
{
  Lisp_Object copied_queue = ctx->copied_queue;
  while (!NILP (copied_queue))
    {
      Lisp_Object copied = dump_pop (&copied_queue);
      if (SYMBOLP (copied))
        {
          eassert (dump_builtin_symbol_p (copied));
          dump_pre_dump_symbol (ctx, XSYMBOL (copied));
        }
    }
}

static void
dump_copied_objects (struct dump_context *ctx)
{
  Lisp_Object copied_queue = ctx->copied_queue;
  ctx->copied_queue = Qnil;
  /* Dump the objects and generate a copy relocation for each.  Don't
     bother trying to reduce the number of copy relocations we
     generate: we'll merge adjacent copy relocations upon output.
     The overfall result is that to the greatest extent possible while
     maintaining strictly increasing address order, we copy into Emacs
     in nice big chunks.  */
  while (!NILP (copied_queue))
    {
      Lisp_Object copied = dump_pop (&copied_queue);
      void *optr = dump_object_emacs_ptr (copied);
      eassert (optr != NULL);

      /* We should have already fully scanned these objects, so assert
         that we're not adding more entries to the dump queue.  */
      struct dump_flags old_flags = ctx->flags;
      ctx->flags.assert_already_seen = true;
      dump_off start_offset = dump_object_1 (ctx, copied);
      ctx->flags = old_flags;

      dump_off size = ctx->offset - start_offset;
      dump_emacs_reloc_copy_from_dump (ctx, start_offset, optr, size);
    }
}

static void
dump_cold_string (struct dump_context *ctx, Lisp_Object string)
{
  /* Dump string contents.  */
  dump_off string_offset = dump_recall_object (ctx, string);
  eassert (string_offset > 0);
  if (SBYTES (string) > DUMP_OFF_T_MAX - 1)
    error ("string too large");
  dump_off total_size = ptrdiff_t_to_dump_off (SBYTES (string) + 1);
  eassert (total_size > 0);
  dump_remember_fixup_ptr_raw (
    ctx,
    string_offset + dump_offsetof (struct Lisp_String, u.s.data),
    ctx->offset);
  dump_write (ctx, XSTRING (string)->u.s.data, total_size);
}

static void
dump_cold_charset (struct dump_context *ctx, Lisp_Object data)
{
  /* Dump charset lookup tables.  */
  ALLOW_IMPLICIT_CONVERSION;
  int cs_i = XFASTINT (XCAR (data));
  DISALLOW_IMPLICIT_CONVERSION;
  dump_off cs_dump_offset = dump_off_from_lisp (XCDR (data));
  dump_remember_fixup_ptr_raw (
    ctx,
    cs_dump_offset + dump_offsetof (struct charset, code_space_mask),
    ctx->offset);
  struct charset *cs = charset_table + cs_i;
  dump_write (ctx, cs->code_space_mask, 256);
}

static void
dump_cold_buffer (struct dump_context *ctx, Lisp_Object data)
{
  /* Dump buffer text.  */
  dump_off buffer_offset = dump_recall_object (ctx, data);
  eassert (buffer_offset > 0);
  struct buffer *b = XBUFFER (data);
  eassert (b->text == &b->own_text);
  /* Zero the gap so we don't dump uninitialized bytes.  */
  memset (BUF_GPT_ADDR (b), 0, BUF_GAP_SIZE (b));
  /* See buffer.c for this calculation.  */
  ptrdiff_t nbytes =
    BUF_Z_BYTE (b)
    - BUF_BEG_BYTE (b)
    + BUF_GAP_SIZE (b)
    + 1;
  if (nbytes > DUMP_OFF_T_MAX)
    error ("buffer too large");
  dump_remember_fixup_ptr_raw (
    ctx,
    buffer_offset + dump_offsetof (struct buffer, own_text.beg),
    ctx->offset);
  dump_write (ctx, b->own_text.beg, ptrdiff_t_to_dump_off (nbytes));
}

static void
dump_cold_data (struct dump_context *ctx)
{
  Lisp_Object cold_queue = Fnreverse (ctx->cold_queue);
  ctx->cold_queue = Qnil;

  /* We should have already scanned all objects to which our cold
     objects refer, so die if an object points to something we haven't
     seen.  */
  struct dump_flags old_flags = ctx->flags;
  ctx->flags.assert_already_seen = true;

  while (!NILP (cold_queue))
    {
      Lisp_Object item = dump_pop (&cold_queue);
      enum cold_op op = (enum cold_op) XFASTINT (XCAR (item));
      Lisp_Object data = XCDR (item);
      switch (op)
        {
        case COLD_OP_STRING:
          dump_cold_string (ctx, data);
          break;
        case COLD_OP_CHARSET:
          dump_cold_charset (ctx, data);
          break;
        case COLD_OP_BUFFER:
          dump_cold_buffer (ctx, data);
          break;
        case COLD_OP_OBJECT:
          /* Objects that we can put in the cold section
             must not refer to other objects.  */
          eassert (dump_queue_empty_p (&ctx->dump_queue));
          eassert (ctx->flags.dump_object_contents);
          dump_object (ctx, data);
          eassert (dump_queue_empty_p (&ctx->dump_queue));
          break;
        default:
          emacs_abort ();
        }
    }

  ctx->flags = old_flags;
}

static void
read_ptr_raw_and_lv (const void *mem,
                     enum Lisp_Type type,
                     void **out_ptr,
                     Lisp_Object *out_lv)
{
  memcpy (out_ptr, mem, sizeof (*out_ptr));
  if (*out_ptr != NULL)
    {
      switch (type)
        {
        case Lisp_Symbol:
          *out_lv = make_lisp_symbol (*out_ptr);
          break;
        case Lisp_Misc:
        case Lisp_String:
        case Lisp_Vectorlike:
        case Lisp_Cons:
        case Lisp_Float:
          *out_lv = make_lisp_ptr (*out_ptr, type);
          break;
        default:
          emacs_abort ();
        }
    }
}

/* Enqueue for dumping objects referenced by static non-Lisp_Object
   pointers inside Emacs.  */
static void
dump_user_remembered_data_hot (struct dump_context *ctx)
{
  for (int i = 0; i < nr_remembered_data; ++i)
    {
      void *mem = remembered_data[i].mem;
      int sz = remembered_data[i].sz;
      if (sz <= 0)
        {
          enum Lisp_Type type = -sz;
          void *value;
          Lisp_Object lv;
          read_ptr_raw_and_lv (mem, type, &value, &lv);
          if (value != NULL)
            {
              DUMP_SET_REFERRER (ctx, dump_ptr_referrer ("user data", mem));
              dump_enqueue_object (ctx, lv, WEIGHT_NONE);
              DUMP_CLEAR_REFERRER (ctx);
            }
        }
    }
}

/* Dump user-specified non-relocated data.  */
static void
dump_user_remembered_data_cold (struct dump_context *ctx)
{
  for (int i = 0; i < nr_remembered_data; ++i)
    {
      void *mem = remembered_data[i].mem;
      int sz = remembered_data[i].sz;
      if (sz > 0)
        {
          /* Scalar: try to inline the value into the relocation if
             it's small enough; if it's bigger than we can fit in a
             relocation, we have to copy the data into the dump proper
             and issue a copy relocation.  */
          if (sz <= sizeof (intmax_t))
            dump_emacs_reloc_immediate (ctx, mem, mem, sz);
          else
            {
              dump_emacs_reloc_copy_from_dump (ctx, ctx->offset, mem, sz);
              dump_write (ctx, mem, sz);
            }
        }
      else
        {
          /* *mem is a raw pointer to a Lisp object of some sort.
             The object to which it points should have already been
             dumped by dump_user_remembered_data_hot.  */
          void *value;
          Lisp_Object lv;
          enum Lisp_Type type = -sz;
          read_ptr_raw_and_lv (mem, type, &value, &lv);
          if (value == NULL)
            /* We can't just ignore NULL: the variable might have
               transitioned from non-NULL to NULL, and we want to
               record this fact.  */
            dump_emacs_reloc_immediate_ptrdiff_t (ctx, mem, 0);
          else
            {
              if (dump_object_emacs_ptr (lv) != NULL)
                {
                  /* We have situation like this:

                     static Lisp_Symbol *foo;
                     ...
                     foo = XSYMBOL(Qt);
                     ...
                     pdumper_remember_lv_ptr_raw (&foo, Lisp_Symbol);

                     Built-in symbols like Qt aren't in the dump!
                     They're actually in Emacs proper.  We need a
                     special case to point this value back at Emacs
                     instead of to something in the dump that
                     isn't there.

                     An analogous situation applies to subrs, since
                     Lisp_Subr structures always live in Emacs, not
                     the dump.

                  */
                  dump_emacs_reloc_to_emacs_ptr_raw (
                    ctx, mem, dump_object_emacs_ptr (lv));
                }
              else
                {
                  eassert (!dump_object_self_representing_p (lv));
                  dump_off dump_offset = dump_recall_object (ctx, lv);
                  if (dump_offset <= 0)
                    error ("raw-pointer object not dumped?!");
                  dump_emacs_reloc_to_dump_ptr_raw (ctx, mem, dump_offset);
                }
            }
        }
    }
}

static void
dump_unwind_cleanup (void *data)
{
  struct dump_context *ctx = data;
  if (ctx->fd >= 0)
    emacs_close (ctx->fd);
#ifdef REL_ALLOC
  if (ctx->blocked_ralloc)
    r_alloc_inhibit_buffer_relocation (0);
#endif
  Vpurify_flag = ctx->old_purify_flag;
  unblock_input ();
}

static void
dump_do_fixup (struct dump_context *ctx, Lisp_Object fixup)
{
  enum dump_fixup_type type =
    (enum dump_fixup_type) XFASTINT (XCAR (fixup));
  fixup = XCDR (fixup);
  dump_off dump_fixup_offset = dump_off_from_lisp (XCAR (fixup));
  fixup = XCDR (fixup);
  Lisp_Object arg = XCAR (fixup);
  eassert (NILP (XCDR (fixup)));
  dump_seek (ctx, dump_fixup_offset);
  intptr_t dump_value;
  bool do_write = true;
  switch (type)
    {
    case DUMP_FIXUP_LISP_OBJECT:
    case DUMP_FIXUP_LISP_OBJECT_RAW:
      /* Dump wants a pointer to a Lisp object.
         If DUMP_FIXUP_LISP_OBJECT_RAW, we should stick a C pointer in
         the dump; otherwise, a Lisp_Object.  */
      if (SUBRP (arg))
        {
          dump_value = emacs_offset (XSUBR (arg));
          if (type == DUMP_FIXUP_LISP_OBJECT)
            dump_reloc_dump_to_emacs_lv (ctx, ctx->offset, XTYPE (arg));
          else
            dump_reloc_dump_to_emacs_ptr_raw (ctx, ctx->offset);
        }
      else if (dump_builtin_symbol_p (arg))
        {
          eassert (dump_object_self_representing_p (arg));
          /* These symbols are part of Emacs, so point there.  If we
             want a Lisp_Object, we're set.  If we want a raw pointer,
             we need to emit a relocation.  */
          if (type == DUMP_FIXUP_LISP_OBJECT)
            {
              do_write = false;
              dump_write (ctx, &arg, sizeof (arg));
            }
          else
            {
              dump_value = emacs_offset (XSYMBOL (arg));
              dump_reloc_dump_to_emacs_ptr_raw (ctx, ctx->offset);
            }
        }
      else
        {
          eassert (dump_object_emacs_ptr (arg) == NULL);
          dump_value = dump_recall_object (ctx, arg);
          if (dump_value <= 0)
            error ("fixup object not dumped");
          if (type == DUMP_FIXUP_LISP_OBJECT)
            dump_reloc_dump_to_dump_lv (ctx, ctx->offset, XTYPE (arg));
          else
            dump_reloc_dump_to_dump_ptr_raw (ctx, ctx->offset);
        }
      break;
    case DUMP_FIXUP_PTR_DUMP_RAW:
      /* Dump wants a raw pointer to something that's not a lisp
         object.  It knows the exact location it wants, so just
         believe it.  */
      dump_value = dump_off_from_lisp (arg);
      dump_reloc_dump_to_dump_ptr_raw (ctx, ctx->offset);
      break;
    default:
      emacs_abort ();
    }
  if (do_write)
    dump_write (ctx, &dump_value, sizeof (dump_value));
}

/* Return DUMP_OFFSET, making sure it is within the heap.  */
static dump_off
dump_check_dump_off (struct dump_context *ctx, dump_off dump_offset)
{
  eassert (dump_offset > 0);
  if (ctx)
    eassert (dump_offset < ctx->end_heap);
  return dump_offset;
}

static void
dump_check_emacs_off (dump_off emacs_off)
{
  eassert (labs (emacs_off) <= 30*1024*1024);
}

static void
dump_emit_dump_reloc (struct dump_context *ctx, Lisp_Object lreloc)
{
  struct dump_reloc reloc;
  dump_object_start (ctx, 1, &reloc, sizeof (reloc));
  dump_reloc_set_type (
    &reloc,
    (enum dump_reloc_type) XFASTINT (dump_pop (&lreloc)));
  eassert (reloc.type <= RELOC_DUMP_TO_EMACS_LV + Lisp_Float);
  dump_reloc_set_offset (&reloc, dump_off_from_lisp (dump_pop (&lreloc)));
  if (dump_reloc_get_offset (reloc) < ctx->header.discardable_start)
    ctx->number_hot_relocations += 1;
  else
    ctx->number_discardable_relocations += 1;

  dump_check_dump_off (ctx, dump_reloc_get_offset (reloc));
  eassert (NILP (lreloc));
  dump_object_finish (ctx, &reloc, sizeof (reloc));
}

static struct emacs_reloc
decode_emacs_reloc (struct dump_context *ctx, Lisp_Object lreloc)
{
  struct emacs_reloc reloc;
  memset (&reloc, 0, sizeof (reloc));
  ALLOW_IMPLICIT_CONVERSION;
  int type = XFASTINT (dump_pop (&lreloc));
  DISALLOW_IMPLICIT_CONVERSION;
  reloc.emacs_offset = dump_off_from_lisp (dump_pop (&lreloc));
  dump_check_emacs_off (reloc.emacs_offset);
  switch (type)
    {
    case RELOC_EMACS_COPY_FROM_DUMP:
      {
        emacs_reloc_set_type (&reloc, type);
        reloc.u.dump_offset = dump_off_from_lisp (dump_pop (&lreloc));
        dump_check_dump_off (ctx, reloc.u.dump_offset);
        dump_off length = dump_off_from_lisp (dump_pop (&lreloc));
        ALLOW_IMPLICIT_CONVERSION;
        reloc.length = length;
        DISALLOW_IMPLICIT_CONVERSION;
        if (reloc.length != length)
          error ("relocation copy length too large");
      }
      break;
    case RELOC_EMACS_IMMEDIATE:
      {
        emacs_reloc_set_type (&reloc, type);
        intmax_t value = intmax_t_from_lisp (dump_pop (&lreloc));
        dump_off size = dump_off_from_lisp (dump_pop (&lreloc));
        reloc.u.immediate = value;
        ALLOW_IMPLICIT_CONVERSION;
        reloc.length = size;
        DISALLOW_IMPLICIT_CONVERSION;
        eassert (reloc.length == size);
      }
      break;
    default:
      {
        eassert (RELOC_EMACS_DUMP_LV <= type);
        eassert (type <= RELOC_EMACS_DUMP_LV + Lisp_Float);
        emacs_reloc_set_type (&reloc, RELOC_EMACS_DUMP_LV);
        ALLOW_IMPLICIT_CONVERSION;
        reloc.length = type - RELOC_EMACS_DUMP_LV;
        DISALLOW_IMPLICIT_CONVERSION;
        eassert (reloc.length == type - RELOC_EMACS_DUMP_LV);
        Lisp_Object target_value = dump_pop (&lreloc);
        /* If the object is self-representing,
           dump_emacs_reloc_to_dump_lv didn't do its job.
           dump_emacs_reloc_to_dump_lv should have added a
           RELOC_EMACS_IMMEDIATE relocation instead.  */
        eassert (!dump_object_self_representing_p (target_value));
        reloc.u.dump_offset = dump_recall_object (ctx, target_value);
        if (reloc.u.dump_offset <= 0)
          {
            Lisp_Object repr = Fprin1_to_string (target_value, Qnil);
            error ("relocation target was not dumped: %s", SDATA (repr));
          }
        dump_check_dump_off (ctx, reloc.u.dump_offset);
      }
      break;
    case RELOC_EMACS_EMACS_PTR_RAW:
      emacs_reloc_set_type (&reloc, type);
      reloc.u.emacs_offset2 = dump_off_from_lisp (dump_pop (&lreloc));
      dump_check_emacs_off (reloc.u.emacs_offset2);
      break;
    case RELOC_EMACS_DUMP_PTR_RAW:
      emacs_reloc_set_type (&reloc, type);
      reloc.u.dump_offset = dump_off_from_lisp (dump_pop (&lreloc));
      dump_check_dump_off (ctx, reloc.u.dump_offset);
      break;
    }

  eassert (NILP (lreloc));
  return reloc;
}

static void
dump_emit_emacs_reloc (struct dump_context *ctx, Lisp_Object lreloc)
{
  struct emacs_reloc reloc;
  dump_object_start (ctx, 1, &reloc, sizeof (reloc));
  reloc = decode_emacs_reloc (ctx, lreloc);
  dump_object_finish (ctx, &reloc, sizeof (reloc));
}

static Lisp_Object
dump_merge_emacs_relocs (Lisp_Object lreloc_a, Lisp_Object lreloc_b)
{
  /* Combine copy relocations together if they're copying from
     adjacent chunks to adjacent chunks.  */

  if (XFASTINT (XCAR (lreloc_a)) != RELOC_EMACS_COPY_FROM_DUMP ||
      XFASTINT (XCAR (lreloc_b)) != RELOC_EMACS_COPY_FROM_DUMP)
    return Qnil;

  struct emacs_reloc reloc_a = decode_emacs_reloc (NULL, lreloc_a);
  struct emacs_reloc reloc_b = decode_emacs_reloc (NULL, lreloc_b);

  eassert (reloc_a.type == RELOC_EMACS_COPY_FROM_DUMP);
  eassert (reloc_b.type == RELOC_EMACS_COPY_FROM_DUMP);

  if (reloc_a.emacs_offset + reloc_a.length != reloc_b.emacs_offset)
    return Qnil;

  if (reloc_a.u.dump_offset + reloc_a.length != reloc_b.u.dump_offset)
    return Qnil;

  dump_off new_length = reloc_a.length + reloc_b.length;
  ALLOW_IMPLICIT_CONVERSION;
  reloc_a.length = new_length;
  DISALLOW_IMPLICIT_CONVERSION;
  if (reloc_a.length != new_length)
    return Qnil; /* Overflow */

  return list4 (make_number (RELOC_EMACS_COPY_FROM_DUMP),
                dump_off_to_lisp (reloc_a.emacs_offset),
                dump_off_to_lisp (reloc_a.u.dump_offset),
                dump_off_to_lisp (reloc_a.length));
}

static void
drain_reloc_list (struct dump_context *ctx,
                  void (*handler)(struct dump_context *, Lisp_Object),
                  Lisp_Object (*merger)(Lisp_Object a, Lisp_Object b),
                  Lisp_Object *reloc_list,
                  struct dump_table_locator *out_locator)
{
  Lisp_Object relocs = Fsort (Fnreverse (*reloc_list),
                              Qdump_emacs_portable__sort_predicate);
  *reloc_list = Qnil;
  dump_align_output (ctx, sizeof (dump_off));
  struct dump_table_locator locator;
  memset (&locator, 0, sizeof (locator));
  locator.offset = ctx->offset;
  for (; !NILP (relocs); locator.nr_entries += 1)
    {
      Lisp_Object reloc = dump_pop (&relocs);
      Lisp_Object merged;
      while (merger != NULL &&
             !NILP (relocs) &&
             ((merged = merger (reloc, XCAR (relocs))), !NILP (merged)))
        {
          reloc = merged;
          relocs = XCDR (relocs);
        }
      handler (ctx, reloc);
    }
  *out_locator = locator;
}

static void
dump_do_fixups (struct dump_context *ctx)
{
  dump_off saved_offset = ctx->offset;
  Lisp_Object fixups = Fsort (Fnreverse (ctx->fixups),
                              Qdump_emacs_portable__sort_predicate);
  ctx->fixups = Qnil;
  while (!NILP (fixups))
    dump_do_fixup (ctx, dump_pop (&fixups));
  dump_seek (ctx, saved_offset);
}

DEFUN ("dump-emacs-portable",
       Fdump_emacs_portable, Sdump_emacs_portable,
       1, 2, 0,
       doc: /* Dump current state of Emacs into dump file FILENAME.
If TRACK-REFERRERS is non-nil, keep additional debugging information
that can help track down the provenance of unsupported object
types.  */)
     (Lisp_Object filename, Lisp_Object track_referrers)
{
  eassert (initialized);

  if (will_dump_with_unexec_p ())
    error ("This Emacs instance was started under the assumption "
           "that it would be dumped with unexec, not the portable "
           "dumper.  Dumping with the portable dumper may produce "
           "unexpected results.");

  if (!main_thread_p (current_thread))
    error ("Function can be called only on main thread");

  /* Clear out any detritus in memory.  */
  do {
    number_finalizers_run = 0;
    Fgarbage_collect ();
  } while (number_finalizers_run);

  ptrdiff_t count = SPECPDL_INDEX ();

  /* Bind `command-line-processed' to nil before dumping,
     so that the dumped Emacs will process its command line
     and set up to work with X windows if appropriate.  */
  Lisp_Object symbol = intern ("command-line-processed");
  specbind (symbol, Qnil);

  CHECK_STRING (filename);
  filename = Fexpand_file_name (filename, Qnil);
  filename = ENCODE_FILE (filename);

  struct dump_context ctx_buf;
  struct dump_context *ctx = &ctx_buf;
  memset (ctx, 0, sizeof (*ctx));
  ctx->fd = -1;

  ctx->objects_dumped = make_eq_hash_table ();
  dump_queue_init (&ctx->dump_queue);
  ctx->deferred_hash_tables = Qnil;
  ctx->deferred_symbols = Qnil;
  ctx->fixups = Qnil;
  ctx->symbol_aux = Qnil;
  ctx->copied_queue = Qnil;
  ctx->cold_queue = Qnil;
  ctx->dump_relocs = Qnil;
  ctx->object_starts = Qnil;
  ctx->emacs_relocs = Qnil;

  /* Ordinarily, dump_object should remember where it saw objects and
     actually write the object contents to the dump file.  In special
     circumstances below, we temporarily change this default
     behavior.  */
  ctx->flags.dump_object_contents = true;
  ctx->flags.dump_object_starts = true;

  /* We want to consolidate certain object types that we know are very likely
     to be modified.  */
  ctx->flags.defer_hash_tables = true;
  // ctx->flags.defer_symbols = true;

  ctx->current_referrer = Qnil;
  if (!NILP (track_referrers))
    ctx->referrers = make_eq_hash_table ();

  ctx->dump_filename = filename;

  record_unwind_protect_ptr (dump_unwind_cleanup, ctx);
  block_input ();

#ifdef REL_ALLOC
  r_alloc_inhibit_buffer_relocation (1);
  ctx->blocked_ralloc = true;
#endif

  ctx->old_purify_flag = Vpurify_flag;
  Vpurify_flag = Qnil;

  /* Make sure various weird things are less likely to happen.  */
  ctx->old_post_gc_hook = Vpost_gc_hook;
  Vpost_gc_hook = Qnil;

  ctx->fd = emacs_open (SSDATA (filename),
                        O_RDWR | O_TRUNC | O_CREAT, 0666);
  if (ctx->fd < 0)
    report_file_error ("Opening dump output", filename);
  verify (sizeof (ctx->header.magic) == sizeof (dump_magic));
  memcpy (&ctx->header.magic, dump_magic, sizeof (dump_magic));
  ctx->header.magic[0] = '!'; /* Note that dump is incomplete.  */

  verify (sizeof (fingerprint) == sizeof (ctx->header.fingerprint));
  memcpy (ctx->header.fingerprint, fingerprint, sizeof (fingerprint));

  const dump_off header_start = ctx->offset;
  dump_fingerprint ("dumping fingerprint", ctx->header.fingerprint);
  dump_write (ctx, &ctx->header, sizeof (ctx->header));
  const dump_off header_end = ctx->offset;

  const dump_off hot_start = ctx->offset;
  /* Start the dump process by processing the static roots and
     queuing up the objects to which they refer.   */
  dump_roots (ctx);

  dump_charset_table (ctx);
  dump_finalizer_list_head_ptr (ctx, &finalizers.prev);
  dump_finalizer_list_head_ptr (ctx, &finalizers.next);
  dump_finalizer_list_head_ptr (ctx, &doomed_finalizers.prev);
  dump_finalizer_list_head_ptr (ctx, &doomed_finalizers.next);
  dump_user_remembered_data_hot (ctx);

  /* We've already remembered all the objects to which GC roots point,
     but we have to manually save the list of GC roots itself.  */
  dump_metadata_for_pdumper (ctx);
  for (int i = 0; i < staticidx; ++i)
    dump_emacs_reloc_to_emacs_ptr_raw (ctx, &staticvec[i], staticvec[i]);
  dump_emacs_reloc_immediate_int (ctx, &staticidx, staticidx);

  /* Dump until while we keep finding objects to dump.  We add new
     objects to the queue by side effect during dumping.  */
  while (!dump_queue_empty_p (&ctx->dump_queue))
    dump_object (ctx, dump_queue_dequeue (&ctx->dump_queue, ctx->offset));
  eassert (dump_queue_empty_p (&ctx->dump_queue));

  /* We may have deferred some objects.  */
  ctx->flags.defer_hash_tables = false;
  ctx->deferred_hash_tables = Fnreverse (ctx->deferred_hash_tables);
  while (!NILP (ctx->deferred_hash_tables))
    dump_object (ctx, dump_pop (&ctx->deferred_hash_tables));
  while (!dump_queue_empty_p (&ctx->dump_queue))
    dump_object (ctx, dump_queue_dequeue (&ctx->dump_queue, ctx->offset));
  eassert (dump_queue_empty_p (&ctx->dump_queue));

  /* We may have deferred some symbols.  */
  ctx->flags.defer_symbols = false;
  ctx->deferred_symbols = Fnreverse (ctx->deferred_symbols);
  while (!NILP (ctx->deferred_symbols))
    dump_object (ctx, dump_pop (&ctx->deferred_symbols));
  while (!dump_queue_empty_p (&ctx->dump_queue))
    dump_object (ctx, dump_queue_dequeue (&ctx->dump_queue, ctx->offset));
  eassert (dump_queue_empty_p (&ctx->dump_queue));

  dump_sort_copied_objects (ctx);
  ctx->symbol_aux = make_eq_hash_table ();
  dump_hot_parts_of_discardable_objects (ctx);
  const dump_off hot_end = ctx->offset;

  /* Emacs, after initial dump loading, can forget about the portion
     of the dump that runs from here to the start of the cold section.
     This section consists of objects that need to be memcpy()ed into
     the Emacs data section instead of just used directly.  */
  ctx->header.discardable_start = ctx->offset;
  ctx->flags.dump_object_starts = false;

  dump_copied_objects (ctx);
  eassert (dump_queue_empty_p (&ctx->dump_queue));
  eassert (NILP (ctx->copied_queue));
  dump_off discardable_end = ctx->offset;
  dump_align_output (ctx, dump_get_page_size ());
  ctx->header.cold_start = ctx->offset;

  /* Resume recording object starts, since the cold section will stick
     around.  */
  ctx->flags.dump_object_starts = true;

  /* Start the cold section.  This section contains bytes that should
     never change and so can be direct-mapped from the dump without
     special processing.  */
  dump_cold_data (ctx);
   /* dump_user_remembered_data_cold needs to be after dump_cold_data
      in case dump_cold_data dumps a lisp object to which C code
      points.  dump_user_remembered_data_cold assumes that all lisp
      objects have been dumped.  */
  dump_user_remembered_data_cold (ctx);

  /* After this point, the dump file contains no data that can be part
     of the Lisp heap.  */
  ctx->end_heap = ctx->offset;

  /* Make remembered modifications to the dump file itself.  */
  dump_do_fixups (ctx);

  /* Emit instructions for Emacs to execute when loading the dump.
     Note that this relocation information ends up in the cold section
     of the dump.  */
  drain_reloc_list (
    ctx, dump_emit_dump_reloc, NULL,
    &ctx->dump_relocs,
    &ctx->header.dump_relocs);
  unsigned number_hot_relocations = ctx->number_hot_relocations;
  ctx->number_hot_relocations = 0;
  unsigned number_discardable_relocations = ctx->number_discardable_relocations;
  ctx->number_discardable_relocations = 0;
  drain_reloc_list (
    ctx, dump_emit_dump_reloc, NULL,
    &ctx->object_starts,
    &ctx->header.object_starts);
  drain_reloc_list (
    ctx, dump_emit_emacs_reloc, dump_merge_emacs_relocs,
    &ctx->emacs_relocs,
    &ctx->header.emacs_relocs);

  const dump_off cold_end = ctx->offset;

  eassert (dump_queue_empty_p (&ctx->dump_queue));
  eassert (NILP (ctx->fixups));
  eassert (NILP (ctx->dump_relocs));
  eassert (NILP (ctx->emacs_relocs));

  /* Dump is complete.  Go back to the header and write the magic
     indicating that the dump is complete and can be loaded.  */
  ctx->header.magic[0] = dump_magic[0];
  dump_seek (ctx, 0);
  dump_write (ctx, &ctx->header, sizeof (ctx->header));

  fprintf (stderr, "Dump complete\n");
  fprintf (stderr,
           "Byte counts: header=%lu hot=%lu discardable=%lu cold=%lu\n",
           (unsigned long) (header_end - header_start),
           (unsigned long) (hot_end - hot_start),
           (unsigned long) (discardable_end - ctx->header.discardable_start),
           (unsigned long) (cold_end - ctx->header.cold_start));
  fprintf (stderr, "Reloc counts: hot=%u discardable=%u\n",
           number_hot_relocations,
           number_discardable_relocations);

  return unbind_to (count, Qnil);


}

DEFUN ("dump-emacs-portable--sort-predicate",
       Fdump_emacs_portable__sort_predicate,
       Sdump_emacs_portable__sort_predicate,
       2, 2, 0,
       doc: /* Internal relocation sorting function.  */)
     (Lisp_Object a, Lisp_Object b)
{
  dump_off a_offset = dump_off_from_lisp (XCAR (XCDR (a)));
  dump_off b_offset = dump_off_from_lisp (XCAR (XCDR (b)));
  return a_offset < b_offset ? Qt : Qnil;
}

DEFUN ("dump-emacs-portable--sort-predicate-copied",
       Fdump_emacs_portable__sort_predicate_copied,
       Sdump_emacs_portable__sort_predicate_copied,
       2, 2, 0,
       doc: /* Internal relocation sorting function.  */)
     (Lisp_Object a, Lisp_Object b)
{
  eassert (dump_object_emacs_ptr (a));
  eassert (dump_object_emacs_ptr (b));
  return dump_object_emacs_ptr (a) < dump_object_emacs_ptr (b) ? Qt : Qnil;
}

void
pdumper_do_now_and_after_load_impl (pdumper_hook hook)
{
  if (nr_dump_hooks == ARRAYELTS (dump_hooks))
    fatal ("out of dump hooks: make dump_hooks[] bigger");
  dump_hooks[nr_dump_hooks++] = hook;
  hook ();
}

static void
pdumper_remember_user_data_1 (void *mem, int nbytes)
{
  if (nr_remembered_data == ARRAYELTS (remembered_data))
    fatal ("out of remembered data slots: make remembered_data[] bigger");
  remembered_data[nr_remembered_data].mem = mem;
  remembered_data[nr_remembered_data].sz = nbytes;
  nr_remembered_data += 1;
}

void
pdumper_remember_scalar_impl (void *mem, ptrdiff_t nbytes)
{
  eassert (0 <= nbytes && nbytes <= INT_MAX);
  if (nbytes > 0)
    pdumper_remember_user_data_1 (mem, (int) nbytes);
}

void
pdumper_remember_lv_ptr_raw_impl (void* ptr, enum Lisp_Type type)
{
  pdumper_remember_user_data_1 (ptr, -type);
}


/* Dump runtime */
enum dump_memory_protection {
  DUMP_MEMORY_ACCESS_NONE = 1,
  DUMP_MEMORY_ACCESS_READ = 2,
  DUMP_MEMORY_ACCESS_READWRITE = 3,
};

static void *
dump_anonymous_allocate_w32 (void *base,
                             size_t size,
                             enum dump_memory_protection protection)
{
#if VM_SUPPORTED != VM_MS_WINDOWS
  (void) base;
  (void) size;
  (void) protection;
  emacs_abort ();
#else
  void *ret;
  DWORD mem_type;
  DWORD mem_prot;

  switch (protection)
    {
    case DUMP_MEMORY_ACCESS_NONE:
      mem_type = MEM_RESERVE;
      mem_prot = PAGE_NOACCESS;
      break;
    case DUMP_MEMORY_ACCESS_READ:
      mem_type = MEM_COMMIT;
      mem_prot = PAGE_READONLY;
      break;
    case DUMP_MEMORY_ACCESS_READWRITE:
      mem_type = MEM_COMMIT;
      mem_prot = PAGE_READWRITE;
      break;
    default:
      emacs_abort ();
    }

  ret = VirtualAlloc (base, size, mem_type, mem_prot);
  if (ret == NULL)
    errno = (base && GetLastError () == ERROR_INVALID_ADDRESS)
      ? EBUSY
      : EPERM;
  return ret;
#endif
}

static void *
dump_anonymous_allocate_posix (void *base,
                               size_t size,
                               enum dump_memory_protection protection)
{
#if VM_SUPPORTED != VM_POSIX
  (void) base;
  (void) size;
  (void) protection;
  emacs_abort ();
#else
  void *ret;
  int mem_prot;

  switch (protection)
    {
    case DUMP_MEMORY_ACCESS_NONE:
      mem_prot = PROT_NONE;
      break;
    case DUMP_MEMORY_ACCESS_READ:
      mem_prot = PROT_READ;
      break;
    case DUMP_MEMORY_ACCESS_READWRITE:
      mem_prot = PROT_READ | PROT_WRITE;
      break;
    default:
      emacs_abort ();
    }

  int mem_flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (mem_prot != PROT_NONE)
    mem_flags |= MAP_POPULATE;
  if (base)
    mem_flags |= MAP_FIXED;

  bool retry;
  do
    {
      retry = false;
      ret = mmap (base, size, mem_prot, mem_flags, -1, 0);
      if (ret == MAP_FAILED &&
          errno == EINVAL &&
          (mem_flags & MAP_POPULATE))
        {
          /* This system didn't understand MAP_POPULATE, so try
             again without it.  */
          mem_flags &= ~MAP_POPULATE;
          retry = true;
        }
    }
  while (retry);

  if (ret == MAP_FAILED)
    ret = NULL;
  return ret;
#endif
}

/* Perform anonymous memory allocation.  */
static void *
dump_anonymous_allocate (void *base,
                         const size_t size,
                         enum dump_memory_protection protection)
{
  void *ret = NULL;
  if (VM_SUPPORTED == VM_MS_WINDOWS)
    ret = dump_anonymous_allocate_w32 (base, size, protection);
  else if (VM_SUPPORTED == VM_POSIX)
    ret = dump_anonymous_allocate_posix (base, size, protection);
  else
    errno = ENOSYS;
  return ret;
}

/* Undo the effect of dump_reserve_address_space().  */
static void
dump_anonymous_release (void *addr, size_t size)
{
  eassert (size >= 0);
#if VM_SUPPORTED == VM_MS_WINDOWS
  (void) size;
  if (!VirtualFree (addr, 0, MEM_RELEASE))
    emacs_abort ();
#elif VM_SUPPORTED == VM_POSIX
  if (munmap (addr, size) < 0)
    emacs_abort ();
#else
  (void) addr;
  (void) size;
  emacs_abort ();
#endif
}

static void *
dump_map_file_w32 (
  void *base,
  int fd,
  off_t offset,
  size_t size,
  enum dump_memory_protection protection)
{
#if VM_SUPPORTED != VM_MS_WINDOWS
  (void) base;
  (void) fd;
  (void) offset;
  (void) size;
  (void) protection;
  emacs_abort ();
#else
  void *ret = NULL;
  HANDLE section = NULL;
  HANDLE file;

  uint64_t full_offset = offset;
  uint32_t offset_high = (uint32_t) (full_offset >> 32);
  uint32_t offset_low = (uint32_t) (full_offset & 0xffffffff);

  int error;
  DWORD map_access;

  file = (HANDLE) _get_osfhandle (fd);
  if (file == INVALID_HANDLE_VALUE)
    goto out;

  section = CreateFileMapping (
    file,
    /*lpAttributes=*/NULL,
    PAGE_READONLY,
    /*dwMaximumSizeHigh=*/0,
    /*dwMaximumSizeLow=*/0,
    /*lpName=*/NULL);
  if (!section)
    {
      errno = EINVAL;
      goto out;
    }

  switch (protection)
    {
    case DUMP_MEMORY_ACCESS_NONE:
    case DUMP_MEMORY_ACCESS_READ:
      map_access = FILE_MAP_READ;
      break;
    case DUMP_MEMORY_ACCESS_READWRITE:
      map_access = FILE_MAP_COPY;
      break;
    default:
      emacs_abort ();
    }

  ret = MapViewOfFileEx (section,
                         map_access,
                         offset_high,
                         offset_low,
                         size,
                         base);

  error = GetLastError ();
  if (ret == NULL)
    errno = (error == ERROR_INVALID_ADDRESS ? EBUSY : EPERM);
 out:
  if (section && !CloseHandle (section))
    emacs_abort ();
  return ret;
#endif
}

static void *
dump_map_file_posix (
  void *base,
  int fd,
  off_t offset,
  size_t size,
  enum dump_memory_protection protection)
{
#if VM_SUPPORTED != VM_POSIX
  (void) base;
  (void) fd;
  (void) offset;
  (void) size;
  (void) protection;
  emacs_abort ();
#else
  void *ret;
  int mem_prot;
  int mem_flags;

  switch (protection)
    {
    case DUMP_MEMORY_ACCESS_NONE:
      mem_prot = PROT_NONE;
      mem_flags = MAP_SHARED;
      break;
    case DUMP_MEMORY_ACCESS_READ:
      mem_prot = PROT_READ;
      mem_flags = MAP_SHARED;
      break;
    case DUMP_MEMORY_ACCESS_READWRITE:
      mem_prot = PROT_READ | PROT_WRITE;
      mem_flags = MAP_PRIVATE;
      break;
    default:
      emacs_abort ();
    }

  if (base)
    mem_flags |= MAP_FIXED;

  ret = mmap (base, size, mem_prot, mem_flags, fd, offset);
  if (ret == MAP_FAILED)
    ret = NULL;
  return ret;
#endif
}

/* Map a file into memory.  */
static void *
dump_map_file (
  void *base,
  int fd,
  off_t offset,
  size_t size,
  enum dump_memory_protection protection)
{
  void *ret = NULL;
  if (VM_SUPPORTED == VM_MS_WINDOWS)
    ret = dump_map_file_w32 (base, fd, offset, size, protection);
  else if (VM_SUPPORTED == VM_POSIX)
    ret = dump_map_file_posix (base, fd, offset, size, protection);
  else
    errno = ENOSYS;
  return ret;
}

/* Remove a virtual memory mapping.

   On failure, abort Emacs.  For maximum platform compatibility, ADDR
   and SIZE must match the mapping exactly.  */
static void
dump_unmap_file (void *addr, size_t size)
{
  eassert (size >= 0);
#if !VM_SUPPORTED
  (void) addr;
  (void) size;
  emacs_abort ();
#elif defined (WINDOWSNT)
  (void) size;
  if (!UnmapViewOfFile (addr))
    emacs_abort ();
#else
  if (munmap (addr, size) < 0)
    emacs_abort ();
#endif
}

struct dump_memory_map_spec
{
  int fd;  /* File to map; anon zero if negative.  */
  size_t size;  /* Number of bytes to map.  */
  off_t offset;  /* Offset within fd.  */
  enum dump_memory_protection protection;
};

struct dump_memory_map {
  struct dump_memory_map_spec spec;
  void *mapping;  /* Actual mapped memory.  */
  void (*release)(struct dump_memory_map *);
  void *private;
};

/* Mark the pages as unneeded, potentially zeroing them, without
   releasing the address space reservation.  */
static void
dump_discard_mem (void *mem, size_t size)
{
#if VM_SUPPORTED == VM_MS_WINDOWS
      /* Discard COWed pages.  */
      (void) VirtualFree (mem, size, MEM_DECOMMIT);
      /* Release the commit charge for the mapping.  */
      (void) VirtualProtect (mem, size, PAGE_NOACCESS, NULL);
#elif VM_SUPPORTED == VM_POSIX
# ifdef HAVE_POSIX_MADVISE
      /* Discard COWed pages.  */
      (void) posix_madvise (mem, size, POSIX_MADV_DONTNEED);
# endif
      /* Release the commit charge for the mapping.  */
      (void) mprotect (mem, size, PROT_NONE);
#endif
}

static void
dump_mmap_discard_contents (struct dump_memory_map *map)
{
  if (map->mapping)
    dump_discard_mem (map->mapping, map->spec.size);
}

static void
dump_mmap_reset (struct dump_memory_map *map)
{
  map->mapping = NULL;
  map->release = NULL;
  map->private = NULL;
}

static void
dump_mmap_release (struct dump_memory_map *map)
{
  if (map->release)
    map->release (map);
  dump_mmap_reset (map);
}

/* Allows heap-allocated dump_mmap to "free" maps individually.  */
struct dump_memory_map_heap_control_block {
  int refcount;
  void *mem;
};

static void
dump_mm_heap_cb_release (struct dump_memory_map_heap_control_block *cb)
{
  eassert (cb->refcount > 0);
  if (--cb->refcount == 0)
    {
      free (cb->mem);
      free (cb);
    }
}

static void
dump_mmap_release_heap (struct dump_memory_map *map)
{
  struct dump_memory_map_heap_control_block *cb = map->private;
  dump_mm_heap_cb_release (cb);
}

/* Implement dump_mmap using malloc and read.  */
static bool
dump_mmap_contiguous_heap (
  struct dump_memory_map *maps,
  int nr_maps,
  size_t total_size)
{
  bool ret = false;
  struct dump_memory_map_heap_control_block *cb = calloc (1, sizeof (*cb));
  char *mem;
  if (!cb)
    goto out;
  cb->refcount = 1;
  cb->mem = malloc (total_size);
  if (!cb->mem)
    goto out;
  mem = cb->mem;
  for (int i = 0; i < nr_maps; ++i)
    {
      struct dump_memory_map *map = &maps[i];
      const struct dump_memory_map_spec spec = map->spec;
      if (!spec.size)
        continue;
      map->mapping = mem;
      mem += spec.size;
      map->release = dump_mmap_release_heap;
      map->private = cb;
      cb->refcount += 1;
      if (spec.fd < 0)
        memset (map->mapping, 0, spec.size);
      else
        {
          if (lseek (spec.fd, spec.offset, SEEK_SET) < 0)
            goto out;
          ssize_t nb = dump_read_all (spec.fd,
                                      map->mapping,
                                      spec.size);
          if (nb >= 0 && nb != spec.size)
            errno = EIO;
          if (nb != spec.size)
            goto out;
        }
    }

  ret = true;
 out:
  dump_mm_heap_cb_release (cb);
  if (!ret)
    for (int i = 0; i < nr_maps; ++i)
      dump_mmap_release (&maps[i]);
  return ret;
}

static void
dump_mmap_release_vm (struct dump_memory_map *map)
{
  if (map->spec.fd < 0)
    dump_anonymous_release (map->mapping, map->spec.size);
  else
    dump_unmap_file (map->mapping, map->spec.size);
}

static bool
dump_mmap_contiguous_vm (
  struct dump_memory_map *maps,
  int nr_maps,
  size_t total_size)
{
  bool ret = false;
  void *resv = NULL;
  bool retry = false;

  do
    {
      if (retry)
        {
          eassert (VM_SUPPORTED == VM_MS_WINDOWS);
          retry = false;
          for (int i = 0; i < nr_maps; ++i)
            dump_mmap_release (&maps[i]);
        }

      eassert (resv == NULL);
      resv = dump_anonymous_allocate (NULL,
                                      total_size,
                                      DUMP_MEMORY_ACCESS_NONE);
      if (!resv)
        goto out;

      char *mem = resv;

      if (VM_SUPPORTED == VM_MS_WINDOWS)
        {
          /* Windows lacks atomic mapping replace; need to release the
             reservation so we can allocate within it.  Will retry the
             loop if someone squats on our address space before we can
             finish allocation.  On POSIX systems, we leave the
             reservation around for atomicity.  */
          dump_anonymous_release (resv, total_size);
          resv = NULL;
        }

      for (int i = 0; i < nr_maps; ++i)
        {
          struct dump_memory_map *map = &maps[i];
          const struct dump_memory_map_spec spec = map->spec;
          if (!spec.size)
            continue;

          if (spec.fd < 0)
            map->mapping = dump_anonymous_allocate (
              mem, spec.size, spec.protection);
          else
            map->mapping = dump_map_file (
              mem, spec.fd, spec.offset, spec.size, spec.protection);
          mem += spec.size;
          if (VM_SUPPORTED == VM_MS_WINDOWS &&
              map->mapping == NULL &&
              errno == EBUSY)
            {
              retry = true;
              continue;
            }
          if (map->mapping == NULL)
            goto out;
          map->release = dump_mmap_release_vm;
        }
    }
  while (retry);

  ret = true;
  resv = NULL;
 out:
  if (resv)
    dump_anonymous_release (resv, total_size);
  if (!ret)
    {
      for (int i = 0; i < nr_maps; ++i)
	{
	  if (VM_SUPPORTED == VM_MS_WINDOWS)
	    dump_mmap_reset (&maps[i]);
	  else
	    dump_mmap_release (&maps[i]);
	}
    }
  return ret;
}

/* Map a range of addresses into a chunk of contiguous memory.

   Each dump_memory_map structure describes how to fill the
   corresponding range of memory. On input, all members except MAPPING
   are valid. On output, MAPPING contains the location of the given
   chunk of memory. The MAPPING for MAPS[N] is MAPS[N-1].mapping +
   MAPS[N-1].size.

   Each mapping SIZE must be a multiple of the system page size except
   for the last mapping.

   Return true on success or false on failure with errno set.  */
static bool
dump_mmap_contiguous (
  struct dump_memory_map *maps,
  int nr_maps)
{
  if (!nr_maps)
    return true;

  size_t total_size = 0;
  int worst_case_page_size = dump_get_page_size ();

  for (int i = 0; i < nr_maps; ++i)
    {
      eassert (maps[i].mapping == NULL);
      eassert (maps[i].release == NULL);
      eassert (maps[i].private == NULL);
      if (i != nr_maps - 1)
        eassert (maps[i].spec.size % worst_case_page_size == 0);
      total_size += maps[i].spec.size;
    }

  return (VM_SUPPORTED ?
          dump_mmap_contiguous_vm :
          dump_mmap_contiguous_heap)
    (maps, nr_maps, total_size);
}

typedef uint_fast32_t dump_bitset_word;

struct dump_bitset {
  dump_bitset_word *restrict bits;
  ptrdiff_t number_words;
};

static bool
dump_bitset_init (struct dump_bitset *bitset, size_t number_bits)
{
  memset (bitset, 0, sizeof (*bitset));
  int xword_size = sizeof (bitset->bits[0]);
  int bits_per_word = xword_size * CHAR_BIT;
  ptrdiff_t words_needed = DIVIDE_ROUND_UP (number_bits, bits_per_word);
  bitset->number_words = words_needed;
  bitset->bits = calloc (words_needed, xword_size);
  return bitset->bits != NULL;
}

static void
dump_bitset_destroy (struct dump_bitset *bitset)
{
  free (bitset->bits);
}

static dump_bitset_word *
dump_bitset__bit_slot (const struct dump_bitset *bitset,
                       size_t bit_number)
{
  int xword_size = sizeof (bitset->bits[0]);
  int bits_per_word = xword_size * CHAR_BIT;
  ptrdiff_t word_number = bit_number / bits_per_word;
  eassert (word_number < bitset->number_words);
  return &bitset->bits[word_number];
}

static bool
dump_bitset_bit_set_p (const struct dump_bitset *bitset,
                       size_t bit_number)
{
  unsigned xword_size = sizeof (bitset->bits[0]);
  unsigned bits_per_word = xword_size * CHAR_BIT;
  dump_bitset_word bit = 1;
  bit <<= bit_number % bits_per_word;
  return *dump_bitset__bit_slot (bitset, bit_number) & bit;
}

static void
dump_bitset__set_bit_value (struct dump_bitset *bitset,
                            size_t bit_number,
                            bool bit_is_set)
{
  int xword_size = sizeof (bitset->bits[0]);
  int bits_per_word = xword_size * CHAR_BIT;
  dump_bitset_word * slot = dump_bitset__bit_slot (bitset, bit_number);
  dump_bitset_word bit = 1;
  bit <<= bit_number % bits_per_word;
  if (bit_is_set)
    *slot = *slot | bit;
  else
    *slot = *slot & ~bit;
}

static void
dump_bitset_set_bit (struct dump_bitset *bitset, size_t bit_number)
{
  dump_bitset__set_bit_value (bitset, bit_number, true);
}

static void
dump_bitset_clear (struct dump_bitset *bitset)
{
  int xword_size = sizeof (bitset->bits[0]);
  memset (bitset->bits, 0, bitset->number_words * xword_size);
}

struct pdumper_loaded_dump_private
{
  /* Copy of the header we read from the dump.  */
  struct dump_header header;
  /* Mark bits for objects in the dump; used during GC.  */
  struct dump_bitset mark_bits;
};

struct pdumper_loaded_dump dump_public;
struct pdumper_loaded_dump_private dump_private;

/* Return a pointer to offset OFFSET within the dump, which begins at
   DUMP_BASE. DUMP_BASE must be equal to the current dump load
   location; it's passed as a parameter for efficiency.

   The returned pointer points to the primary memory image of the
   currently-loaded dump file.  The entire dump file is accessible
   using this function.  */
static void *
dump_ptr (intptr_t dump_base, dump_off offset)
{
  eassert (dump_base == dump_public.start);
  eassert (dump_public.start + offset < dump_public.end);
  return (char *)dump_public.start + offset;
}

/* Read a pointer-sized word of memory at OFFSET within the dump,
   which begins at DUMP_BASE. DUMP_BASE must be equal to the current
   dump load location; it's passed as a parameter for efficiency.  */
static uintptr_t
dump_read_word_from_dump (intptr_t dump_base, dump_off offset)
{
  uintptr_t value;
  /* The compiler optimizes this memcpy into a read.  */
  memcpy (&value, dump_ptr (dump_base, offset), sizeof (value));
  return value;
}

/* Write a word to the dump. DUMP_BASE and OFFSET are as for
   dump_read_word_from_dump; VALUE is the word to write at the given
   offset.  */
static void
dump_write_word_to_dump (intptr_t dump_base,
                         dump_off offset,
                         uintptr_t value)
{
  /* The compiler optimizes this memcpy into a write.  */
  memcpy (dump_ptr (dump_base, offset), &value, sizeof (value));
}

/* Write a Lisp_Object to the dump. DUMP_BASE and OFFSET are as for
   dump_read_word_from_dump; VALUE is the Lisp_Object to write at the
   given offset.  */
static void
dump_write_lv_to_dump (intptr_t dump_base,
                       dump_off offset,
                       Lisp_Object value)
{
  /* The compiler optimizes this memcpy into a write.  */
  memcpy (dump_ptr (dump_base, offset), &value, sizeof (value));
}

/* Search for a relocation given a relocation target.

   DUMP is the dump metadata structure.  TABLE is the relocation table
   to search.  KEY is the dump offset to find.  Return the relocation
   RELOC such that RELOC.offset is the smallest RELOC.offset that
   satisfies the constraint KEY <= RELOC.offset --- that is, return
   the first relocation at KEY or after KEY.  Return NULL if no such
   relocation exists.  */
static const struct dump_reloc *
dump_find_relocation (const struct dump_table_locator *const table,
                      const dump_off key)
{
  const struct dump_reloc *const relocs = dump_ptr (
    dump_public.start, table->offset);
  const struct dump_reloc *found = NULL;
  ptrdiff_t idx_left = 0;
  ptrdiff_t idx_right = table->nr_entries;

  eassert (key >= 0);

  while (idx_left < idx_right)
    {
      const ptrdiff_t idx_mid = idx_left + (idx_right - idx_left) / 2;
      const struct dump_reloc *mid = &relocs[idx_mid];
      if (key > dump_reloc_get_offset (*mid))
        idx_left = idx_mid + 1;
      else
        {
          found = mid;
          idx_right = idx_mid;
          if (idx_right <= idx_left ||
              key > dump_reloc_get_offset (relocs[idx_right - 1]))
            break;
        }
   }

  return found;
}

static bool
dump_loaded_p (void)
{
  return dump_public.start != 0;
}

bool
pdumper_cold_object_p_impl (const void *obj)
{
  eassert (pdumper_object_p (obj));
  eassert (pdumper_object_p_precise (obj));
  dump_off offset = ptrdiff_t_to_dump_off (
    (intptr_t) obj - dump_public.start);
  return offset >= dump_private.header.cold_start;
}

enum Lisp_Type
pdumper_find_object_type_impl (const void *obj)
{
  eassert (pdumper_object_p (obj));
  dump_off offset = ptrdiff_t_to_dump_off (
    (intptr_t) obj - dump_public.start);
  if (offset % GCALIGNMENT != 0)
    return PDUMPER_NO_OBJECT;
  const struct dump_reloc *reloc =
    dump_find_relocation (&dump_private.header.object_starts, offset);
  return (reloc != NULL && dump_reloc_get_offset (*reloc) == offset)
    ? (enum Lisp_Type) reloc->type
    : PDUMPER_NO_OBJECT;
}

bool
pdumper_marked_p_impl (const void *obj)
{
  eassert (pdumper_object_p (obj));
  ptrdiff_t offset = (intptr_t) obj - dump_public.start;
  eassert (offset % GCALIGNMENT == 0);
  eassert (offset < dump_private.header.cold_start);
  eassert (offset < dump_private.header.discardable_start);
  ptrdiff_t bitno = offset / GCALIGNMENT;
  return dump_bitset_bit_set_p (&dump_private.mark_bits, bitno);
}

void
pdumper_set_marked_impl (const void *obj)
{
  eassert (pdumper_object_p (obj));
  ptrdiff_t offset = (intptr_t) obj - dump_public.start;
  eassert (offset % GCALIGNMENT == 0);
  eassert (offset < dump_private.header.cold_start);
  eassert (offset < dump_private.header.discardable_start);
  ptrdiff_t bitno = offset / GCALIGNMENT;
  dump_bitset_set_bit (&dump_private.mark_bits, bitno);
}

void
pdumper_clear_marks_impl (void)
{
  dump_bitset_clear (&dump_private.mark_bits);
}

static ssize_t
dump_read_all (int fd, void *buf, size_t bytes_to_read)
{
  /* We don't want to use emacs_read, since that relies on the lisp
     world, and we're not in the lisp world yet.  */
  eassert (bytes_to_read <= SSIZE_MAX);
  size_t bytes_read = 0;
  while (bytes_read < bytes_to_read)
    {
      ssize_t chunk =
        read (fd, (char*) buf + bytes_read, bytes_to_read - bytes_read);
      if (chunk < 0)
        return chunk;
      if (chunk == 0)
        break;
      bytes_read += chunk;
    }

  return bytes_read;
}

static void *
emacs_ptr (const ptrdiff_t offset)
{
  // TODO: assert somehow that offset is actually inside Emacs
  return (void *) (emacs_basis () + offset);
}

/* Return the number of bytes written when we perform the given
   relocation.  */
static int
dump_reloc_size (const struct dump_reloc reloc)
{
  if (sizeof (Lisp_Object) == sizeof (void*))
    return sizeof (Lisp_Object);
  if (reloc.type == RELOC_DUMP_TO_EMACS_PTR_RAW ||
      reloc.type == RELOC_DUMP_TO_DUMP_PTR_RAW)
    return sizeof (void*);
  return sizeof (Lisp_Object);
}

static Lisp_Object
dump_make_lv_from_reloc (
  const intptr_t dump_base,
  const struct dump_reloc reloc)
{
  const dump_off reloc_offset = dump_reloc_get_offset (reloc);
  uintptr_t value = dump_read_word_from_dump (dump_base, reloc_offset);
  enum Lisp_Type lisp_type;

  if (RELOC_DUMP_TO_DUMP_LV <= reloc.type &&
      reloc.type < RELOC_DUMP_TO_EMACS_LV)
    {
      lisp_type = reloc.type - RELOC_DUMP_TO_DUMP_LV;
      value += dump_base;
    }
  else
    {
      eassert (RELOC_DUMP_TO_EMACS_LV <= reloc.type);
      eassert (reloc.type < RELOC_DUMP_TO_EMACS_LV + 8);
      lisp_type = reloc.type - RELOC_DUMP_TO_EMACS_LV;
      value += emacs_basis ();
    }

  eassert (lisp_type != Lisp_Int0 && lisp_type != Lisp_Int1);

  Lisp_Object lv;
  if (lisp_type == Lisp_Symbol)
    lv = make_lisp_symbol ((void *) value);
  else
    lv = make_lisp_ptr ((void *) value, lisp_type);

  return lv;
}

/* Actually apply a dump relocation.  */
static INLINE void
dump_do_dump_relocation (
  const intptr_t dump_base,
  const struct dump_reloc reloc)
{
  const dump_off reloc_offset = dump_reloc_get_offset (reloc);

  /* We should never generate a relocation in the cold section.  */
  eassert (reloc_offset < dump_private.header.cold_start);

  switch (reloc.type)
    {
    case RELOC_DUMP_TO_EMACS_PTR_RAW:
      {
        uintptr_t value = dump_read_word_from_dump (dump_base, reloc_offset);
        eassert (dump_reloc_size (reloc) == sizeof (value));
        value += emacs_basis ();
        dump_write_word_to_dump (dump_base, reloc_offset, value);
        break;
      }
    case RELOC_DUMP_TO_DUMP_PTR_RAW:
      {
        uintptr_t value = dump_read_word_from_dump (dump_base, reloc_offset);
        eassert (dump_reloc_size (reloc) == sizeof (value));
        value += dump_base;
        dump_write_word_to_dump (dump_base, reloc_offset, value);
        break;
      }
    default: /* Lisp_Object in the dump; precise type in reloc.type */
      {
        Lisp_Object lv = dump_make_lv_from_reloc (dump_base, reloc);
        eassert (dump_reloc_size (reloc) == sizeof (lv));
        dump_write_lv_to_dump (dump_base, reloc_offset, lv);
        break;
      }
    }
}

static void
dump_do_all_dump_relocations (
  const struct dump_header *const header,
  const intptr_t dump_base)
{
  struct dump_reloc *r = dump_ptr (dump_base, header->dump_relocs.offset);
  dump_off nr_entries = header->dump_relocs.nr_entries;
  for (dump_off i = 0; i < nr_entries; ++i)
    dump_do_dump_relocation (dump_base, r[i]);
}

static void
dump_do_emacs_relocation (
  const intptr_t dump_base,
  const struct emacs_reloc reloc)
{
  ptrdiff_t pval;
  Lisp_Object lv;

  switch (reloc.type)
    {
    case RELOC_EMACS_COPY_FROM_DUMP:
      eassume (reloc.length > 0);
      memcpy (emacs_ptr (reloc.emacs_offset),
              dump_ptr (dump_base, reloc.u.dump_offset),
              reloc.length);
      break;
    case RELOC_EMACS_IMMEDIATE:
      eassume (reloc.length > 0);
      eassume (reloc.length <= sizeof (reloc.u.immediate));
      memcpy (emacs_ptr (reloc.emacs_offset),
              &reloc.u.immediate,
              reloc.length);
      break;
    case RELOC_EMACS_DUMP_PTR_RAW:
      pval = reloc.u.dump_offset + dump_base;
      memcpy (emacs_ptr (reloc.emacs_offset), &pval, sizeof (pval));
      break;
    case RELOC_EMACS_EMACS_PTR_RAW:
      pval = reloc.u.emacs_offset2 + emacs_basis ();
      memcpy (emacs_ptr (reloc.emacs_offset), &pval, sizeof (pval));
      break;
    case RELOC_EMACS_DUMP_LV:
      eassume (reloc.length <= Lisp_Float);
      if (reloc.length == Lisp_Symbol)
        lv = make_lisp_symbol (dump_ptr (dump_base, reloc.u.dump_offset));
      else
        lv = make_lisp_ptr (dump_ptr (dump_base, reloc.u.dump_offset),
                            reloc.length);
      memcpy (emacs_ptr (reloc.emacs_offset), &lv, sizeof (lv));
      break;
    default:
      fatal ("unrecognied relocation type %d", (int) reloc.type);
    }
}

static void
dump_do_all_emacs_relocations (
  const struct dump_header *const header,
  const intptr_t dump_base)
{
  const dump_off nr_entries = header->emacs_relocs.nr_entries;
  struct emacs_reloc *r = dump_ptr (dump_base, header->emacs_relocs.offset);
  for (dump_off i = 0; i < nr_entries; ++i)
    dump_do_emacs_relocation (dump_base, r[i]);
}

enum dump_section
  {
   DS_HOT,
   DS_DISCARDABLE,
   DS_COLD,
   NUMBER_DUMP_SECTIONS,
  };

/* Load a dump from DUMP_FILENAME.  Return an error code.

   N.B. We run very early in initialization, so we can't use lisp,
   unwinding, xmalloc, and so on.  */
enum pdumper_load_result
pdumper_load (const char *dump_filename)
{
  enum pdumper_load_result err = PDUMPER_LOAD_ERROR;

  int dump_fd = -1;
  intptr_t dump_size;
  struct stat stat;
  intptr_t dump_base;
  int dump_page_size;
  dump_off adj_discardable_start;

  struct dump_bitset mark_bits;
  bool free_mark_bits = false;
  size_t mark_bits_needed;

  struct dump_header header_buf;
  struct dump_header *header = &header_buf;
  struct dump_memory_map sections[NUMBER_DUMP_SECTIONS];

  memset (&header_buf, 0, sizeof (header_buf));
  memset (&sections, 0, sizeof (sections));

  /* Overwriting an initialized Lisp universe will not go well.  */
  eassert (!initialized);

  /* We can load only one dump.  */
  eassert (!dump_loaded_p ());

  err = PDUMPER_LOAD_FILE_NOT_FOUND;
  dump_fd = emacs_open (dump_filename, O_RDONLY, 0);
  if (dump_fd < 0)
    goto out;

  err = PDUMPER_LOAD_FILE_NOT_FOUND;
  if (fstat (dump_fd, &stat) < 0)
    goto out;

  err = PDUMPER_LOAD_BAD_FILE_TYPE;
  if (stat.st_size > INTPTR_MAX)
    goto out;
  dump_size = (intptr_t) stat.st_size;

  err = PDUMPER_LOAD_BAD_FILE_TYPE;
  if (dump_size < sizeof (*header))
    goto out;

  err = PDUMPER_LOAD_BAD_FILE_TYPE;
  if (dump_read_all (dump_fd,
                     header,
                     sizeof (*header)) < sizeof (*header))
    goto out;

  err = PDUMPER_LOAD_BAD_FILE_TYPE;
  if (memcmp (header->magic, dump_magic, sizeof (dump_magic)) != 0)
    goto out;

  err = PDUMPER_LOAD_VERSION_MISMATCH;
  verify (sizeof (header->fingerprint) == sizeof (fingerprint));
  if (memcmp (header->fingerprint, fingerprint, sizeof (fingerprint)) != 0)
    {
      dump_fingerprint ("desired fingerprint", fingerprint);
      dump_fingerprint ("found fingerprint", header->fingerprint);
      goto out;
    }

  err = PDUMPER_LOAD_OOM;

  adj_discardable_start = header->discardable_start;
  dump_page_size = dump_get_page_size ();
  /* Snap to next page boundary.  */
  adj_discardable_start = ROUNDUP (
    adj_discardable_start,
    dump_page_size);
  eassert (adj_discardable_start % dump_page_size == 0);
  eassert (adj_discardable_start <= header->cold_start);

  sections[DS_HOT].spec = (struct dump_memory_map_spec)
    {
     .fd = dump_fd,
     .size = adj_discardable_start,
     .offset = 0,
     .protection = DUMP_MEMORY_ACCESS_READWRITE,
    };

  sections[DS_DISCARDABLE].spec = (struct dump_memory_map_spec)
    {
     .fd = dump_fd,
     .size = header->cold_start - adj_discardable_start,
     .offset = adj_discardable_start,
     .protection = DUMP_MEMORY_ACCESS_READWRITE,
    };

  sections[DS_COLD].spec = (struct dump_memory_map_spec)
    {
     .fd = dump_fd,
     .size = dump_size - header->cold_start,
     .offset = header->cold_start,
     .protection = DUMP_MEMORY_ACCESS_READWRITE,
    };

  if (!dump_mmap_contiguous (sections, ARRAYELTS (sections)))
    goto out;

  err = PDUMPER_LOAD_ERROR;
  mark_bits_needed =
    DIVIDE_ROUND_UP (header->discardable_start, GCALIGNMENT);
  if (!dump_bitset_init (&mark_bits, mark_bits_needed))
    goto out;
  free_mark_bits = true;

  /* Point of no return.  */
  err = PDUMPER_LOAD_SUCCESS;
  dump_base = (intptr_t) sections[DS_HOT].mapping;
  gflags.dumped_with_pdumper_ = true;
  free_mark_bits = false;
  dump_private.header = *header;
  dump_private.mark_bits = mark_bits;
  dump_public.start = dump_base;
  dump_public.end = dump_public.start + dump_size;

  dump_do_all_dump_relocations (header, dump_base);
  dump_do_all_emacs_relocations (header, dump_base);

  dump_mmap_discard_contents (&sections[DS_DISCARDABLE]);
  for (int i = 0; i < ARRAYELTS (sections); ++i)
    dump_mmap_reset (&sections[i]);

  /* Run the functions Emacs registered for doing post-dump-load
     initialization.  */
  for (int i = 0; i < nr_dump_hooks; ++i)
    dump_hooks[i] ();
  initialized = true;

 out:
  for (int i = 0; i < ARRAYELTS (sections); ++i)
    dump_mmap_release (&sections[i]);
  if (free_mark_bits)
    dump_bitset_destroy (&mark_bits);
  if (dump_fd >= 0)
    emacs_close (dump_fd);
  return err;
}

DEFUN ("pdumper-stats",
       Fpdumper_stats, Spdumper_stats,
       0, 0, 0,
       doc: /* Return statistics about the portable dumper.  */)
     (void)
{
  Lisp_Object stats = Qnil;
  if (dumped_with_pdumper_p ())
    dump_push (&stats, Fcons (Qdumped_with_pdumper, Qt));
  return Fnreverse (stats);
}

#endif /* HAVE_PDUMPER */



void
syms_of_pdumper (void)
{
#ifdef HAVE_PDUMPER
  defsubr (&Sdump_emacs_portable);
  defsubr (&Sdump_emacs_portable__sort_predicate);
  defsubr (&Sdump_emacs_portable__sort_predicate_copied);
  DEFSYM (Qdump_emacs_portable__sort_predicate,
          "dump-emacs-portable--sort-predicate");
  DEFSYM (Qdump_emacs_portable__sort_predicate_copied,
          "dump-emacs-portable--sort-predicate-copied");
  DEFSYM (Qdumped_with_pdumper, "dumped-with-pdumper");
  defsubr (&Spdumper_stats);
#endif /* HAVE_PDUMPER */
}
