// NOTE: Functions in these module are somewhat different from what can be seen
// in IDA because of two new helper functions that deal with incoming bits. I
// bet something like these were implemented via function-like macro in the
// same manner zlib deals with bits. The pattern is so common in this module so
// I made an exception and extracted it into separate functions to increase
// readability.

#include "plib/db/lzss.h"

#include <string.h>

namespace fallout {

static inline void lzss_fill_decode_buffer(FILE* stream);
static inline void lzss_decode_chunk_to_buf(unsigned int type, unsigned char** dest, unsigned int* length);
static inline void lzss_decode_chunk_to_file(unsigned int type, FILE* stream, unsigned int* length);

// 0x6B0860
static unsigned char decode_buffer[1024];

// 0x6B0C60
static unsigned char* decode_buffer_position;

// 0x6B0C64
static unsigned int decode_bytes_left;

// 0x6B0C68
static int ring_buffer_index;

// 0x6B0C6C
static unsigned char* decode_buffer_end;

// 0x6B0C70
static unsigned char ring_buffer[4116];

// 0x4CA260
//
// NOTE (PS2): The original implementation calls a small helper per decoded
// chunk and re-applies the ring buffer wrap masks on every byte. This is a
// behavior-preserving rewrite: the same `length` accounting (consumed input
// bytes), the same decode_buffer refill points and the same stop checks are
// kept, but match copies take a wrap-free fast path (no per-byte masking) and
// the chunk bodies are inlined.
#define LZSS_DECODE_TO_BUF_CHUNK(bit_mask) \
    do { \
        if ((byte & (bit_mask)) != 0) { \
            len -= 1; \
            unsigned char b = *decode_buffer_position++; \
            *curr++ = b; \
            ring_buffer[ring_buffer_index] = b; \
            ring_buffer_index = (ring_buffer_index + 1) & 0xFFF; \
        } else { \
            len -= 2; \
            unsigned char low = *decode_buffer_position++; \
            unsigned char high = *decode_buffer_position++; \
            int dict_offset = low | ((high & 0xF0) << 4); \
            int chunk_length = (high & 0x0F) + 3; \
            if (dict_offset + chunk_length <= 4096 && ring_buffer_index + chunk_length <= 4096) { \
                unsigned char* rs = &ring_buffer[dict_offset]; \
                unsigned char* rd = &ring_buffer[ring_buffer_index]; \
                for (int k = 0; k < chunk_length; k++) { \
                    unsigned char b = rs[k]; \
                    rd[k] = b; \
                    *curr++ = b; \
                } \
                ring_buffer_index = (ring_buffer_index + chunk_length) & 0xFFF; \
            } else { \
                for (int k = 0; k < chunk_length; k++) { \
                    unsigned char b = ring_buffer[dict_offset]; \
                    dict_offset = (dict_offset + 1) & 0xFFF; \
                    ring_buffer[ring_buffer_index] = b; \
                    ring_buffer_index = (ring_buffer_index + 1) & 0xFFF; \
                    *curr++ = b; \
                } \
            } \
        } \
    } while (0)

int lzss_decode_to_buf(FILE* in, unsigned char* dest, unsigned int length)
{
    unsigned char* curr;
    unsigned int len;
    unsigned char byte;

    curr = dest;
    len = length;

    memset(ring_buffer, ' ', 4078);
    ring_buffer_index = 4078;
    decode_buffer_end = decode_buffer;
    decode_buffer_position = decode_buffer;
    decode_bytes_left = length;

    while (len > 16) {
        lzss_fill_decode_buffer(in);

        len -= 1;
        byte = *decode_buffer_position++;
        LZSS_DECODE_TO_BUF_CHUNK(0x01);
        LZSS_DECODE_TO_BUF_CHUNK(0x02);
        LZSS_DECODE_TO_BUF_CHUNK(0x04);
        LZSS_DECODE_TO_BUF_CHUNK(0x08);
        LZSS_DECODE_TO_BUF_CHUNK(0x10);
        LZSS_DECODE_TO_BUF_CHUNK(0x20);
        LZSS_DECODE_TO_BUF_CHUNK(0x40);
        LZSS_DECODE_TO_BUF_CHUNK(0x80);
    }

    do {
        if (len == 0) break;

        lzss_fill_decode_buffer(in);

        len -= 1;
        byte = *decode_buffer_position++;

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x01);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x02);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x04);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x08);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x10);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x20);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x40);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x80);

        if (len == 0) break;

        len -= 1;
        byte = *decode_buffer_position++;

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x01);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x02);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x04);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x08);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x10);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x20);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x40);

        if (len == 0) break;
        LZSS_DECODE_TO_BUF_CHUNK(0x80);
    } while (0);

    return curr - dest;
}

