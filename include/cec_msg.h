/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* HDMI-CEC frame parser and builders. Pure C: no hardware, no allocation.
 * Opcode and operand values are facts cross-checked in docs/cec.md. */
#ifndef CEC_MSG_H
#define CEC_MSG_H
#include <stdint.h>

#define CEC_MAX_LEN 16u
#define CEC_ADDR_TV 0u
#define CEC_ADDR_PLAYBACK_1 4u
#define CEC_ADDR_PLAYBACK_2 8u
#define CEC_ADDR_PLAYBACK_3 11u
#define CEC_ADDR_BROADCAST 15u /* destination 15; as initiator it means unregistered */
#define CEC_PHYS_UNKNOWN 0xFFFFu
#define CEC_DEVTYPE_PLAYBACK 4u
#define CEC_VERSION_1_4 5u
#define CEC_OSD_NAME_MAX 14u

#define CEC_OP_FEATURE_ABORT 0x00u
#define CEC_OP_IMAGE_VIEW_ON 0x04u
#define CEC_OP_STANDBY 0x36u
#define CEC_OP_USER_CONTROL_PRESSED 0x44u
#define CEC_OP_USER_CONTROL_RELEASED 0x45u
#define CEC_OP_GIVE_OSD_NAME 0x46u
#define CEC_OP_SET_OSD_NAME 0x47u
#define CEC_OP_ACTIVE_SOURCE 0x82u
#define CEC_OP_GIVE_PHYSICAL_ADDR 0x83u
#define CEC_OP_REPORT_PHYSICAL_ADDR 0x84u
#define CEC_OP_REQUEST_ACTIVE_SOURCE 0x85u
#define CEC_OP_SET_STREAM_PATH 0x86u
#define CEC_OP_DEVICE_VENDOR_ID 0x87u
#define CEC_OP_GIVE_DEVICE_VENDOR_ID 0x8Cu
#define CEC_OP_MENU_REQUEST 0x8Du
#define CEC_OP_MENU_STATUS 0x8Eu
#define CEC_OP_GIVE_POWER_STATUS 0x8Fu
#define CEC_OP_REPORT_POWER_STATUS 0x90u
#define CEC_OP_CEC_VERSION 0x9Eu
#define CEC_OP_GET_CEC_VERSION 0x9Fu

#define CEC_ABORT_UNRECOGNIZED 0u
#define CEC_POWER_ON 0u
#define CEC_MENU_ACTIVATED 0u
#define CEC_MENU_DEACTIVATED 1u
#define CEC_VENDOR_UNKNOWN 0u /* 24-bit vendor id sent when asked; none is claimed */

struct cec_msg {
    uint8_t len;               /* 1..16; header only = a poll */
    uint8_t b[CEC_MAX_LEN];    /* b[0] = initiator<<4 | destination */
};

/* Copy and validate raw bytes. 0 on success, -1 if len is 0 or over 16, or the
 * initiator is 15 on a directed frame, or a poll is broadcast. */
int cec_parse(struct cec_msg *m, const uint8_t *bytes, unsigned len);
unsigned cec_initiator(const struct cec_msg *m);
unsigned cec_dest(const struct cec_msg *m);
int cec_is_broadcast(const struct cec_msg *m);
int cec_is_poll(const struct cec_msg *m);             /* header only */
int cec_opcode(const struct cec_msg *m);              /* -1 for a poll */
unsigned cec_nparams(const struct cec_msg *m);        /* operand bytes after opcode */
const uint8_t *cec_params(const struct cec_msg *m);

/* Builders return 0, or -1 on a bad address (over 15) or name. */
int cec_build_poll(struct cec_msg *m, unsigned addr);
int cec_build_report_physical_address(struct cec_msg *m, unsigned src, uint16_t phys, unsigned devtype);
int cec_build_set_osd_name(struct cec_msg *m, unsigned src, unsigned dst, const char *name);
int cec_build_feature_abort(struct cec_msg *m, unsigned src, unsigned dst, uint8_t opcode, uint8_t reason);
int cec_build_image_view_on(struct cec_msg *m, unsigned src, unsigned dst);
int cec_build_active_source(struct cec_msg *m, unsigned src, uint16_t phys);
int cec_build_report_power_status(struct cec_msg *m, unsigned src, unsigned dst, uint8_t status);
int cec_build_menu_status(struct cec_msg *m, unsigned src, unsigned dst, uint8_t state);
int cec_build_device_vendor_id(struct cec_msg *m, unsigned src, uint32_t vendor); /* broadcast */
int cec_build_cec_version(struct cec_msg *m, unsigned src, unsigned dst, uint8_t version);
#endif
