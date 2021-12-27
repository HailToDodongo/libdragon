#ifndef RSP_RSPQ_INC
#define RSP_RSPQ_INC

# Globally reserved registers
#define rspq_dmem_buf_ptr gp

# Can be used with l* instructions to get contents of the current command at the specified offset.
# The total command size needs to be specified as well.
#define CMD_ADDR(offset, cmdsize) (%lo(RSPQ_DMEM_BUFFER) + (offset) - (cmdsize)) (rspq_dmem_buf_ptr)

#define OVERLAY_HEADER_SIZE 0x8

# TODO: commands smaller than 4 bytes?
.macro commandTableEntry function size
    .ifne ((\size) % 4)
        .error "Invalid size - must be multiple of 4"
        .exitm
    .endif

    .ifle (\size)
        .error "Invalid size - valid range: [4, 60]"
        .exitm
    .endif

    .ifgt ((\size) - 60)
        .error "Invalid size - valid range: [4, 60]"
        .exitm
    .endif

    # Put the command size (as number of 4 byte words) into the high 4 bits,
    # which are not used by the RSP anyway.
    .short (\function - _start) | ((\size) & 0x3C) << 10
.endm

.macro overlayHeader stateStart, stateEnd, cmdBase
    OVERLAY_HEADER:
    .long \stateStart
    .short (\stateEnd - \stateStart) - 1
    .short (\cmdBase << 1)
.endm

#endif