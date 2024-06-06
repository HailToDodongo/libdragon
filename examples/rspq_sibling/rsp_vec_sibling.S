#include <rsp_queue.inc>

.set noreorder
.set nomacro

.data
  RSPQ_BeginOverlayHeader
    .short 0x0FFF
    .short 0xFFFF
    .short 0xFFFF
    .short 0xFFFF

    .short 0xFFFF
    .short 0xFFFF
    .short 0xFFFF
    .short 0xFFFF

    .short 0xFFFF
    .short 0xFFFF
    .short 0xFFFF
    .short 0xFFFF

    .short 0xFFFF
    .short 0xFFFF
    .short 0xFFFF
    .short 0xFFFF

    RSPQ_DefineCommand Cmd_ReadValue, 4
  RSPQ_EndOverlayHeader

  RSPQ_BeginSavedState
    STATE_MEM_START:
    .align 4
    VAL: .ds.b 64
    STATE_MEM_END:
  RSPQ_EndSavedState

.text

Cmd_ReadValue:
  ori t0, zero, 63
  
  # emux_dump_gpr $a0, $a1, $t0
  ori s4, zero, %lo(VAL)
  addiu t2, zero, -32756
  j DMAExec ## Args: $t0, $t1, $s0, $s4, $t2
  or s0, zero, a0

.set macro