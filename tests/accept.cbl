       IDENTIFICATION DIVISION.
       PROGRAM-ID. ACCEPTT.
      * ACCEPT at level 1, II-53: one transfer from the implementor's
      * device, which here is SYSIN. The FROM phrase -- the mnemonic
      * form and DATE/DAY/TIME alike -- is level 2 and is refused.
      *
      * General rule 2 leaves the size of a transfer to the
      * implementor: one 80-column record. The buffer is blanked
      * first, so a receiver wider than a card is padded and a read
      * past the last card returns spaces rather than the card before
      * it -- which is what the third ACCEPT here shows.
       DATA DIVISION.
       WORKING-STORAGE SECTION.
       01  L1 PIC X(20).
       01  L2 PIC X(20).
       01  L3 PIC X(20).
       PROCEDURE DIVISION.
           ACCEPT L1.
           DISPLAY 'ONE   [' L1 ']'.
           ACCEPT L2.
           DISPLAY 'TWO   [' L2 ']'.
           ACCEPT L3.
           DISPLAY 'PASTEOF[' L3 ']'.
           STOP RUN.
