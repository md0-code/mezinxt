; MezinXT - the Winchester coupler's option ROM (src/hw/hdc.h), 8 KiB at C8000h.
;
; MezinXT's own code, written for tools/asm86.py (tools/hdrom.py assembles it,
; pads it to 8 KiB, sets the checksum byte and writes src/hw/guest/hdrom.c).
; It drives the coupler only through its registers (320h-323h) and the 8237's
; channel 3, from the IBM Fixed Disk Adapter's documented interface; no code of
; IBM's, Xebec's or anyone else's ROM is in it.
;
; The POST (BIOS 4.0 scans C8000h for 55h AAh) calls the entry at offset 3:
;   - the coupler is reset and must answer; each drive is asked for its geometry
;     with MezinXT's command C0h (drive 0 first; a missing drive 0 ends the
;     count), given its characteristics (0Ch) and recalibrated;
;   - the fixed disk parameter tables (16 bytes, the PC/AT layout) are those of
;     this ROM for the period geometries, else built in 1 KiB taken from the top
;     of memory; INT 41h points at drive 0's, INT 46h at drive 1's;
;   - INT 13h moves to INT 40h (the Junior BIOS's diskette service, called from
;     here for drives 00h-7Fh) and INT 13h becomes this ROM's; INT 19h boots from
;     the diskette in A: first, then from the fixed disk;
;   - 40:74h is the last status, 40:75h the number of fixed disks;
;   - a line for each drive is printed.
;
; INT 13h for DL = 80h, 81h, the PC/XT functions: 00h reset, 01h status (AL),
; 02h read, 03h write, 04h verify, 05h format track, 06h format bad track,
; 07h format drive, 08h parameters, 09h initialize, 0Ah read long, 0Bh write
; long, 0Ch seek, 0Dh alternate reset, 0Eh read buffer, 0Fh write buffer, 10h
; test ready, 11h recalibrate, 12h-14h diagnostics, and the PC/AT's 15h (disk
; type). Data move by DMA (channel 3); the ROM polls the status register for
; the end of a command and leaves the coupler's interrupt masked.

org 0

DATA equ 320h
STAT equ 321h
SELECT equ 322h
MASKR equ 323h

; the frame of the INT 13h service and of the start-up code: BP, then
F_DX equ 2                      ; the caller's DX (DL at +2, DH at +3)
F_CX equ 4                      ; CX (CL +4, CH +5)
F_BX equ 6
F_ES equ 12
DCB equ 6                       ; [bp-6] .. [bp-1]: the command block
L_MASK equ 7                    ; mask register value for the command
L_FUNC equ 8                    ; AH of the call
L_COUNT equ 9                   ; AL of the call (returned as AL)
L_UNIT equ 10
L_STATUS equ 11                 ; the completion byte
L_MODE equ 12                   ; 8237 mode
L_SIZE equ 14                   ; bytes a sector of the transfer (word)
L_WAIT equ 16                   ; outer loops of a wait (word)
L_BUF equ 24                    ; [bp-24] .. [bp-17]: eight bytes
L_TABLE equ 28                  ; the drive's table: offset [bp-28], segment [bp-26]
L_FOUND equ 30                  ; drives found (start-up)
L_RAM equ 32                    ; the segment of the RAM tables, 0 for none (start-up)
LOCALS equ 32

start:
 db 55h, 0AAh, 16               ; 16 blocks of 512 bytes
 jmp init

 db 'MezinXT Winchester coupler BIOS 1.0', 0

; --- start-up -------------------------------------------------------------------------------

init:
 pushf
 push ax
 push bx
 push cx
 push dx
 push si
 push di
 push ds
 push es
 push bp
 mov bp,sp
 sub sp,LOCALS
 cld
 mov word [bp-L_WAIT],20
 mov word [bp-L_RAM],0
 mov byte [bp-L_FOUND],0
 mov si,banner
 call print
 ; the coupler: a reset leaves it at rest (status 0); open bus reads FFh
 mov dx,STAT
 out dx,al
 mov bl,0FFh
 mov bh,0
 mov di,2
 call wait_st
 jnc init_found
 mov si,no_coupler
 call print
 jmp init_end
