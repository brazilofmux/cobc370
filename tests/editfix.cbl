000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. EDITFIX.
000300* Editing and MOVE cases measured against IKFCBL00 (#30, #31): a
000400* fixed and a floating currency symbol, a fixed sign after a
000500* floating symbol, an all-Z picture with a point, high-order
000600* truncation into an edited field, asterisk fill at a sign
000700* position, all-floating pictures at zero, a numeric-edited
000800* sender moved to PIC X, and an alphanumeric-edited receiver from
000900* an item and from a literal. Expected output recorded from IBM
001000* ANS COBOL on TK5.
001100 ENVIRONMENT DIVISION.
001200 DATA DIVISION.
001300 WORKING-STORAGE SECTION.
001400 01  N     PIC S9(5)V99 VALUE 5.
001500 01  NM    PIC S9(5)V99 VALUE -5.
001600 01  NS    PIC S9(5)V99 VALUE -0.05.
001700 01  NZ    PIC S9(5)V99 VALUE 0.
001800 01  NB    PIC S9(5) VALUE 10005.
001900 01  E1    PIC $ZZ9.99.
002000 01  E1S   PIC $***9.99.
002100 01  E2    PIC $$$,$$9.99-.
002200 01  E3    PIC ZZ.ZZ.
002300 01  E4    PIC ZZZ9.
002400 01  E6    PIC **9.99-.
002500 01  E7    PIC ZZ9-.
002600 01  E8    PIC ++++.
002700 01  E9    PIC $$$$.
002800 01  A1    PIC XXBXXBXX.
002900 01  S2    PIC XX VALUE 'AB'.
003000 01  A2    PIC X(6).
003100 01  A4    PIC X(4).
003200 PROCEDURE DIVISION.
003300     MOVE N TO E1.   DISPLAY 'F1  [' E1 ']'.
003400     MOVE N TO E1S.  DISPLAY 'F1S [' E1S ']'.
003500     MOVE NM TO E2.  DISPLAY 'F2- [' E2 ']'.
003600     MOVE N TO E2.   DISPLAY 'F2+ [' E2 ']'.
003700     MOVE NS TO E3.  DISPLAY 'F9  [' E3 ']'.
003800     MOVE NZ TO E3.  DISPLAY 'F9Z [' E3 ']'.
003900     MOVE NB TO E4.  DISPLAY 'F10 [' E4 ']'.
004000     MOVE N TO E6.   DISPLAY 'F11 [' E6 ']'.
004100     MOVE NZ TO E8.  DISPLAY 'F12+[' E8 ']'.
004200     MOVE NZ TO E9.  DISPLAY 'F12$[' E9 ']'.
004300     MOVE NM TO E7.  MOVE E7 TO A2. DISPLAY 'F3  [' A2 ']'.
004400     MOVE S2 TO A1.  DISPLAY 'F4  [' A1 ']'.
004500     MOVE 'ABCDEF' TO A1. DISPLAY 'F5  [' A1 ']'.
004600     STOP RUN.
