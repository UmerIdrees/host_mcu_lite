#include "hml_filestream.h"

#include "hml/hml_crc.h"
#include "hml/hml_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * File-stream sensor. The HAL is open/acquire/release/close, so this is the
 * same shape as the synthetic adapter: acquire() hands out the next slice of
 * the frame in flight and returns 0 at a clean end of file, and the driver
 * loops. The difference is that a file record is a whole frame of a length the
 * HAL does not carry, so hml_filestream_peek() reports that length first.
 *
 * Every malformed record fails closed with a message naming the record index;
 * nothing is ever partially delivered.
 */
typedef struct hml_fs_impl {
  hml_fs_cfg cfg;
  FILE *f;
  uint8_t *buf;      /* payload of the record in flight */
  size_t buf_len;    /* its length, 0 when none is loaded */
  size_t buf_off;    /* cursor into it */
  bool loaded;       /* buf holds a record */
  bool eof;          /* clean end of file reached */
  hml_fs_stats st;
  char err[128];
} hml_fs_impl;

struct hml_fs_writer {
  FILE *f;
  uint64_t records;
  uint64_t bytes;
  uint16_t file_crc;
  char err[128];
};

static void fs_put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void fs_put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static uint32_t fs_get_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Chained whole-file CRC-16. hml_crc16() is one-shot, so the chain folds a
 * per-record digest -- (previous chain, this record's payload CRC, this
 * record's length) -- instead of re-reading the bytes. Reset to 0 on every
 * rewind, so one pass of a --repeat run and a single pass of a plain run
 * produce the same value and a soak run can be compared against a short one.
 */
static uint16_t fs_chain(uint16_t prev, uint16_t frame_crc, uint32_t len) {
  uint8_t d[8];
  fs_put_le16(d + 0, prev);
  fs_put_le16(d + 2, frame_crc);
  fs_put_le32(d + 4, len);
  return hml_crc16(d, sizeof(d));
}

static hml_status fs_failf(hml_fs_impl *im, hml_status st, const char *fmt, uint64_t index,
                           const char *detail) {
  snprintf(im->err, sizeof(im->err), fmt, (unsigned long long)index, detail);
  return st;
}

/* Read one record into im->buf. Sets im->eof at a clean end of file. */
static hml_status fs_load(hml_fs_impl *im) {
  uint8_t hdr[HML_FS_REC_HDR];
  const uint64_t index = im->st.index;

  size_t n = fread(hdr, 1, sizeof(hdr), im->f);
  if (n == 0 && feof(im->f)) {
    if (!im->cfg.repeat) {
      im->eof = true;
      return HML_OK;
    }
    if (fseek(im->f, 0L, SEEK_SET) != 0) {
      snprintf(im->err, sizeof(im->err), "rewind failed: %s", strerror(errno));
      return HML_ERR_IO;
    }
    clearerr(im->f);
    im->st.rewinds++;
    im->st.index = 0;
    im->st.file_crc = 0; /* one chain per pass, see fs_chain() */
    n = fread(hdr, 1, sizeof(hdr), im->f);
    if (n != sizeof(hdr))
      return fs_failf(im, HML_ERR_CRC, "record %llu: %s", 0ull, "file is shorter after a rewind");
  }
  if (n != sizeof(hdr)) return fs_failf(im, HML_ERR_CRC, "record %llu: %s", index,
                                        "truncated record header");
  if (memcmp(hdr, HML_FS_MAGIC, HML_FS_MAGIC_LEN) != 0) {
    snprintf(im->err, sizeof(im->err), "record %llu: bad magic %02x%02x%02x%02x, expected \"HMLF\"",
             (unsigned long long)index, (unsigned)hdr[0], (unsigned)hdr[1], (unsigned)hdr[2],
             (unsigned)hdr[3]);
    return HML_ERR_PROTO;
  }

  const uint32_t len = fs_get_le32(hdr + HML_FS_MAGIC_LEN);
  if (len == 0) {
    snprintf(im->err, sizeof(im->err), "record %llu: zero-length frame", (unsigned long long)index);
    return HML_ERR_PROTO;
  }
  if ((size_t)len > im->cfg.max_frame) {
    snprintf(im->err, sizeof(im->err), "record %llu: frame_len %lu exceeds the %zu byte limit",
             (unsigned long long)index, (unsigned long)len, im->cfg.max_frame);
    return HML_ERR_TOOLARGE;
  }

  if (im->buf_len < (size_t)len) {
    uint8_t *grown = (uint8_t *)realloc(im->buf, (size_t)len);
    if (!grown) {
      snprintf(im->err, sizeof(im->err), "out of memory for a %lu byte frame", (unsigned long)len);
      return HML_ERR_NOMEM;
    }
    im->buf = grown;
    im->buf_len = (size_t)len;
  }

  size_t got = fread(im->buf, 1, (size_t)len, im->f);
  if (got != (size_t)len) {
    snprintf(im->err, sizeof(im->err),
             "record %llu: payload truncated, %zu of %lu bytes present, crc=0x%04x over the %zu "
             "bytes read",
             (unsigned long long)index, got, (unsigned long)len, (unsigned)hml_crc16(im->buf, got),
             got);
    return HML_ERR_CRC;
  }

  im->st.frame_crc = hml_crc16(im->buf, (size_t)len);
  im->st.file_crc = fs_chain(im->st.file_crc, im->st.frame_crc, len);
  im->st.frame_bytes = (size_t)len;
  im->st.index++;
  im->buf_off = 0;
  im->loaded = true;
  im->err[0] = '\0';
  return HML_OK;
}

static hml_status fs_open(hml_sensor_t *s, void *param) {
  if (!s) return HML_ERR_ARG;
  if (s->impl) return HML_ERR_STATE;

  hml_fs_cfg cfg;
  memset(&cfg, 0, sizeof(cfg));
  if (param) cfg = *(const hml_fs_cfg *)param;
  if (!cfg.path) return HML_ERR_ARG;
  if (cfg.max_frame == 0) cfg.max_frame = (size_t)HML_MAX_FRAME_SIZE;
  if (cfg.max_frame > (size_t)HML_MAX_FRAME_SIZE) cfg.max_frame = (size_t)HML_MAX_FRAME_SIZE;

  hml_fs_impl *im = (hml_fs_impl *)calloc(1, sizeof(*im));
  if (!im) return HML_ERR_NOMEM;
  im->cfg = cfg;
  im->f = fopen(cfg.path, "rb");
  if (!im->f) {
    /* The impl is kept so the reason survives in hml_filestream_last_error();
     * close() frees it and is a no-op on a NULL FILE*. */
    snprintf(im->err, sizeof(im->err), "cannot open '%s': %s", cfg.path, strerror(errno));
    s->impl = im;
    return HML_ERR_IO;
  }
  s->impl = im;
  return HML_OK;
}

static hml_status fs_acquire(hml_sensor_t *s, uint8_t *dst, size_t cap, size_t *out) {
  if (!s || !out) return HML_ERR_ARG;
  *out = 0;
  if (cap > 0 && !dst) return HML_ERR_ARG;

  hml_fs_impl *im = (hml_fs_impl *)s->impl;
  if (!im || !im->f) return HML_ERR_STATE;

  if (!im->loaded) {
    hml_status st = fs_load(im);
    if (st != HML_OK) return st;
    if (im->eof) return HML_OK; /* *out == 0: clean end of file */
  }

  size_t left = im->buf_len - im->buf_off;
  size_t n = cap < left ? cap : left;
  if (n) memcpy(dst, im->buf + im->buf_off, n);
  im->buf_off += n;
  *out = n;

  if (im->buf_off == im->buf_len) {
    im->loaded = false;
    im->st.records++;
    im->st.bytes += (uint64_t)im->buf_len;
  }
  return HML_OK;
}

static hml_status fs_release(hml_sensor_t *s) {
  if (!s) return HML_ERR_ARG;
  return s->impl ? HML_OK : HML_ERR_STATE;
}

static void fs_close(hml_sensor_t *s) {
  if (!s || !s->impl) return;
  hml_fs_impl *im = (hml_fs_impl *)s->impl;
  if (im->f) fclose(im->f);
  free(im->buf);
  free(im);
  s->impl = NULL;
}

static const hml_sensor_ops g_filestream_ops = {"filestream", fs_open,   fs_acquire, fs_release,
                                                fs_close};

const hml_sensor_ops *hml_filestream_ops(void) { return &g_filestream_ops; }

hml_status hml_filestream_register(void) { return HML_OK; }

hml_status hml_filestream_peek(const hml_sensor_t *s, size_t *out) {
  if (!s || !out) return HML_ERR_ARG;
  *out = 0;
  if (!s->impl) return HML_ERR_STATE;
  hml_fs_impl *im = (hml_fs_impl *)s->impl;
  if (im->loaded) {
    *out = im->buf_len;
    return HML_OK;
  }
  hml_status st = fs_load(im);
  if (st != HML_OK) return st;
  *out = im->eof ? 0u : im->buf_len;
  return HML_OK;
}

void hml_filestream_get_stats(const hml_sensor_t *s, hml_fs_stats *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!s || !s->impl) return;
  *out = ((const hml_fs_impl *)s->impl)->st;
}

