/* serial.c: Glulxe code for saving and restoring the VM state.
    Designed by Andrew Plotkin <erkyrath@eblong.com>
    http://www.eblong.com/zarf/glulx/index.html
*/

#include <string.h>
#include "glk.h"
#include "glulxe.h"

/* This structure allows us to write either to a Glk stream or to
   a dynamically-allocated memory chunk. */
typedef struct dest_struct {
  int ismem;
  
  /* If it's a Glk stream: */
  strid_t str;

  /* If it's a block of memory: */
  unsigned char *ptr;
  glui32 pos;
  glui32 size;
} dest_t;

#define IFFID(c1, c2, c3, c4)  \
  ( (((glui32)c1) << 24)    \
  | (((glui32)c2) << 16)    \
  | (((glui32)c3) << 8)     \
  | (((glui32)c4)) )

/* This can be adjusted before startup by platform-specific startup
   code -- that is, preference code. */
int max_undo_level = 8;

static int undo_chain_size = 0;
static int undo_chain_num = 0;
unsigned char **undo_chain = NULL;

static glui32 protect_pos = 0;
static glui32 protect_len = 0;

static glui32 write_memstate(dest_t *dest);
static glui32 write_stackstate(dest_t *dest, int portable);
static glui32 read_memstate(dest_t *dest, glui32 chunklen);
static glui32 read_stackstate(dest_t *dest, glui32 chunklen, int portable);
static int write_long(dest_t *dest, glui32 val);
static int read_long(dest_t *dest, glui32 *val);
static int write_byte(dest_t *dest, unsigned char val);
static int read_byte(dest_t *dest, unsigned char *val);
static int reposition_write(dest_t *dest, glui32 pos);

/* init_serial():
   Set up the undo chain and anything else that needs to be set up.
*/
int init_serial()
{
  undo_chain_num = 0;
  undo_chain_size = max_undo_level;
  undo_chain = (unsigned char **)glulx_malloc(sizeof(unsigned char *) * undo_chain_size);
  if (!undo_chain)
    return FALSE;

  return TRUE;
}

/* perform_saveundo():
   Add a state pointer to the undo chain. This returns 0 on success,
   1 on failure.
*/
glui32 perform_saveundo()
{
  dest_t dest;
  glui32 res;
  glui32 memstart, memlen, stackstart, stacklen;

  /* The format for undo-saves is simpler than for saves on disk. We
     just have a memory chunk followed by a stack chunk, and we skip
     the IFF chunk headers (although the size fields are still there.)
     We also don't bother with IFF's 16-bit alignment. */

  if (undo_chain_size == 0)
    return 1;

  dest.ismem = TRUE;
  dest.size = 0;
  dest.pos = 0;
  dest.ptr = NULL;
  dest.str = NULL;

  res = 0;
  if (res == 0) {
    res = write_long(&dest, 0); /* space for chunk length */
  }
  if (res == 0) {
    memstart = dest.pos;
    res = write_memstate(&dest);
    memlen = dest.pos - memstart;
  }
  if (res == 0) {
    res = write_long(&dest, 0); /* space for chunk length */
  }
  if (res == 0) {
    stackstart = dest.pos;
    res = write_stackstate(&dest, FALSE);
    stacklen = dest.pos - stackstart;
  }
  if (res == 0) {
    /* Trim it down to the perfect size. */
    dest.ptr = glulx_realloc(dest.ptr, dest.pos);
    if (!dest.ptr)
      res = 1;
  }
  if (res == 0) {
    res = reposition_write(&dest, memstart-4);
  }
  if (res == 0) {
    res = write_long(&dest, memlen);
  }
  if (res == 0) {
    res = reposition_write(&dest, stackstart-4);
  }
  if (res == 0) {
    res = write_long(&dest, stacklen);
  }

  if (res == 0) {
    /* It worked. */
    if (undo_chain_size > 1)
      memmove(undo_chain+1, undo_chain, 
        (undo_chain_size-1) * sizeof(unsigned char *));
    undo_chain[0] = dest.ptr;
    if (undo_chain_num < undo_chain_size)
      undo_chain_num += 1;
    dest.ptr = NULL;
  }
  else {
    /* It didn't work. */
    if (dest.ptr) {
      glulx_free(dest.ptr);
      dest.ptr = NULL;
    }
  }
    
  return res;
}