init_found:
 xor ax,ax
 mov ds,ax
 mov byte [bp-L_UNIT],0
init_unit:
 call probe
 jc init_units_done
 call place_table
 call init_drive
 inc byte [bp-L_FOUND]
 call show_drive
 inc byte [bp-L_UNIT]
 cmp byte [bp-L_UNIT],2
 jb init_unit
init_units_done:
 cmp byte [bp-L_FOUND],0
 jne init_hook
 mov si,no_drive
 call print
 jmp init_end
init_hook:
 xor ax,ax
 mov ds,ax
 cli
 mov ax,[4Eh]
 mov bx,cs
 cmp ax,bx
 je init_vectors                 ; INT 13h is ours already (a restart that kept the vectors): INT 40h has the diskettes
 mov ax,[4Ch]
 mov [100h],ax
 mov ax,[4Eh]
 mov [102h],ax
init_vectors:
 mov word [4Ch],int13
 mov [4Eh],cs
 mov word [64h],boot
 mov [66h],cs
 mov ax,40h
 mov ds,ax
 mov al,[bp-L_FOUND]
 mov [75h],al
 mov byte [74h],0
 mov byte [76h],0
 mov byte [77h],0
init_end:
 mov sp,bp
 pop bp
 pop es
 pop ds
 pop di
 pop si
 pop dx
 pop cx
 pop bx
 pop ax
 popf
 retf

; The geometry of drive [bp-L_UNIT] (command C0h) into [bp-L_BUF]: cylinders high and low, heads, sectors.
; CF set when there is no such drive.
probe:
 mov byte [bp-DCB],0C0h
 call unit_dcb
 mov byte [bp-L_MASK],0
 call execute
 jc probe_none
 ; data (REQ without C/D) or the completion byte at once (an error: no drive)
 mov dx,STAT
 mov bx,0101h
 mov di,20
 call wait_st
 jc probe_none
 in al,dx
 test al,4
 jnz probe_end
 lea si,[bp-L_BUF]
 mov cx,8
 call read_bytes
 jc probe_none
 call complete
 or ah,ah
 jnz probe_none
 clc
 ret
probe_end:
 mov dx,DATA
 in al,dx
probe_none:
 stc
 ret

; The parameter table of the drive's geometry: this ROM's own for a period drive, else one built in RAM taken
; from the top of memory; INT 41h (drive 0) or INT 46h (drive 1) then points at it. DS = 0.
place_table:
 mov ah,[bp-L_BUF]
 mov al,[bp-L_BUF+1]
 mov bl,[bp-L_BUF+2]
 mov si,tables
 mov cx,TABLES
pt_search:
 cmp ax,[cs:si]
 jne pt_next
 cmp bl,[cs:si+2]
 je pt_rom
pt_next:
 add si,16
 loop pt_search
 ; a geometry of its own: 16 bytes in RAM
 push ax
 cmp word [bp-L_RAM],0
 jne pt_have
 mov ax,[413h]
 dec ax
 mov [413h],ax
 mov cl,6
 shl ax,cl
 mov [bp-L_RAM],ax
pt_have:
 pop ax
 mov es,[bp-L_RAM]
 mov di,0
 cmp byte [bp-L_UNIT],0
 je pt_fill
 mov di,16
pt_fill:
 mov [es:di],ax
 mov [es:di+2],bl
 mov [es:di+3],ax
 mov [es:di+5],ax
 mov byte [es:di+7],0Bh
 mov byte [es:di+8],05h
 mov byte [es:di+9],0Ch
 mov byte [es:di+10],0B4h
 mov byte [es:di+11],28h
 mov [es:di+12],ax
 mov byte [es:di+14],17
 mov byte [es:di+15],0
 mov ax,es
 mov si,di
 jmp short pt_set
