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
_kbd_prev0:
	.ds 1
_kbd_prev1:
	.ds 1
_kbd_prev2:
	.ds 1
_kbd_prev3:
	.ds 1
_kbd_prev4:
	.ds 1
_kbd_prev5:
	.ds 1
_kbd_prev6:
	.ds 1
_kbd_prev7:
	.ds 1
_wcmd_poll_load_border_phase_65536_47:
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
;zxprog.c:378: static unsigned char load_border_phase = 0u;
	ld	iy, #_wcmd_poll_load_border_phase_65536_47
	ld	0 (iy), #0x00
;--------------------------------------------------------
; Home
;--------------------------------------------------------
	.area _HOME
	.area _HOME
;--------------------------------------------------------
; code
;--------------------------------------------------------
	.area _CODE
;zxprog.c:112: static void zcopy(unsigned char *dst, const unsigned char *src, unsigned int len) __naked
;	---------------------------------
; Function zcopy
; ---------------------------------
_zcopy:
;zxprog.c:131: __endasm;
	push	ix
	ld	ix, #0
	add	ix, sp
	ld	c, 4 (ix)
	ld	b, 5 (ix)
	ex	de, hl
	ld	a, b
	or	a, c
	jr	z, 00001$
	ldir
	00001$:
	pop	ix
	pop	hl
	pop	af
	jp	(hl)
;zxprog.c:132: }
;zxprog.c:135: static void zfill(unsigned char *dst, unsigned char value, unsigned int len)
;	---------------------------------
; Function zfill
; ---------------------------------
_zfill:
	push	ix
	ld	ix,#0
	add	ix,sp
	ld	c, l
	ld	b, h
;zxprog.c:137: while (len--) {
	ld	e, 5 (ix)
	ld	d, 6 (ix)
00101$:
	ld	a, e
	ld	l, d
;	spillPairReg hl
;	spillPairReg hl
	dec	de
	or	a, l
	jr	Z, 00104$
;zxprog.c:138: *dst++ = value;
	ld	a, 4 (ix)
	ld	(bc), a
	inc	bc
	jr	00101$
00104$:
;zxprog.c:140: }
	pop	ix
	pop	hl
	pop	af
	inc	sp
	jp	(hl)
;zxprog.c:143: static unsigned char kbd_row0_read(void) __naked
;	---------------------------------
; Function kbd_row0_read
; ---------------------------------
_kbd_row0_read:
;zxprog.c:150: __endasm;
	ld	bc, #0xFEFE
	in	a, (c)
	ld	l, a
	ret
;zxprog.c:151: }
;zxprog.c:153: static unsigned char kbd_row1_read(void) __naked
;	---------------------------------
; Function kbd_row1_read
; ---------------------------------
_kbd_row1_read:
;zxprog.c:160: __endasm;
	ld	bc, #0xFDFE
	in	a, (c)
	ld	l, a
	ret
;zxprog.c:161: }
;zxprog.c:163: static unsigned char kbd_row2_read(void) __naked
;	---------------------------------
; Function kbd_row2_read
; ---------------------------------
_kbd_row2_read:
;zxprog.c:170: __endasm;
	ld	bc, #0xFBFE
	in	a, (c)
	ld	l, a
	ret
;zxprog.c:171: }
;zxprog.c:173: static unsigned char kbd_row3_read(void) __naked
;	---------------------------------
; Function kbd_row3_read
; ---------------------------------
_kbd_row3_read:
;zxprog.c:180: __endasm;
	ld	bc, #0xF7FE
	in	a, (c)
	ld	l, a
	ret
;zxprog.c:181: }
;zxprog.c:183: static unsigned char kbd_row4_read(void) __naked
;	---------------------------------
; Function kbd_row4_read
; ---------------------------------
_kbd_row4_read:
;zxprog.c:190: __endasm;
	ld	bc, #0xEFFE
	in	a, (c)
	ld	l, a
	ret
;zxprog.c:191: }
;zxprog.c:193: static unsigned char kbd_row5_read(void) __naked
;	---------------------------------
; Function kbd_row5_read
; ---------------------------------
_kbd_row5_read:
;zxprog.c:200: __endasm;
	ld	bc, #0xDFFE
	in	a, (c)
	ld	l, a
	ret
