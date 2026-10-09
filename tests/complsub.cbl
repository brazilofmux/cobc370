000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. COMPLSUB.
000300* A literal or ZERO moved into a COMP item that sits under a group
000400* OCCURS, with a data-name subscript, takes the one-MVC path -- the
000500* item's own width is the element width, so the halfword/fullword
000600* test passes. That path built the receiver as 0(6)(4), a no-length
000700* reference with a length appended, which assembles only for a
000800* plain item. GL040 found it with MOVE 0 TO YT-LINEAR-DATE (WS-IDX).
000900 DATA DIVISION.
001000 WORKING-STORAGE SECTION.
001100 01  T.
001200     05  E OCCURS 3 TIMES.
001300         10  H PIC S9(4) COMP.
001400         10  F PIC S9(8) COMP.
001500 01  I PIC S9(4) COMP VALUE 1.
001600 01  J PIC S9(4) COMP VALUE 2.
001700 01  K PIC S9(4) COMP VALUE 3.
001800 01  SH PIC -9(4).
001900 01  SF PIC -9(8).
002000 PROCEDURE DIVISION.
002100     MOVE ZERO TO H (I).
002200     MOVE 1234 TO H (J).
002300     MOVE -77 TO H (K).
002400     MOVE 0 TO F (I).
002500     MOVE 12345678 TO F (J).
002600     MOVE -5 TO F (K).
002700     MOVE H (I) TO SH. DISPLAY 'H1 ' SH.
002800     MOVE H (J) TO SH. DISPLAY 'H2 ' SH.
002900     MOVE H (K) TO SH. DISPLAY 'H3 ' SH.
003000     MOVE F (I) TO SF. DISPLAY 'F1 ' SF.
003100     MOVE F (J) TO SF. DISPLAY 'F2 ' SF.
003200     MOVE F (K) TO SF. DISPLAY 'F3 ' SF.
003300     STOP RUN.