pt_rom:
 mov ax,cs
pt_set:
 mov bx,104h
 cmp byte [bp-L_UNIT],0
 je pt_vector
 mov bx,118h
pt_vector:
 mov [bx],si
 mov [bx+2],ax
 ret

; "Fixed disk 0: 615 cylinders, 4 heads, 17 sectors"
show_drive:
 mov si,msg_disk
 call print
 mov al,[bp-L_UNIT]
 add al,'0'
 call putc
 mov si,msg_colon
 call print
 mov ah,[bp-L_BUF]
 mov al,[bp-L_BUF+1]
 call print_dec
 mov si,msg_cyl
 call print
 mov al,[bp-L_BUF+2]
 xor ah,ah
 call print_dec
 mov si,msg_heads
 call print
 mov al,[bp-L_BUF+3]
 xor ah,ah
 call print_dec
 mov si,msg_sectors
 call print
 ret

; --- the coupler ----------------------------------------------------------------------------

; Waits until (status AND BL) = BH; DX the status port, DI the outer loops (about half a second each,
; 0 for 65536). CF set on a timeout.
wait_st:
 push cx
ws_outer:
 xor cx,cx
ws_inner:
 in al,dx
 and al,bl
 cmp al,bh
 je ws_ok
 loop ws_inner
 dec di
 jnz ws_outer
 pop cx
 stc
 ret
ws_ok:
 pop cx
 clc
 ret

; The command block at [bp-6] sent: the mask register set from [bp-L_MASK], select, six bytes.
; CF set and AH = 80h when the coupler does not take it.
execute:
 mov dx,MASKR
 mov al,[bp-L_MASK]
 out dx,al
 mov dx,SELECT
 out dx,al
 mov dx,STAT
 mov bx,0D0Fh                    ; BSY, C/D, REQ; I/O clear
 mov di,20
 call wait_st
 jc ex_timeout
 lea si,[bp-DCB]
 mov cx,6
ex_byte:
 mov dx,STAT
 mov bx,0507h                    ; REQ with C/D, to the coupler
 mov di,20
 call wait_st
 jc ex_timeout
 mov al,[ss:si]
 mov dx,DATA
 out dx,al
 inc si
 loop ex_byte
 clc
 ret
ex_timeout:
 mov ah,80h
 stc
 ret

; CX bytes from the data port into SS:SI (a data phase from the coupler). CF on a timeout.
read_bytes:
 mov dx,STAT
 mov bx,0307h                    ; REQ with I/O, data
 mov di,20
 call wait_st
 jc rb_end
 mov dx,DATA
 in al,dx
 mov [ss:si],al
 inc si
 loop read_bytes
 clc
rb_end:
 ret

; The end of a command: the completion byte read, the mask register and DMA channel 3 masked again. AH = 0,
; or the BIOS status of the error (through request sense), or 80h when the coupler does not end.
complete:
 mov dx,STAT
 mov bx,0707h                    ; REQ, C/D, I/O: the completion byte
 mov di,[bp-L_WAIT]
 call wait_st
 jc cp_timeout
 mov dx,DATA
 in al,dx
 mov [bp-L_STATUS],al
 mov dx,MASKR
 xor al,al
 out dx,al
 mov al,7
 out 0Ah,al
 test byte [bp-L_STATUS],2
 jnz cp_sense
 xor ah,ah
 ret
cp_timeout:
 mov al,7
 out 0Ah,al
 mov ah,80h
 ret
cp_sense:
 mov byte [bp-DCB],03h
 call unit_dcb
 mov byte [bp-L_MASK],0
 call execute
 jc cp_failed
 lea si,[bp-L_BUF+4]             ; the sense bytes (the first four may hold a geometry)
 mov cx,4
 call read_bytes
 jc cp_failed
 mov dx,STAT
 mov bx,0707h
 mov di,20
 call wait_st
 jc cp_failed
 mov dx,DATA
 in al,dx
 test al,2
 jnz cp_failed
 mov bl,[bp-L_BUF+4]
 and bx,3Fh
 mov ah,[cs:bx+errmap]
 ret
