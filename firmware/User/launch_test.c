/* launch_test.c — step-by-step diagnostics for the snapshot launch path.
 *
 * Each test isolates one phase of the .z80 trampoline mechanism.  Call from
 * the monitor with `launchtest <id>`.  Tests use uncontended upper-RAM
 * (0xBE80..0xBEBF) for progress markers so BUSREQ readback is reliable.
 *
 *   1 = NMI write/read roundtrip across regions (mailbox sanity)
 *   2 = marker trampoline (no POPs) — verifies SnapshotEnter+Commit, spin
 *       exit, post-spin instructions, then HALTs.  Tells us exactly how
 *       far through the trampoline the Z80 gets.
 *   3 = marker trampoline + LD SP + POPs from synthetic regblock + HALT
 *       (no user PC jump) — adds the SP/POP phase.
 *   4 = full marker trampoline + JP to a planted user stub at 0xC000 that
 *       writes a final marker — verifies handover-RET path.
 */

#include <string.h>
#include <stdio.h>
#include "ch32v30x.h"
#include "debug.h"
#include "zx_bus.h"
#include "launch_test.h"

#define LT_MARKER_BASE   0xBE80u   /* uncontended upper RAM */
#define LT_TRAMP_ADDR    0xFEC0u
#define LT_ALIVE_ADDR    0xFEFDu
#define LT_GO_ADDR       0xFEFEu
#define LT_HANDOVER_ADDR 0xFFFEu
#define LT_USER_STUB_PC  0xC000u

#define LT_NMI_TIMEOUT   400u
#define LT_REGBLOCK_ADDR 0xFE80u

/* ===== Test 1 — NMI mailbox sanity ============================== */

static int lt_test_nmi_roundtrip (void) {
    static const uint16_t addrs[] = {
        0x4000u, 0x4321u, 0x5800u, 0x7FFFu,   /* contended bank */
        0x8000u, 0xBE80u, 0xC000u, 0xFE9A,    /* uncontended */
        0xFEC0u, 0xFFFEu                       /* launch region */
    };
    static const uint8_t pats[] = { 0x55u, 0xAAu, 0x33u, 0xCCu };
    uint8_t orig[sizeof (addrs) / sizeof (addrs[0])];
    uint8_t i, p;
    int passed = 1;

    printf ("LT1: NMI write+read roundtrip across %u addresses\r\n",
            (unsigned)(sizeof (addrs) / sizeof (addrs[0])));

    /* Save originals via NMI read (ground truth). */
    for (i = 0u; i < sizeof (addrs) / sizeof (addrs[0]); ++i) {
        if (!ZX_NmiReadBlock (addrs[i], &orig[i], 1u, LT_NMI_TIMEOUT)) {
            printf ("LT1:   [%04X] save: NMI read FAILED\r\n", addrs[i]);
            return 0;
        }
    }

    for (p = 0u; p < sizeof (pats); ++p) {
        uint8_t pat = pats[p];
        for (i = 0u; i < sizeof (addrs) / sizeof (addrs[0]); ++i) {
            uint8_t rb = 0u;
            if (!ZX_NmiWriteBlock (addrs[i], &pat, 1u, LT_NMI_TIMEOUT)) {
                printf ("LT1:   [%04X]<-%02X: write FAILED\r\n", addrs[i], pat);
                passed = 0;
                continue;
            }
            if (!ZX_NmiReadBlock (addrs[i], &rb, 1u, LT_NMI_TIMEOUT)) {
                printf ("LT1:   [%04X]<-%02X: read FAILED\r\n", addrs[i], pat);
                passed = 0;
                continue;
            }
            if (rb != pat) {
                printf ("LT1:   [%04X]<-%02X readback=%02X MISMATCH\r\n",
                        addrs[i], pat, rb);
                passed = 0;
            }
        }
    }

    /* Restore. */
    for (i = 0u; i < sizeof (addrs) / sizeof (addrs[0]); ++i) {
        (void)ZX_NmiWriteBlock (addrs[i], &orig[i], 1u, LT_NMI_TIMEOUT);
    }

    printf ("LT1: %s\r\n", passed ? "PASS" : "FAIL");
    return passed;
}

/* ===== Marker readback helper ==================================== */

static void lt_show_markers (uint8_t n) {
    uint8_t buf[16];
    uint8_t i;

    if (n > sizeof (buf)) { n = sizeof (buf); }
    memset (buf, 0xEEu, sizeof (buf));   /* 0xEE = "couldn't read" sentinel */

    if (!ZX_BusReadBlock (LT_MARKER_BASE, buf, n)) {
        printf ("LT:   marker BUSREQ readback FAILED\r\n");
        return;
    }
    printf ("LT:   markers @0x%04X:", LT_MARKER_BASE);
    for (i = 0u; i < n; ++i) {
        printf (" %02X", buf[i]);
    }
    printf ("\r\n");
}

/* Pre-clear marker area (uncontended, BUSREQ write OK) */
static int lt_clear_markers (uint8_t n) {
    uint8_t zeros[16];
    if (n > sizeof (zeros)) { n = sizeof (zeros); }
    memset (zeros, 0, sizeof (zeros));
    if (!ZX_BusWriteBlock (LT_MARKER_BASE, zeros, n)) {
        printf ("LT:   marker clear BUSREQ write FAILED\r\n");
        return 0;
    }
    return 1;
}

/* ===== Test 2 — marker trampoline ================================ */

