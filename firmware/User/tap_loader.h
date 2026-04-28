#ifndef __TAP_LOADER_H
#define __TAP_LOADER_H

#include <stdint.h>

/* Result codes returned by the .tap loader. */
#define TAP_OK              0
#define TAP_ERR_OPEN        1
#define TAP_ERR_READ        2
#define TAP_ERR_FORMAT      3
#define TAP_ERR_CHECKSUM    4
#define TAP_ERR_NMI_WRITE   5
#define TAP_ERR_NO_CODE     6

/* Parses a .tap file from the FATFS volume and prints a summary of every
   block (header type, filename, size, load address, checksum status). Does
   not modify ZX RAM. Returns TAP_OK on success. */
int Tap_Info (const char *path);

/* Streams the .tap file from FATFS, writing every CODE (type 3) data block
   into ZX RAM via NMI writes. BASIC / array headers are listed but skipped
   (their data blocks are also skipped). On success, *out_start_addr is set
   to the load address of the first CODE block (a sensible default RANDOMIZE
   USR target) and *out_block_count to the number of CODE blocks loaded. */
int Tap_Load (const char *path, uint16_t *out_start_addr, int *out_block_count);

/* Convenience: load the file and immediately hand control to start_addr by
   calling ZX_LaunchZ80(). If start_addr == 0 the start address from
   Tap_Load() is used. */
int Tap_LoadAndRun (const char *path, uint16_t start_addr);

#endif
