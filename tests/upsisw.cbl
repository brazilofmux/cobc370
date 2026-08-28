       IDENTIFICATION DIVISION.
       PROGRAM-ID. UPSISW.
      * SPECIAL-NAMES switch conditions, II-8 and II-44. The 1974
      * standard leaves the implementor-names open; IKFCBL00 -- asked
      * directly -- rejects UPSI-n and SWITCH-n outright, because
      * OS/360 ANS COBOL has no external switches at all. Both
      * spellings are taken here, from the IBM systems that do have
      * them, and both reach one byte with UPSI-0 as its leftmost bit.
      *
      * The bits arrive as PARM='/UPSI(10100000)' on the EXEC card.
      * This run passes 10100000: UPSI-0 on, UPSI-2 on, rest off.
       ENVIRONMENT DIVISION.
       CONFIGURATION SECTION.
       SPECIAL-NAMES.
           UPSI-0 IS YEAR-END-SW
               ON  STATUS IS YEAR-END
               OFF STATUS IS NORMAL-RUN
           SWITCH-2 IS DETAIL-SW
               ON  STATUS IS DETAIL-RUN.
       DATA DIVISION.
       WORKING-STORAGE SECTION.
       01  R PIC X(3).
       PROCEDURE DIVISION.
           IF YEAR-END MOVE 'YES' TO R ELSE MOVE 'NO ' TO R.
           DISPLAY 'UPSI0  ' R.
           IF NORMAL-RUN MOVE 'YES' TO R ELSE MOVE 'NO ' TO R.
           DISPLAY 'NORMAL ' R.
           IF DETAIL-RUN MOVE 'YES' TO R ELSE MOVE 'NO ' TO R.
           DISPLAY 'SWITCH2' R.
           STOP RUN.