/* perform_restoreundo():
   Pull a state pointer from the undo chain. This returns 0 on success,
   1 on failure. Note that if it succeeds, the frameptr, localsbase,
   and valstackbase registers are invalid; they must be rebuilt from
   the stack.
*/
glui32 perform_restoreundo()
{
  dest_t dest;
  glui32 res, val;

  if (undo_chain_size == 0 || undo_chain_num == 0)
    return 1;

  dest.ismem = TRUE;
  dest.size = 0;
  dest.pos = 0;
  dest.ptr = undo_chain[0];
  dest.str = NULL;

  res = 0;
  if (res == 0) {
    res = read_long(&dest, &val);
  }
  if (res == 0) {
    res = read_memstate(&dest, val);
  }
  if (res == 0) {
    res = read_long(&dest, &val);
  }
  if (res == 0) {
    res = read_stackstate(&dest, val, FALSE);
  }
  /* ### really, many of the failure modes of those two calls ought to
     cause fatal errors. The stack or main memory may be damaged now. */

  if (res == 0) {
    /* It worked. */
    if (undo_chain_size > 1)
      memmove(undo_chain, undo_chain+1,
        (undo_chain_size-1) * sizeof(unsigned char *));
    undo_chain_num -= 1;
    glulx_free(dest.ptr);
    dest.ptr = NULL;
  }
  else {
    /* It didn't work. */
    dest.ptr = NULL;
  }

  return res;
}

/* perform_save():
   Write the state to the output stream. This returns 0 on success,
   1 on failure.
*/
glui32 perform_save(strid_t str)
{
  dest_t dest;
  int ix;
  glui32 res, lx, val;
  glui32 memstart, memlen, stackstart, stacklen, filestart, filelen;

  stream_get_iosys(&val, &lx);
  if (val != 2) {
    /* Not using the Glk I/O system, so bail. This function only
       knows how to write to a Glk stream. */
    fatal_error("Streams are only available in Glk I/O system.");
  }

  if (str == 0)
    return 1;

  dest.ismem = FALSE;
  dest.size = 0;
  dest.pos = 0;
  dest.ptr = NULL;
  dest.str = str;

  res = 0;

  /* Quetzal header. */
  if (res == 0) {
    res = write_long(&dest, IFFID('F', 'O', 'R', 'M'));
  }
  if (res == 0) {
    res = write_long(&dest, 0); /* space for file length */
    filestart = dest.pos;
  }

  if (res == 0) {
    res = write_long(&dest, IFFID('I', 'F', 'Z', 'S')); /* ### ? */
  }

  /* Header chunk. This is the first 128 bytes of memory. */
  if (res == 0) {
    res = write_long(&dest, IFFID('I', 'F', 'h', 'd'));
  }
  if (res == 0) {
    res = write_long(&dest, 128);
  }
  for (ix=0; res==0 && ix<128; ix++) {
    res = write_byte(&dest, Mem1(ix));
  }
  /* Always even, so no padding necessary. */
  
  /* Memory chunk. */
  if (res == 0) {
    res = write_long(&dest, IFFID('C', 'M', 'e', 'm'));
  }
  if (res == 0) {
    res = write_long(&dest, 0); /* space for chunk length */
  }
  if (res == 0) {
    memstart = dest.pos;
    res = write_memstate(&dest);
    memlen = dest.pos - memstart;
  }
  if (res == 0 && (memlen & 1) != 0) {
    res = write_byte(&dest, 0);
  }

  /* Stack chunk. */
  if (res == 0) {
    res = write_long(&dest, IFFID('S', 't', 'k', 's'));
  }
  if (res == 0) {
    res = write_long(&dest, 0); /* space for chunk length */
  }
  if (res == 0) {
    stackstart = dest.pos;
    res = write_stackstate(&dest, TRUE);
    stacklen = dest.pos - stackstart;
  }
  if (res == 0 && (stacklen & 1) != 0) {
    res = write_byte(&dest, 0);
  }

  filelen = dest.pos - filestart;

  /* Okay, fill in all the lengths. */
  if (res == 0) {
    res = reposition_write(&dest, memstart-4);
  }
  if (res == 0) {
    res = write_long(&dest, memlen);
  }
  if (res == 0) {
    res = reposition_write(&dest, stackstart-4);
  }
  if (res == 0) {
    res = write_long(&dest, stacklen);
  }
  if (res == 0) {
    res = reposition_write(&dest, filestart-4);
  }
  if (res == 0) {
    res = write_long(&dest, filelen);
  }

  /* All done. */
    
  return res;
}

