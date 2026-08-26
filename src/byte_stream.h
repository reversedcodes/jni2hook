#ifndef JNI2HOOK_BYTE_STREAM_H
#define JNI2HOOK_BYTE_STREAM_H

#include "visitors/visitor.h"

/* Bounds checked big endian reader. Once a read runs past the end the cursor
   latches into a failed state, so callers may read a whole structure and check
   once at the end instead of after every field. */
typedef struct
{
    const u1 *begin;
    const u1 *p;
    const u1 *end;
    bool ok;
} byte_cursor;

void byte_cursor_init(byte_cursor *c, const u1 *data, size_t size);
bool byte_cursor_ok(const byte_cursor *c);
bool byte_cursor_exhausted(const byte_cursor *c);
size_t byte_cursor_offset(const byte_cursor *c);
size_t byte_cursor_remaining(const byte_cursor *c);

u1 byte_cursor_u1(byte_cursor *c);
u2 byte_cursor_u2(byte_cursor *c);
u4 byte_cursor_u4(byte_cursor *c);
const u1 *byte_cursor_bytes(byte_cursor *c, size_t length);

/* Growable big endian writer with the same latching failure behaviour. */
typedef struct
{
    u1 *data;
    size_t size;
    size_t capacity;
    bool ok;
} byte_buffer;

void byte_buffer_init(byte_buffer *b);
void byte_buffer_free(byte_buffer *b);
bool byte_buffer_ok(const byte_buffer *b);
void byte_buffer_reserve(byte_buffer *b, size_t extra);

void byte_buffer_u1(byte_buffer *b, u1 value);
void byte_buffer_u2(byte_buffer *b, u2 value);
void byte_buffer_u4(byte_buffer *b, u4 value);
void byte_buffer_bytes(byte_buffer *b, const void *data, size_t length);

/* Hands the buffer over to the caller, who then owns it. */
u1 *byte_buffer_release(byte_buffer *b, size_t *size);

#endif