cp_failed:
 mov ah,0FFh                     ; sense failed
 ret

; [bp-6] = command already set; the rest of the block: the unit, zeros.
unit_dcb:
 mov al,[bp-L_UNIT]
 mov cl,5
 shl al,cl
 mov [bp-DCB+1],al
 mov byte [bp-DCB+2],0
 mov byte [bp-DCB+3],0
 mov byte [bp-DCB+4],0
 mov byte [bp-DCB+5],0
 ret

; The parameter table of drive [bp-L_UNIT] into [bp-L_TABLE] (from INT 41h or INT 46h).
get_table:
 push es
 xor ax,ax
 mov es,ax
 mov bx,104h
 cmp byte [bp-L_UNIT],0
 je gt_vector
 mov bx,118h
gt_vector:
 mov ax,[es:bx]
 mov [bp-L_TABLE],ax
 mov ax,[es:bx+2]
 mov [bp-L_TABLE+2],ax
 pop es
 ret

; Drive [bp-L_UNIT] given its characteristics from its table (0Ch), then recalibrated. AH = status.
init_drive:
 call get_table
 mov byte [bp-DCB],0Ch
 call unit_dcb
 mov byte [bp-L_MASK],0
 call execute
 jc id_end
 push es
 les di,[bp-L_TABLE]
 mov si,init_order
 mov cx,8
id_byte:
 push cx
 mov dx,STAT
 mov bx,0107h                    ; REQ, data, to the coupler
 mov di,20
 call wait_st
 pop cx
 jc id_timeout
 mov bl,[cs:si]
 xor bh,bh
 les di,[bp-L_TABLE]
 mov al,[es:bx+di]
 mov dx,DATA
 out dx,al
 inc si
 loop id_byte
 pop es
 call complete
 or ah,ah
 jnz id_end
 mov byte [bp-DCB],01h
 call unit_dcb
 mov byte [bp-L_MASK],0
 call execute
 jc id_end
 call complete
id_end:
 ret
id_timeout:
 pop es
 mov ah,80h
 ret

; --- INT 13h --------------------------------------------------------------------------------

int13:
 sti
 cmp dl,80h
 jae hd
 int 40h                         ; a diskette: the BIOS's own service
 retf 2
hd:
 push ds
 push es
 push si
 push di
 push bx
 push cx
 push dx
 push bp
 mov bp,sp
 sub sp,LOCALS
 cld
 mov [bp-L_FUNC],ah
 mov [bp-L_COUNT],al
 mov al,dl
 sub al,80h
 mov [bp-L_UNIT],al
 mov word [bp-L_WAIT],20
 mov ax,40h
 mov ds,ax
 mov ah,[bp-L_FUNC]
 cmp ah,8
 jne hd_check
 jmp f_params
hd_check:
 cmp ah,15h
 ja bad_command
 mov al,[bp-L_UNIT]
 cmp al,[75h]
 jae bad_command
 call get_table
 call make_dcb
 mov bl,[bp-L_FUNC]
 xor bh,bh
 shl bx,1
 jmp [cs:bx+ftable]

bad_command:
 mov ah,1

; AH = status: kept in 40:74h, CF set when it is not 0, AL = [bp-L_COUNT].
done:
 mov [74h],ah
 mov al,[bp-L_COUNT]
 mov sp,bp
 pop bp
 pop dx
 pop cx
 pop bx
 pop di
 pop si
 pop es
 pop ds
 cmp ah,1
 cmc
 retf 2

