;--------------------------------------------------------
; File Created by SDCC : free open source ANSI-C Compiler
; Version 4.2.0 #13081 (Linux)
;--------------------------------------------------------
	.module zxprog
	.optsdcc -mz80
	
;--------------------------------------------------------
; Public variables in this module
;--------------------------------------------------------
	.globl _main
	.globl _nmi_handler_c
	.globl _zx_launcher
;--------------------------------------------------------
; special function registers
;--------------------------------------------------------
_ULA_PORT	=	0x00fe
;--------------------------------------------------------
; ram data
;--------------------------------------------------------
	.area _DATA
_wcmd_last_seq:
	.ds 1
_rcmd_last_seq:
	.ds 1
;--------------------------------------------------------
; ram data
;--------------------------------------------------------
	.area _INITIALIZED
;--------------------------------------------------------
; absolute external ram data
;--------------------------------------------------------
	.area _DABS (ABS)
;--------------------------------------------------------
; global & static initialisations
;--------------------------------------------------------
	.area _HOME
	.area _GSINIT
	.area _GSFINAL
	.area _GSINIT
;--------------------------------------------------------
; Home
;--------------------------------------------------------
	.area _HOME
	.area _HOME
;--------------------------------------------------------
; code
;--------------------------------------------------------
	.area _CODE
;zxprog.c:83: static void zcopy(unsigned char *dst, const unsigned char *src, unsigned int len)
;	---------------------------------
; Function zcopy
; ---------------------------------
_zcopy:
	push	ix
	ld	ix,#0
	add	ix,sp
	push	af
	push	af
	ex	(sp), hl
	ld	-2 (ix), e
	ld	-1 (ix), d
;zxprog.c:85: while (len--) {
	ld	c, 4 (ix)
	ld	b, 5 (ix)
00101$:
	ld	a, c
	ld	e, b
	dec	bc
	or	a, e
	jr	Z, 00104$
;zxprog.c:86: *dst++ = *src++;
	ld	l, -2 (ix)
	ld	h, -1 (ix)
	ld	a, (hl)
	inc	-2 (ix)
	jr	NZ, 00117$
	inc	-1 (ix)
00117$:
	pop	hl
	push	hl
	ld	(hl), a
	inc	-4 (ix)
	jr	NZ, 00101$
	inc	-3 (ix)
	jr	00101$
00104$:
;zxprog.c:88: }
	ld	sp, ix
	pop	ix
	pop	hl
	pop	af
	jp	(hl)
;zxprog.c:91: static void wcmd_poll(void)
;	---------------------------------
; Function wcmd_poll
; ---------------------------------
_wcmd_poll:
	push	ix
	ld	ix,#0
	add	ix,sp
	dec	sp
