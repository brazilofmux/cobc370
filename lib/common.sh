# Shared by the bin/ scripts. Deliberately small: this repo knows how to
# compile COBOL and how to check the result, and nothing at all about how to
# start a mainframe or where a password is kept.
say() { printf '%s\n' "$*" >&2; }
die() { say "ERROR: $*"; exit 1; }

# Running a deck on the guest is delegated. COBC370_RUNNER names a program
# taking two arguments -- a JCL deck and a file to write the printed output to
# -- and is the single seam between this repo and whatever is hosting MVS.
#
# The generated job cards carry USER=HERC01,PASSWORD=@HERC01PW@. Substituting
# that token is the runner's business, which is how no credential, and no path
# to one, appears anywhere in this repository.
RUNNER=${COBC370_RUNNER:-tk5-run}
run_deck() {
    command -v "$RUNNER" >/dev/null 2>&1 \
        || die "no runner: set COBC370_RUNNER, or put '$RUNNER' on PATH.
       It takes <deck.jcl> <outfile> and runs the deck on an MVS 3.8j guest.
       See README.md, 'Running the tests'."
    "$RUNNER" "$1" "$2" >/dev/null 2>&1
}