; The command block from the caller's registers: drive and head, cylinder, sector (0-based on the coupler),
; count, and the table's control byte.
make_dcb:
 mov al,[bp-L_UNIT]
 mov cl,5
 shl al,cl
 mov ah,[bp+F_DX+1]
 and ah,1Fh
 or al,ah
 mov [bp-DCB+1],al
 mov al,[bp+F_CX]
 mov ah,al
 and ah,0C0h
 and al,3Fh
 dec al
 and al,3Fh
 or al,ah
 mov [bp-DCB+2],al
 mov al,[bp+F_CX+1]
 mov [bp-DCB+3],al
 mov al,[bp-L_COUNT]
 mov [bp-DCB+4],al
 push es
 les di,[bp-L_TABLE]
 mov al,[es:di+8]
 pop es
 mov [bp-DCB+5],al
 ret

ftable:
 dw f_reset, f_status, f_read, f_write, f_verify, f_ftrack, f_fbad, f_fdrive
 dw f_params, f_init, f_rlong, f_wlong, f_seek, f_areset, f_rbuffer, f_wbuffer
 dw f_ready, f_recal, f_dram, f_ddrive, f_dctrl, f_type

; 00h: the coupler reset, then every drive given its characteristics and recalibrated.
f_reset:
 mov dx,STAT
 out dx,al
 mov bx,000Fh
 mov di,4
 call wait_st
 jnc fr_drives
 mov ah,5                        ; reset failed
 jmp done
fr_drives:
 mov al,[bp-L_UNIT]
 push ax
 mov byte [bp-L_UNIT],0
fr_next:
 call init_drive
 or ah,ah
 jnz fr_end
 inc byte [bp-L_UNIT]
 mov al,[bp-L_UNIT]
 cmp al,[75h]
 jb fr_next
 xor ah,ah
fr_end:
 pop bx
 mov [bp-L_UNIT],bl
 jmp done

; 01h: the last status in AL.
f_status:
 mov al,[74h]
 mov [bp-L_COUNT],al
 xor ah,ah
 jmp done

f_read:
 mov byte [bp-DCB],08h
 mov byte [bp-L_MODE],47h        ; single, increment, write to memory, channel 3
 mov word [bp-L_SIZE],512
 jmp dma_command
f_write:
 mov byte [bp-DCB],0Ah
 mov byte [bp-L_MODE],4Bh        ; read from memory
 mov word [bp-L_SIZE],512
 jmp dma_command
f_rlong:
 mov byte [bp-DCB],0E5h
 mov byte [bp-L_MODE],47h
 mov word [bp-L_SIZE],516
 jmp dma_command
f_wlong:
 mov byte [bp-DCB],0E6h
 mov byte [bp-L_MODE],4Bh
 mov word [bp-L_SIZE],516
 jmp dma_command
f_rbuffer:
 mov byte [bp-DCB],0Eh
 mov byte [bp-L_MODE],47h
 jmp buffer_command
f_wbuffer:
 mov byte [bp-DCB],0Fh
 mov byte [bp-L_MODE],4Bh
buffer_command:
 mov word [bp-L_SIZE],512
 mov byte [bp-L_COUNT],1
 jmp dma_command

f_verify:
 mov byte [bp-DCB],05h
 jmp plain_command
f_ftrack:
 mov byte [bp-DCB],06h
 jmp format_command
f_fbad:
 mov byte [bp-DCB],07h
 jmp format_command
f_fdrive:
 mov byte [bp-DCB],04h
 mov word [bp-L_WAIT],0          ; a whole drive takes minutes
format_command:
 and byte [bp-DCB+2],0C0h        ; no sector; byte 4 is the interleave (AL)
 jmp plain_command
f_seek:
 mov byte [bp-DCB],0Bh
 jmp plain_command
f_ready:
 mov byte [bp-DCB],00h
 jmp plain_command
f_recal:
 mov byte [bp-DCB],01h
 jmp plain_command
f_dram:
 mov byte [bp-DCB],0E0h
 jmp plain_command
f_ddrive:
 mov byte [bp-DCB],0E3h
 jmp plain_command
f_dctrl:
 mov byte [bp-DCB],0E4h
plain_command:
 mov byte [bp-L_MASK],0
 call execute
 jc pc_end
 call complete
pc_end:
 jmp done

