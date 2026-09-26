000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. POWFIX.
000300* 1.05 ** 9 once took a data exception on the ninth multiply: the
000400* 8-byte multiplier capped the running product at 15 digits. The
000500* multiplier is now as long as the base, so the product may fill
000600* the whole work area (#27). Expected output recorded from IBM
000700* ANS COBOL on TK5.
000800 ENVIRONMENT DIVISION.
000900 DATA DIVISION.
001000 WORKING-STORAGE SECTION.
001100 01  R1        PIC 9(5)V9(4).
001200 01  R2        PIC 9(9)V99.
001300 01  R3        PIC S9(7)V9(3).
001400 01  BASE      PIC 9V99 VALUE 1.05.
001500 01  N         PIC 99.
001600 PROCEDURE DIVISION.
001700 MAIN.
001800     COMPUTE R1 = 1.05 ** 9.
001900     DISPLAY 'A ' R1.
002000     COMPUTE R1 = BASE ** 9.
002100     DISPLAY 'B ' R1.
002200     COMPUTE R2 = 7 ** 11.
002300     DISPLAY 'D ' R2.
002400     COMPUTE R3 = -3 ** 5.
002500     DISPLAY 'E ' R3.
002600     COMPUTE R3 ROUNDED = 1.5 ** 7.
002700     DISPLAY 'F ' R3.
002800     COMPUTE R2 = 123456 ** 2.
002900     DISPLAY 'G ' R2.
003000     STOP RUN.
