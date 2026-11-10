/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define PERIPH 0x20000000u
#define MAX_IMAGE (2u*1024u*1024u)
#define RUNTIME_BASE 0x100000u
#define PROFILE 1u
#ifdef HOST_TEST
static inline void barrier(void) {}
#else
static inline void barrier(void) { uint32_t z=0; __asm__ volatile("mcr p15,0,%0,c7,c10,5"::"r"(z):"memory"); }
#endif
#ifdef HOST_TEST
uint32_t micros(void);
#else
static inline uint32_t micros(void) { return REG(PERIPH+0x3004); }
#endif
static inline bool expired(uint32_t start,uint32_t us) { return (uint32_t)(micros()-start)>=us; }
void gpio_mode(unsigned pin,unsigned mode);
void gpio_set(unsigned pin,bool value);
bool gpio_get(unsigned pin);
void delay_us(uint32_t us);
void uart_init(void);
void uart_puts(const char *s);
void irq_init(void);
void irq_handler(void);
extern volatile uint32_t ticks;
void mmu_init(void);
void watchdog_feed(void);
void reboot(void) __attribute__((noreturn));
void panic(unsigned kind) __attribute__((noreturn));
bool property(uint32_t tag,uint32_t *values,unsigned words);
extern const char *property_fail; /* why the last property call failed */
bool property_message(uint32_t *message,unsigned words,uint32_t timeout_us);
bool mmu_map_uncached(uint32_t phys,uint32_t bytes);
const uint32_t *mmu_table(void); /* the privileged L1 table */
void service_io(void);
bool sd_init(void);
bool sd_init_after_handoff(const uint32_t in[8]);
void sd_export(uint32_t out[8]); /* selector: card state for the runtime */
bool sd_adopt(const uint32_t in[8]); /* runtime: take it over if the card is still in transfer state */
extern const char *sd_fail; /* short code of the failing init step */
extern const char *store_fail; /* short code of the failing store_init step */
bool sd_read(uint32_t lba,void *data);
bool sd_write(uint32_t lba,const void *data);
uint32_t sd_sectors(void);
uint32_t crc32(const void *p,size_t n);
void sha256(const void *p,size_t n,uint8_t out[32]);
void number(char *out,uint32_t n);
void uptime(char *out,uint32_t seconds);
void log_line(const char *s);
void cpu_speed(void); /* the ARM clock to its maximum (firmware mailbox), branch prediction on; logs both */
void boot_note(const char *s); /* log now and again for each new USB reader */
void boot_summary(void);
void asset_probe(void);
extern const char *last_error;
void error(const char *s);
void panel_start(void);
bool panel_boot_done(void);
void panel_task(const char *state,uint32_t seconds);
void usb_init(void);
void usb_irq(void);
bool usb_healthy(void);
void usb_task(void);
const char *usb_state(void);
void protocol_task(void);
bool protocol_busy(void);
bool verify_confirmed(void);
void protocol_reset(void);
void usb_reply(const char *s);
void usb_log(const char *s);
bool usb_reply_pending(void);
/* Wire format is LE uint32 fields, never host native structs on disk. */
#define IMAGE_MAGIC 0x31494b53u
#define CONTROL_MAGIC 0x31434b53u
#define NO_SLOT 0xffffffffu
#define HANDOFF_MAGIC 0x31484b53u
/* image header, 256 bytes: magic/version/profile/ABI/payload size/load/entry,
 * reserved word, SHA256[32], build ID[32], zero padding. */
bool image_valid(const uint8_t *image,uint32_t bytes);
struct file_extent { uint32_t sectors[4097]; uint32_t count,bytes; };
extern struct file_extent slots[2],controls[2];
extern uint32_t control[128];
/* control words: magic,version,sequence,confirmed,pending,consumed,
 * confirmed generation,pending generation,no-OS, then zero to word126; CRC127. */
bool store_init(void);
extern const uint32_t *sd_handoff_block; /* runtime: selector card state (8 words), else NULL */
bool file_read(const struct file_extent *f,uint32_t sector,void *data);
bool file_write(const struct file_extent *f,uint32_t sector,const void *data);
bool control_commit(const uint32_t next[128]);
bool control_healthy(void);
bool slot_load(unsigned slot,uint8_t *dest,uint32_t *bytes);
void selector_main(void) __attribute__((noreturn));
void kernel_main(void) __attribute__((noreturn));
