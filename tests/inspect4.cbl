000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. INSPECT4.
000300* INSPECT with two phrases whose strings overlap: one left-to-
000400* right pass, the phrases tried in the order written at each
000500* position, the first match taking the position (II-68 to II-70).
000600* On 'AABAA', ALL 'A' then ALL 'AA' tallies 4 and 0; a pass per
000700* phrase would give 4 and 2. Written the other way round, ALL
000800* 'AA' takes both pairs and ALL 'A' finds nothing left. The same
000900* two orders for REPLACING. IKFCBL00 has no INSPECT; expected
001000* output is the standard's rule, worked by hand, and matches
001100* s32-cobc and GnuCOBOL 4.0 -std=cobol85 (issue #42).
001200 ENVIRONMENT DIVISION.
001300 DATA DIVISION.
001400 WORKING-STORAGE SECTION.
001500 01  F   PIC X(5).
001600 01  C1  PIC 99.
001700 01  C2  PIC 99.
001800 PROCEDURE DIVISION.
001900 MAIN-PARA.
002000     MOVE 'AABAA' TO F.
002100     MOVE 0 TO C1 C2.
002200     INSPECT F TALLYING C1 FOR ALL 'A' C2 FOR ALL 'AA'.
002300     DISPLAY 'T1 ' C1 ' ' C2.
002400     MOVE 0 TO C1 C2.
002500     INSPECT F TALLYING C1 FOR ALL 'AA' C2 FOR ALL 'A'.
002600     DISPLAY 'T2 ' C1 ' ' C2.
002700     INSPECT F REPLACING ALL 'A' BY 'X' ALL 'AA' BY 'YY'.
002800     DISPLAY 'R1 [' F ']'.
002900     MOVE 'AABAA' TO F.
003000     INSPECT F REPLACING ALL 'AA' BY 'YY' ALL 'A' BY 'X'.
003100     DISPLAY 'R2 [' F ']'.
003200     MOVE 'AAABA' TO F.
003300     INSPECT F REPLACING ALL 'AA' BY 'YY' ALL 'A' BY 'X'.
003400     DISPLAY 'R3 [' F ']'.
003500     STOP RUN.
