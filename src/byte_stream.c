#include "byte_stream.h"

#include <stdlib.h>
#include <string.h>

void byte_cursor_init(byte_cursor *c, const u1 *data, size_t size)
{
    c->begin = data;
    c->p = data;
    c->end = data + size;
    c->ok = true;
}

bool byte_cursor_ok(const byte_cursor *c) { return c->ok; }

bool byte_cursor_exhausted(const byte_cursor *c) { return c->p == c->end; }

size_t byte_cursor_offset(const byte_cursor *c) { return (size_t)(c->p - c->begin); }

size_t byte_cursor_remaining(const byte_cursor *c)
{
    return c->ok ? (size_t)(c->end - c->p) : 0;
}

static bool cursor_need(byte_cursor *c, size_t n)
{
    if (!c->ok)
        return false;
    if ((size_t)(c->end - c->p) < n)
    {
        c->ok = false;
        return false;
    }
    return true;
}

u1 byte_cursor_u1(byte_cursor *c)
{
    if (!cursor_need(c, 1))
        return 0;
    return *c->p++;
}

u2 byte_cursor_u2(byte_cursor *c)
{
    if (!cursor_need(c, 2))
        return 0;
    const u2 v = (u2)(((u2)c->p[0] << 8) | (u2)c->p[1]);
    c->p += 2;
    return v;
}

u4 byte_cursor_u4(byte_cursor *c)
{
    if (!cursor_need(c, 4))
        return 0;
    const u4 v = ((u4)c->p[0] << 24) | ((u4)c->p[1] << 16) | ((u4)c->p[2] << 8) | (u4)c->p[3];
    c->p += 4;
    return v;
}

const u1 *byte_cursor_bytes(byte_cursor *c, size_t length)
{
    if (!cursor_need(c, length))
        return NULL;
    const u1 *at = c->p;
    c->p += length;
    return at;
}

void byte_buffer_init(byte_buffer *b)
{
    b->data = NULL;
    b->size = 0;
    b->capacity = 0;
    b->ok = true;
}

void byte_buffer_free(byte_buffer *b)
{
    free(b->data);
    byte_buffer_init(b);
}

bool byte_buffer_ok(const byte_buffer *b) { return b->ok; }

void byte_buffer_reserve(byte_buffer *b, size_t extra)
{
    if (!b->ok || b->size + extra <= b->capacity)
        return;

    size_t capacity = b->capacity ? b->capacity : 4096;
    while (capacity < b->size + extra)
        capacity *= 2;

    u1 *grown = realloc(b->data, capacity);
    if (grown == NULL)
    {
        b->ok = false;
        return;
    }
    b->data = grown;
    b->capacity = capacity;
}

void byte_buffer_u1(byte_buffer *b, u1 value)
{
    byte_buffer_reserve(b, 1);
    if (!b->ok)
        return;
    b->data[b->size++] = value;
}

void byte_buffer_u2(byte_buffer *b, u2 value)
{
    byte_buffer_reserve(b, 2);
    if (!b->ok)
        return;
    b->data[b->size++] = (u1)(value >> 8);
    b->data[b->size++] = (u1)(value & 0xFF);
}

void byte_buffer_u4(byte_buffer *b, u4 value)
{
    byte_buffer_reserve(b, 4);
    if (!b->ok)
        return;
    b->data[b->size++] = (u1)(value >> 24);
    b->data[b->size++] = (u1)(value >> 16);
    b->data[b->size++] = (u1)(value >> 8);
    b->data[b->size++] = (u1)(value & 0xFF);
}

void byte_buffer_bytes(byte_buffer *b, const void *data, size_t length)
{
    if (length == 0)
        return;
    byte_buffer_reserve(b, length);
    if (!b->ok)
        return;
    memcpy(b->data + b->size, data, length);
    b->size += length;
}

u1 *byte_buffer_release(byte_buffer *b, size_t *size)
{
    u1 *data = b->data;
    if (size != NULL)
        *size = b->size;
    byte_buffer_init(b);
    return data;
}
