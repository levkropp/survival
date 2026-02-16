/*
 * flasher.h — Orchestrator: GPT + FAT32 format + decompress + write
 */
#ifndef FLASHER_H
#define FLASHER_H

/* Run the full flash sequence for the given architecture.
 * arch: "aarch64" or "x86_64"
 * SD card must be initialized before calling.
 * Returns 0 on success. */
int flasher_run(const char *arch);

#endif /* FLASHER_H */