/* Tramp layout (offsets in bytes from LT_TRAMP_ADDR=0xFEC0):
 *   00: F3              DI
 *   01: 3E 11           LD A,0x11
 *   03: 32 80 BE        LD (0xBE80),A    ; M0: entered
 *   06: AF              XOR A
 *   07: 32 FE FE        LD (0xFEFE),A    ; clear go
 *   0A: 3E AA           LD A,0xAA
 *   0C: 32 FD FE        LD (0xFEFD),A    ; ALIVE
 *   0F: 3E 22           LD A,0x22
 *   11: 32 81 BE        LD (0xBE81),A    ; M1: about to spin
 *  spin (offset 0x14):
 *   14: 3A FE FE        LD A,(0xFEFE)
 *   17: FE 55           CP 0x55
 *   19: 20 F9           JR NZ,spin       (-7 to offset 0x14)
 *   1B: 3E 33           LD A,0x33
 *   1D: 32 82 BE        LD (0xBE82),A    ; M2: exited spin
 *   20: 3E 44           LD A,0x44
 *   22: 32 83 BE        LD (0xBE83),A    ; M3: end
 *   25: 76              HALT
 */
static const uint8_t lt_tramp_marker[] = {
    /* 00 */ 0xF3,
    /* 01 */ 0x3E, 0x11,
    /* 03 */ 0x32, 0x80, 0xBE,
    /* 06 */ 0xAF,
    /* 07 */ 0x32, 0xFE, 0xFE,
    /* 0A */ 0x3E, 0xAA,
    /* 0C */ 0x32, 0xFD, 0xFE,
    /* 0F */ 0x3E, 0x22,
    /* 11 */ 0x32, 0x81, 0xBE,
    /* 14 */ 0x3A, 0xFE, 0xFE,
    /* 17 */ 0xFE, 0x55,
    /* 19 */ 0x20, 0xF9,
    /* 1B */ 0x3E, 0x33,
    /* 1D */ 0x32, 0x82, 0xBE,
    /* 20 */ 0x3E, 0x44,
    /* 22 */ 0x32, 0x83, 0xBE,
    /* 25 */ 0x76
};

static int lt_test_marker_tramp (void) {
    printf ("LT2: marker trampoline (no POPs)\r\n");
    printf ("LT2:   expect markers = 11 22 33 44 after run\r\n");

    if (!lt_clear_markers (4u)) { return 0; }

    /* Install trampoline via NMI (lower bank reliable) */
    if (!ZX_NmiWriteBlock (LT_TRAMP_ADDR, lt_tramp_marker,
                           sizeof (lt_tramp_marker), LT_NMI_TIMEOUT)) {
        printf ("LT2:   trampoline NMI write FAILED\r\n");
        return 0;
    }

    if (!ZX_SnapshotEnter (LT_TRAMP_ADDR, LT_ALIVE_ADDR, 0xAAu, 1000u)) {
        printf ("LT2:   SnapshotEnter FAILED (no ALIVE marker)\r\n");
        lt_show_markers (4u);
        return 0;
    }
    printf ("LT2:   ALIVE marker seen (trampoline started, M0+M1 should be set)\r\n");

    /* Before commit: read M0,M1 by BUSREQ (Z80 is spinning, BUSREQ accepted) */
    lt_show_markers (4u);

    /* This trampoline ends in HALT, not JP 0xFFFE, so we cannot use
       ZX_SnapshotCommit (which waits for an M1 fetch at handover_addr).
       Instead: release ROMCS first (cart ROM out of the way), then
       BUSREQ-write 0x55 to 0xFEFE so the spin exits.  Z80 will run M2,
       M3, then HALT in user space. */
    ZX_RomcsRelease ();
    {
        uint8_t go = 0x55u;
        if (!ZX_BusWriteBlock (LT_GO_ADDR, &go, 1u)) {
            printf ("LT2:   go-flag BUSREQ write FAILED\r\n");
            return 0;
        }
    }

    /* Z80 should now exit spin, write M2 (0x33), M3 (0x44), HALT. */
    Delay_Ms (50u);
    printf ("LT2:   after go-flag write:\r\n");
    lt_show_markers (4u);
    printf ("LT2: done — interpret markers above.\r\n");
    return 1;
}

/* ===== Test 3 — minimal launch redirect ========================= */

/* Tiny stub at 0xFEC0 (where the real trampoline lives):
 *   00: 3E EE           LD A,0xEE
 *   02: 32 80 BE        LD (0xBE80),A   ; M0 = launched
 *   05: 3E AA           LD A,0xAA
 *   07: 32 FD FE        LD (0xFEFD),A   ; ALIVE — what SnapshotEnter polls
 *   0A: 3E DD           LD A,0xDD
 *   0C: 32 81 BE        LD (0xBE81),A   ; M1 = past ALIVE
 *   0F: 76              HALT            (Z80 stays HALTed; cart ROM still on)
 */
static const uint8_t lt3_mini_stub[] = {
    0x3E, 0xEE,
    0x32, 0x80, 0xBE,
    0x3E, 0xAA,
    0x32, 0xFD, 0xFE,
    0x3E, 0xDD,
    0x32, 0x81, 0xBE,
    0x76
};

