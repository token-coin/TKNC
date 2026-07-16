# Chinese comment removed
# Usage: cmake -DINPUT_FILE=xxx.cl -DOUTPUT_FILE=xxx.h -P embed_cl.cmake
#
# Output: a header containing TOKENHASH_CL_KERNEL[] as raw string literal

file(READ "${INPUT_FILE}" CL_CONTENT)

# Write as C++ raw string literal (no escaping needed for OpenCL code)
# Chinese comment removed
file(WRITE "${OUTPUT_FILE}"
"// (Chinese comment removed)
// Source: ${INPUT_FILE}

#ifndef TKNC_EMBEDDED_TOKENHASH_CL_H
#define TKNC_EMBEDDED_TOKENHASH_CL_H

static const char TOKENHASH_CL_KERNEL[] = R\"clraw(
${CL_CONTENT}
)clraw\";

static const size_t TOKENHASH_CL_KERNEL_SIZE = sizeof(TOKENHASH_CL_KERNEL) - 1;

#endif // TKNC_EMBEDDED_TOKENHASH_CL_H
")
