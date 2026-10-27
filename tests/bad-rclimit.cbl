       IDENTIFICATION DIVISION.
       PROGRAM-ID. RCLIMIT.
      * A copybook that copies itself: COPY nests past the compiler's
      * table of four, a limit of the compiler, RC 16.
       DATA DIVISION.
       WORKING-STORAGE SECTION.
           COPY BAD-RCLIM.
       PROCEDURE DIVISION.
           STOP RUN.