static int lt_test_mini_launch (void) {
    uint8_t v[2];

    printf ("LT3: minimal launch redirect — does zxprog redirect PC to 0xFEC0?\r\n");
    printf ("LT3:   expect M0=EE M1=DD after run\r\n");

    if (!lt_clear_markers (4u)) { return 0; }

    if (!ZX_NmiWriteBlock (LT_TRAMP_ADDR, lt3_mini_stub, sizeof (lt3_mini_stub),
                           LT_NMI_TIMEOUT)) {
        printf ("LT3:   stub NMI write FAILED — zxprog not responding\r\n");
        return 0;
    }

    /* Verify staged stub via NMI read (Z80 ground truth). */
    if (!ZX_NmiReadBlock (LT_TRAMP_ADDR, v, 2u, LT_NMI_TIMEOUT)) {
        printf ("LT3:   stub readback NMI FAILED\r\n");
        return 0;
    }
    printf ("LT3:   stub @FEC0..1 = %02X %02X (want 3E EE)\r\n", v[0], v[1]);
    if (v[0] != 0x3Eu || v[1] != 0xEEu) {
        printf ("LT3: FAIL — stub didn't land in DRAM\r\n");
        return 0;
    }

    if (!ZX_SnapshotEnter (LT_TRAMP_ADDR, LT_ALIVE_ADDR, 0xAAu, 1000u)) {
        printf ("LT3: FAIL — SnapshotEnter timeout\r\n");
        printf ("LT3:        Means zxprog LAUNCH mailbox redirect didn't fire.\r\n");
        /* Read M0 anyway to see if anything happened. */
        Delay_Ms (50u);
        lt_show_markers (4u);
        return 0;
    }

    /* Z80 should be HALTed at FEC0+0x10. */
    Delay_Ms (50u);
    printf ("LT3:   final markers:\r\n");
    lt_show_markers (4u);
    if (!ZX_BusReadBlock (LT_MARKER_BASE, v, 2u)) {
        return 0;
    }
    if (v[0] == 0xEEu && v[1] == 0xDDu) {
        printf ("LT3: PASS — launch redirect works, Z80 reached FEC0 and ran stub\r\n");
        return 1;
    }
    printf ("LT3: FAIL — markers wrong (M0=%02X M1=%02X)\r\n", v[0], v[1]);
    return 0;
}

/* ===== Test 4 — full real trampoline + minimal user stub ========= */


/* User stub at 0xC000 (uncontended upper RAM, BUSREQ-readable):
 *   00: 3E 55           LD A,0x55
 *   02: 32 84 BE        LD (0xBE84),A   ; M4 = stub entered
 *   05: 3E 66           LD A,0x66
 *   07: 32 85 BE        LD (0xBE85),A   ; M5 = stub progressed
 *   0A: 76              HALT
 */
#define LT3_USER_PC      0xC000u
#define LT3_USER_SP      0xC100u   /* uncontended, plenty of room above stub */

static const uint8_t lt3_user_stub[] = {
    0x3E, 0x55,
    0x32, 0x84, 0xBE,
    0x3E, 0x66,
    0x32, 0x85, 0xBE,
    0x76
};

/* Real trampoline (matches Z80L_TRAMPOLINE in z80_loader.c).  We duplicate
   it here rather than expose it, so this test stays self-contained.
   Patch points: [37]=border, [51]=IM opcode, [56]=EI/NOP. */
static const uint8_t lt3_tramp[60] = {
    0xF3,
    0xAF,
    0x32, 0xFE, 0xFE,
    0x3E, 0xAA,
    0x32, 0xFD, 0xFE,
    0x3A, 0xFE, 0xFE,
    0xFE, 0x55,
    0x20, 0xF9,
    0x31, 0x80, 0xFE,
    0xF1,                       /* POP AF  */
    0xC1,                       /* POP BC  */
    0xD1,                       /* POP DE  */
    0xE1,                       /* POP HL  */
    0xD9,                       /* EXX     */
    0xC1,                       /* POP BC' */
    0xD1,                       /* POP DE' */
    0xE1,                       /* POP HL' */
    0xD9,                       /* EXX     */
    0x08,                       /* EX AF,AF' */
    0xF1,                       /* POP AF (alt) */
    0x08,                       /* EX AF,AF' */
    0xDD, 0xE1,                 /* POP IX  */
    0xFD, 0xE1,                 /* POP IY  */
    0x3E, 0x00,                 /* [36..37] LD A,border  patch [37] */
    0xD3, 0xFE,                 /* OUT (FE),A */
    0x3A, 0x94, 0xFE,           /* LD A,(FE94) ; I */
    0xED, 0x47,                 /* LD I,A */
    0x3A, 0x95, 0xFE,           /* LD A,(FE95) ; R */
    0xED, 0x4F,                 /* LD R,A */
    0xED, 0x56,                 /* IM 1   patch [51] */
    0xED, 0x7B, 0x9A, 0xFE,     /* LD SP,(FE9A) */
    0x00,                       /* NOP    patch [56]: 0xFB=EI or 0x00=NOP */
    0xC3, 0xFE, 0xFF            /* JP 0xFFFE */
};

