000100 IDENTIFICATION DIVISION.
000200 PROGRAM-ID. SUBDSP.
000300* A called subprogram that DISPLAYs, so it carries the runtime
000400* (COBRT) in its own object, as its caller does: two copies of
000500* COBRT in one load module. The linkage editor keeps the first
000600* and drops the second; both are the same code (#46).
000700 DATA DIVISION.
000800 WORKING-STORAGE SECTION.
000900 01  WS-N  PIC 9(4) VALUE 0.
001000 LINKAGE SECTION.
001100 01  LS-N  PIC S9(4) COMP.
001200 PROCEDURE DIVISION USING LS-N.
001300 MAIN-PARA.
001400     MOVE LS-N TO WS-N.
001500     DISPLAY 'RESULT SUBDSP SEES ' WS-N.
001600     ADD 1 TO LS-N.
001700     GOBACK.