/* perform_restore():
   Pull a state pointer from a stream. This returns 0 on success,
   1 on failure. Note that if it succeeds, the frameptr, localsbase,
   and valstackbase registers are invalid; they must be rebuilt from
   the stack.
*/
glui32 perform_restore(strid_t str)
{
  dest_t dest;
  int ix;
  glui32 lx, res, val;
  glui32 filestart, filelen;

  stream_get_iosys(&val, &lx);
  if (val != 2) {
    /* Not using the Glk I/O system, so bail. This function only
       knows how to read from a Glk stream. */
    fatal_error("Streams are only available in Glk I/O system.");
  }

  if (str == 0)
    return 1;

  dest.ismem = FALSE;
  dest.size = 0;
  dest.pos = 0;
  dest.ptr = NULL;
  dest.str = str;

  res = 0;

  /* ### the format errors checked below should send error messages to
     the current stream. */

  if (res == 0) {
    res = read_long(&dest, &val);
  }
  if (res == 0 && val != IFFID('F', 'O', 'R', 'M')) {
    /* ### bad header */
    return 1;
  }
  if (res == 0) {
    res = read_long(&dest, &filelen);
  }
  filestart = dest.pos;

  if (res == 0) {
    res = read_long(&dest, &val);
  }
  if (res == 0 && val != IFFID('I', 'F', 'Z', 'S')) { /* ### ? */
    /* ### bad header */
    return 1;
  }

  while (res == 0 && dest.pos < filestart+filelen) {
    /* Read a chunk and deal with it. */
    glui32 chunktype, chunkstart, chunklen;
    unsigned char dummy;

    if (res == 0) {
      res = read_long(&dest, &chunktype);
    }
    if (res == 0) {
      res = read_long(&dest, &chunklen);
    }
    chunkstart = dest.pos;

    if (chunktype == IFFID('I', 'F', 'h', 'd')) {
      for (ix=0; res==0 && ix<128; ix++) {
        res = read_byte(&dest, &dummy);
        if (res == 0 && Mem1(ix) != dummy) {
          /* ### non-matching header */
          return 1;
        }
      }
    }
    else if (chunktype == IFFID('C', 'M', 'e', 'm')) {
      res = read_memstate(&dest, chunklen);
    }
    else if (chunktype == IFFID('S', 't', 'k', 's')) {
      res = read_stackstate(&dest, chunklen, TRUE);
    }
    else {
      /* Unknown chunk type. Skip it. */
      for (lx=0; res==0 && lx<chunklen; lx++) {
        res = read_byte(&dest, &dummy);
      }
    }

    if (chunkstart+chunklen != dest.pos) {
      /* ### funny chunk length */
      return 1;
    }

    if ((chunklen & 1) != 0) {
      if (res == 0) {
        res = read_byte(&dest, &dummy);
      }
    }
  }

  if (res)
    return 1;

  return 0;
}

static int reposition_write(dest_t *dest, glui32 pos)
{
  if (dest->ismem) {
    dest->pos = pos;
  }
  else {
    glk_stream_set_position(dest->str, pos, seekmode_Start);
    dest->pos = pos;
  }

  return 0;
}

