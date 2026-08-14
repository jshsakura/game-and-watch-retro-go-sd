# Strip C comments and literals, so a gate can count CALLS and not prose.
#
# Sourced by tests/test_lcd_swap_audited.sh and tests/test_ingame_overlay_wired.sh.
# It exists because this tree's older idiom for the same job, `^[^*/]*name\(`,
# is wrong in a way that is invisible until it matters:
#
#     if (*flag) name();      <- a real call, NOT matched (the * comes first)
#     if (a / b) name();      <- a real call, NOT matched
#     printf("name() now");   <- not a call, MATCHED
#
# Missing a call is the dangerous half. When that pattern is used for
# DISCOVERY, the file does not merely go uncounted -- it never enters the scan,
# so the gate passes while seeing nothing. The census gate shipped that way and
# matched two of six calls.
#
# The awk program tracks block comments, line comments, string literals AND
# character literals. The last is not a nicety: a plain `char q = '"';` turns on
# the string state and swallows the rest of the file. Six files in scope carry
# exactly that literal.
#
# CH (the single quote) has to arrive as a variable -- a ' cannot appear inside
# the single-quoted program below.

C_STRIP_AWK='
{
  line = $0; out = ""; i = 1; n = length(line)
  while (i <= n) {
    two = substr(line, i, 2); one = substr(line, i, 1)
    if (inblock)          { if (two == "*/") { inblock = 0; i += 2 } else i++ }
    else if (instr)       { if (one == "\\") i += 2
                            else { if (one == "\"") instr = 0; i++ } }
    else if (inchr)       { if (one == "\\") i += 2
                            else { if (one == CH) inchr = 0; i++ } }
    else if (two == "/*") { inblock = 1; i += 2 }
    else if (two == "//") { break }
    else if (one == "\"") { instr = 1; i++ }
    else if (one == CH)   { inchr = 1; i++ }
    else                  { out = out one; i++ }
  }
  print out
}'

# c_strip <file> -- the file with comments and literals removed
c_strip() { awk -v CH="'" "$C_STRIP_AWK" "$1"; }

# c_calls <file> <name> -- how many times <name>( is CALLED in <file>
#
# Substring-safe by the caller's choice of name: `foo(` also matches `foo_bar(`
# only if the caller asks for a name that is a prefix of another. Where that
# matters (lcd_swap vs lcd_swap_stale) the two counts are taken separately and
# the longer name is subtracted by the caller if needed -- `lcd_swap(` does not
# match `lcd_swap_stale(` because of the paren.
c_calls() { c_strip "$1" | grep -o "$2(" | wc -l | tr -d ' '; }
