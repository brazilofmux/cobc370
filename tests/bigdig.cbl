000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. BIGDIG.
000300* 18 digits is the standard's ceiling: 1 NUC 1,2 puts numeric
000400* literals at 1 through 18 and arithmetic operands at 18.
000500* Packed that is 10 bytes, which is why the decimal scratch
000600* areas are 16 bytes and not 8. Zoned stops at 16, because
000700* PACK and UNPK hold their operand lengths in four bits.
000800* Every value here stays inside its own picture: this tests the
000900* width, not what a compiler does when a result will not fit.
001000 DATA DIVISION.
001100 WORKING-STORAGE SECTION.
001200 01  Z16  PIC 9(16)         VALUE 1234567890123456.
001300 01  ZR   PIC 9(16)         VALUE 0.
001400 01  A18  PIC S9(18) COMP-3 VALUE 999999999999999999.
001500 01  B18  PIC S9(18) COMP-3 VALUE 111111111111111111.
001600 01  N18  PIC S9(18) COMP-3 VALUE -123456789012345678.
001700 01  Q18  PIC S9(18) COMP-3 VALUE 0.
001800 01  M18  PIC S9(16)V99 COMP-3 VALUE 0.
001900 01  E18  PIC -9(18).
002000 01  E17  PIC -9(9).9(8).
002100 01  FLAG PIC X(5).
002200 PROCEDURE DIVISION.
002300     DISPLAY Z16.
002400     MOVE Z16 TO ZR.
002500     DISPLAY ZR.
002600     SUBTRACT 1 FROM ZR.
002700     DISPLAY ZR.
002800     MOVE A18 TO E18.
002900     DISPLAY E18.
003000     SUBTRACT B18 FROM A18 GIVING Q18.
003100     MOVE Q18 TO E18.
003200     DISPLAY E18.
003300     ADD B18 TO Q18.
003400     MOVE Q18 TO E18.
003500     DISPLAY E18.
003600     MOVE N18 TO E18.
003700     DISPLAY E18.
003800     COMPUTE Q18 = A18 + N18.
003900     MOVE Q18 TO E18.
004000     DISPLAY E18.
004100     MOVE 12345678901234.56 TO M18.
004200     MOVE 123456789.12345678 TO E17.
004300     DISPLAY E17.
004400     MOVE -123456789.12345678 TO E17.
004500     DISPLAY E17.
004600     IF A18 > B18
004700         MOVE 'GT   ' TO FLAG
004800     ELSE
004900         MOVE 'NOTGT' TO FLAG.
005000     DISPLAY FLAG.
005050* The oracle is wrong on the next one, and this is the expected
005060* value rather than GnuCOBOL's. GnuCOBOL 4.0-early-dev reports a
005070* signed COMP-3 of exactly 18 digits as not less than zero. At 17
005080* digits it is right, on a zoned 18 it is right, and comparing the
005090* same item against the full literal is right -- only the compare
005095* against zero at 18 packed digits is wrong.
005100     IF N18 < 0
005200         MOVE 'NEG  ' TO FLAG
005300     ELSE
005400         MOVE 'POS  ' TO FLAG.
005500     DISPLAY FLAG.
005600     STOP RUN.
