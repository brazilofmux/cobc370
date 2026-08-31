000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. SEARCHZ.
000300* The text defines AT END only for an index past the last occurrence;
000310* below 1 is undefined. This compiler takes AT END rather than read
000320* storage before the table, which is what GnuCOBOL does (it says HIT).
000400 DATA DIVISION.
000500 WORKING-STORAGE SECTION.
000600 01  T.
000700     05  E OCCURS 3 INDEXED BY IX PIC X VALUE 'A'.
000800 PROCEDURE DIVISION.
000900     SET IX TO 1.
001000     SET IX DOWN BY 1.
001100     SEARCH E
001200         AT END DISPLAY 'ATEND'
001300         WHEN E (IX) = 'A' DISPLAY 'HIT'.
001400     STOP RUN.
