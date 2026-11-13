/* SPDX-License-Identifier: MIT; apps: the manifest (APP.JSON) validator, the capability set, the path
 * policy and the AUTORUN line. Pure C with no interpreter and no platform calls, so the host tests and
 * the board build share it (tests/host/app_test.c, tests/host/appcheck.c; js/tools/app_check.py is the
 * same validator for app authors). docs/apps.md is the author's guide. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#define APP_API_VERSION 1u /* what a manifest's "api" is compared with: the runtime must be at least that new */
/* Capabilities (the contract.json names). The first four are always granted. */
enum {
  APP_CAP_LOG = 1u << 0, APP_CAP_TIME = 1u << 1, APP_CAP_MEMORY = 1u << 2, APP_CAP_LIFECYCLE = 1u << 3,
  APP_CAP_FS_READ = 1u << 4,        /* "fs.read": the app's own folder and its save folder */
  APP_CAP_FS_READ_SHARED = 1u << 5, /* "fs.read.shared": any /sd file outside /sd/OS (games, music) */
  APP_CAP_FS_WRITE_SAVES = 1u << 6, /* "fs.write.saves": /sd/OS/SAVES/<id>/ */
  APP_CAP_FS_WRITE_CACHE = 1u << 7, /* "fs.write.cache": the file /sd/OS/CACHE/<id> */
  APP_CAP_DISPLAY = 1u << 8, APP_CAP_INPUT = 1u << 9, APP_CAP_AUDIO = 1u << 10, APP_CAP_EINK = 1u << 11,
  APP_CAP_SERIAL = 1u << 12, APP_CAP_POWER = 1u << 13,
  APP_CAP_APPS_LAUNCH = 1u << 14, /* "apps.launch": apps.list(), apps.launch(), apps.exit() */
  APP_CAP_ALL = (1u << 15) - 1u, APP_CAP_BASE = 0xfu
};
struct app_manifest {
  char name[33], version[16], entry[13];
  uint32_t api, caps; /* caps always includes APP_CAP_BASE */
};
const char *app_cap_name(unsigned bit_index);         /* "fs.read"; NULL past the last */
uint32_t app_cap_bit(const char *name);               /* 0 when unknown */
#define APP_MANIFEST_MAX 2048u
/* 0 when valid; else -1 with a short plain message in err (it fits an on-screen row). */
int app_manifest_parse(const char *json, uint32_t n, struct app_manifest *m, char *err, size_t errcap);
/* An app's folders, from its id (the folder name under /sd/OS/APPS). */
struct app_policy {char dir[48], saves[48], cache[48]; uint32_t caps;};
void app_policy_init(struct app_policy *p, const char *id, uint32_t caps);
/* Checks a path an app wants to read (write = 0) or write (1). 0 when allowed; else -1 with a reason in why. */
int app_path_check(const struct app_policy *p, const char *path, int write, const char **why);
/* The kernel's AUTORUN.TXT: line 1 is the kernel's, line 2 names the app (APPS/NAME or PICKER).
   0: dir is "/sd/OS/APPS/NAME" (empty for the picker); -1: a bad line, a message in err (dir is empty). */
int app_autorun_parse(const char *text, uint32_t n, char *dir, size_t dircap, char *err, size_t errcap);
/* The id (folder name) from "/sd/OS/APPS/NAME"; 0 when dir is not that shape. */
const char *app_id_of(const char *dir);
