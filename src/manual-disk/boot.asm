; The manual disk's boot sector: loads the reader from the sectors after it to 1000h:0000 and starts it.
; No operating system: the BIOS's diskette service reads, one sector at a time.
; MezinXT's own; assembled by tools/asm86.py. The sector count at `count` is set by manual-disk/tools/build.py.

        org 7C00h
        cli
        xor ax,ax
        mov ss,ax
        mov sp,7C00h
        mov ds,ax
        sti
        cld
        mov [drive],dl
        mov ax,1000h
        mov es,ax
        xor bx,bx
        mov si,1                ; the sector to read, counted from the start of the diskette
        mov di,[count]
sector: mov byte [tries],4
again:  mov ax,si
        mov cl,9
        div cl                  ; al = track and side, ah = sector on the track
        mov cl,ah
        inc cl
        mov ch,al
        shr ch,1
        mov dh,al
        and dh,1
        mov dl,[drive]
        mov ax,0201h
        int 13h
        jnc good
        xor ah,ah
        mov dl,[drive]
        int 13h
        dec byte [tries]
        jnz again
        mov si,failed
say:    lodsb
        or al,al
        jz wait
        mov ah,0Eh
        xor bx,bx
        int 10h
        jmp say
wait:   xor ah,ah
        int 16h
        int 19h
good:   add bx,512
        inc si
        dec di
        jnz sector
        mov dl,[drive]
        jmp far 1000h:0

drive:  db 0
tries:  db 0
count:  dw 0
failed: db 'Eroare la incarcarea de pe discheta. Se apasa o tasta.', 0

        times 7DFEh-$ db 0
        dw 0AA55h
