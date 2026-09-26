000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. COMPUNSG.
000300* COMP receivers: a value wider than the picture is truncated on
000400* the left rather than abending in CVB, and an unsigned item
000500* takes the magnitude (#28). Expected output recorded from IBM
000600* ANS COBOL on TK5, except the truncated value: IBM keeps the low
000700* 32 bits of a 10-digit value, which is not a rule worth copying;
000800* the standard's left truncation is used.
000900 ENVIRONMENT DIVISION.
001000 DATA DIVISION.
001100 WORKING-STORAGE SECTION.
001200 01  X9    PIC S9(9) COMP.
001300 01  XD    PIC -(9)9.
001400 01  W     PIC 9(10) VALUE 9999999999.
001500 01  U     PIC 9(4) COMP VALUE 5.
001600 01  UD    PIC -9999.
001700 PROCEDURE DIVISION.
001800     SUBTRACT 10 FROM U. MOVE U TO UD.
001900     DISPLAY 'UNSIGNED [' UD ']'.
002000     IF U > 0 DISPLAY 'UNSIGNED POS'
002100         ELSE DISPLAY 'UNSIGNED NOT POS'.
002200     COMPUTE U = 3 - 8. MOVE U TO UD.
002300     DISPLAY 'UNSIGNED COMPUTE [' UD ']'.
002400     MOVE W TO X9. MOVE X9 TO XD.
002500     DISPLAY 'TRUNCATED [' XD ']'.
002600     STOP RUN.
