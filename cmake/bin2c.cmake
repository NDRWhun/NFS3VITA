# Converts a binary file into a C array. Script mode: -DINPUT= -DOUTPUT= -DSYMBOL=
file(READ "${INPUT}" HEX_DATA HEX)
string(LENGTH "${HEX_DATA}" HEX_LENGTH)
math(EXPR BYTE_COUNT "${HEX_LENGTH} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," ARRAY_DATA "${HEX_DATA}")
string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n    " ARRAY_DATA "${ARRAY_DATA}")
file(WRITE "${OUTPUT}"
"/* Generated from ${INPUT} */
#include <stddef.h>
__attribute__((aligned(16))) const unsigned char ${SYMBOL}[] = {
    ${ARRAY_DATA}
};
const size_t ${SYMBOL}_size = ${BYTE_COUNT};
")
