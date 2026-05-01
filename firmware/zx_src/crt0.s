; crt0.s  —  ZX Spectrum cart ROM startup, NMI wrapper, and launcher.
;
; Provides:
;   _startup      — called via JP at 0x0000 (RST/power-on)
;   _nmi_wrapper  — full register-save wrapper, called from JP at 0x0066
;   _zx_launcher  — snapshot launch; restores all Z80 regs, never returns
;
; Vector layout (ABS, hardwired Z80 addresses):
;   0x0000  JP _startup       RST 0 / power-on
;   0x0038  EI / RETI         IM1 handler (minimal — just re-enable and return)
;   0x0066  JP _nmi_wrapper   NMI dispatch to full C-callable wrapper
;
; Border colour sequence visible on a real Spectrum display:
;   CYAN  (5) — zxprog started, waiting for launch trigger
;   YELLOW(6) — trigger 0x55 seen in main poll loop  (set before calling launcher)
;   WHITE (7) — launcher entered, alive byte written
;   <snap>    — snapshot's own border colour restored from regblock

    .module crt0

    .globl _main
    .globl _nmi_handler_c

; ============================================================
;  ABSOLUTE section: Z80 fixed-address vectors (0x0000..0x0068)
; ============================================================
    .area _VECTOR (ABS)

    .org 0x0000
        jp      _startup        ; RST 0 / power-on: jump to startup

    ; Bytes 0x0003..0x0037 are unspecified (filled with 0xFF by objcopy)

    .org 0x0038
        ; IM1 handler: minimal — re-enable interrupts and return.
        ; Running z88dk's complex handler here caused it to call timer
        ; routines that jumped into game code (already loaded in ZX RAM).
        ei
        reti                    ; ED 4D  (2 bytes -> fills 0x0039..0x003A)

    ; Bytes 0x003B..0x0065 unspecified (filled with 0xFF by objcopy)

    .org 0x0066
        jp      _nmi_wrapper    ; NMI: jump to full register-save wrapper

; ============================================================
;  CODE section: relocatable, placed at --code-loc = 0x0069
; ============================================================
    .area _CODE

; ---- Startup -----------------------------------------------
; Called via the JP at 0x0000.  Sets up stack, asserts CYAN border
; (confirms zxprog is live), enables interrupts, jumps to main().
_startup::
        di
        ld      sp, #0x3EFE         ; stack grows down from 0x3EFE

        ; Border = CYAN (5): "zxprog alive, waiting for trigger"
        ld      a, #5
        out     (0xFE), a

        ei
        jp      _main

_halt::
        di
        halt
        jr      _halt

; ---- NMI wrapper -------------------------------------------
; Saves the full Z80 register set (including alternate regs and IX/IY),
; calls _nmi_handler_c() (C function in zxprog.c), then restores and RETNs.
; The NMI is used for WCMD (write-block) and RCMD (read-back) mailboxes.
_nmi_wrapper::
        push    af
        push    bc
        push    de
        push    hl
        push    ix
        push    iy
        ex      af, af'
        push    af
        exx
        push    bc
        push    de
        push    hl

        call    _nmi_handler_c

        pop     hl
        pop     de
        pop     bc
        exx
        pop     af
        ex      af, af'
        pop     iy
        pop     ix
        pop     hl
        pop     de
        pop     bc
        pop     af
        retn

; ---- Snapshot launcher -------------------------------------
; Called from main() when LAUNCH_TRIGGER == 0x55.  Never returns.
;
; Regblock layout at 0x3F90 (written by z80_loader.c):
;   +0..+1   BC'            +2..+3   DE'            +4..+5   HL'
;   +6..+7   AF'            +8..+9   IX             +10..+11 IY
;   +12..+13 I (hi) / junk  +14..+15 border(lo)/R_comp(hi)
;   +16..+17 BC             +18..+19 DE             +20..+21 HL
;   +22..+23 AF             +24..+25 user_sp
;
; Launcher tail at 0x3FF0 (written by z80_loader.c):
;   0x3FF0  ED              IM prefix
;   0x3FF1  46/56/5E        IM 0/1/2 byte
;   0x3FF2  FB or 00        EI or NOP (depending on snapshot IFF)
;   0x3FF3  C3              JP
;   0x3FF4  pc_lo           user_pc low byte
;   0x3FF5  pc_hi           user_pc high byte
;
; R compensation: exactly 12 M1 cycles from LD R,A to user_pc M1 fetch:
;   POP BC/DE/HL/AF (4 M1s) + LD SP,(nn) ED+7B (2) + JP C3 (1) +
;   IM ED (1) + IM byte (1) + EI/NOP (1) + JP C3 (1) + user_pc M1 (1) = 12
_zx_launcher::
        di                          ; prevent IM1 from corrupting SP/regblock

        ; Border = WHITE (7): "launcher running, alive byte being written"
        ld      a, #7
        out     (0xFE), a

        ; Alive marker — CH32 polls sp->ram[0x3FAA] to confirm entry
        ld      a, #0xAA
        ld      (0x3FAA), a

        ; Restore all Z80 registers from regblock
        ld      sp, #0x3F90

        pop     bc                  ; BC'  (rb[0..1])
        pop     de                  ; DE'  (rb[2..3])
        pop     hl                  ; HL'  (rb[4..5])
        exx                         ; BC,DE,HL -> alternate set

        pop     af                  ; AF'  (rb[6..7]: lo=F', hi=A')
        ex      af, af'

        pop     ix                  ; IX   (rb[8..9])
        pop     iy                  ; IY   (rb[10..11])

        pop     af                  ; A=I, F=junk  (rb[12..13])
        ld      i, a

        pop     bc                  ; C=border, B=R_comp  (rb[14..15])
        ld      a, c
        out     (0xFE), a           ; restore snapshot's original border colour
        ld      a, b
        ld      r, a                ; R loaded; exactly 12 M1 cycles remain

        pop     bc                  ; main BC  (rb[16..17])
        pop     de                  ; main DE  (rb[18..19])
        pop     hl                  ; main HL  (rb[20..21])
        pop     af                  ; main AF  (rb[22..23]: lo=F, hi=A)

        ld      sp, (0x3FA8)        ; user_sp  (rb[24..25])

        jp      0x3FF0              ; launcher tail: IM x / EI-or-NOP / JP user_pc
