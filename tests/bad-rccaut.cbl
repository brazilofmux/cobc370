       IDENTIFICATION DIVISION.
       PROGRAM-ID. RCCAUT.
      * A VALUE longer than its PICTURE: IKFCBL00 truncates it with
      * IKF2126I-C, RC 8. cobc370 cuts it too, and says so: a caution.
       DATA DIVISION.
       WORKING-STORAGE SECTION.
       77  B PIC X(4) VALUE 'ABCDEFG'.
       PROCEDURE DIVISION.
           DISPLAY B.
           STOP RUN.