#undef LZSS_DECODE_TO_BUF_CHUNK

// 0x4CB570
void lzss_decode_to_file(FILE* in, FILE* out, unsigned int length)
{
    unsigned char byte;

    memset(ring_buffer, ' ', 4078);
    ring_buffer_index = 4078;
    decode_buffer_end = decode_buffer;
    decode_buffer_position = decode_buffer;
    decode_bytes_left = length;

    while (length > 16) {
        lzss_fill_decode_buffer(in);

        length -= 1;
        byte = *decode_buffer_position++;
        lzss_decode_chunk_to_file(byte & 0x01, out, &length);
        lzss_decode_chunk_to_file(byte & 0x02, out, &length);
        lzss_decode_chunk_to_file(byte & 0x04, out, &length);
        lzss_decode_chunk_to_file(byte & 0x08, out, &length);
        lzss_decode_chunk_to_file(byte & 0x10, out, &length);
        lzss_decode_chunk_to_file(byte & 0x20, out, &length);
        lzss_decode_chunk_to_file(byte & 0x40, out, &length);
        lzss_decode_chunk_to_file(byte & 0x80, out, &length);
    }

    do {
        if (length == 0) break;

        lzss_fill_decode_buffer(in);

        length -= 1;
        byte = *decode_buffer_position++;

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x01, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x02, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x04, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x08, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x10, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x20, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x40, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x80, out, &length);

        if (length == 0) break;

        length -= 1;
        byte = *decode_buffer_position++;

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x01, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x02, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x04, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x08, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x10, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x20, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x40, out, &length);

        if (length == 0) break;
        lzss_decode_chunk_to_file(byte & 0x80, out, &length);
    } while (0);
}

static inline void lzss_fill_decode_buffer(FILE* stream)
{
    size_t bytes_to_read;
    size_t bytes_read;

    if (decode_bytes_left != 0 && decode_buffer_end - decode_buffer_position <= 16) {
        if (decode_buffer_position == decode_buffer_end) {
            decode_buffer_end = decode_buffer;
        } else {
            memmove(decode_buffer, decode_buffer_position, decode_buffer_end - decode_buffer_position);
            decode_buffer_end = decode_buffer + (decode_buffer_end - decode_buffer_position);
        }

        decode_buffer_position = decode_buffer;

        bytes_to_read = 1024 - (decode_buffer_end - decode_buffer);
        if (bytes_to_read > decode_bytes_left) {
            bytes_to_read = decode_bytes_left;
        }

        bytes_read = fread(decode_buffer_end, 1, bytes_to_read, stream);
        decode_buffer_end += bytes_read;
        decode_bytes_left -= bytes_read;
    }
}

static inline void lzss_decode_chunk_to_buf(unsigned int type, unsigned char** dest, unsigned int* length)
{
    unsigned char low;
    unsigned char high;
    int dict_offset;
    int dict_index;
    int chunk_length;
    int index;

    if (type != 0) {
        *length -= 1;
        *(*dest) = *decode_buffer_position++;
        ring_buffer[ring_buffer_index] = *(*dest)++;
        ring_buffer_index += 1;
        ring_buffer_index &= 0xFFF;
    } else {
        *length -= 2;
        low = *decode_buffer_position++;
        high = *decode_buffer_position++;
        dict_offset = low | ((high & 0xF0) << 4);
        chunk_length = (high & 0x0F) + 3;

        for (index = 0; index < chunk_length; index++) {
            dict_index = (dict_offset + index) & 0xFFF;
            *(*dest) = ring_buffer[dict_index];
            ring_buffer[ring_buffer_index] = *(*dest)++;
            ring_buffer_index += 1;
            ring_buffer_index &= 0xFFF;
        }
    }
}

static inline void lzss_decode_chunk_to_file(unsigned int type, FILE* stream, unsigned int* length)
{
    unsigned char low;
    unsigned char high;
    int dict_offset;
    int dict_index;
    int chunk_length;
    int index;

    if (type != 0) {
        *length -= 1;
        fputc(*decode_buffer_position, stream);
        ring_buffer[ring_buffer_index] = *decode_buffer_position++;
        ring_buffer_index += 1;
        ring_buffer_index &= 0xFFF;
    } else {
        *length -= 2;
        low = *decode_buffer_position++;
        high = *decode_buffer_position++;
        dict_offset = low | ((high & 0xF0) << 4);
        chunk_length = (high & 0x0F) + 3;

        for (index = 0; index < chunk_length; index++) {
            dict_index = (dict_offset + index) & 0xFFF;
            fputc(ring_buffer[dict_index], stream);
            ring_buffer[ring_buffer_index] = ring_buffer[dict_index];
            ring_buffer_index += 1;
            ring_buffer_index &= 0xFFF;
        }
    }
}

} // namespace fallout