const char *hml_filestream_last_error(const hml_sensor_t *s) {
  if (!s || !s->impl) return "";
  return ((const hml_fs_impl *)s->impl)->err;
}

/* ---- writer ------------------------------------------------------------- */

hml_status hml_fs_writer_open(hml_fs_writer_t **out, const char *path) {
  if (!out || !path) return HML_ERR_ARG;
  *out = NULL;
  hml_fs_writer_t *w = (hml_fs_writer_t *)calloc(1, sizeof(*w));
  if (!w) return HML_ERR_NOMEM;
  w->f = fopen(path, "wb");
  if (!w->f) {
    snprintf(w->err, sizeof(w->err), "cannot create '%s': %s", path, strerror(errno));
    free(w);
    return HML_ERR_IO;
  }
  *out = w;
  return HML_OK;
}

hml_status hml_fs_writer_add(hml_fs_writer_t *w, const uint8_t *frame, size_t n, uint16_t *crc_out) {
  if (crc_out) *crc_out = 0;
  if (!w || !w->f || !frame) return HML_ERR_ARG;
  if (n == 0) {
    snprintf(w->err, sizeof(w->err), "refusing to write a zero-length frame");
    return HML_ERR_ARG;
  }
  if (n > (size_t)HML_MAX_FRAME_SIZE) {
    snprintf(w->err, sizeof(w->err), "frame of %zu bytes exceeds the %u byte limit", n,
             (unsigned)HML_MAX_FRAME_SIZE);
    return HML_ERR_TOOLARGE;
  }

  uint8_t hdr[HML_FS_REC_HDR];
  memcpy(hdr, HML_FS_MAGIC, HML_FS_MAGIC_LEN);
  fs_put_le32(hdr + HML_FS_MAGIC_LEN, (uint32_t)n);

  if (fwrite(hdr, 1, sizeof(hdr), w->f) != sizeof(hdr) ||
      fwrite(frame, 1, n, w->f) != n) {
    snprintf(w->err, sizeof(w->err), "write of a %zu byte record failed: %s", n, strerror(errno));
    return HML_ERR_IO;
  }

  const uint16_t crc = hml_crc16(frame, n);
  w->file_crc = fs_chain(w->file_crc, crc, (uint32_t)n);
  w->records++;
  w->bytes += (uint64_t)n;
  w->err[0] = '\0';
  if (crc_out) *crc_out = crc;
  return HML_OK;
}

hml_status hml_fs_writer_close(hml_fs_writer_t *w) {
  if (!w) return HML_ERR_ARG;
  hml_status st = HML_OK;
  if (w->f) {
    if (fflush(w->f) != 0 || fclose(w->f) != 0) {
      snprintf(w->err, sizeof(w->err), "close failed: %s", strerror(errno));
      st = HML_ERR_IO;
    }
    w->f = NULL;
  }
  free(w);
  return st;
}

void hml_fs_writer_stats(const hml_fs_writer_t *w, uint64_t *records, uint64_t *bytes,
                         uint16_t *file_crc) {
  if (records) *records = w ? w->records : 0u;
  if (bytes) *bytes = w ? w->bytes : 0u;
  if (file_crc) *file_crc = w ? w->file_crc : 0u;
}

const char *hml_fs_writer_last_error(const hml_fs_writer_t *w) { return w ? w->err : ""; }
