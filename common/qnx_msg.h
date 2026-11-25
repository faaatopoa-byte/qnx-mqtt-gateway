/*
 * qnx_msg.h — wire framing for the qnx-mqtt-gateway (see DESIGN.md).
 *
 * One framed message, max 4096 bytes total:
 *
 *   uint32_t magic;       // 'QMBR' = 0x514D4252
 *   uint32_t topic_len;   // bytes, no NUL
 *   uint32_t payload_len; // bytes, no NUL
 *   uint8_t  data[];      // topic bytes, then payload bytes
 *
 * Byte order: host byte order on both ends. Both endpoints run on the same
 * QNX machine (plugin inside the broker, apps via message passing), so the
 * order is identical by construction. Fields are copied with memcpy, never
 * by casting the buffer to the struct, so there are no alignment/padding
 * surprises.
 *
 * Portable: no QNX or Mosquitto dependency. Used by the plugin, the demo
 * app, and the host unit tests.
 */
#ifndef QNX_MSG_H
#define QNX_MSG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QNX_MSG_MAGIC       0x514D4252u /* 'QMBR' */
#define QNX_MSG_HEADER_LEN  12          /* magic + topic_len + payload_len */
#define QNX_MSG_MAX_TOTAL   4096        /* max framed message, bytes */
#define QNX_MSG_MAX_TOPIC_LEN 255       /* topic bytes, no NUL */

/* qnx_msg_pack / qnx_msg_unpack return values (0 = ok): */
#define QNX_MSG_OK          0
#define QNX_MSG_ERR_ARG    -1  /* NULL pointer / zero capacity / empty topic */
#define QNX_MSG_ERR_MAGIC  -2  /* magic mismatch on unpack */
#define QNX_MSG_ERR_TRUNC  -3  /* buffer shorter than the lengths claim (lying length) */
#define QNX_MSG_ERR_TOOBIG -4  /* packed form would exceed cap / QNX_MSG_MAX_TOTAL */
#define QNX_MSG_ERR_TOPIC  -5  /* topic too long for topic_out (unpack) */

/*
 * Pack topic + payload into a framed message.
 * topic must be NUL-terminated, non-empty, <= QNX_MSG_MAX_TOPIC_LEN bytes.
 * On success *out_len is the framed length (12 + topic + payload).
 */
int qnx_msg_pack(const char *topic, const void *payload, size_t payload_len,
                 uint8_t *out, size_t cap, size_t *out_len);

/*
 * Unpack a framed message.
 * Strict: buf must be EXACTLY 12 + topic_len + payload_len bytes; trailing
 * bytes or a length field claiming more than the buffer holds are rejected.
 * On success topic_out is NUL-terminated (needs topic_cap > topic_len) and
 * *payload_out points into buf just past the topic bytes.
 */
int qnx_msg_unpack(const uint8_t *buf, size_t len,
                   char *topic_out, size_t topic_cap,
                   const uint8_t **payload_out, size_t *payload_len_out);

#ifdef __cplusplus
}
#endif

#endif /* QNX_MSG_H */