static int write_buffer(dest_t *dest, unsigned char *ptr, glui32 len)
{
  if (dest->ismem) {
    if (dest->pos+len > dest->size) {
      dest->size = dest->pos+len+1024;
      if (!dest->ptr) {
        dest->ptr = glulx_malloc(dest->size);
      }
      else {
        dest->ptr = glulx_realloc(dest->ptr, dest->size);
      }
      if (!dest->ptr)
        return 1;
    }
    memcpy(dest->ptr+dest->pos, ptr, len);
  }
  else {
    glk_put_buffer_stream(dest->str, (char *)ptr, len);
  }

  dest->pos += len;

  return 0;
}

static int read_buffer(dest_t *dest, unsigned char *ptr, glui32 len)
{
  glui32 newlen;

  if (dest->ismem) {
    memcpy(ptr, dest->ptr+dest->pos, len);
  }
  else {
    newlen = glk_get_buffer_stream(dest->str, (char *)ptr, len);
    if (newlen != len)
      return 1;
  }

  dest->pos += len;

  return 0;
}

static int write_long(dest_t *dest, glui32 val)
{
  unsigned char buf[4];
  Write4(buf, val);
  return write_buffer(dest, buf, 4);
}

static int write_short(dest_t *dest, glui16 val)
{
  unsigned char buf[2];
  Write2(buf, val);
  return write_buffer(dest, buf, 2);
}

static int write_byte(dest_t *dest, unsigned char val)
{
  return write_buffer(dest, &val, 1);
}

static int read_long(dest_t *dest, glui32 *val)
{
  unsigned char buf[4];
  int res = read_buffer(dest, buf, 4);
  if (res)
    return res;
  *val = Read4(buf);
  return 0;
}

static int read_short(dest_t *dest, glui16 *val)
{
  unsigned char buf[2];
  int res = read_buffer(dest, buf, 2);
  if (res)
    return res;
  *val = Read2(buf);
  return 0;
}

static int read_byte(dest_t *dest, unsigned char *val)
{
  return read_buffer(dest, val, 1);
}

static glui32 write_memstate(dest_t *dest)
{
  glui32 res, pos;
  int val;
  int runlen;
  unsigned char ch;

  res = write_long(dest, endmem);
  if (res)
    return res;

  runlen = 0;
  glk_stream_set_position(gamefile, gamefile_start+ramstart, seekmode_Start);

  for (pos=ramstart; pos<endmem; pos++) {
    ch = Mem1(pos);
    if (pos < endgamefile) {
      val = glk_get_char_stream(gamefile);
      if (val == -1) {
        fatal_error("The game file ended unexpectedly while saving.");
      }
      ch ^= (unsigned char)val;
    }
    if (ch == 0) {
      runlen++;
    }
    else {
      /* Write any run we've got. */
      while (runlen) {
        if (runlen >= 0x100)
          val = 0x100;
        else
          val = runlen;
        res = write_byte(dest, 0);
        if (res)
          return res;
        res = write_byte(dest, (val-1));
        if (res)
          return res;
        runlen -= val;
      }
      /* Write the byte we got. */
      res = write_byte(dest, ch);
      if (res)
        return res;
    }
  }
  /* It's possible we've got a run left over, but we don't write it. */

  return 0;
}

static glui32 read_memstate(dest_t *dest, glui32 chunklen)
{
  glui32 chunkend = dest->pos + chunklen;
  glui32 newlen;
  glui32 res, pos;
  int val;
  int runlen;
  unsigned char ch, ch2;

  res = read_long(dest, &newlen);
  if (res)
    return res;

  res = change_memsize(newlen);
  if (res)
    return res;

  runlen = 0;
  glk_stream_set_position(gamefile, gamefile_start+ramstart, seekmode_Start);

  for (pos=ramstart; pos<endmem; pos++) {
    if (pos < endgamefile) {
      val = glk_get_char_stream(gamefile);
      if (val == -1) {
        fatal_error("The game file ended unexpectedly while restoring.");
      }
      ch = (unsigned char)val;
    }
    else {
      ch = 0;
    }

    if (dest->pos >= chunkend) {
      /* we're into the final, unstored run. */
    }
    else if (runlen) {
      runlen--;
    }
    else {
      res = read_byte(dest, &ch2);
      if (res)
        return res;
      if (ch2 == 0) {
        res = read_byte(dest, &ch2);
        if (res)
          return res;
        runlen = (glui32)ch2;
      }
      else {
        ch ^= ch2;
      }
    }

    if (pos >= protectstart && pos < protectend)
      continue;

    MemW1(pos, ch);
  }

  return 0;
}

