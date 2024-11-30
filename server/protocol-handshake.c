/* nbdkit
 * Copyright (C) 2013-2019 Red Hat Inc.
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
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"
#include "byte-swapping.h"
#include "protocol.h"

int
protocol_handshake (struct connection *conn)
{
  int r;

  lock_request (conn);
  if (!newstyle)
    r = protocol_handshake_oldstyle (conn);
  else
    r = protocol_handshake_newstyle (conn);
  unlock_request (conn);

  return r;
}

/* Common code used by oldstyle and newstyle protocols to:
 *
 * - call the backend .open method
 *
 * - get the export size
 *
 * - compute the eflags (same between oldstyle and newstyle
 *   protocols)
 *
 * The protocols must defer this as late as possible so that
 * unauthorized clients can't cause unnecessary work in .open by
 * simply opening a TCP connection.
 */
int
protocol_common_open (struct connection *conn,
                      uint64_t *exportsize, uint16_t *flags)
{
  int64_t size;
  uint16_t eflags = NBD_FLAG_HAS_FLAGS;
  int fl;

  if (backend->open (backend, conn, readonly) == -1)
    return -1;

  /* Prepare (for filters), called just after open. */
  if (backend->prepare (backend, conn) == -1)
    return -1;

  size = backend->get_size (backend, conn);
  if (size == -1)
    return -1;
  if (size < 0) {
    nbdkit_error (".get_size function returned invalid value "
                  "(%" PRIi64 ")", size);
    return -1;
  }

  fl = backend->can_write (backend, conn);
  if (fl == -1)
    return -1;
  if (readonly || !fl) {
    eflags |= NBD_FLAG_READ_ONLY;
    conn->readonly = true;
  }
  if (!conn->readonly) {
    fl = backend->can_zero (backend, conn);
    if (fl == -1)
      return -1;
    if (fl) {
      eflags |= NBD_FLAG_SEND_WRITE_ZEROES;
      conn->can_zero = true;
    }

    fl = backend->can_trim (backend, conn);
    if (fl == -1)
      return -1;
    if (fl) {
      eflags |= NBD_FLAG_SEND_TRIM;
      conn->can_trim = true;
    }

    fl = backend->can_fua (backend, conn);
    if (fl == -1)
      return -1;
    if (fl) {
      eflags |= NBD_FLAG_SEND_FUA;
      conn->can_fua = true;
    }
  }

  fl = backend->can_flush (backend, conn);
  if (fl == -1)
    return -1;
  if (fl) {
    eflags |= NBD_FLAG_SEND_FLUSH;
    conn->can_flush = true;
  }

  fl = backend->is_rotational (backend, conn);
  if (fl == -1)
    return -1;
  if (fl) {
    eflags |= NBD_FLAG_ROTATIONAL;
    conn->is_rotational = true;
  }

  /* multi-conn is useless if parallel connections are not allowed */
  if (backend->thread_model (backend) >
      NBDKIT_THREAD_MODEL_SERIALIZE_CONNECTIONS) {
    fl = backend->can_multi_conn (backend, conn);
    if (fl == -1)
      return -1;
    if (fl) {
      eflags |= NBD_FLAG_CAN_MULTI_CONN;
      conn->can_multi_conn = true;
    }
  }

  /* The result of this is not returned to callers here (or at any
   * time during the handshake).  However it makes sense to do it once
   * per connection and store the result in the handle anyway.  This
   * protocol_compute_eflags function is a bit misnamed XXX.
   */
  fl = backend->can_extents (backend, conn);
  if (fl == -1)
    return -1;
  if (fl)
    conn->can_extents = true;

  if (conn->structured_replies)
    eflags |= NBD_FLAG_SEND_DF;

  *exportsize = size;
  *flags = eflags;
  return 0;
}