static int lt_test_full_handover (void) {
    uint8_t tramp[60];
    uint8_t regblock[28];
    uint8_t pc_bytes[2];
    uint8_t handover_byte = 0xC9u;
    uint16_t sp_minus_2 = (uint16_t)(LT3_USER_SP - 2u);

    printf ("LT3: full real trampoline + minimal user stub @0xC000\r\n");
    printf ("LT3:   expect markers M0=11 M1=22 (tramp), M4=55 M5=66 (stub)\r\n");

    if (!lt_clear_markers (8u)) { return 0; }

    /* Plant user stub via NMI (upper RAM but go through NMI for consistency). */
    if (!ZX_NmiWriteBlock (LT3_USER_PC, lt3_user_stub, sizeof (lt3_user_stub),
                           LT_NMI_TIMEOUT)) {
        printf ("LT3:   user stub NMI write FAILED\r\n");
        return 0;
    }

    /* Build trampoline copy with NOP for EI (IFF stays cleared, no spurious INT). */
    memcpy (tramp, lt3_tramp, sizeof (tramp));
    tramp[37] = 0x00;       /* border = 0 (black) */
    tramp[51] = 0x56;       /* IM 1   */
    tramp[56] = 0x00;       /* NOP (no EI) */

    /* Insert tiny marker writes into the real tramp:
       at offset 1 (after DI) inject 'LD A,0x11; LD (0xBE80),A' would push
       the rest of the tramp out of place (sizes change).  Instead skip
       marker injection — we rely on M4/M5 from the user stub to confirm
       end-to-end success, and on stack/regblock state for partial-failure
       diagnosis. */

    /* Build regblock: all regs = 0, I = 0, R = 0, sp_minus_2 -> regblock[26..27] */
    memset (regblock, 0, sizeof (regblock));
    regblock[26] = (uint8_t)(sp_minus_2 & 0xFFu);
    regblock[27] = (uint8_t)(sp_minus_2 >> 8);

    /* Pre-push user PC at sp-2 */
    pc_bytes[0] = (uint8_t)(LT3_USER_PC & 0xFFu);
    pc_bytes[1] = (uint8_t)(LT3_USER_PC >> 8);

    if (!ZX_NmiWriteBlock (LT_REGBLOCK_ADDR, regblock, sizeof (regblock),
                           LT_NMI_TIMEOUT)) {
        printf ("LT3:   regblock NMI write FAILED\r\n");
        return 0;
    }
    if (!ZX_NmiWriteBlock (LT_TRAMP_ADDR, tramp, sizeof (tramp),
                           LT_NMI_TIMEOUT)) {
        printf ("LT3:   trampoline NMI write FAILED\r\n");
        return 0;
    }
    if (!ZX_NmiWriteBlock (LT_HANDOVER_ADDR, &handover_byte, 1u,
                           LT_NMI_TIMEOUT)) {
        printf ("LT3:   handover-byte NMI write FAILED\r\n");
        return 0;
    }
    if (!ZX_NmiWriteBlock (sp_minus_2, pc_bytes, 2u, LT_NMI_TIMEOUT)) {
        printf ("LT3:   PC pre-push NMI write FAILED\r\n");
        return 0;
    }

    /* Verify staged bytes via NMI-read (Z80-perspective ground truth). */
    {
        uint8_t v[8];
        if (ZX_NmiReadBlock (LT_HANDOVER_ADDR, &v[0], 1u, LT_NMI_TIMEOUT) &&
            ZX_NmiReadBlock (sp_minus_2,        &v[1], 2u, LT_NMI_TIMEOUT) &&
            ZX_NmiReadBlock (LT_TRAMP_ADDR,     &v[3], 4u, LT_NMI_TIMEOUT)) {
            printf ("LT3:   pre-launch state: [FFFE]=%02X (want C9), "
                    "[%04X..%04X]=%02X %02X (want %02X %02X), "
                    "[FEC0..3]=%02X %02X %02X %02X (want F3 AF 32 FE)\r\n",
                    v[0], (unsigned)sp_minus_2, (unsigned)(sp_minus_2 + 1u),
                    v[1], v[2], pc_bytes[0], pc_bytes[1],
                    v[3], v[4], v[5], v[6]);
        }
    }

    /* Launch via the real path. */
    if (!ZX_SnapshotEnter (LT_TRAMP_ADDR, LT_ALIVE_ADDR, 0xAAu, 1000u)) {
        printf ("LT3:   SnapshotEnter FAILED\r\n");
        return 0;
    }
    if (!ZX_SnapshotCommit (LT_HANDOVER_ADDR, LT_GO_ADDR, 0x55u, 200u)) {
        printf ("LT3:   SnapshotCommit FAILED (M1 at 0xFFFE never fetched)\r\n");
        return 0;
    }

    /* Z80 should now be HALTed in user space at ~0xC00B. */
    Delay_Ms (100u);
    {
        uint8_t buf[8];
        memset (buf, 0xEEu, sizeof (buf));
        if (!ZX_BusReadBlock (LT_MARKER_BASE, buf, 8u)) {
            printf ("LT3:   marker readback FAILED\r\n");
            return 0;
        }
        printf ("LT3:   final markers: %02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
        printf ("LT3:   M4=0x%02X (want 55) M5=0x%02X (want 66)\r\n",
                buf[4], buf[5]);

        if (buf[4] == 0x55u && buf[5] == 0x66u) {
            printf ("LT3: PASS — full handover path WORKS, real .z80 bug is\r\n");
            printf ("LT3:        elsewhere (snapshot regs/IM/IFF or user code).\r\n");
            return 1;
        }
        if (buf[4] == 0x55u) {
            printf ("LT3: PARTIAL — stub started but didn't finish (refresh/glitch)\r\n");
            return 0;
        }
        printf ("LT3: FAIL — stub never executed. Trampoline POP/JP path broken.\r\n");
        return 0;
    }
}

/* ===== Test 5 — visible "hello world" via full trampoline ======== */

/* Module flag: when set, lt_test_hello uses EI (0xFB) at tramp [56] instead
   of NOP.  Used by test 6 to isolate whether enabling interrupts is what
   crashes real .z80 snapshots. */
static uint8_t lt5_force_ei = 0u;

/* Module flag: when set, lt_test_hello runs an extended LT7 visual program
    instead of the short LT5 demo. */
static uint8_t lt7_use_extended_prog = 0u;


/* Z80 program at 0xC000.  Visibly demonstrates a successful launch:
 *   - writes M4=0xAB (entered)
 *   - sets border red
 *   - fills the entire attribute area (0x5800..0x5AFF) with 0x46
 *     (bright + paper black + ink yellow-ish) so the screen takes a
 *     uniform colour — instantly visible
 *   - writes M5=0xCD (paint done)
 *   - cycles the border through 8 colours with a delay loop the user
 *     can clearly see for ~5-10 seconds
 *   - writes M6=0xEF (cycle done)
 *   - HALTs
 *
 *  Hand-assembled, byte-accurate, 50 bytes.
 */
static const uint8_t lt5_user_prog[] = {
    /*  0 */ 0x3E, 0xAB,                /* LD A,0xAB                      */
    /*  2 */ 0x32, 0x84, 0xBE,          /* LD (0xBE84),A   M4=entered     */
    /*  5 */ 0x3E, 0x02,                /* LD A,2                         */
    /*  7 */ 0xD3, 0xFE,                /* OUT (0xFE),A    border red     */
    /*  9 */ 0x21, 0x00, 0x58,          /* LD HL,0x5800                   */
    /* 12 */ 0x11, 0x01, 0x58,          /* LD DE,0x5801                   */
    /* 15 */ 0x01, 0xFF, 0x02,          /* LD BC,767                      */
    /* 18 */ 0x36, 0x46,                /* LD (HL),0x46                   */
    /* 20 */ 0xED, 0xB0,                /* LDIR             paint attrs   */
    /* 22 */ 0x3E, 0xCD,                /* LD A,0xCD                      */
    /* 24 */ 0x32, 0x85, 0xBE,          /* LD (0xBE85),A   M5=paint done  */
    /* 27 */ 0x06, 0x08,                /* LD B,8           outer count   */
    /* 29 */ 0x78,                      /* outer: LD A,B                  */
    /* 30 */ 0xE6, 0x07,                /* AND 7                          */
    /* 32 */ 0xD3, 0xFE,                /* OUT (0xFE),A                   */
    /* 34 */ 0x21, 0xFF, 0xFF,          /* LD HL,0xFFFF                   */
    /* 37 */ 0x2B,                      /* inner: DEC HL                  */
    /* 38 */ 0x7C,                      /* LD A,H                         */
    /* 39 */ 0xB5,                      /* OR L                           */
    /* 40 */ 0x20, 0xFB,                /* JR NZ,inner    (-5)            */
    /* 42 */ 0x10, 0xF1,                /* DJNZ outer     (-15)           */
    /* 44 */ 0x3E, 0xEF,                /* LD A,0xEF                      */
    /* 46 */ 0x32, 0x86, 0xBE,          /* LD (0xBE86),A   M6=cycle done  */
    /* 49 */ 0x76                       /* HALT                           */
};

/* LT7 program at 0xC000.  Extended visual + RAM test:
 *   - writes M4=0xAB (entered)
 *   - clears pixel + attribute screen, with visible pauses between stages
 *   - paints visible patterns (AA full bitmap + 55 middle band)
 *   - writes M5=0xCD (visual stage done)
 *   - RAM test at 0xC200..0xC2FF with 0x55/0xAA write+verify
 *   - writes M6=0xEF on pass (green border), M6=0xF1 on fail (red border)
 *   - after pass/fail, scans keyboard matrix; any key press toggles a
 *     screen attribute and border colour so the keyboard path is visible
 */
static const uint8_t lt7_user_prog[] = {
    0x3E, 0xAB,                   /* LD A,0xAB                      */
    0x32, 0x84, 0xBE,             /* LD (0xBE84),A                  */
    0xAF,                         /* XOR A                          */
    0xD3, 0xFE,                   /* OUT (0xFE),A  border black     */

    0x21, 0x00, 0x40,             /* LD HL,0x4000                   */
    0x11, 0x01, 0x40,             /* LD DE,0x4001                   */
    0x01, 0xFF, 0x17,             /* LD BC,0x17FF                   */
    0x36, 0x00,                   /* LD (HL),0x00                   */
    0xED, 0xB0,                   /* LDIR (clear bitmap)            */

    0x21, 0x00, 0x58,             /* LD HL,0x5800                   */
    0x11, 0x01, 0x58,             /* LD DE,0x5801                   */
    0x01, 0xFF, 0x02,             /* LD BC,0x02FF                   */
    0x36, 0x07,                   /* LD (HL),0x07                   */
    0xED, 0xB0,                   /* LDIR (clear attrs)             */

    0x21, 0xFF, 0xFF,             /* delay1: LD HL,0xFFFF           */
    0x2B,                         /* d1: DEC HL                     */
    0x7C,                         /*     LD A,H                     */
    0xB5,                         /*     OR L                       */
    0x20, 0xFB,                   /*     JR NZ,d1                   */

    0x3E, 0xCD,                   /* LD A,0xCD                      */
    0x32, 0x85, 0xBE,             /* LD (0xBE85),A                  */

    0x3E, 0x01,                   /* LD A,1                         */
    0xD3, 0xFE,                   /* OUT (0xFE),A  stage border     */
    0x21, 0x00, 0x40,             /* LD HL,0x4000                   */
    0x11, 0x01, 0x40,             /* LD DE,0x4001                   */
    0x01, 0xFF, 0x17,             /* LD BC,0x17FF                   */
    0x36, 0xAA,                   /* LD (HL),0xAA                   */
    0xED, 0xB0,                   /* LDIR (pattern 1)               */

    0x21, 0xFF, 0xFF,             /* delay2: LD HL,0xFFFF           */
    0x2B,                         /* d2: DEC HL                     */
    0x7C,                         /*     LD A,H                     */
    0xB5,                         /*     OR L                       */
    0x20, 0xFB,                   /*     JR NZ,d2                   */

    0x21, 0x00, 0x48,             /* LD HL,0x4800                   */
    0x11, 0x01, 0x48,             /* LD DE,0x4801                   */
    0x01, 0xFF, 0x07,             /* LD BC,0x07FF                   */
    0x36, 0x55,                   /* LD (HL),0x55                   */
    0xED, 0xB0,                   /* LDIR (pattern 2)               */

    0x21, 0x00, 0x58,             /* LD HL,0x5800                   */
    0x11, 0x01, 0x58,             /* LD DE,0x5801                   */
    0x01, 0xFF, 0x02,             /* LD BC,0x02FF                   */
    0x36, 0x2E,                   /* LD (HL),0x2E                   */
    0xED, 0xB0,                   /* LDIR (bright attrs)            */

    0x21, 0xFF, 0xFF,             /* delay3: LD HL,0xFFFF           */
    0x2B,                         /* d3: DEC HL                     */
    0x7C,                         /*     LD A,H                     */
    0xB5,                         /*     OR L                       */
    0x20, 0xFB,                   /*     JR NZ,d3                   */

    0x3E, 0x03,                   /* LD A,3                         */
    0xD3, 0xFE,                   /* OUT (0xFE),A  RAM test stage   */

    0x21, 0x00, 0xC2,             /* LD HL,0xC200                   */
    0x06, 0x00,                   /* LD B,0x00 (256 iters)          */
    0x36, 0x55,                   /* w55: LD (HL),0x55              */
    0x23,                         /*      INC HL                    */
    0x10, 0xFB,                   /*      DJNZ w55                  */

    0x21, 0x00, 0xC2,             /* LD HL,0xC200                   */
    0x06, 0x00,                   /* LD B,0x00                      */
    0x7E,                         /* v55: LD A,(HL)                 */
    0xFE, 0x55,                   /*      CP 0x55                   */
    0x20, 0x25,                   /*      JR NZ,fail                */
    0x23,                         /*      INC HL                    */
    0x10, 0xF8,                   /*      DJNZ v55                  */

    0x21, 0x00, 0xC2,             /* LD HL,0xC200                   */
    0x06, 0x00,                   /* LD B,0x00                      */
    0x36, 0xAA,                   /* waa: LD (HL),0xAA              */
    0x23,                         /*      INC HL                    */
    0x10, 0xFB,                   /*      DJNZ waa                  */

    0x21, 0x00, 0xC2,             /* LD HL,0xC200                   */
    0x06, 0x00,                   /* LD B,0x00                      */
    0x7E,                         /* vaa: LD A,(HL)                 */
    0xFE, 0xAA,                   /*      CP 0xAA                   */
    0x20, 0x0E,                   /*      JR NZ,fail                */
    0x23,                         /*      INC HL                    */
    0x10, 0xF8,                   /*      DJNZ vaa                  */

    0x3E, 0xEF,                   /* LD A,0xEF                      */
    0x32, 0x86, 0xBE,             /* LD (0xBE86),A                  */
    0x3E, 0x04,                   /* LD A,4 (green)                 */
    0xD3, 0xFE,                   /* OUT (0xFE),A                   */
    0x18, 0x10,                   /* JR pass_loop_init              */

    0x3E, 0xF1,                   /* fail: LD A,0xF1                */
    0x32, 0x86, 0xBE,             /* LD (0xBE86),A                  */
    0x3E, 0x02,                   /* LD A,2 (red)                   */
    0xD3, 0xFE,                   /* OUT (0xFE),A                   */
    0x3E, 0x00,                   /* LD A,0                         */
    0x32, 0x87, 0xBE,             /* LD (0xBE87),A                  */
    0x18, 0x41,                   /* JR fail_loop                   */

    0x3E, 0x00,                   /* pass_loop_init: LD A,0         */
    0x32, 0x87, 0xBE,             /* LD (0xBE87),A                  */

    0x21, 0xFF, 0x7F,             /* pass_loop: LD HL,0x7FFF        */
    0x2B,                         /* pdelay: DEC HL                 */
    0x7C,                         /*        LD A,H                  */
    0xB5,                         /*        OR L                    */
    0x20, 0xFB,                   /*        JR NZ,pdelay            */
    0x01, 0xFE, 0xFE,             /*        LD BC,0xFEFE            */
    0x16, 0x08,                   /*        LD D,8                  */
    0xED, 0x78,                   /* pscan: IN A,(C)                */
    0xE6, 0x1F,                   /*        AND 0x1F                */
    0xFE, 0x1F,                   /*        CP 0x1F                 */
    0x20, 0x07,                   /*        JR NZ,pkey              */
    0xCB, 0x00,                   /*        RLC B                   */
    0x15,                         /*        DEC D                   */
    0x20, 0xF5,                   /*        JR NZ,pscan             */
    0x18, 0xE4,                   /*        JR pass_loop            */
    0x3A, 0x00, 0x58,             /* pkey:  LD A,(0x5800)           */
    0xEE, 0x38,                   /*        XOR 0x38                */
    0x32, 0x00, 0x58,             /*        LD (0x5800),A           */
    0x3A, 0x87, 0xBE,             /*        LD A,(0xBE87)           */
    0xEE, 0x01,                   /*        XOR 1                   */
    0x32, 0x87, 0xBE,             /*        LD (0xBE87),A           */
    0xE6, 0x01,                   /*        AND 1                   */
    0x28, 0x06,                   /*        JR Z,pcol0              */
    0x3E, 0x06,                   /*        LD A,6                  */
    0xD3, 0xFE,                   /*        OUT (0xFE),A            */
    0x18, 0xCA,                   /*        JR pass_loop            */
    0x3E, 0x04,                   /* pcol0:  LD A,4                 */
    0xD3, 0xFE,                   /*        OUT (0xFE),A            */
    0x18, 0xC4,                   /*        JR pass_loop            */

    0x21, 0xFF, 0x7F,             /* fail_loop: LD HL,0x7FFF        */
    0x2B,                         /* fdelay: DEC HL                 */
    0x7C,                         /*        LD A,H                  */
    0xB5,                         /*        OR L                    */
    0x20, 0xFB,                   /*        JR NZ,fdelay            */
    0x01, 0xFE, 0xFE,             /*        LD BC,0xFEFE            */
    0x16, 0x08,                   /*        LD D,8                  */
    0xED, 0x78,                   /* fscan: IN A,(C)                */
    0xE6, 0x1F,                   /*        AND 0x1F                */
    0xFE, 0x1F,                   /*        CP 0x1F                 */
    0x20, 0x07,                   /*        JR NZ,fkey              */
    0xCB, 0x00,                   /*        RLC B                   */
    0x15,                         /*        DEC D                   */
    0x20, 0xF5,                   /*        JR NZ,fscan             */
    0x18, 0xE4,                   /*        JR fail_loop            */
    0x3A, 0x00, 0x58,             /* fkey:  LD A,(0x5800)           */
    0xEE, 0x38,                   /*        XOR 0x38                */
    0x32, 0x00, 0x58,             /*        LD (0x5800),A           */
    0x3A, 0x87, 0xBE,             /*        LD A,(0xBE87)           */
    0xEE, 0x01,                   /*        XOR 1                   */
    0x32, 0x87, 0xBE,             /*        LD (0xBE87),A           */
    0xE6, 0x01,                   /*        AND 1                   */
    0x28, 0x06,                   /*        JR Z,fcol0              */
    0x3E, 0x01,                   /*        LD A,1                  */
    0xD3, 0xFE,                   /*        OUT (0xFE),A            */
    0x18, 0xCA,                   /*        JR fail_loop            */
    0x3E, 0x02,                   /* fcol0:  LD A,2                 */
    0xD3, 0xFE,                   /*        OUT (0xFE),A            */
    0x18, 0xC4                    /*        JR fail_loop            */
};

#define LT5_USER_PC      0xC000u
#define LT5_USER_SP      0xC100u

static int lt_test_hello (void) {
    uint8_t tramp[60];
    uint8_t regblock[28];
    uint8_t pc_bytes[2];
    uint8_t handover_byte = 0xC9u;
    const uint8_t *user_prog = lt5_user_prog;
    size_t user_prog_len = sizeof (lt5_user_prog);
    uint16_t sp_minus_2 = (uint16_t)(LT5_USER_SP - 2u);

    if (lt7_use_extended_prog) {
        user_prog = lt7_user_prog;
        user_prog_len = sizeof (lt7_user_prog);
        printf ("LT7: extended visual+RAM launch demo via full trampoline\r\n");
        printf ("LT7:   clear screen, draw patterns, RAM test @0xC200..0xC2FF\r\n");
        printf ("LT7:   markers: M4=AB entered, M5=CD visual done, M6=EF pass\r\n");
        printf ("LT7:   border green=pass, red=fail\r\n");
        printf ("LT7:   press any key -> border toggles and screen block flips\r\n");
    } else {
        printf ("LT5: visible launch demo via full trampoline path\r\n");
        printf ("LT5:   user program @0xC000: paint red border, fill attrs,\r\n");
        printf ("LT5:                         cycle border 8 colours, halt.\r\n");
        printf ("LT5:   markers: M4=AB(entered) M5=CD(painted) M6=EF(cycled)\r\n");
    }

    if (!lt_clear_markers (8u)) { return 0; }

    /* Plant user program */
    if (!ZX_NmiWriteBlock (LT5_USER_PC, user_prog,
                           user_prog_len, LT_NMI_TIMEOUT)) {
        printf ("LT5:   user program NMI write FAILED — reset ZX & retry\r\n");
        return 0;
    }

    /* Real trampoline; clear regs; IM 1; EI controlled by lt5_force_ei. */
    memcpy (tramp, lt3_tramp, sizeof (tramp));
    tramp[37] = 0x00;       /* border patch (overwritten by user prog anyway) */
    tramp[51] = 0x56;       /* IM 1 */
    tramp[56] = lt5_force_ei ? 0xFBu : 0x00u;

    memset (regblock, 0, sizeof (regblock));
    regblock[26] = (uint8_t)(sp_minus_2 & 0xFFu);
    regblock[27] = (uint8_t)(sp_minus_2 >> 8);
    pc_bytes[0]  = (uint8_t)(LT5_USER_PC & 0xFFu);
    pc_bytes[1]  = (uint8_t)(LT5_USER_PC >> 8);

    if (!ZX_NmiWriteBlock (LT_REGBLOCK_ADDR, regblock, sizeof (regblock),
                           LT_NMI_TIMEOUT) ||
        !ZX_NmiWriteBlock (LT_TRAMP_ADDR, tramp, sizeof (tramp),
                           LT_NMI_TIMEOUT) ||
        !ZX_NmiWriteBlock (LT_HANDOVER_ADDR, &handover_byte, 1u,
                           LT_NMI_TIMEOUT) ||
        !ZX_NmiWriteBlock (sp_minus_2, pc_bytes, 2u, LT_NMI_TIMEOUT)) {
        printf ("LT5:   staging NMI writes FAILED\r\n");
        return 0;
    }

    if (!ZX_SnapshotEnter (LT_TRAMP_ADDR, LT_ALIVE_ADDR, 0xAAu, 1000u)) {
        printf ("LT5:   SnapshotEnter FAILED\r\n");
        return 0;
    }
    if (!ZX_SnapshotCommit (LT_HANDOVER_ADDR, LT_GO_ADDR, 0x55u, 200u)) {
        printf ("LT5:   SnapshotCommit FAILED — Z80 didn't reach JP 0xFFFE\r\n");
        return 0;
    }

    /* User program runs ~5-10s; wait for cycle-end marker. */
    {
        uint32_t waited = 0u;
        uint8_t buf[8];
        while (waited < 15000u) {
            Delay_Ms (200u);
            waited += 200u;
            if (ZX_BusReadBlock (LT_MARKER_BASE, buf, 8u) && buf[6] == 0xEFu) {
                break;
            }
        }
        memset (buf, 0xEEu, sizeof (buf));
        if (!ZX_BusReadBlock (LT_MARKER_BASE, buf, 8u)) {
            printf ("LT5:   marker readback FAILED\r\n");
            return 0;
        }
        printf ("LT5:   markers: M4=%02X(want AB) M5=%02X(want CD) M6=%02X(want EF) "
                "after %lums\r\n",
                buf[4], buf[5], buf[6], (unsigned long)waited);

        if (buf[4] == 0xABu && buf[5] == 0xCDu && buf[6] == 0xEFu) {
            printf ("LT5: PASS — full launch+execute path WORKS end-to-end.\r\n");
            return 1;
        }
        if (buf[4] == 0xABu && buf[5] == 0xCDu) {
            printf ("LT5: PARTIAL — code ran, paint OK, cycle didn't finish (timeout?)\r\n");
            return 0;
        }
        if (buf[4] == 0xABu) {
            printf ("LT5: PARTIAL — entered but LDIR didn't complete (DRAM contention)\r\n");
            return 0;
        }
        printf ("LT5: FAIL — user program never executed at 0xC000\r\n");
        return 0;
    }
}

/* ===== Public dispatcher ========================================= */

static int lt_test_hello_with_ei (void) {
    int rc;
    lt5_force_ei = 1u;
    printf ("LT6: same as LT5 but with EI at end of trampoline\r\n");
    printf ("LT6:   if this crashes/resets the ZX, EI itself is the problem\r\n");
    rc = lt_test_hello ();
    lt5_force_ei = 0u;
    return rc;
}

static int lt_test_hello_with_ei_loop (void) {
    int rc;
    lt5_force_ei = 1u;
    lt7_use_extended_prog = 1u;
    printf ("LT7: same trampoline path as LT6 (EI on), extended visual program\r\n");
    printf ("LT7:   includes clear/pattern/RAM test with green/red pass-fail\r\n");
    rc = lt_test_hello ();
    lt5_force_ei = 0u;
    lt7_use_extended_prog = 0u;
    return rc;
}

int LaunchTest_Run (int id) {
    switch (id) {
        case 1: return lt_test_nmi_roundtrip ();
        case 2: return lt_test_marker_tramp ();
        case 3: return lt_test_mini_launch ();
        case 4: return lt_test_full_handover ();
        case 5: return lt_test_hello ();
        case 6: return lt_test_hello_with_ei ();
        case 7: return lt_test_hello_with_ei_loop ();
        default:
            printf ("launchtest: unknown id %d\r\n", id);
            printf ("  1 = NMI mailbox roundtrip\r\n");
            printf ("  2 = marker trampoline (no handover, BUSREQ go-flag)\r\n");
            printf ("  3 = minimal launch redirect (zxprog redirect PC?)\r\n");
            printf ("  4 = full real trampoline + user stub at 0xC000\r\n");
            printf ("  5 = visible hello: paint + cycle border (no EI)\r\n");
            printf ("  6 = same as 5 but WITH EI (isolates IM1+EI crash)\r\n");
            printf ("  7 = EI + extended visual demo + RAM test + pass/fail border\r\n");
            return 0;
    }
}
