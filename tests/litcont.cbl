000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. LITCONT.
000300*A nonnumeric literal may be broken across lines: a hyphen in
000400*column 7, area A blank, and a quotation mark as the first
000500*nonblank in area B. All spaces at the end of the continued
000600*line are part of the literal (I-106, 5.8.2.2), so a line
000700*that stops early still runs to column 72.
000800 DATA DIVISION.
000900 WORKING-STORAGE SECTION.
001000 01  CHARSET PIC X(51) VALUE           "ABCDEFGHIJKLMNOPQRSTUVWXYZ
001100-    " 0123456789 +-*/=$,.;()><".
001200 01  PADLIT  PIC X(40) VALUE "AB
001300-    "CD".
001400 PROCEDURE DIVISION.
001500     DISPLAY '[' CHARSET ']'.
001600     DISPLAY '[' PADLIT ']'.
001700     STOP RUN.
