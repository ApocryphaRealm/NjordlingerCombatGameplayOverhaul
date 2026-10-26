; Njordlinger Combat Gameplay Overhaul - the progression module's trampoline stubs.
; Copied from Character Progression Control (ApocryphaRealm, GPL-3.0-or-later) with its symbols renamed.
;
; WHY THIS IS ASSEMBLY AND NOT A BYTE ARRAY:
; the patch site is not a call we can redirect - it is a 9-byte instruction that loads the
; constant 100.0 into a register, and replacing it means calling code of our own that puts a
; different number there and returns. That code has to obey a register contract exactly. Hand
; encoding it as a blob of hex would be untestable and unreviewable; written here, the assembler
; validates every instruction at build time and the contract is readable.
;
; THE CONTRACT AT THE PATCH SITE (Skyrim SE 1.5.97, Address Library 41561 + 0x76), read from
; the working reference - Kasplat's Skyrim Skill Uncapper, whose Nexus permissions allow it:
;   ESI    holds the skill's ActorValue id (6 = One-handed ... 23 = Enchanting)
;   XMM0   holds the skill's CURRENT level and must survive untouched - the instruction right
;          after the site is `comiss xmm0, xmm10`
;   the maximum goes into XMM8 on Skyrim SE 1.5.97 (Kassent's 2017 site: `comiss xmm6, xmm8`)
;          and into XMM10 on the AE shape (Kasplat's listing: `comiss xmm0, xmm10`) - one stub per
;          shape below, chosen at install time from which shape the scan actually found
; The site is reached by a 5-byte CALL; the 4 bytes after it are NOPs, so a plain RET resumes
; the game at the instruction after the original 9-byte load.
;
; This stub inserts a CALL into the MIDDLE of a game function, so it preserves every volatile
; register the compiled callee may clobber (RAX, RCX, RDX, R8-R11, XMM0-XMM5) and aligns the
; stack to 16 bytes before calling, as the x64 convention requires. XMM6-XMM15 are callee-saved,
; so the callee preserves them itself; XMM10 is set deliberately, after the call.
;
; Technique credited to the Skyrim Skill Uncapper lineage (Kassent, Vadfromnu, Elys, Kasplat).
; No code is copied: this is our own stub, our own settings and our own location check.

EXTERN NJ_GetSkillCap:PROC          ; float NJ_GetSkillCap(unsigned int skillId)
EXTERN NJ_LevelExp_Hook:PROC        ; float NJ_LevelExp_Hook(float value, unsigned int skillId)
EXTERN NJ_LevelExpOriginal:QWORD    ; the game's level-experience function; set at install
EXTERN NJ_PerkPool_Hook:PROC        ; void NJ_PerkPool_Hook(int count)
EXTERN NJ_PerkPoolReturn:QWORD       ; filled in at install time - the instruction after the sequence
EXTERN NJ_PerkPoolPlayer:QWORD      ; address of the game's player-pointer global
EXTERN NJ_PerkPoolNewCount:DWORD    ; the new perk count the hook settled on

.code

CAP_STUB MACRO name, capreg
name PROC
    ; --- preserve every volatile general-purpose register the game may still need ---
    push    rax
    push    rcx
    push    rdx
    push    r8
    push    r9
    push    r10
    push    r11
    push    rbx                             ; non-volatile: holds the pre-alignment RSP for us
    mov     rbx, rsp

    ; --- 16-byte-aligned save area for XMM0..XMM5 (6 x 16 bytes), then the call ---
    sub     rsp, 60h
    and     rsp, -16
    movaps  xmmword ptr [rsp + 00h], xmm0
    movaps  xmmword ptr [rsp + 10h], xmm1
    movaps  xmmword ptr [rsp + 20h], xmm2
    movaps  xmmword ptr [rsp + 30h], xmm3
    movaps  xmmword ptr [rsp + 40h], xmm4
    movaps  xmmword ptr [rsp + 50h], xmm5

    mov     ecx, esi                        ; skill id -> first integer argument
    sub     rsp, 20h                        ; shadow space; RSP stays 16-byte aligned
    call    NJ_GetSkillCap                 ; returns the cap in XMM0
    add     rsp, 20h
    movss   capreg, xmm0                    ; the maximum belongs in the register this shape uses

    ; --- restore XMM0..XMM5 (XMM0 = the current level, which the compare needs intact) ---
    movaps  xmm0, xmmword ptr [rsp + 00h]
    movaps  xmm1, xmmword ptr [rsp + 10h]
    movaps  xmm2, xmmword ptr [rsp + 20h]
    movaps  xmm3, xmmword ptr [rsp + 30h]
    movaps  xmm4, xmmword ptr [rsp + 40h]
    movaps  xmm5, xmmword ptr [rsp + 50h]
    mov     rsp, rbx
    pop     rbx

    ; --- restore the general-purpose registers in reverse order and resume ---
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rcx
    pop     rax
    ret                                     ; back to the NOPs after the call, then the game
name ENDP
ENDM

; SE 1.5.97 shape: the cap is loaded straight into xmm8 (Kassent's 2017 site).
CAP_STUB NJ_SkillCapStubXmm8, xmm8
; AE shape: the level is copied to xmm8 and the cap loaded into xmm10 (Kasplat's listing).
CAP_STUB NJ_SkillCapStubXmm10, xmm10

; Stage 3, skill-to-level income. This takes the place of the game's `call` to its level-experience
; function (float, with up to four float arguments in xmm0..xmm3 and the result in xmm0). The original
; is called with xmm0..xmm3 exactly as the game set them, its result is scaled through the hook (the
; skill id the caller keeps in rsi goes along as the second argument), and RET returns to the game's
; call site with the scaled value in xmm0.
;
; THE CALL SITE IS NOT AN ORDINARY ONE. The compiler built it knowing exactly which registers the
; original leaf touches, so the game RELIES ON THE REST SURVIVING THE CALL: the instruction right
; after it is `addss xmm0, [rcx]` - rcx still holds the experience field (measured 2026-09-06: a
; logger call inside the hook clobbered rcx and the game read 0xFFFFFFFFFFFFFFFF, crash log
; crash-2026-09-06-04-04-19.log). A compiled C++ hook may clobber any volatile register, so every
; one the original does not provably clobber is preserved around the hook call: rcx, rdx, r8-r11 and
; xmm1-xmm5 (the original converts through eax - `cvttss2si eax, xmm1` is in its shape - so rax is
; not relied on). RSP is 8 mod 16 on entry (after the game's call); 28h aligns it for the original,
; and after six pushes (48 bytes) 78h aligns it again with room for five XMM saves above the shadow.
NJ_LevelExpCallStub PROC
    sub     rsp, 28h
    call    qword ptr [NJ_LevelExpOriginal]  ; the game's own function, the clobber set the site expects
    add     rsp, 28h
    push    rcx
    push    rdx
    push    r8
    push    r9
    push    r10
    push    r11
    sub     rsp, 78h                          ; [rsp..+20h) shadow space, [rsp+20h..+70h) XMM1..XMM5
    movaps  xmmword ptr [rsp + 20h], xmm1
    movaps  xmmword ptr [rsp + 30h], xmm2
    movaps  xmmword ptr [rsp + 40h], xmm3
    movaps  xmmword ptr [rsp + 50h], xmm4
    movaps  xmmword ptr [rsp + 60h], xmm5
    mov     edx, esi                          ; skill id -> second argument (xmm0 = the value, first)
    call    NJ_LevelExp_Hook                 ; result in xmm0
    movaps  xmm1, xmmword ptr [rsp + 20h]
    movaps  xmm2, xmmword ptr [rsp + 30h]
    movaps  xmm3, xmmword ptr [rsp + 40h]
    movaps  xmm4, xmmword ptr [rsp + 50h]
    movaps  xmm5, xmmword ptr [rsp + 60h]
    add     rsp, 78h
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rcx
    ret
NJ_LevelExpCallStub ENDP

; Stage 4, perk points. This REPLACES the game's whole "perk pool += count" sequence (0x1C bytes,
; via a 5-byte branch + NOPs), mid-function, so everything volatile is preserved. Contract there:
; ebx = the count the game meant to add (negative for a removal). The hook applies this mod's table
; through CommonLibSSE-NG's accessor and the stub jumps to the instruction after the sequence.
NJ_PerkPoolStub PROC
    push    rax
    push    rcx
    push    rdx
    push    r8
    push    r9
    push    r10
    push    r11
    push    rbx
    mov     rbx, rsp
    sub     rsp, 60h
    and     rsp, -16
    movaps  xmmword ptr [rsp + 00h], xmm0
    movaps  xmmword ptr [rsp + 10h], xmm1
    movaps  xmmword ptr [rsp + 20h], xmm2
    movaps  xmmword ptr [rsp + 30h], xmm3
    movaps  xmmword ptr [rsp + 40h], xmm4
    movaps  xmmword ptr [rsp + 50h], xmm5
    mov     ecx, dword ptr [rbx]            ; the saved rbx (pushed last) = the game's ebx = the count
    movsx   ecx, cl                         ; the game adds it as a signed byte
    sub     rsp, 20h
    call    NJ_PerkPool_Hook
    add     rsp, 20h
    movaps  xmm0, xmmword ptr [rsp + 00h]
    movaps  xmm1, xmmword ptr [rsp + 10h]
    movaps  xmm2, xmmword ptr [rsp + 20h]
    movaps  xmm3, xmmword ptr [rsp + 30h]
    movaps  xmm4, xmmword ptr [rsp + 40h]
    movaps  xmm5, xmmword ptr [rsp + 50h]
    mov     rsp, rbx
    pop     rbx
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rcx
    pop     rax
    ; leave the registers as the replaced sequence would have: rdx = the player, ecx = eax = the new count
    mov     rdx, qword ptr [NJ_PerkPoolPlayer]
    mov     rdx, qword ptr [rdx]
    mov     ecx, dword ptr [NJ_PerkPoolNewCount]
    mov     eax, ecx
    jmp     qword ptr [NJ_PerkPoolReturn]
NJ_PerkPoolStub ENDP

END
