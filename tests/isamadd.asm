*        ISAMADD: add one record to an ISAM data set with BISAM
*        WRITE KN, so that it lands in the overflow area. cobc370 has
*        no ISAM add (QISAM load mode is its only ISAM write), so the
*        round trip does it here. Hand-written, for
*        bin/cobc-isam-roundtrip.
*        The area WRITE KN names begins with 16 bytes the control
*        program uses; the record follows them (GC26-3873, WRITE for
*        BISAM). The data set needs an overflow area: APPLY
*        CYL-OVERFLOW in the loader. OPEN's option is ignored for
*        BISAM, and MSWA/SMSW may be left to the system.
ISAMADD  CSECT
         STM   14,12,12(13)
         BALR  12,0
         USING *,12
         ST    13,SAVE+4
         LA    13,SAVE
         OPEN  (ISDCB,(UPDAT),OUTDCB,(OUTPUT))
         TM    ISDCB+48,X'10'       did the ISAM data set open?
         BZ    NOOPEN
         WRITE DECB1,KN,ISDCB,AREA,'S',REC+1
         WAIT  ECB=DECB1
         TM    DECB1+24,X'FF'       any exception?
         BNZ   BADADD
         PUT   OUTDCB,MSGOK
         B     DONE
BADADD   UNPK  HEX(5),DECB1+24(3)   the exception bytes, as hex
         TR    HEX(4),HEXTAB-240
         MVC   MSGBAD+37(4),HEX
         PUT   OUTDCB,MSGBAD
         B     DONE
NOOPEN   PUT   OUTDCB,MSGNO
         CLOSE (OUTDCB)
         B     EXIT
DONE     CLOSE (ISDCB,,OUTDCB)
EXIT     L     13,SAVE+4
         LM    14,12,12(13)
         SR    15,15
         BR    14
SAVE     DS    18F
AREA     DS    XL16                 the control program's 16 bytes
REC      DC    C' ',C'0000000250',CL70'TWO FIFTY, ADDED AFTER THE LOAD'
MSGOK    DC    CL80'ISAMADD: 0000000250 ADDED'
MSGBAD   DC    CL80'ISAMADD: WRITE KN TOOK AN EXCEPTION ....'
MSGNO    DC    CL80'ISAMADD: OPEN FAILED'
HEX      DS    CL5
HEXTAB   DC    C'0123456789ABCDEF'
ISDCB    DCB   DDNAME=DESCIDX,DSORG=IS,MACRF=(WA)
OUTDCB   DCB   DDNAME=SYSOUT,DSORG=PS,MACRF=(PM),RECFM=FB,LRECL=80,    X
               BLKSIZE=80
         END   ISAMADD
