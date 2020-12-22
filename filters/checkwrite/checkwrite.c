/* nbdkit
 * Copyright (C) 2019-2020 Red Hat Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of Red Hat nor the names of its contributors may be
 * used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY RED HAT AND CONTRIBUTORS ''AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL RED HAT OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <nbdkit-filter.h>

#include "cleanup.h"
#include "iszero.h"
#include "minmax.h"

static void *
checkwrite_open (nbdkit_next_open *next, nbdkit_backend *nxdata,
                 int readonly, const char *exportname, int is_tls)
{
  /* Ignore readonly flag passed in, open the plugin readonly. */
  if (next (nxdata, 1, exportname) == -1)
    return NULL;
  return NBDKIT_HANDLE_NOT_NEEDED;
}

/* Whatever the underlying plugin can or can't do, we can do all the
 * write-like operations.
 */
static int
checkwrite_can_write (struct nbdkit_next_ops *next_ops, void *nxdata,
                      void *handle)
{
  return 1;
}

static int
checkwrite_can_flush (struct nbdkit_next_ops *next_ops, void *nxdata,
                      void *handle)
{
  return 1;
}

static int
checkwrite_can_fua (struct nbdkit_next_ops *next_ops, void *nxdata,
                    void *handle)
{
  return NBDKIT_FUA_NATIVE;
}

static int
checkwrite_can_trim (struct nbdkit_next_ops *next_ops, void *nxdata,
                     void *handle)
{
  return 1;
}

static int
checkwrite_can_zero (struct nbdkit_next_ops *next_ops, void *nxdata,
                     void *handle)
{
  return NBDKIT_ZERO_NATIVE;
}

static inline int
data_does_not_match (int *err)
{
  *err = EIO;
  nbdkit_error ("data written does not match expected");
  return -1;
}

/* Provide write-like operations which perform the additional checks. */
static int
checkwrite_pwrite (struct nbdkit_next_ops *next_ops, void *nxdata,
                   void *handle,
                   const void *buf, uint32_t count, uint64_t offset,
                   uint32_t flags, int *err)
{
  CLEANUP_FREE char *expected;

  expected = malloc (count);
  if (expected == NULL) {
    *err = errno;
    nbdkit_error ("malloc: %m");
    return -1;
  }

  /* Read underlying plugin data into the buffer. */
  if (next_ops->pread (nxdata, expected, count, offset, 0, err) == -1)
    return -1;

  /* If data written doesn't match data expected, inject EIO. */
  if (memcmp (buf, expected, count) != 0)
    return data_does_not_match (err);

  return 0;
}

static int
checkwrite_flush (struct nbdkit_next_ops *next_ops, void *nxdata,
                  void *handle, uint32_t flags, int *err)
{
  /* Does nothing, we just have to support it. */
  return 0;
}

#define MAX_REQUEST_SIZE (64 * 1024 * 1024) /* XXX */

/* Trim and zero are effectively the same operation for this plugin.
 * We have to check that the underlying plugin contains all zeroes.
 *
 * Note we don't check that the extents exactly match since a valid
 * copying operation is to either add sparseness (qemu-img convert -S)
 * or create a fully allocated target (nbdcopy --allocated).
 */
static int
checkwrite_trim_zero (struct nbdkit_next_ops *next_ops, void *nxdata,
                      void *handle, uint32_t count, uint64_t offset,
                      uint32_t flags, int *err)
{
  /* If the plugin supports extents, speed this up by using them. */
  if (next_ops->can_extents (nxdata)) {
    while (count > 0) {
      struct nbdkit_extents *exts;
      size_t i, n;

      exts = nbdkit_extents_new (offset, offset + count);
      if (exts == NULL)
        return -1;
      if (next_ops->extents (nxdata, count, offset, 0, exts, err) == -1)
        return -1;

      /* Ignore any extents or partial extents which are outside the
       * offset/count that we are looking at.  The plugin is required
       * to return at least one relevant extent so we can assume this
       * loop will make forward progress.
       */
      n = nbdkit_extents_count (exts);
      for (i = 0; i < n && count > 0; ++i) {
        uint64_t next_extent_offset;
        struct nbdkit_extent e;

        e = nbdkit_get_extent (exts, i);

        if (e.offset + e.length <= offset)
          continue;
        if (e.offset > offset)
          break;

        next_extent_offset = e.offset + e.length;

        /* Anything that reads back as zero is good. */
        if ((e.type & NBDKIT_EXTENT_ZERO) != 0) {
          const uint64_t zerolen = MIN (count, next_extent_offset - offset);

          offset += zerolen;
          count -= zerolen;
          continue;
        }

        /* Otherwise we have to read the underlying data and check. */
        while (count > 0) {
          const size_t buflen =
            MIN3 (MAX_REQUEST_SIZE, count, next_extent_offset - offset);
          CLEANUP_FREE char *buf = malloc (buflen);
          if (buf == NULL) {
            *err = errno;
            nbdkit_error ("malloc: %m");
            return -1;
          }

          if (next_ops->pread (nxdata, buf, buflen, offset, 0, err) == -1)
            return -1;
          if (! is_zero (buf, buflen))
            return data_does_not_match (err);

          count -= buflen;
          offset += buflen;
        }
      } /* for extent */

      nbdkit_extents_free (exts);
    } /* while (count > 0) */
  }

  /* Otherwise the plugin does not support extents, so do this the
   * slow way.
   */
  else {
    CLEANUP_FREE char *buf;

    buf = malloc (MIN (MAX_REQUEST_SIZE, count));
    if (buf == NULL) {
      *err = errno;
      nbdkit_error ("malloc: %m");
      return -1;
    }

    while (count > 0) {
      uint32_t n = MIN (MAX_REQUEST_SIZE, count);

      if (next_ops->pread (nxdata, buf, n, offset, 0, err) == -1)
        return -1;
      if (! is_zero (buf, n))
        return data_does_not_match (err);
      count -= n;
      offset += n;
    }
  }

  return 0;
}

static struct nbdkit_filter filter = {
  .name              = "checkwrite",
  .longname          = "nbdkit checkwrite filter",

  .open              = checkwrite_open,
  .can_write         = checkwrite_can_write,
  .can_flush         = checkwrite_can_flush,
  .can_fua           = checkwrite_can_fua,
  .can_trim          = checkwrite_can_trim,
  .can_zero          = checkwrite_can_zero,

  .pwrite            = checkwrite_pwrite,
  .flush             = checkwrite_flush,
  .trim              = checkwrite_trim_zero,
  .zero              = checkwrite_trim_zero,
};

NBDKIT_REGISTER_FILTER(filter)
