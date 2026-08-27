# Where the front end stops and S/370 begins

A note for whoever retargets this compiler — quite possibly the person who
wrote it, six months from now. It records an observation, not a plan. Nothing
here has been acted on and nothing needs to be.

## The boundary already exists

`src/cobc370.c` is one file of about 5,000 lines, and it divides cleanly at
line 2291, `/* ---- base locator cells ---- */`:

| lines | what | target |
|---|---|---|
| 34 | source reader, fixed format | any |
| 64 | tokenizer | any |
| 157 | emitter (`asm_line`, `asm_comment`, `asm_cont`) | shared plumbing |
| 204-2290 | parser, data model, PICTURE, DATA DIVISION, expressions, conditions, statements, Report Writer, OF/IN qualification | any |
| **2291** | **base locator cells** | **S/370** |
| 2485-end | arithmetic, code generation, control blocks, runtime | S/370 |

Measured rather than assumed: of 626 calls to the emission helpers, **624 are
below line 2291**. The two above it are the *definitions* of `asm_line` and
`asm_comment` themselves. **No code in the front end emits a single line of
assembler.**

That the split lands exactly at "base locator cells" is not a coincidence.
Base registers are an S/370 addressing concept — a consequence of
base-plus-displacement with a 4,096-byte ceiling — and they are the first thing
in the file that could not mean anything on a flat-address machine.

## What crosses it

The back end does not call the front end. It reads what the front end built:

    syms[]     the symbol table          66 references below the line
    stmts[]    the statement list        13
    files[]    SELECT/FD, one per file   11
    reports[]  RD, with rlines/rfields   10 (+3)
    paras[]    paragraphs and ranges      4
    wslen      WORKING-STORAGE size       4

Those six arrays and one integer are the whole interface. A second back end
would read the same things and emit something else.

## What the front end would have to give up

Not everything above the line is innocent. Known leaks, worth knowing before
anyone believes the boundary is free:

- **Sizes are S/370 sizes**, decided in the front end at line 1136. COMP is 2
  bytes to 4 digits and 4 bytes to 9 — halfword and fullword — and refuses
  anything wider; COMP-3 is `digits/2 + 1`, which is packed decimal and nothing
  else. A machine with different integer widths, or without packed decimal,
  needs both parameterised.
- **`wslen > 64 * 1024` is diagnosed in the front end**, because that is where
  base locator cells run out. That check belongs to the back end.
- **Column 72** is a fixed-format COBOL rule, not an S/370 one, so it stays —
  but the *assembler* continuation convention that `asm_cont` implements is
  target-specific and lives in the shared emitter section.

## When to act on this

Not yet, and possibly not ever. There is one back end, so there is nothing for
an abstraction to be right or wrong about; a seam designed against a single
caller is shaped like that caller. The cost of waiting is zero **for as long as
there is only one copy of this file.**

The event that should trigger the split is beginning a second back end — and
the rule that matters is that it begins *in this tree*, as a sibling, never as
a copy of `cobc370.c`. Two copies diverge, and then every fix has to be
carried across by hand, precisely while one side is being pushed harder than
the other. Extract what the second back end actually reaches for, when it
reaches for it; two callers define an interface far better than one caller and
an imagination.

`~/slow32-public/selfhost` already runs this pattern: `src/` holds the
canonical front end and the stage08 and cross-compiler trees symlink to it,
"to avoid duplication while developing." Same problem, already solved, in
house.
