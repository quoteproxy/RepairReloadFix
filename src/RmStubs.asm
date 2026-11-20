; RepairReloadFix - pass-through for the Restart Manager functions (rstrtmgr.dll).
;
; Each exported stub loads its index into eax and jumps to RmCommon. RmCommon keeps the four argument registers,
; asks ResolveRm (RepairReloadFix.cpp) for the system function and jumps to it, so the caller's arguments and
; return address reach the system function unchanged. None of these functions takes floating-point arguments.

EXTERN ResolveRm:PROC

.code

RmCommon PROC FRAME
	push rcx
	.pushreg rcx
	push rdx
	.pushreg rdx
	push r8
	.pushreg r8
	push r9
	.pushreg r9
	sub rsp, 28h
	.allocstack 28h
	.endprolog
	mov ecx, eax
	call ResolveRm
	add rsp, 28h
	pop r9
	pop r8
	pop rdx
	pop rcx
	jmp rax
RmCommon ENDP

RM_STUB MACRO name, index
Proxy_&name PROC
	mov eax, index
	jmp RmCommon
Proxy_&name ENDP
ENDM

; The order must match kRmNames in RepairReloadFix.cpp.
RM_STUB RmAddFilter, 0
RM_STUB RmCancelCurrentTask, 1
RM_STUB RmEndSession, 2
RM_STUB RmGetFilterList, 3
RM_STUB RmGetList, 4
RM_STUB RmJoinSession, 5
RM_STUB RmRegisterResources, 6
RM_STUB RmRemoveFilter, 7
RM_STUB RmReserveHeap, 8
RM_STUB RmRestart, 9
RM_STUB RmShutdown, 10
RM_STUB RmStartSession, 11

END