;zxprog.c:201: }
;zxprog.c:203: static unsigned char kbd_row6_read(void) __naked
;	---------------------------------
; Function kbd_row6_read
; ---------------------------------
_kbd_row6_read:
;zxprog.c:210: __endasm;
	ld	bc, #0xBFFE
	in	a, (c)
	ld	l, a
	ret
;zxprog.c:211: }
;zxprog.c:213: static unsigned char kbd_row7_read(void) __naked
;	---------------------------------
; Function kbd_row7_read
; ---------------------------------
_kbd_row7_read:
;zxprog.c:220: __endasm;
	ld	bc, #0x7FFE
	in	a, (c)
	ld	l, a
	ret
;zxprog.c:221: }
;zxprog.c:223: static void kbd_publish(unsigned char code)
;	---------------------------------
; Function kbd_publish
; ---------------------------------
_kbd_publish:
;zxprog.c:227: if (code == 0u) {
	ld	c, a
	or	a, a
;zxprog.c:228: return;
	ret	Z
;zxprog.c:231: KEY_CODE = code;
	ld	hl, #0x3029
	ld	(hl), c
;zxprog.c:232: seq = (unsigned char)(KEY_SEQ + 1u);
	ld	a, (#0x3028)
	inc	a
	ld	c, a
;zxprog.c:233: if (seq == 0u) {
	or	a, a
	jr	NZ, 00104$
;zxprog.c:234: seq = 1u;
	ld	c, #0x01
00104$:
;zxprog.c:236: KEY_SEQ = seq;
	ld	hl, #0x3028
	ld	(hl), c
;zxprog.c:237: }
	ret
;zxprog.c:239: static unsigned char kbd_decode_press(unsigned char row, unsigned char bit, unsigned char shift_down)
;	---------------------------------
; Function kbd_decode_press
; ---------------------------------
_kbd_decode_press:
	push	ix
	ld	ix,#0
	add	ix,sp
	dec	sp
	ld	-1 (ix), a
	ld	c, l
;zxprog.c:241: switch (row) {
	ld	a, #0x07
	sub	a, -1 (ix)
	jp	C, 00163$
	ld	e, -1 (ix)
	ld	d, #0x00
	ld	hl, #00424$
	add	hl, de
	add	hl, de
	add	hl, de
	jp	(hl)
00424$:
	jp	00101$
	jp	00108$
	jp	00116$
	jp	00124$
	jp	00132$
	jp	00140$
	jp	00148$
	jp	00156$
;zxprog.c:242: case 0u:
00101$:
;zxprog.c:243: switch (bit) {
	ld	a, c
	dec	a
	jr	Z, 00102$
	ld	a,c
	cp	a,#0x02
	jr	Z, 00103$
	cp	a,#0x03
	jr	Z, 00104$
	sub	a, #0x04
	jr	Z, 00105$
	jr	00106$
;zxprog.c:244: case 1u: return shift_down ? 'Z' : 'z';
00102$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00167$
	ld	bc, #0x005a
	jr	00168$
00167$:
	ld	bc, #0x007a
00168$:
	ld	a, c
	jp	00165$
;zxprog.c:245: case 2u: return shift_down ? 'X' : 'x';
00103$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00169$
	ld	bc, #0x0058
	jr	00170$
00169$:
	ld	bc, #0x0078
00170$:
	ld	a, c
	jp	00165$
;zxprog.c:246: case 3u: return shift_down ? 'C' : 'c';
00104$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00171$
	ld	bc, #0x0043
	jr	00172$
00171$:
	ld	bc, #0x0063
00172$:
	ld	a, c
	jp	00165$
;zxprog.c:247: case 4u: return shift_down ? 'V' : 'v';
00105$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00173$
	ld	bc, #0x0056
	jr	00174$
00173$:
	ld	bc, #0x0076
00174$:
	ld	a, c
	jp	00165$
;zxprog.c:248: default: return 0u;
00106$:
	xor	a, a
	jp	00165$
;zxprog.c:250: case 1u:
00108$:
;zxprog.c:251: switch (bit) {
	ld	a, #0x04
	sub	a, c
	jr	C, 00114$
	ld	b, #0x00
	ld	hl, #00429$
	add	hl, bc
	add	hl, bc
	add	hl, bc
	jp	(hl)
00429$:
	jp	00109$
	jp	00110$
	jp	00111$
	jp	00112$
	jp	00113$
;zxprog.c:252: case 0u: return shift_down ? 'A' : 'a';
00109$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00175$
	ld	bc, #0x0041
	jr	00176$
00175$:
	ld	bc, #0x0061
00176$:
	ld	a, c
	jp	00165$
;zxprog.c:253: case 1u: return shift_down ? 'S' : 's';
00110$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00177$
	ld	bc, #0x0053
	jr	00178$
00177$:
	ld	bc, #0x0073
00178$:
	ld	a, c
	jp	00165$
;zxprog.c:254: case 2u: return shift_down ? 'D' : 'd';
00111$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00179$
	ld	bc, #0x0044
	jr	00180$
00179$:
	ld	bc, #0x0064
00180$:
	ld	a, c
	jp	00165$
;zxprog.c:255: case 3u: return shift_down ? 'F' : 'f';
00112$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00181$
	ld	bc, #0x0046
	jr	00182$
00181$:
	ld	bc, #0x0066
00182$:
	ld	a, c
	jp	00165$
;zxprog.c:256: case 4u: return shift_down ? 'G' : 'g';
00113$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00183$
	ld	bc, #0x0047
	jr	00184$
00183$:
	ld	bc, #0x0067
00184$:
	ld	a, c
	jp	00165$
;zxprog.c:257: default: return 0u;
00114$:
	xor	a, a
	jp	00165$
;zxprog.c:259: case 2u:
00116$:
;zxprog.c:260: switch (bit) {
	ld	a, #0x04
	sub	a, c
	jr	C, 00122$
	ld	b, #0x00
	ld	hl, #00430$
	add	hl, bc
	add	hl, bc
	add	hl, bc
	jp	(hl)
00430$:
	jp	00117$
	jp	00118$
	jp	00119$
	jp	00120$
	jp	00121$
;zxprog.c:261: case 0u: return shift_down ? 'Q' : 'q';
00117$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00185$
	ld	bc, #0x0051
	jr	00186$
00185$:
	ld	bc, #0x0071
00186$:
	ld	a, c
	jp	00165$
;zxprog.c:262: case 1u: return shift_down ? 'W' : 'w';
00118$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00187$
	ld	bc, #0x0057
	jr	00188$
00187$:
	ld	bc, #0x0077
00188$:
	ld	a, c
	jp	00165$
;zxprog.c:263: case 2u: return shift_down ? 'E' : 'e';
00119$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00189$
	ld	bc, #0x0045
	jr	00190$
00189$:
	ld	bc, #0x0065
00190$:
	ld	a, c
	jp	00165$
;zxprog.c:264: case 3u: return shift_down ? 'R' : 'r';
00120$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00191$
	ld	bc, #0x0052
	jr	00192$
00191$:
	ld	bc, #0x0072
00192$:
	ld	a, c
	jp	00165$
;zxprog.c:265: case 4u: return shift_down ? 'T' : 't';
00121$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00193$
	ld	bc, #0x0054
	jr	00194$
00193$:
	ld	bc, #0x0074
00194$:
	ld	a, c
	jp	00165$
;zxprog.c:266: default: return 0u;
00122$:
	xor	a, a
	jp	00165$
;zxprog.c:268: case 3u:
00124$:
;zxprog.c:269: switch (bit) {
	ld	a, #0x04
	sub	a, c
	jr	C, 00130$
	ld	b, #0x00
	ld	hl, #00431$
	add	hl, bc
	add	hl, bc
	add	hl, bc
	jp	(hl)
00431$:
	jp	00125$
	jp	00126$
	jp	00127$
	jp	00128$
	jp	00129$
;zxprog.c:270: case 0u: return '1';
00125$:
	ld	a, #0x31
	jp	00165$
;zxprog.c:271: case 1u: return '2';
00126$:
	ld	a, #0x32
	jp	00165$
;zxprog.c:272: case 2u: return '3';
00127$:
	ld	a, #0x33
	jp	00165$
;zxprog.c:273: case 3u: return '4';
00128$:
	ld	a, #0x34
	jp	00165$
;zxprog.c:274: case 4u: return '5';
00129$:
	ld	a, #0x35
	jp	00165$
;zxprog.c:275: default: return 0u;
00130$:
	xor	a, a
	jp	00165$
;zxprog.c:277: case 4u:
00132$:
;zxprog.c:278: switch (bit) {
	ld	a, #0x04
	sub	a, c
	jr	C, 00138$
	ld	b, #0x00
	ld	hl, #00432$
	add	hl, bc
	add	hl, bc
	add	hl, bc
	jp	(hl)
00432$:
	jp	00133$
	jp	00134$
	jp	00135$
	jp	00136$
	jp	00137$
;zxprog.c:279: case 0u: return '0';
00133$:
	ld	a, #0x30
	jp	00165$
;zxprog.c:280: case 1u: return '9';
00134$:
	ld	a, #0x39
	jp	00165$
;zxprog.c:281: case 2u: return '8';
00135$:
	ld	a, #0x38
	jp	00165$
;zxprog.c:282: case 3u: return '7';
00136$:
	ld	a, #0x37
	jp	00165$
;zxprog.c:283: case 4u: return '6';
00137$:
	ld	a, #0x36
	jp	00165$
;zxprog.c:284: default: return 0u;
00138$:
	xor	a, a
	jp	00165$
;zxprog.c:286: case 5u:
00140$:
;zxprog.c:287: switch (bit) {
	ld	a, #0x04
	sub	a, c
	jr	C, 00146$
	ld	b, #0x00
	ld	hl, #00433$
	add	hl, bc
	add	hl, bc
	add	hl, bc
	jp	(hl)
00433$:
	jp	00141$
	jp	00142$
	jp	00143$
	jp	00144$
	jp	00145$
;zxprog.c:288: case 0u: return shift_down ? 'P' : 'p';
00141$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00195$
	ld	bc, #0x0050
	jr	00196$
00195$:
	ld	bc, #0x0070
00196$:
	ld	a, c
	jp	00165$
;zxprog.c:289: case 1u: return shift_down ? 'O' : 'o';
00142$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00197$
	ld	bc, #0x004f
	jr	00198$
00197$:
	ld	bc, #0x006f
00198$:
	ld	a, c
	jp	00165$
;zxprog.c:290: case 2u: return shift_down ? 'I' : 'i';
00143$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00199$
	ld	bc, #0x0049
	jr	00200$
00199$:
	ld	bc, #0x0069
00200$:
	ld	a, c
	jp	00165$
;zxprog.c:291: case 3u: return shift_down ? 'U' : 'u';
00144$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00201$
	ld	bc, #0x0055
	jr	00202$
00201$:
	ld	bc, #0x0075
00202$:
	ld	a, c
	jp	00165$
;zxprog.c:292: case 4u: return shift_down ? 'Y' : 'y';
00145$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00203$
	ld	bc, #0x0059
	jr	00204$
00203$:
	ld	bc, #0x0079
00204$:
	ld	a, c
	jp	00165$
;zxprog.c:293: default: return 0u;
00146$:
	xor	a, a
	jp	00165$
;zxprog.c:295: case 6u:
00148$:
;zxprog.c:296: switch (bit) {
	ld	a, #0x04
	sub	a, c
	jr	C, 00154$
	ld	b, #0x00
	ld	hl, #00434$
	add	hl, bc
	add	hl, bc
	add	hl, bc
	jp	(hl)
00434$:
	jp	00149$
	jp	00150$
	jp	00151$
	jp	00152$
	jp	00153$
;zxprog.c:297: case 0u: return '\n';
00149$:
	ld	a, #0x0a
	jp	00165$
;zxprog.c:298: case 1u: return shift_down ? 'L' : 'l';
00150$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00205$
	ld	bc, #0x004c
	jr	00206$
00205$:
	ld	bc, #0x006c
00206$:
	ld	a, c
	jp	00165$
;zxprog.c:299: case 2u: return shift_down ? 'K' : 'k';
00151$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00207$
	ld	bc, #0x004b
	jr	00208$
00207$:
	ld	bc, #0x006b
00208$:
	ld	a, c
	jr	00165$
;zxprog.c:300: case 3u: return shift_down ? 'J' : 'j';
00152$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00209$
	ld	bc, #0x004a
	jr	00210$
00209$:
	ld	bc, #0x006a
00210$:
	ld	a, c
	jr	00165$
;zxprog.c:301: case 4u: return shift_down ? 'H' : 'h';
00153$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00211$
	ld	bc, #0x0048
	jr	00212$
00211$:
	ld	bc, #0x0068
00212$:
	ld	a, c
	jr	00165$
;zxprog.c:302: default: return 0u;
00154$:
	xor	a, a
	jr	00165$
;zxprog.c:304: case 7u:
00156$:
;zxprog.c:305: switch (bit) {
	ld	a, c
	or	a, a
	jr	Z, 00157$
	ld	a,c
	cp	a,#0x02
	jr	Z, 00158$
	cp	a,#0x03
	jr	Z, 00159$
	sub	a, #0x04
	jr	Z, 00160$
	jr	00161$
;zxprog.c:306: case 0u: return ' ';
00157$:
	ld	a, #0x20
	jr	00165$
;zxprog.c:307: case 2u: return shift_down ? 'M' : 'm';
00158$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00213$
	ld	bc, #0x004d
	jr	00214$
00213$:
	ld	bc, #0x006d
00214$:
	ld	a, c
	jr	00165$
;zxprog.c:308: case 3u: return shift_down ? 'N' : 'n';
00159$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00215$
	ld	bc, #0x004e
	jr	00216$
00215$:
	ld	bc, #0x006e
00216$:
	ld	a, c
	jr	00165$
;zxprog.c:309: case 4u: return shift_down ? 'B' : 'b';
00160$:
	ld	a, 4 (ix)
	or	a, a
	jr	Z, 00217$
	ld	bc, #0x0042
	jr	00218$
00217$:
	ld	bc, #0x0062
00218$:
	ld	a, c
	jr	00165$
;zxprog.c:310: default: return 0u;
00161$:
	xor	a, a
	jr	00165$
;zxprog.c:312: default:
00163$:
;zxprog.c:313: return 0u;
	xor	a, a
;zxprog.c:314: }
00165$:
;zxprog.c:315: }
	inc	sp
	pop	ix
	pop	hl
	inc	sp
	jp	(hl)
;zxprog.c:317: static void kbd_poll_publish(void)
;	---------------------------------
; Function kbd_poll_publish
; ---------------------------------
_kbd_poll_publish:
	push	ix
	ld	ix,#0
	add	ix,sp
	ld	hl, #-21
	add	hl, sp
	ld	sp, hl
;zxprog.c:325: rows[0] = kbd_row0_read();
	ld	hl, #0
	add	hl, sp
	push	hl
	call	_kbd_row0_read
	pop	bc
	ld	(bc), a
;zxprog.c:326: rows[1] = kbd_row1_read();
	push	bc
	call	_kbd_row1_read
;zxprog.c:327: rows[2] = kbd_row2_read();
	ld	-20 (ix), a
	call	_kbd_row2_read
;zxprog.c:328: rows[3] = kbd_row3_read();
	ld	-19 (ix), a
	call	_kbd_row3_read
;zxprog.c:329: rows[4] = kbd_row4_read();
	ld	-18 (ix), a
	call	_kbd_row4_read
;zxprog.c:330: rows[5] = kbd_row5_read();
	ld	-17 (ix), a
	call	_kbd_row5_read
;zxprog.c:331: rows[6] = kbd_row6_read();
	ld	-16 (ix), a
	call	_kbd_row6_read
;zxprog.c:332: rows[7] = kbd_row7_read();
	ld	-15 (ix), a
	call	_kbd_row7_read
	pop	bc
	ld	-14 (ix), a
;zxprog.c:334: prev[0] = kbd_prev0;
	ld	a, (_kbd_prev0+0)
	ld	-13 (ix), a
;zxprog.c:335: prev[1] = kbd_prev1;
	ld	a, (_kbd_prev1+0)
	ld	-12 (ix), a
;zxprog.c:336: prev[2] = kbd_prev2;
	ld	a, (_kbd_prev2+0)
	ld	-11 (ix), a
;zxprog.c:337: prev[3] = kbd_prev3;
	ld	a, (_kbd_prev3+0)
	ld	-10 (ix), a
;zxprog.c:338: prev[4] = kbd_prev4;
	ld	a, (_kbd_prev4+0)
	ld	-9 (ix), a
;zxprog.c:339: prev[5] = kbd_prev5;
	ld	a, (_kbd_prev5+0)
	ld	-8 (ix), a
;zxprog.c:340: prev[6] = kbd_prev6;
	ld	a, (_kbd_prev6+0)
	ld	-7 (ix), a
;zxprog.c:341: prev[7] = kbd_prev7;
	ld	a, (_kbd_prev7+0)
	ld	-6 (ix), a
;zxprog.c:343: kbd_prev0 = rows[0];
	ld	a, (bc)
	ld	(_kbd_prev0+0), a
;zxprog.c:344: kbd_prev1 = rows[1];
	ld	a, -20 (ix)
	ld	(_kbd_prev1+0), a
;zxprog.c:345: kbd_prev2 = rows[2];
	ld	a, -19 (ix)
	ld	(_kbd_prev2+0), a
;zxprog.c:346: kbd_prev3 = rows[3];
	ld	a, -18 (ix)
	ld	(_kbd_prev3+0), a
;zxprog.c:347: kbd_prev4 = rows[4];
	ld	a, -17 (ix)
	ld	(_kbd_prev4+0), a
;zxprog.c:348: kbd_prev5 = rows[5];
	ld	a, -16 (ix)
	ld	(_kbd_prev5+0), a
;zxprog.c:349: kbd_prev6 = rows[6];
	ld	a, -15 (ix)
	ld	(_kbd_prev6+0), a
;zxprog.c:350: kbd_prev7 = rows[7];
	ld	a, -14 (ix)
	ld	(_kbd_prev7+0), a
;zxprog.c:352: shift_down = ((rows[0] & 0x01u) == 0u) ? 1u : 0u; /* CAPS SHIFT */
	ld	a, (bc)
	rrca
	jr	C, 00111$
	ld	de, #0x0001
	jr	00112$
00111$:
	ld	de, #0x0000
00112$:
	ld	-5 (ix), e
;zxprog.c:354: for (r = 0u; r < 8u; ++r) {
	ld	-4 (ix), #0x00
	ld	e, #0x00
00107$:
;zxprog.c:355: unsigned char new_presses = (unsigned char)(prev[r] & (unsigned char)~rows[r]);
	push	de
	ld	d, #0x00
	ld	hl, #10
	add	hl, sp
	add	hl, de
	pop	de
	ld	d, (hl)
	ld	l, e
	ld	h, #0x00
	add	hl, bc
	ld	a, (hl)
	cpl
	and	a, d
	ld	-3 (ix), a
;zxprog.c:356: for (b = 0u; b < 5u; ++b) {
	ld	-2 (ix), #0x00
	ld	d, #0x00
00105$:
;zxprog.c:357: if ((new_presses & (unsigned char)(1u << b)) != 0u) {
	push	de
	ld	l, #0x01
;	spillPairReg hl
;	spillPairReg hl
	pop	af
	inc	a
	jr	00141$
00140$:
	sla	l
00141$:
	dec	a
	jr	NZ,00140$
	ld	a, -3 (ix)
	and	a, l
	ld	-1 (ix), a
	or	a, a
	jr	Z, 00106$
;zxprog.c:358: kbd_publish(kbd_decode_press(r, b, shift_down));
	ld	a, -5 (ix)
	push	af
	inc	sp
	ld	l, -2 (ix)
;	spillPairReg hl
;	spillPairReg hl
	ld	a, -4 (ix)
	call	_kbd_decode_press
	call	_kbd_publish
;zxprog.c:359: return;
	jr	00109$
00106$:
;zxprog.c:356: for (b = 0u; b < 5u; ++b) {
	inc	d
	ld	-2 (ix), d
	ld	a, d
	sub	a, #0x05
	jr	C, 00105$
;zxprog.c:354: for (r = 0u; r < 8u; ++r) {
	inc	e
	ld	-4 (ix), e
	ld	a, e
	sub	a, #0x08
	jr	C, 00107$
00109$:
;zxprog.c:363: }
	ld	sp, ix
	pop	ix
	ret
;zxprog.c:366: static void zx_startup_clear(void)
;	---------------------------------
; Function zx_startup_clear
; ---------------------------------
_zx_startup_clear:
;zxprog.c:369: zfill((unsigned char *)ZX_RAM_BASE_ADDR, 0x00u, ZX_RAM_SIZE);
	ld	hl, #0xc000
	push	hl
	xor	a, a
	push	af
	inc	sp
	ld	h, #0x40
	call	_zfill
;zxprog.c:372: zfill((unsigned char *)ATTR_BASE, ATTR(0, WHITE, BLACK), ATTR_SIZE);
	ld	hl, #0x0300
	push	hl
	ld	a, #0x38
	push	af
	inc	sp
	ld	h, #0x58
	call	_zfill
;zxprog.c:373: }
	ret
;zxprog.c:376: static void wcmd_poll(void)
;	---------------------------------
; Function wcmd_poll
; ---------------------------------
_wcmd_poll:
	push	ix
	ld	ix,#0
	add	ix,sp
	push	af
;zxprog.c:382: unsigned char seq = WCMD_SEQ;
	ld	hl, #0x302e
	ld	c, (hl)
;zxprog.c:385: if (seq != WCMD_DONE) {
	ld	a, (#0x302f)
	sub	a, c
	jr	Z, 00107$
;zxprog.c:386: unsigned int dst = (unsigned int)WCMD_DST_LO |
	ld	a, (#0x3030)
	ld	b, #0x00
	ld	hl, #0x3031
	ld	d, (hl)
	ld	e, #0x00
	or	a, e
	ld	e, a
	ld	a, b
	or	a, d
	ld	-2 (ix), e
	ld	-1 (ix), a
;zxprog.c:388: unsigned int len = (unsigned int)WCMD_LEN_LO |
	ld	a, (#0x3032)
	ld	b, #0x00
	ld	hl, #0x3033
	ld	d, (hl)
	ld	e, #0x00
	or	a, e
	ld	e, a
	ld	a, b
	or	a, d
	ld	d, a
;zxprog.c:391: if (len > WCMD_MAX_DATA) {
	xor	a, a
	cp	a, e
	ld	a, #0x02
	sbc	a, d
	jr	NC, 00102$
;zxprog.c:392: len = WCMD_MAX_DATA;
	ld	de, #0x0200
00102$:
;zxprog.c:395: zcopy((unsigned char *)dst,
	pop	hl
	push	hl
	push	bc
	push	de
	ld	de, #0x3034
	call	_zcopy
	pop	bc
;zxprog.c:400: if ((LOADER_FLAGS & LOADER_FLAG_BORDER_ANIM) != 0u) {
	ld	a, (#0x3f11)
	rrca
	jr	NC, 00104$
;zxprog.c:401: zx_border(load_border_cycle[load_border_phase]);
	ld	a, #<(_wcmd_poll_load_border_cycle_65536_47)
	ld	hl, #_wcmd_poll_load_border_phase_65536_47
	add	a, (hl)
	ld	e, a
	ld	a, #>(_wcmd_poll_load_border_cycle_65536_47)
	adc	a, #0x00
	ld	d, a
	ld	a, (de)
	and	a, #0x07
	out	(_ULA_PORT), a
;zxprog.c:402: load_border_phase = (unsigned char)((load_border_phase + 1u) % 6u);
	ld	a, (_wcmd_poll_load_border_phase_65536_47+0)
	ld	h, #0x00
;	spillPairReg hl
;	spillPairReg hl
	ld	l, a
;	spillPairReg hl
;	spillPairReg hl
	inc	hl
	push	bc
	ld	de, #0x0006
	call	__moduint
	pop	bc
	ld	hl, #_wcmd_poll_load_border_phase_65536_47
	ld	(hl), e
00104$:
;zxprog.c:405: WCMD_DONE = seq;
	ld	hl, #0x302f
	ld	(hl), c
00107$:
;zxprog.c:407: }
	ld	sp, ix
	pop	ix
	ret
_wcmd_poll_load_border_cycle_65536_47:
	.db #0x01	; 1
	.db #0x05	; 5
	.db #0x02	; 2
	.db #0x03	; 3
	.db #0x06	; 6
	.db #0x04	; 4
;zxprog.c:410: static void rcmd_poll(void)
;	---------------------------------
; Function rcmd_poll
; ---------------------------------
_rcmd_poll:
;zxprog.c:412: unsigned char seq = RCMD_SEQ;
	ld	hl, #0x3f20
	ld	c, (hl)
;zxprog.c:415: if (seq != RCMD_DONE) {
	ld	a, (#0x3f21)
	sub	a, c
	ret	Z
;zxprog.c:416: unsigned int src = (unsigned int)RCMD_SRC_LO |
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
;zxprog.c:418: unsigned char len = RCMD_LEN;
	ld	hl, #0x3f24
	ld	l, (hl)
;	spillPairReg hl
;zxprog.c:420: if (len > RCMD_MAX_LEN) {
	ld	a, #0x40
	sub	a, l
	jr	NC, 00102$
;zxprog.c:421: len = RCMD_MAX_LEN;
	ld	l, #0x40
;	spillPairReg hl
;	spillPairReg hl
00102$:
;zxprog.c:425: (const unsigned char *)src, (unsigned int)len);
	ld	h, #0x00
;	spillPairReg hl
;	spillPairReg hl
;zxprog.c:424: zcopy((unsigned char *)RCMD_BUF_ADDR,
	push	bc
	push	hl
	ld	hl, #0x3f40
	call	_zcopy
	pop	bc
;zxprog.c:427: RCMD_DONE = seq;
	ld	hl, #0x3f21
	ld	(hl), c
;zxprog.c:429: }
	ret
;zxprog.c:446: static void pgcmd_do_out(void) __naked
;	---------------------------------
; Function pgcmd_do_out
; ---------------------------------
_pgcmd_do_out:
;zxprog.c:453: __endasm;
	ld	a, (0x302C) ; read (*((volatile unsigned char *)0x302Cu)) from cart RAM
	ld	bc, #0x7FFD
	out	(c), a ; OUT (C), A with BC=0x7FFD
	ret
;zxprog.c:454: }
;zxprog.c:458: static void pgcmd_poll(void)
;	---------------------------------
; Function pgcmd_poll
; ---------------------------------
_pgcmd_poll:
;zxprog.c:460: unsigned char seq = PGCMD_SEQ;
	ld	hl, #0x302a
	ld	c, (hl)
;zxprog.c:461: if (seq != PGCMD_DONE) {
	ld	a, (#0x302b)
	sub	a, c
	ret	Z
;zxprog.c:462: pgcmd_do_out();
	push	bc
	call	_pgcmd_do_out
	pop	bc
;zxprog.c:463: PGCMD_DONE = seq;
	ld	hl, #0x302b
	ld	(hl), c
;zxprog.c:465: }
	ret
;zxprog.c:468: void nmi_handler_c(void)
;	---------------------------------
; Function nmi_handler_c
; ---------------------------------
_nmi_handler_c::
;zxprog.c:470: wcmd_poll();
	call	_wcmd_poll
;zxprog.c:471: rcmd_poll();
	call	_rcmd_poll
;zxprog.c:472: pgcmd_poll();
	call	_pgcmd_poll
;zxprog.c:473: kbd_poll_publish();
;zxprog.c:474: }
	jp	_kbd_poll_publish
;zxprog.c:479: void main(void)
;	---------------------------------
; Function main
; ---------------------------------
_main::
;zxprog.c:481: zx_startup_clear();
	call	_zx_startup_clear
;zxprog.c:482: zx_border(GREEN); /* startup complete, main poll loop running */
	ld	a, #0x04
	out	(_ULA_PORT), a
00104$:
;zxprog.c:487: wcmd_poll();
	call	_wcmd_poll
;zxprog.c:488: rcmd_poll();
	call	_rcmd_poll
;zxprog.c:489: pgcmd_poll();
	call	_pgcmd_poll
;zxprog.c:490: kbd_poll_publish();
	call	_kbd_poll_publish
;zxprog.c:494: if (LAUNCH_TRIGGER == 0x55u) {
	ld	a, (#0x3f10)
	sub	a, #0x55
	jr	NZ, 00104$
;zxprog.c:498: zx_launcher();
	call	_zx_launcher
;zxprog.c:501: }
	jr	00104$
	.area _CODE
	.area _INITIALIZER
	.area _CABS (ABS)