;zxprog.c:93: unsigned char seq = WCMD_SEQ;
	ld	a, (#0x302e)
	ld	-1 (ix), a
;zxprog.c:95: if (seq != wcmd_last_seq) {
	ld	a, (_wcmd_last_seq+0)
	sub	a, -1 (ix)
	jr	Z, 00105$
;zxprog.c:96: unsigned int dst = (unsigned int)WCMD_DST_LO |
	ld	a, (#0x3030)
	ld	c, a
	ld	b, #0x00
	ld	a, (#0x3031)
	ld	d, a
	xor	a, a
	or	a, c
	ld	e, a
	ld	a, d
	or	a, b
	ld	d, a
;zxprog.c:98: unsigned int len = (unsigned int)WCMD_LEN_LO |
	ld	a, (#0x3032)
	ld	c, a
	ld	b, #0x00
	ld	a, (#0x3033)
;	spillPairReg hl
;	spillPairReg hl
;	spillPairReg hl
;	spillPairReg hl
	ld	h, a
;	spillPairReg hl
;	spillPairReg hl
	ld	l, #0x00
;	spillPairReg hl
;	spillPairReg hl
	ld	a, c
	or	a, l
	ld	c, a
	ld	a, b
	or	a, h
	ld	b, a
;zxprog.c:101: if (len > WCMD_MAX_DATA) {
	xor	a, a
	cp	a, c
	ld	a, #0x02
	sbc	a, b
	jr	NC, 00102$
;zxprog.c:102: len = WCMD_MAX_DATA;
	ld	bc, #0x0200
00102$:
;zxprog.c:105: zcopy((unsigned char *)dst,
	push	bc
	ex	de, hl
	ld	de, #0x3034
	call	_zcopy
;zxprog.c:108: WCMD_DONE = seq;
	ld	hl, #0x302f
	ld	a, -1 (ix)
	ld	(hl), a
;zxprog.c:109: wcmd_last_seq = seq;
	ld	a, -1 (ix)
	ld	(_wcmd_last_seq+0), a
00105$:
;zxprog.c:111: }
	inc	sp
	pop	ix
	ret
;zxprog.c:114: static void rcmd_poll(void)
;	---------------------------------
; Function rcmd_poll
; ---------------------------------
_rcmd_poll:
;zxprog.c:116: unsigned char seq = RCMD_SEQ;
	ld	hl, #0x3f20
	ld	b, (hl)
;zxprog.c:118: if (seq != rcmd_last_seq) {
	ld	a, (_rcmd_last_seq+0)
	sub	a, b
	ret	Z
;zxprog.c:119: unsigned int src = (unsigned int)RCMD_SRC_LO |
	ld	a, (#0x3f22)
	ld	e, a
	ld	d, #0x00
	ld	a, (#0x3f23)
;	spillPairReg hl
;	spillPairReg hl
;	spillPairReg hl
;	spillPairReg hl
	ld	h, a
;	spillPairReg hl
;	spillPairReg hl
	ld	l, #0x00
;	spillPairReg hl
;	spillPairReg hl
	ld	a, e
	or	a, l
	ld	e, a
	ld	a, d
	or	a, h
	ld	d, a
;zxprog.c:121: unsigned char len = RCMD_LEN;
	ld	hl, #0x3f24
	ld	l, (hl)
;	spillPairReg hl
;zxprog.c:123: if (len > RCMD_MAX_LEN) {
	ld	a, #0x40
	sub	a, l
	jr	NC, 00102$
;zxprog.c:124: len = RCMD_MAX_LEN;
	ld	l, #0x40
;	spillPairReg hl
;	spillPairReg hl
00102$:
;zxprog.c:128: (const unsigned char *)src, (unsigned int)len);
	ld	h, #0x00
;	spillPairReg hl
;	spillPairReg hl
;zxprog.c:127: zcopy((unsigned char *)RCMD_BUF_ADDR,
	push	bc
	push	hl
	ld	hl, #0x3f40
	call	_zcopy
	pop	bc
;zxprog.c:130: RCMD_DONE = seq;
	ld	hl, #0x3f21
	ld	(hl), b
;zxprog.c:131: rcmd_last_seq = seq;
	ld	hl, #_rcmd_last_seq
	ld	(hl), b
;zxprog.c:133: }
	ret
;zxprog.c:136: void nmi_handler_c(void)
;	---------------------------------
; Function nmi_handler_c
; ---------------------------------
_nmi_handler_c::
;zxprog.c:138: wcmd_poll();
	call	_wcmd_poll
;zxprog.c:139: rcmd_poll();
;zxprog.c:140: }
	jp	_rcmd_poll
;zxprog.c:145: void main(void)
;	---------------------------------
; Function main
; ---------------------------------
_main::
;zxprog.c:149: wcmd_last_seq = WCMD_SEQ;
	ld	hl, #0x302e
	ld	a, (hl)
	ld	(_wcmd_last_seq+0), a
;zxprog.c:150: rcmd_last_seq = RCMD_SEQ;
	ld	hl, #0x3f20
	ld	a, (hl)
	ld	(_rcmd_last_seq+0), a
00104$:
;zxprog.c:153: wcmd_poll();
	call	_wcmd_poll
;zxprog.c:154: rcmd_poll();
	call	_rcmd_poll
;zxprog.c:156: if (LAUNCH_TRIGGER == LAUNCH_TRIGGER_GO) {
	ld	a, (#0x3f10)
	sub	a, #0x55
	jr	NZ, 00104$
;zxprog.c:158: ULA_PORT = YELLOW;
	ld	a, #0x06
	out	(_ULA_PORT), a
;zxprog.c:162: zx_launcher();
	call	_zx_launcher
;zxprog.c:165: }
	jr	00104$
	.area _CODE
	.area _INITIALIZER
	.area _CABS (ABS)
