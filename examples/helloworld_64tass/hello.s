*= $0200   ; Hello, World!
LDX #$00   ; X = 0
loop:
BIT $FFE0  ; N = ready to send
BPL loop   ; If N = 0 goto loop
LDA text,X ; A = text[X]
STA $FFE1  ; UART Tx A
INX        ; X = X + 1
CMP #$00   ; if A - 0 ...
BNE loop   ; ... != 0 goto loop
STA $FFEF  ; Halt 6502
text:
.text  "Hello, World!"
.text $0D, $0A, $00
