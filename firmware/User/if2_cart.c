#include "if2_cart.h"
#include "zx_bus.h"
#include "ff.h"

#include <stdio.h>
#include <string.h>

#define IF2_ROM_SIZE 0x4000u   /* 16 KB Interface 2 window */

/* SRAM-resident cartridge image.  Sized to 16 KB so any shorter .ROM is
   zero-padded into the upper bytes (matches an unpopulated EPROM). */
static uint8_t s_if2_rom[IF2_ROM_SIZE];

int IF2_LoadRomFromFile (const char *path) {
    FIL fp;
    FRESULT fr;
    UINT br = 0u;
    FSIZE_t size;

    if ((path == NULL) || (path[0] == '\0')) {
        return 0;
    }

    fr = f_open (&fp, path, FA_READ);
    if (fr != FR_OK) {
        printf ("if2: f_open('%s') failed (fr=%d)\r\n", path, (int)fr);
        return 0;
    }

    size = f_size (&fp);
    if (size == 0u) {
        printf ("if2: '%s' is empty\r\n", path);
        f_close (&fp);
        return 0;
    }
    if ((uint32_t)size > IF2_ROM_SIZE) {
        printf ("if2: '%s' is %lu bytes (max %u) — truncating\r\n",
                path, (unsigned long)size, (unsigned)IF2_ROM_SIZE);
        size = IF2_ROM_SIZE;
    }

    memset (s_if2_rom, 0xFFu, sizeof (s_if2_rom));   /* unprogrammed EPROM = 0xFF */

    fr = f_read (&fp, s_if2_rom, (UINT)size, &br);
    f_close (&fp);
    if ((fr != FR_OK) || ((FSIZE_t)br != size)) {
        printf ("if2: f_read('%s') failed (fr=%d br=%u/%lu)\r\n",
                path, (int)fr, (unsigned)br, (unsigned long)size);
        return 0;
    }

    printf ("if2: loaded %lu bytes from '%s', booting into cartridge\r\n",
            (unsigned long)size, path);

    ZX_BecomeInterface2 (s_if2_rom);
    ZX_Z80Reset();
    return 1;
}
