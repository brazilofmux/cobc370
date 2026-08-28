000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. SIGNCLAU.
000300* The SIGN clause, II-31. Without SEPARATE the sign is an
000400* overpunch on the leading or trailing digit and the S costs
000500* nothing; with it the sign is its own character position, '+' or
000600* '-', and the S is counted in the size. Trailing overpunch is
000700* what this compiler does when no clause is given -- the choice
000800* general rule 2 leaves to the implementor.
000900*
001000* Only the SEPARATE layouts are compared byte for byte. An
001100* overpunch is implementor-defined and character-set dependent, so
001200* those forms are checked through their value instead.
001300 DATA DIVISION.
001400 WORKING-STORAGE SECTION.
001500 01  DEF-T   PIC S9(5).
001600 01  LEA-O   PIC S9(5) SIGN IS LEADING.
001700 01  TRA-O   PIC S9(5) SIGN IS TRAILING.
001800 01  G-LEA-S.
001900     02  LEA-S PIC S9(5) SIGN IS LEADING SEPARATE CHARACTER.
002000 01  X-LEA-S REDEFINES G-LEA-S PIC X(6).
002100 01  G-TRA-S.
002200     02  TRA-S PIC S9(5) SIGN IS TRAILING SEPARATE CHARACTER.
002300 01  X-TRA-S REDEFINES G-TRA-S PIC X(6).
002400 01  SHOW-N  PIC -9(5).
002500 PROCEDURE DIVISION.
002600     MOVE -12345 TO DEF-T LEA-O TRA-O LEA-S TRA-S.
002700     MOVE DEF-T TO SHOW-N.
002800     DISPLAY 'DEF-T  ' SHOW-N.
002900     MOVE LEA-O TO SHOW-N.
003000     DISPLAY 'LEA-O  ' SHOW-N.
003100     MOVE TRA-O TO SHOW-N.
003200     DISPLAY 'TRA-O  ' SHOW-N.
003300     MOVE LEA-S TO SHOW-N.
003400     DISPLAY 'LEA-S  ' SHOW-N.
003500     MOVE TRA-S TO SHOW-N.
003600     DISPLAY 'TRA-S  ' SHOW-N.
003700     DISPLAY 'LEA-S neg [' X-LEA-S ']'.
003800     DISPLAY 'TRA-S neg [' X-TRA-S ']'.
003900     MOVE 67890 TO LEA-S TRA-S LEA-O.
004000     DISPLAY 'LEA-S pos [' X-LEA-S ']'.
004100     DISPLAY 'TRA-S pos [' X-TRA-S ']'.
004200     MOVE LEA-O TO SHOW-N.
004300     DISPLAY 'LEA-O pos ' SHOW-N.
004400     ADD 1 TO LEA-O.
004500     MOVE LEA-O TO SHOW-N.
004600     DISPLAY 'LEA-O +1  ' SHOW-N.
004700     SUBTRACT 2 FROM TRA-S.
004800     DISPLAY 'TRA-S -2  [' X-TRA-S ']'.
004900     STOP RUN.