static glui32 write_stackstate(dest_t *dest, int portable)
{
  glui32 res, pos;
  glui32 val, lx;
  unsigned char ch;
  glui32 lastframe;

  /* If we're storing for the purpose of undo, we don't need to do any
     byte-swapping, because the result will only be used by this session. */
  if (!portable) {
    res = write_buffer(dest, stack, stackptr);
    if (res)
      return res;
    return 0;
  }

  /* Write a portable stack image. To do this, we have to write stack
     frames in order, bottom to top. Remember that the last word of
     every stack frame is a pointer to the beginning of that stack frame.
     (This includes the last frame, because the save opcode pushes on
     a call stub before it calls perform_save().) */

  lastframe = (glui32)(-1);
  while (1) {
    glui32 frameend, frm, frm2, frm3;
    unsigned char loctype, loccount;
    glui32 numlocals, frlen, locpos;

    /* Find the next stack frame (after the one in lastframe). Sadly,
       this requires searching the stack from the top down. We have to
       do this for *every* frame, which takes N^2 time overall. But
       save routines usually aren't nested very deep. 
       If it becomes a practical problem, we can build a stack-frame 
       array, which requires dynamic allocation. */
    for (frm = stackptr, frameend = stackptr;
         frm != 0 && (frm2 = Stk4(frm-4)) != lastframe;
         frameend = frm, frm = frm2) { };

    /* Write out the frame. */
    frm2 = frm;

    frlen = Stk4(frm2);
    frm2 += 4;
    res = write_long(dest, frlen);
    if (res)
      return res;
    locpos = Stk4(frm2);
    frm2 += 4;
    res = write_long(dest, locpos);
    if (res)
      return res;

    frm3 = frm2;

    numlocals = 0;
    while (1) {
      loctype = Stk1(frm2);
      frm2 += 1;
      loccount = Stk1(frm2);
      frm2 += 1;

      res = write_byte(dest, loctype);
      if (res)
        return res;
      res = write_byte(dest, loccount);
      if (res)
        return res;

      if (loctype == 0 && loccount == 0)
        break;

      numlocals++;
    }

    if ((numlocals & 1) == 0) {
      res = write_byte(dest, 0);
      if (res)
        return res;
      res = write_byte(dest, 0);
      if (res)
        return res;
      frm2 += 2;
    }

    if (frm2 != frm+locpos)
      fatal_error("Inconsistent stack frame during save.");

    /* Write out the locals. */
    for (lx=0; lx<numlocals; lx++) {
      loctype = Stk1(frm3);
      frm3 += 1;
      loccount = Stk1(frm3);
      frm3 += 1;
      
      if (loctype == 0 && loccount == 0)
        break;

      /* Put in up to 0, 1, or 3 bytes of padding, depending on loctype. */
      while (frm2 & (loctype-1)) {
        res = write_byte(dest, 0);
        if (res)
          return res;
        frm2 += 1;
      }

      /* Put in this set of locals. */
      switch (loctype) {

      case 1:
        do {
          res = write_byte(dest, Stk1(frm2));
          if (res)
            return res;
          frm2 += 1;
          loccount--;
        } while (loccount);
        break;

      case 2:
        do {
          res = write_short(dest, Stk2(frm2));
          if (res)
            return res;
          frm2 += 2;
          loccount--;
        } while (loccount);
        break;

      case 4:
        do {
          res = write_long(dest, Stk4(frm2));
          if (res)
            return res;
          frm2 += 4;
          loccount--;
        } while (loccount);
        break;

      }
    }

    if (frm2 != frm+frlen)
      fatal_error("Inconsistent stack frame during save.");

    while (frm2 < frameend) {
      res = write_long(dest, Stk4(frm2));
      if (res)
        return res;
      frm2 += 4;
    }

    /* Go on to the next frame. */
    if (frameend == stackptr)
      break; /* All done. */
    lastframe = frm;
  }

  return 0;
}

