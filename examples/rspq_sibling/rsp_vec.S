## Auto-generated file, transpiled with RSPL
#include <rsp_queue.inc>

.set noreorder

.data
  RSPQ_BeginOverlayHeader
    RSPQ_DefineCommand Cmd_SetValue, 4
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

    .short 0x0FFF
  RSPQ_EndOverlayHeader

  RSPQ_BeginSavedState
    STATE_MEM_START:
    .align 4
    VAL: .ds.b 64
    STATE_MEM_END:
  RSPQ_EndSavedState

.text

Cmd_SetValue:
  # andi $a0, $a0, 255
  
  # emux_dump_gpr $a0
  sb a0, %lo(4 + VAL)(zero)
  j RSPQ_Loop
  sb a0, %lo(VAL)(zero)
  
.set macro