; 09h and 0Dh: the drive given its characteristics again and recalibrated.
f_init:
f_areset:
 call init_drive
 jmp done

; A command that moves sectors by DMA: the 8237's channel 3 set up for ES:BX and AL sectors of [bp-L_SIZE]
; bytes (09h when the buffer crosses a 64 KiB boundary), then the command with the mask's DMA bit.
dma_command:
 mov al,[bp-L_COUNT]
 or al,al
 jnz dc_count
 jmp bad_command
dc_count:
 xor ah,ah
 mul word [bp-L_SIZE]
 or dx,dx
 jnz dc_boundary
 dec ax
 mov cx,ax                       ; bytes less one
 mov ax,[bp+F_ES]
 push cx
 mov cl,4
 rol ax,cl
 mov ch,al
 and al,0F0h
 add ax,[bp+F_BX]
 jnc dc_page
 inc ch
dc_page:
 mov si,ax                       ; the address in its 64 KiB page
 mov dl,ch
 and dl,0Fh                      ; the page
 pop cx
 mov di,si
 add di,cx
 jc dc_boundary
 cli
 out 0Ch,al                      ; the first/last flip-flop
 mov al,[bp-L_MODE]
 out 0Bh,al
 mov ax,si
 out 06h,al
 mov al,ah
 out 06h,al
 mov al,dl
 out 82h,al
 mov ax,cx
 out 07h,al
 mov al,ah
 out 07h,al
 sti
 mov al,3
 out 0Ah,al                      ; channel 3 unmasked
 mov byte [bp-L_MASK],1          ; DMA enabled, the interrupt masked
 call execute
 jc dc_end
 call complete
dc_end:
 jmp done
dc_boundary:
 mov ah,9
 jmp done

; 08h: DL drives, DH the last head, CH and CL the last usable cylinder (the last one is kept for
; diagnostics, as on the PC/XT) and the sectors.
f_params:
 mov al,[75h]
 mov [bp+F_DX],al
 mov al,[bp-L_UNIT]
 cmp al,[75h]
 jb fp_drive
 mov word [bp+F_CX],0
 mov byte [bp+F_DX+1],0
 mov ah,7
 jmp done
fp_drive:
 call get_table
 push es
 les di,[bp-L_TABLE]
 mov ax,[es:di]
 mov bl,[es:di+2]
 pop es
 sub ax,2
 mov [bp+F_CX+1],al
 mov cl,2
 shr ax,cl
 and al,0C0h
 or al,17
 mov [bp+F_CX],al
 dec bl
 mov [bp+F_DX+1],bl
 mov byte [bp-L_COUNT],0
 xor ah,ah
 jmp done

; 15h (as on the PC/AT): AH = 3, a fixed disk; CX:DX its usable sectors.
f_type:
 push es
 les di,[bp-L_TABLE]
 mov ax,[es:di]
 dec ax
 mov bl,[es:di+2]
 pop es
 xor bh,bh
 mul bx
 mov bx,17
 mul bx
 mov [bp+F_CX],dx
 mov [bp+F_DX],ax
 mov byte [74h],0
 mov al,[bp-L_COUNT]
 mov sp,bp
 pop bp
 pop dx
 pop cx
 pop bx
 pop di
 pop si
 pop es
 pop ds
 mov ah,3
 clc
 retf 2

; --- INT 19h --------------------------------------------------------------------------------

; The diskette in drive A: first (through the BIOS's diskette service; an empty drive is not tried
; again), then the fixed disk's master boot record.
boot:
 sti
 cld
 xor ax,ax
 mov ds,ax
 mov es,ax
 mov si,3
bt_floppy:
 xor ax,ax
 xor dx,dx
 int 40h
 mov ax,0201h
 mov bx,7C00h
 mov cx,1
 xor dx,dx
 int 40h
 jnc bt_go_floppy
 cmp ah,80h
 je bt_fixed
 dec si
 jnz bt_floppy
bt_fixed:
 mov si,2
