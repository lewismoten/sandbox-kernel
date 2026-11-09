/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* HDMI-CEC frame parser and builders (host-testable, no hardware). */
#include "cec_msg.h"

int cec_parse(struct cec_msg *m, const uint8_t *bytes, unsigned len) {
    if (!bytes || len == 0 || len > CEC_MAX_LEN) return -1;
    unsigned init = bytes[0] >> 4, dst = bytes[0] & 15u;
    if (len == 1 && dst == CEC_ADDR_BROADCAST) return -1;       /* broadcast poll is meaningless */
    if (init == CEC_ADDR_BROADCAST && dst != CEC_ADDR_BROADCAST && len > 1) return -1;
    m->len = (uint8_t)len;
    for (unsigned i = 0; i < len; i++) m->b[i] = bytes[i];
    for (unsigned i = len; i < CEC_MAX_LEN; i++) m->b[i] = 0;
    return 0;
}
unsigned cec_initiator(const struct cec_msg *m) { return m->b[0] >> 4; }
unsigned cec_dest(const struct cec_msg *m) { return m->b[0] & 15u; }
int cec_is_broadcast(const struct cec_msg *m) { return cec_dest(m) == CEC_ADDR_BROADCAST; }
int cec_is_poll(const struct cec_msg *m) { return m->len == 1; }
int cec_opcode(const struct cec_msg *m) { return m->len >= 2 ? m->b[1] : -1; }
unsigned cec_nparams(const struct cec_msg *m) { return m->len >= 2 ? (unsigned)m->len - 2u : 0u; }
const uint8_t *cec_params(const struct cec_msg *m) { return m->b + 2; }

static int start(struct cec_msg *m, unsigned src, unsigned dst, uint8_t op) {
    if (src > 15u || dst > 15u) return -1;
    m->len = 2;
    m->b[0] = (uint8_t)(src << 4 | dst);
    m->b[1] = op;
    for (unsigned i = 2; i < CEC_MAX_LEN; i++) m->b[i] = 0;
    return 0;
}
int cec_build_poll(struct cec_msg *m, unsigned addr) {
    if (addr > 14u) return -1;
    if (start(m, addr, addr, 0) < 0) return -1;
    m->len = 1;
    m->b[1] = 0;
    return 0;
}
int cec_build_report_physical_address(struct cec_msg *m, unsigned src, uint16_t phys, unsigned devtype) {
    if (start(m, src, CEC_ADDR_BROADCAST, CEC_OP_REPORT_PHYSICAL_ADDR) < 0 || devtype > 255u) return -1;
    m->b[2] = (uint8_t)(phys >> 8); m->b[3] = (uint8_t)phys; m->b[4] = (uint8_t)devtype;
    m->len = 5;
    return 0;
}
int cec_build_set_osd_name(struct cec_msg *m, unsigned src, unsigned dst, const char *name) {
    unsigned n = 0;
    if (!name || start(m, src, dst, CEC_OP_SET_OSD_NAME) < 0) return -1;
    while (name[n]) {
        if (n >= CEC_OSD_NAME_MAX) return -1;
        m->b[2 + n] = (uint8_t)name[n];
        n++;
    }
    if (n == 0) return -1;
    m->len = (uint8_t)(2 + n);
    return 0;
}
int cec_build_feature_abort(struct cec_msg *m, unsigned src, unsigned dst, uint8_t opcode, uint8_t reason) {
    if (dst == CEC_ADDR_BROADCAST || start(m, src, dst, CEC_OP_FEATURE_ABORT) < 0) return -1;
    m->b[2] = opcode; m->b[3] = reason;
    m->len = 4;
    return 0;
}
int cec_build_image_view_on(struct cec_msg *m, unsigned src, unsigned dst) {
    return start(m, src, dst, CEC_OP_IMAGE_VIEW_ON);
}
int cec_build_active_source(struct cec_msg *m, unsigned src, uint16_t phys) {
    if (start(m, src, CEC_ADDR_BROADCAST, CEC_OP_ACTIVE_SOURCE) < 0) return -1;
    m->b[2] = (uint8_t)(phys >> 8); m->b[3] = (uint8_t)phys;
    m->len = 4;
    return 0;
}
int cec_build_report_power_status(struct cec_msg *m, unsigned src, unsigned dst, uint8_t status) {
    if (start(m, src, dst, CEC_OP_REPORT_POWER_STATUS) < 0) return -1;
    m->b[2] = status; m->len = 3;
    return 0;
}
int cec_build_cec_version(struct cec_msg *m, unsigned src, unsigned dst, uint8_t version) {
    if (start(m, src, dst, CEC_OP_CEC_VERSION) < 0) return -1;
    m->b[2] = version; m->len = 3;
    return 0;
}
