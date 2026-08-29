#!/bin/bash
# Each benchmark twice: cobc370 (assembled) and IKFCBL00 (COBUCL). CPU seconds
# come from the IEF374I line of the run step in the class-Z output.
set -u
export PATH="$HOME/mvsops/bin:$PATH"
B="$(cd "$(dirname "$0")" && pwd)"
WORK=${TMPDIR:-/tmp}/cobc-bench; mkdir -p "$WORK"
cd "$B"
for f in *.cbl; do "$B/../src/cobc370" "$f" -o "$WORK/${f%.cbl}.asm" || exit 1; done
cd "$WORK"; cp "$B"/*.cbl .
jobcard() { printf '//%-8s JOB (5161A020,1A11),%s,\n//             CLASS=A,MSGCLASS=Z,MSGLEVEL=(1,1),\n//             USER=HERC01,PASSWORD=@HERC01PW@\n' "$1" "'BENCH'"; }
cpu() { grep -aoE "IEF374I STEP /(GO|RUN) .*CPU +[0-9]+MIN [0-9.]+SEC" "$1" | head -1 | sed -E 's/.*CPU +([0-9]+)MIN ([0-9.]+)SEC.*/\1 \2/' | awk '{printf "%.2f", $1*60+$2}'; }
printf '%-9s %8s %8s %6s\n' bench cobc370 ikfcbl00 ratio
for p in B0EMPTY B1PADD B2CADD B3DMOVE B4DCMP B5PCMP B6DMOV9 B7COMP B8CALL; do
  # ---- cobc370 ----
  { jobcard "C$p"
    if [ "$p" = B8CALL ]; then
      printf '//S0      EXEC ASMFCL,PARM.ASM=%s\n//ASM.SYSIN DD *\n' "'OBJ,NOLIST'"; cat BSUB.asm; printf '/*\n'
      printf '//LKED.SYSLMOD DD DSN=&&BL2(BSUB),DISP=(NEW,PASS),UNIT=SYSDA,\n//             SPACE=(CYL,(1,1,5)),DCB=(RECFM=U,BLKSIZE=19069)\n'
      printf '//S1      EXEC ASMFCLG,PARM.ASM=%s,PARM.LKED=%s\n//ASM.SYSIN DD *\n' "'OBJ,NOLIST'" "'XREF,LET,LIST'"; cat $p.asm; printf '/*\n'
      printf '//LKED.SYSLIB DD DSN=&&BL2,DISP=(OLD,PASS)\n//GO.SYSOUT DD SYSOUT=Z\n'
    else
      printf '//S1      EXEC ASMFCLG,PARM.ASM=%s\n//ASM.SYSIN DD *\n' "'OBJ,NOLIST'"; cat $p.asm; printf '/*\n//GO.SYSOUT DD SYSOUT=Z\n'
    fi
  } > c_$p.jcl
  tk5-run c_$p.jcl c_$p.out >/dev/null 2>&1
  c=$(cpu c_$p.out)
  # ---- IKFCBL00 ----
  { jobcard "I$p"
    if [ "$p" = B8CALL ]; then
      # COBUC punches a deck; asked for LOAD it writes the object to SYSLIN,
      # which the caller's link-edit reads through COBUCL's DDNAME=SYSIN slot.
      printf '//S0      EXEC COBUC,PARM.COB=%s\n' "'LOAD,NODECK'"
      printf '//COB.SYSLIN DD DSN=&&OBJ,DISP=(NEW,PASS),UNIT=SYSDA,\n//             SPACE=(80,(200,50))\n'
      printf '//COB.SYSIN DD *\n'; cut -c1-72 BSUB.cbl; printf '/*\n'
    fi
    printf '//S1      EXEC COBUCL\n//COB.SYSIN DD *\n'; cut -c1-72 $p.cbl; printf '/*\n'
    printf '//LKED.SYSLMOD DD DSN=&&BL(%s),DISP=(NEW,PASS),UNIT=SYSDA,\n//             SPACE=(CYL,(1,1,5)),DCB=(RECFM=U,BLKSIZE=19069)\n' "$p"
    if [ "$p" = B8CALL ]; then printf '//LKED.SYSIN DD DSN=&&OBJ,DISP=(OLD,DELETE)\n'; fi
    printf '//RUN     EXEC PGM=%s\n//STEPLIB DD DSN=&&BL,DISP=(OLD,PASS)\n//SYSOUT  DD SYSOUT=Z\n' "$p"
  } > i_$p.jcl
  tk5-run i_$p.jcl i_$p.out >/dev/null 2>&1
  i=$(cpu i_$p.out)
  cd=$(grep -ac "BENCH DONE" c_$p.out); id=$(grep -ac "BENCH DONE" i_$p.out)
  printf '%-9s %8s %8s %6s  %s\n' "$p" "${c:-?}" "${i:-?}" "$(awk -v a="$c" -v b="$i" 'BEGIN{ if (a!="" && b!="" && b+0>0) printf "%.2f", a/b; else print "?" }')" "$([ "$cd" -ge 1 ] && [ "$id" -ge 1 ] && echo ok || echo "DID NOT RUN: cobc370=$cd ibm=$id")"
done