bt_fixed_try:
 xor ax,ax
 mov dx,80h
 int 13h
 mov ax,0201h
 mov bx,7C00h
 mov cx,1
 mov dx,80h
 int 13h
 jnc bt_check
 dec si
 jnz bt_fixed_try
 jmp short bt_none
bt_check:
 cmp word [7DFEh],0AA55h
 jne bt_none
 mov dx,80h
 jmp far 0:7C00h
bt_go_floppy:
 xor dx,dx
 jmp far 0:7C00h
bt_none:
 mov si,no_system
 call print
 xor ah,ah
 int 16h
 jmp boot

; --- text -----------------------------------------------------------------------------------

; The zero-ended text at CS:SI through the BIOS's teletype.
print:
 push ax
 push bx
pr_char:
 mov al,[cs:si]
 inc si
 or al,al
 jz pr_end
 call putc
 jmp short pr_char
pr_end:
 pop bx
 pop ax
 ret

putc:
 push ax
 push bx
 mov ah,0Eh
 mov bx,7
 int 10h
 pop bx
 pop ax
 ret

; AX in decimal.
print_dec:
 push ax
 push bx
 push cx
 push dx
 mov bx,10
 xor cx,cx
pd_digit:
 xor dx,dx
 div bx
 push dx
 inc cx
 or ax,ax
 jnz pd_digit
pd_out:
 pop ax
 add al,'0'
 call putc
 loop pd_out
 pop dx
 pop cx
 pop bx
 pop ax
 ret

banner: db 'MezinXT Winchester coupler BIOS 1.0', 13, 10, 0
no_coupler: db 'Winchester coupler: no answer', 13, 10, 0
no_drive: db 'Winchester coupler: no drive', 13, 10, 0
msg_disk: db 'Fixed disk ', 0
msg_colon: db ': ', 0
msg_cyl: db ' cylinders, ', 0
msg_heads: db ' heads, ', 0
msg_sectors: db ' sectors', 13, 10, 0
no_system: db 13, 10, 'No system disk: put one in drive A: and press a key', 13, 10, 0

; The order of the characteristics' bytes sent with command 0Ch, as offsets in a parameter table: cylinders
; high and low, heads, reduced write current cylinder high and low, write precompensation high and low, ECC burst.
init_order: db 1, 0, 2, 4, 3, 6, 5, 7

; BIOS status for each sense code (00h-3Fh).
errmap:
 db 00h, 20h, 40h, 0CCh, 0AAh, 0BBh, 40h, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh
 db 10h, 10h, 02h, 0BBh, 04h, 40h, 0BBh, 0BBh, 11h, 0Ah, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh
 db 01h, 04h, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh
 db 20h, 20h, 20h, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh, 0BBh

; Fixed disk parameter tables of the period drives (the PC/AT layout): cylinders, heads, reduced write
; current and write precompensation cylinders (none: the cylinder count), ECC burst, control byte (step
; rate 70 us), the PC/XT's three timeouts, landing zone, sectors a track.
TABLES equ 6
tables:
 dw 615
 db 4
 dw 615, 615
 db 0Bh, 05h, 0Ch, 0B4h, 28h
 dw 615
 db 17, 0
 dw 733
 db 5
 dw 733, 733
 db 0Bh, 05h, 0Ch, 0B4h, 28h
 dw 733
 db 17, 0
 dw 977
 db 5
 dw 977, 977
 db 0Bh, 05h, 0Ch, 0B4h, 28h
 dw 977
 db 17, 0
 dw 977
 db 7
 dw 977, 977
 db 0Bh, 05h, 0Ch, 0B4h, 28h
 dw 977
 db 17, 0
 dw 1024
 db 8
 dw 1024, 1024
 db 0Bh, 05h, 0Ch, 0B4h, 28h
 dw 1024
 db 17, 0
 dw 306
 db 4
 dw 306, 306
 db 0Bh, 05h, 0Ch, 0B4h, 28h
 dw 306
 db 17, 0
