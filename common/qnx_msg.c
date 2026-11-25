/*
 * qnx_msg.c — framing for the qnx-mqtt-gateway (see qnx_msg.h / DESIGN.md).
 * Portable C, no QNX or Mosquitto dependency.
 */
#include "qnx_msg.h"

#include <string.h>

static void put_u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, sizeof(v));
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

int qnx_msg_pack(const char *topic, const void *payload, size_t payload_len,
                 uint8_t *out, size_t cap, size_t *out_len)
{
    size_t topic_len;
    size_t total;

    if (!topic || !out || !out_len) return QNX_MSG_ERR_ARG;
    if (payload_len > 0 && !payload) return QNX_MSG_ERR_ARG;

    topic_len = strlen(topic);
    if (topic_len == 0 || topic_len > QNX_MSG_MAX_TOPIC_LEN)
        return QNX_MSG_ERR_ARG;

    /* 12-byte header + topic + payload must fit in cap and in the 4096 cap. */
    total = QNX_MSG_HEADER_LEN + topic_len + payload_len;
    if (total > cap || total > QNX_MSG_MAX_TOTAL)
        return QNX_MSG_ERR_TOOBIG;

    put_u32(out + 0, QNX_MSG_MAGIC);
    put_u32(out + 4, (uint32_t)topic_len);
    put_u32(out + 8, (uint32_t)payload_len);
    memcpy(out + QNX_MSG_HEADER_LEN, topic, topic_len);
    if (payload_len > 0)
        memcpy(out + QNX_MSG_HEADER_LEN + topic_len, payload, payload_len);

    *out_len = total;
    return QNX_MSG_OK;
}

int qnx_msg_unpack(const uint8_t *buf, size_t len,
                   char *topic_out, size_t topic_cap,
                   const uint8_t **payload_out, size_t *payload_len_out)
{
    uint32_t topic_len;
    uint32_t payload_len;
    size_t total;

    if (!buf || !topic_out || !payload_out || !payload_len_out)
        return QNX_MSG_ERR_ARG;
    if (len < QNX_MSG_HEADER_LEN)
        return QNX_MSG_ERR_TRUNC;
    if (get_u32(buf + 0) != QNX_MSG_MAGIC)
        return QNX_MSG_ERR_MAGIC;

    topic_len = get_u32(buf + 4);
    payload_len = get_u32(buf + 8);

    if (topic_len == 0 || topic_len > QNX_MSG_MAX_TOPIC_LEN)
        return QNX_MSG_ERR_TOPIC;

    /* Bounds-check the claimed lengths against the received byte count.
     * Use size_t arithmetic so a lying length cannot wrap around. */
    total = QNX_MSG_HEADER_LEN + (size_t)topic_len + (size_t)payload_len;
    if (total > QNX_MSG_MAX_TOTAL || total != len)
        return QNX_MSG_ERR_TRUNC;

    if (topic_cap <= (size_t)topic_len)
        return QNX_MSG_ERR_TOPIC;

    memcpy(topic_out, buf + QNX_MSG_HEADER_LEN, topic_len);
    topic_out[topic_len] = '\0';
    *payload_out = buf + QNX_MSG_HEADER_LEN + topic_len;
    *payload_len_out = payload_len;
    return QNX_MSG_OK;
}
