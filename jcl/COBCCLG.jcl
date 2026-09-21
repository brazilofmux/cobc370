//*  COBCCLG -- compile, assemble, link and go, all on MVS 3.8j.
//*
//*  The compiler is the COBC370 load module (docs/PORT-PLAN.md);
//*  the assembler and linkage editor are the system's own. The
//*  COBOL source is a PDS member named by MEM; copybooks resolve
//*  from the same PDS through SYSLIB. Adjust the two dataset
//*  names to where COBC370 and the source live.
//*
//*  Sample invocation at the bottom. IFOX00 needs PARM='OBJ,NODECK'
//*  or it assembles cleanly and produces nothing, RC=16 (IFO257).
//*
//COBCCLG  PROC MEM=
//COBC     EXEC PGM=COBC370,REGION=8192K
//STEPLIB  DD DSN=HERC01.COBC370.LOAD,DISP=SHR
//SYSPRINT DD SYSOUT=*
//SYSTERM  DD SYSOUT=*
//SYSLIB   DD DSN=HERC01.CBLSRC,DISP=SHR
//SYSIN    DD DSN=HERC01.CBLSRC(&MEM),DISP=SHR
//SYSPUNCH DD DSN=&&ASM,DISP=(NEW,PASS),UNIT=SYSDA,
//            SPACE=(TRK,(15,5)),
//            DCB=(RECFM=FB,LRECL=80,BLKSIZE=3120)
//ASM      EXEC PGM=IFOX00,PARM='OBJ,NODECK',REGION=1024K,
//            COND=(0,NE)
//SYSLIB   DD DSN=SYS1.MACLIB,DISP=SHR
//SYSGO    DD DSN=&&OBJ,DISP=(NEW,PASS),UNIT=SYSDA,
//            SPACE=(TRK,(15,5)),
//            DCB=(RECFM=FB,LRECL=80,BLKSIZE=3120)
//SYSUT1   DD UNIT=SYSDA,SPACE=(CYL,(5,5))
//SYSUT2   DD UNIT=SYSDA,SPACE=(CYL,(5,5))
//SYSUT3   DD UNIT=SYSDA,SPACE=(CYL,(5,5))
//SYSPRINT DD DUMMY
//SYSIN    DD DSN=&&ASM,DISP=(OLD,DELETE)
//LKED     EXEC PGM=IEWL,PARM='MAP,LET,LIST',REGION=1024K,
//            COND=((0,NE,COBC),(8,LE,ASM))
//SYSLIN   DD DSN=&&OBJ,DISP=(OLD,DELETE)
//SYSLMOD  DD DSN=&&LOAD(GO),DISP=(NEW,PASS),UNIT=SYSDA,
//            SPACE=(TRK,(20,10,5))
//SYSUT1   DD UNIT=SYSDA,SPACE=(CYL,(5,5))
//SYSPRINT DD DUMMY
//GO       EXEC PGM=*.LKED.SYSLMOD,COND=((0,NE,COBC),(8,LE,ASM))
//SYSOUT   DD SYSOUT=*
//SYSUDUMP DD SYSOUT=*
//         PEND
//RUN      EXEC COBCCLG,MEM=HELLO
