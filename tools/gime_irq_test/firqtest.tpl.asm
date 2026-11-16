* GIME FIRQ entry counter (improvements.11.16.md Phase 0 step 0.2)
* Counts FIRQ handler entries over NFRAMES frames (PIA0 FS flag polled,
* IRQ masked). Results: $4200-$4203 entries (32-bit), $4204 frames,
* $4206 = 1 when done.  Params substituted by the runner:
*   FAST  1 = $FFD9 (1.79 MHz)   MASK  $FF93 enable bits ($10 HBORD, $20 TMR)
*   TINS  $FF91 value ($20 = 3.58 MHz timer, $00 = scanline)  RELOAD 12-bit
FAST    equ     @FAST@
MASK    equ     @MASK@
TINS    equ     @TINS@
RELOAD  equ     @RELOAD@
NFRAMES equ     600
CNT     equ     $4200
FRM     equ     $4204
DONE    equ     $4206

        org     $4000
start   orcc    #$50
        lda     #FAST
        beq     slow
        sta     $FFD9
        bra     go
slow    sta     $FFD8
go      lda     #$7E            JMP handler at the $FEF4 FIRQ hook
        sta     $FEF4
        ldd     #handler
        std     $FEF5
        ldd     #0
        std     CNT
        std     CNT+2
        std     FRM
        std     DONE
        clr     $FF92
        lda     #TINS
        sta     $FF91
        ldd     #RELOAD
        stb     $FF95
        sta     $FF94           MSB write starts the timer
        lda     #MASK
        sta     $FF93
        lda     #$DC            $CC | FEN
        sta     $FF90
        lda     $FF02           clear FS flag
w1      tst     $FF03
        bpl     w1
        lda     $FF02
        andcc   #$BF            FIRQ on
w2      tst     $FF03
        bpl     w2
        lda     $FF02
        ldx     FRM
        leax    1,x
        stx     FRM
        cmpx    #NFRAMES
        bne     w2
        orcc    #$50
        clr     $FF93
        clr     $FF95           stop the timer (reload 0)
        clr     $FF94
        lda     #$CC
        sta     $FF90
        lda     #1
        sta     DONE
        bra     *

handler pshs    a
        lda     $FF93           acknowledge
        inc     CNT+3
        bne     h1
        inc     CNT+2
        bne     h1
        inc     CNT+1
h1      puls    a
        rti
