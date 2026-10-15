#ifndef HML_FILESTREAM_PRIV_H
#define HML_FILESTREAM_PRIV_H

/*
 * File-stream sensor adapter -- private interface.
 *
 * A second hml_sensor_ops implementation beside the synthetic one, so the demo
 * path is not synthetic-only. It replays frames that were captured off the wire
 * (or written by any tool) from a plain file, and it is the writer for the same
 * format, so a capture and a replay are byte-identical operations seen from the
 * two ends. The transport, the framing and the CRCs are the library's: this
 * adapter only moves payload bytes between a FILE* and the HAL, and it never
 * touches a socket.
 *
 * ON-DISK FORMAT -- one record per frame, repeated until EOF:
 *
 *   off  size  field
 *   ---  ----  -------------------------------------------------------
 *    0     4   magic "HMLF"
 *    4     4   frame_len, u32 little-endian
 *    8     n   payload, exactly frame_len bytes
 *
 * The record header is a fixed 8 bytes and carries no CRC field, so integrity is
 * two-layered and both layers are reported rather than assumed:
 *
 *   - The reader fails closed on anything that does not match the layout. A
 *     short record header or a short payload is HML_ERR_CRC, because it is the
 *     record's integrity claim that did not hold; a bad magic, a zero length and
 *     an over-long length are HML_ERR_PROTO / HML_ERR_TOOLARGE.
 *   - hml_crc16() over every payload is published per frame and as a chained
 *     whole-file value, so a replayed frame and a received frame can be
 *     compared without shipping the bytes. See docs/file_stream.md for what
 *     this does and does not catch.
 *
 * The length is little-endian and the CRCs are the library's hml_crc16 (CCITT,
 * poly 0x1021) on the same byte order the T0 datagram payload CRC uses, so a
 * captured frame's file CRC equals the CRC the wire carried for it.
 */

#include "hml/hml_sensor.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HML_FS_MAGIC "HMLF"
#define HML_FS_MAGIC_LEN 4u
#define HML_FS_LEN_BYTES 4u
#define HML_FS_REC_HDR (HML_FS_MAGIC_LEN + HML_FS_LEN_BYTES) /* 8 */

/* open() parameter for the "filestream" sensor. */
typedef struct hml_fs_cfg {
  const char *path;      /* file to replay, opened once and read sequentially */
  size_t max_frame;      /* reject records longer than this; 0 == HML_MAX_FRAME_SIZE */
  bool repeat;           /* rewind at EOF and replay, for soak runs */
} hml_fs_cfg;

typedef struct hml_fs_stats {
  uint64_t records;    /* records fully delivered out of acquire() */
  uint64_t bytes;      /* payload bytes delivered */
  uint64_t rewinds;    /* --repeat rewind count */
  uint64_t index;      /* 0-based index of the record last read from the file */
  size_t frame_bytes;  /* its length; 0 when none has been read yet */
  uint16_t frame_crc;  /* hml_crc16 over that record's payload */
  uint16_t file_crc;   /* chained hml_crc16 over every record read since the last rewind */
} hml_fs_stats;

/* The "filestream" hml_sensor_ops vtable. */
const hml_sensor_ops *hml_filestream_ops(void);

/* No sensor registry exists in the v1 headers, so this is a no-op kept for
 * symmetry with the transports, exactly like hml_synth_register(). */
hml_status hml_filestream_register(void);

/* Last error, as a human-readable sentence ("record 7: payload truncated ...").
 * Cleared by the next successful call that touches the file. open() also parks
 * its reason here when it fails, so a caller must close() the sensor after a
 * failed open; close() is a no-op when nothing was allocated. */
const char *hml_filestream_last_error(const hml_sensor_t *s);

/* Counters. Safe on a NULL sensor, which yields all zeroes. */
void hml_filestream_get_stats(const hml_sensor_t *s, hml_fs_stats *out);

/*
 * Load the next record if none is in flight and report its length in `out`.
 * A clean end of file is HML_OK with *out == 0; any other status is a malformed
 * record, and hml_filestream_last_error() then says which record and why. The
 * length of a variable-length record is not knowable from the HAL alone, so a
 * caller that wants whole frames (to hash them, say) peeks first and then
 * acquire()s exactly that many bytes.
 */
hml_status hml_filestream_peek(const hml_sensor_t *s, size_t *out);

/* ---- writer: the other end of the same format ---------------------------- */

typedef struct hml_fs_writer hml_fs_writer_t;

/* Truncate/create `path` for writing. */
hml_status hml_fs_writer_open(hml_fs_writer_t **out, const char *path);

/* Append one record. `crc_out` (may be NULL) receives the hml_crc16 computed
 * over `frame`, i.e. the same value the reader will report for that record. */
hml_status hml_fs_writer_add(hml_fs_writer_t *w, const uint8_t *frame, size_t n, uint16_t *crc_out);

/* Flush, close and free. Frees `w` even when the flush fails. */
hml_status hml_fs_writer_close(hml_fs_writer_t *w);

/* Records and payload bytes written, and the same chained CRC the reader keeps. */
void hml_fs_writer_stats(const hml_fs_writer_t *w, uint64_t *records, uint64_t *bytes,
                         uint16_t *file_crc);

/* Last writer error, or "" if none. Valid until the next writer call. */
const char *hml_fs_writer_last_error(const hml_fs_writer_t *w);

#ifdef __cplusplus
}
#endif

#endif /* HML_FILESTREAM_PRIV_H */