static glui32 read_stackstate(dest_t *dest, glui32 chunklen, int portable)
{
  glui32 res, pos;
  unsigned char ch;
  glui32 frameend, frm, frm2, frm3, locpos, frlen, numlocals;

  if (chunklen > stacksize)
    return 1;

  stackptr = chunklen;
  frameptr = 0;
  valstackbase = 0;
  localsbase = 0;

  if (!portable) {
    res = read_buffer(dest, stack, stackptr);
    if (res)
      return res;
    return 0;
  }

  /* This isn't going to be pleasant; we're going to read the data in
     as a block, and then convert it in-place. */
  res = read_buffer(dest, stack, stackptr);
  if (res)
    return res;

  frameend = stackptr;
  while (frameend != 0) {
    /* Read the beginning-of-frame pointer. Remember, right now, the
       whole frame is stored big-endian. So we have to read with the
       Read*() macros, and then write with the StkW*() macros. */
    frm = Read4(stack+(frameend-4));

    frm2 = frm;

    frlen = Read4(stack+frm2);
    StkW4(frm2, frlen);
    frm2 += 4;
    locpos = Read4(stack+frm2);
    StkW4(frm2, locpos);
    frm2 += 4;

    /* The locals-format list is in bytes, so we don't have to convert it. */
    frm3 = frm2;
    frm2 = frm+locpos;

    numlocals = 0;

    while (1) {
      unsigned char loctype, loccount;
      loctype = Read1(stack+frm3);
      frm3 += 1;
      loccount = Read1(stack+frm3);
      frm3 += 1;

      if (loctype == 0 && loccount == 0)
        break;

      /* Skip up to 0, 1, or 3 bytes of padding, depending on loctype. */
      while (frm2 & (loctype-1)) {
        StkW1(frm2, 0);
        frm2++;
      }
      
      /* Convert this set of locals. */
      switch (loctype) {
        
      case 1:
        do {
          /* Don't need to convert bytes. */
          frm2 += 1;
          loccount--;
        } while (loccount);
        break;

      case 2:
        do {
          glui16 loc = Read2(stack+frm2);
          StkW2(frm2, loc);
          frm2 += 2;
          loccount--;
        } while (loccount);
        break;

      case 4:
        do {
          glui32 loc = Read4(stack+frm2);
          StkW4(frm2, loc);
          frm2 += 4;
          loccount--;
        } while (loccount);
        break;

      }

      numlocals++;
    }

    if ((numlocals & 1) == 0) {
      StkW1(frm3, 0);
      frm3++;
      StkW1(frm3, 0);
      frm3++;
    }

    if (frm3 != frm+locpos) {
      return 1;
    }

    while (frm2 & 3) {
      StkW1(frm2, 0);
      frm2++;
    }

    if (frm2 != frm+frlen) {
      return 1;
    }

    /* Now, the values pushed on the stack after the call frame itself.
       This includes the stub. */
    while (frm2 < frameend) {
      glui32 loc = Read4(stack+frm2);
      StkW4(frm2, loc);
      frm2 += 4;
    }

    frameend = frm;
  }

  return 0;
}

glui32 perform_verify()
{
  glui32 len, checksum, newlen;
  unsigned char buf[4];
  glui32 val, newsum, ix;

  len = gamefile_len;

  if (len < 256 || (len & 0xFF) != 0)
    return 1;

  glk_stream_set_position(gamefile, gamefile_start, seekmode_Start);
  newsum = 0;

  /* Read the header */
  for (ix=0; ix<9; ix++) {
    newlen = glk_get_buffer_stream(gamefile, (char *)buf, 4);
    if (newlen != 4)
      return 1;
    val = Read4(buf);
    if (ix == 4) {
      if (len != val)
        return 1;
    }
    if (ix == 8)
      checksum = val;
    else
      newsum += val;
  }

  /* Read everything else */
  for (; ix < len/4; ix++) {
    newlen = glk_get_buffer_stream(gamefile, (char *)buf, 4);
    if (newlen != 4)
      return 1;
    val = Read4(buf);
    newsum += val;
  }

  if (newsum != checksum)
    return 1;

  return 0;  
}
