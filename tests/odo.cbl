000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. ODO.
000300* OCCURS integer-1 TO integer-2 DEPENDING ON (III-2), in
000400* WORKING-STORAGE: the count item gives the number of occurrences,
000500* and with it the length of every group containing the table. A
000600* MOVE of such a group moves that many bytes and space fills.
000610* III-3 rule 4 uses the current count whichever side the group is
000620* on, so a MOVE into it with the count at 3 leaves bytes past 13
000630* alone; COBOL-85 uses the maximum for a receiver, GnuCOBOL does
000640* too, and the third line here was corrected by hand from the rule.
000700 DATA DIVISION.
000800 WORKING-STORAGE SECTION.
000900 01  N            PIC 99 VALUE 0.
001000 01  REC.
001100     05  HDR      PIC X(4) VALUE 'HDR-'.
001200     05  E OCCURS 1 TO 5 TIMES DEPENDING ON N PIC X(3).
001300 01  OUT-AREA     PIC X(19) VALUE ALL '.'.
001400 01  BACK.
001500     05  BHDR     PIC X(4).
001600     05  BE OCCURS 1 TO 5 TIMES DEPENDING ON N PIC X(3).
001700 01  LONG-SRC     PIC X(19) VALUE 'hdr:aaabbbcccdddeee'.
001800 01  M            PIC 99.
001900 PROCEDURE DIVISION.
002000 MAIN.
002100     MOVE 'AAA' TO E (1). MOVE 'BBB' TO E (2).
002150     MOVE 'CCC' TO E (3).
002200     MOVE 'DDD' TO E (4). MOVE 'EEE' TO E (5).
002300     MOVE 2 TO N.
002400     MOVE REC TO OUT-AREA.
002500     DISPLAY '[' OUT-AREA ']'.
002600     MOVE 5 TO N.
002700     MOVE ALL '.' TO OUT-AREA.
002800     MOVE REC TO OUT-AREA.
002900     DISPLAY '[' OUT-AREA ']'.
003000     MOVE 3 TO N.
003100     MOVE ALL '*' TO BACK.
003200     MOVE LONG-SRC TO BACK.
003300     MOVE 5 TO N.
003400     MOVE BACK TO OUT-AREA.
003500     DISPLAY '[' OUT-AREA ']'.
003600     MOVE 1 TO N.
003700     MOVE ALL '-' TO OUT-AREA.
003800     MOVE REC TO OUT-AREA.
003900     DISPLAY '[' OUT-AREA ']'.
004000     STOP RUN.
