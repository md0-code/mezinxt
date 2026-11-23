; The manual application: the Junior's technical manual as topics with links, in the 80x25 colour text
; mode, with pictures in the CGA's 160x100 sixteen-colour arrangement and tests that run on the machine.
; Loaded by the boot sector at 1000h:0000 with the boot drive in DL. The BIOS serves the diskette (INT 13h),
; the keyboard (INT 16h) and the mode changes (INT 10h). The text screen is drawn in memory and copied into the
; CGA's store during the vertical retrace, a few rows at a time, so that the 80-column mode shows no snow.
; MezinXT's own; assembled by tools/asm86.py through manual-disk/tools/build.py, which joins this file,
; tests.asm and the tables it makes, and fills the header after the first jump.

DOCSEG  equ 2000h               ; the topic being read, packed
PICSEG  equ 3000h               ; a picture
VSEG    equ 0B800h
TABLE   equ 8000h               ; the directory, in this segment
SHADOW  equ 0C000h              ; the text screen as it is drawn, in this segment
SHOWN   equ 0D000h              ; the text screen as the CGA's store holds it
ENTRY   equ 12                  ; bytes of a directory entry
BODY    equ 22                  ; text rows of a topic (rows 2 to 23)
NORMAL  equ 17h
LINKAT  equ 1Ah
SELAT   equ 2Fh
TITLEAT equ 3Fh
BARAT   equ 70h
TABAT   equ 4Fh
MAXLINK equ 40

        org 0
        jmp begin
hdr_dir:   dw 0                 ; first sector of the directory
hdr_dirn:  db 0                 ; its sectors
hdr_docs:  dw 0                 ; topics and pages
hdr_sect:  times 7 dw 0         ; the topic each section starts with
hdr_pics:  times 2 dw 0         ; first sector of each picture (one is used)

begin:  cli
        mov ax,cs
        mov ds,ax
        mov ss,ax
        mov sp,0FFF0h
        sti
        cld
        mov [drive],dl
        ; nine sectors a track: the BIOS's diskette parameters copied, their last sector set to 9
        xor ax,ax
        mov es,ax
        mov si,[es:78h]
        mov ax,[es:7Ah]
        push ds
        pop es
        push ds
        mov ds,ax
        mov di,dpt
        mov cx,11
        rep movsb
        pop ds
        mov byte [dpt+4],9
        xor ax,ax
        mov es,ax
        cli
        mov word [es:78h],dpt
        mov [es:7Ah],cs
        sti
        mov dl,[drive]
        int 13h
        call text_mode
        push ds
        pop es
        mov bx,TABLE
        mov ax,[hdr_dir]
        mov cl,[hdr_dirn]
        call read
        xor ax,ax
        call picture            ; the intro: the maker's mark, the title, the machine
        call text_mode
        xor ax,ax
        mov [doc],ax
        mov byte [top],0
        mov byte [cur],0
        call load_doc
        jmp redraw

; ---------------------------------------------------------------------------------------------
; the diskette

; AX = first sector, CL = how many, ES:BX = where to
read:   mov [rd_lba],ax
        mov [rd_n],cl
rd_next:
        cmp byte [rd_n],0
        jne rd_go
        ret
rd_go:  mov byte [rd_try],3
rd_again:
        mov ax,[rd_lba]
        mov dl,9
        div dl
        mov cl,ah
        inc cl
        mov ch,al
        shr ch,1
        mov dh,al
        and dh,1
        mov al,9
        sub al,ah
        cmp al,[rd_n]
        jbe rd_cnt
        mov al,[rd_n]
rd_cnt: mov [rd_now],al
        mov dl,[drive]
        mov ah,2
        int 13h
        jnc rd_ok
        xor ah,ah
        mov dl,[drive]
        int 13h
        dec byte [rd_try]
        jnz rd_again
        push es
        push bx
        mov si,m_error
        call status
        call getkey
        pop bx
        pop es
        jmp rd_go
rd_ok:  mov al,[rd_now]
        sub [rd_n],al
        xor ah,ah
        add [rd_lba],ax
        mov cl,9
        shl ax,cl
        add bx,ax
        jmp rd_next

; a key, after the screen is brought up to date
getkey: call flush
        xor ah,ah
        int 16h
        ret

; The rows drawn since the last time go into the CGA's store. In the 80-column text mode the card shows snow
; whenever the processor uses the store while a line is being drawn, so the rows are copied only after the
; vertical retrace begins (port 3DAh, bit 3), three to a retrace; what the store holds is remembered, so that
; it is never read back and only rows that changed are written.
flush:  push ax
        push bx
        push cx
        push dx
        push si
        push di
        push ds
        push es
        push cs
        pop ds
        cld
        xor bx,bx               ; the row's place in the screen
        mov byte [fl_n],0       ; rows copied in this retrace
fl_row: lea si,[bx+SHADOW]
        lea di,[bx+SHOWN]
        push cs
        pop es
        mov cx,80
        repe cmpsw
        jne fl_new
        mov byte [fl_n],0       ; the comparing took its time: the next row waits for a retrace
        jmp fl_next
fl_new: cmp byte [fl_n],0
        jne fl_copy
        mov dx,3DAh
        mov cx,0FFFFh
fl_end: in al,dx                ; a retrace under way is let pass, to have a whole one
        test al,8
        jz fl_wait
        loop fl_end
fl_wait:
        mov cx,0FFFFh
fl_in:  in al,dx
        test al,8
        jnz fl_copy
        loop fl_in
fl_copy:
        lea si,[bx+SHADOW]
        mov di,bx
        mov ax,VSEG
        mov es,ax
        mov cx,80
        rep movsw
        lea si,[bx+SHADOW]
        lea di,[bx+SHOWN]
        push cs
        pop es
        mov cx,80
        rep movsw
        inc byte [fl_n]
        cmp byte [fl_n],3
        jb fl_next
        mov byte [fl_n],0
fl_next:
        add bx,160
        cmp bx,4000
        jb fl_row
        pop es
        pop ds
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        pop ax
        ret

; ---------------------------------------------------------------------------------------------
; the text screen

; 80x25 colour text, no cursor, bright backgrounds in place of blinking
text_mode:
        mov ax,3
        int 10h
        mov ah,1
        mov cx,2000h
        int 10h
        mov dx,3D8h
        mov al,9
        out dx,al
        ; the mode change left the screen blank: both copies say so
        push es
        push di
        push cx
        push cs
        pop es
        mov di,SHADOW
        mov ax,0720h
        mov cx,2000
        rep stosw
        mov di,SHOWN
        mov cx,2000
        rep stosw
        pop cx
        pop di
        pop es
        ret

; DH = row, DL = column: ES:DI = that cell of the screen being drawn
at:     push ax
        push bx
        mov al,80
        mul dh
        mov bl,dl
        xor bh,bh
        add ax,bx
        shl ax,1
        add ax,SHADOW
        mov di,ax
        push cs
        pop es
        pop bx
        pop ax
        ret

; DH = row, AH = attribute: the row blank
fill_row:
        push dx
        push cx
        xor dl,dl
        call at
        mov al,' '
        mov cx,80
        rep stosw
        pop cx
        pop dx
        ret

; DS:SI = text ending in 0, AH = attribute, written at ES:DI
puts:   lodsb
        or al,al
        jz ps_end
        stosw
        jmp puts
ps_end: ret

; SI = text, DH = row, DL = column, AH = attribute
put_at: call at
        jmp puts

; AX = number, BH = attribute, written in decimal at ES:DI
pdec:   push cx
        push dx
        xor cx,cx
pd_div: xor dx,dx
        push bx
        mov bx,10
        div bx
        pop bx
        push dx
        inc cx
        or ax,ax
        jnz pd_div
pd_out: pop ax
        add al,'0'
        mov ah,bh
        stosw
        loop pd_out
        pop dx
        pop cx
        ret

; AL = byte, BH = attribute, written as two hexadecimal digits at ES:DI
phex:   push ax
        push cx
        mov cl,4
        shr al,cl
        call ph_one
        pop cx
        pop ax
        push ax
        and al,0Fh
        call ph_one
        pop ax
        ret
ph_one: add al,'0'
        cmp al,'9'
        jbe ph_put
        add al,7
ph_put: mov ah,bh
        stosw
        ret

; SI = text for the bottom row
status: push si
        mov dh,24
        mov ah,BARAT
        call fill_row
        pop si
        mov dx,1801h
        mov ah,BARAT
        jmp put_at

; ---------------------------------------------------------------------------------------------
; a topic

; [doc] = the topic: reads it and finds its lines
load_doc:
        mov ax,[doc]
        mov bx,ENTRY
        mul bx
        mov bx,ax
        add bx,TABLE
        mov ax,[bx]
        mov cl,[bx+2]
        mov dl,[bx+3]
        mov [nlines],dl
        mov dl,[bx+4]
        mov [section],dl
        mov dl,[bx+5]
        mov [pageno],dl
        mov dx,[bx+6]
        mov [prev],dx
        mov dx,[bx+8]
        mov [next],dx
        mov dx,[bx+10]
        mov [docoff],dx
        mov bx,DOCSEG
        mov es,bx
        xor bx,bx
        call read
        push ds
        pop es
        mov di,linetab
        mov cl,[nlines]
        xor ch,ch
        push ds
        mov si,[docoff]
        mov ax,DOCSEG
        mov ds,ax
        jcxz ld_end
ld_line:
        mov ax,si
        stosw
ld_byte:
        lodsb
        or al,al
        jz ld_next
        cmp al,5
        ja ld_byte
        cmp al,3
        je ld_byte
        inc si                  ; 1 and 4 carry one byte more
        cmp al,5
        jne ld_two
        inc si                  ; 5 carries two
        jmp ld_byte
ld_two: cmp al,2
        jne ld_byte
        add si,3                ; 2 carries four
        jmp ld_byte
ld_next:
        loop ld_line
ld_end: pop ds
        ret

; BX = line of the topic, DH = row, AH = the row's attribute; links are noted when [noting] is set
draw_line:
        mov [base],ah
        call fill_row
        cmp bl,[nlines]
        jb dl_go
        ret
dl_go:  mov dl,1
        call at
        mov ah,[base]
        xor bh,bh
        shl bx,1
        mov si,[bx+linetab]
        mov bl,1                ; the column
        push ds
        mov cx,DOCSEG
        mov ds,cx
dl_next:
        lodsb
        cmp al,5
        ja dl_put
        or al,al
        jz dl_end
        cmp al,1
        jne dl_2
        lodsb
        mov ah,al
        jmp dl_next
dl_2:   cmp al,2
        jne dl_3
        call dl_link
        jmp dl_next
dl_3:   cmp al,3
        jne dl_4
        call dl_close
        jmp dl_next
dl_4:   cmp al,4
        jne dl_5
        lodsb
        mov cl,al
        xor ch,ch
        add bl,al
        mov al,' '
        rep stosw
        jmp dl_next
dl_5:   lodsb
        mov cl,al
        xor ch,ch
        add bl,al
        lodsb
        rep stosw
        jmp dl_next
dl_put: stosw
        inc bl
        jmp dl_next
dl_end: pop ds
        ret
; a link opens: its kind, target and line follow
dl_link:
        mov [cs:keepat],ah
        mov ah,LINKAT
        cmp byte [cs:noting],0
        je dk_skip
        cmp byte [cs:nlinks],MAXLINK
        jae dk_skip
        push di
        push ax
        mov al,[cs:nlinks]
        mov cl,3
        xor ah,ah
        shl ax,cl
        mov di,ax
        add di,linktab
        mov [cs:di],dh
        mov [cs:di+1],bl
        lodsb
        mov [cs:di+3],al
        lodsw
        mov [cs:di+4],ax
        lodsb
        mov [cs:di+6],al
        mov byte [cs:open],1
        pop ax
        pop di
        ret
dk_skip:
        add si,4
        ret
dl_close:
        mov ah,[cs:keepat]
        cmp byte [cs:open],0
        je dc_end
        mov byte [cs:open],0
        push di
        push ax
        mov al,[cs:nlinks]
        mov cl,3
        xor ah,ah
        shl ax,cl
        mov di,ax
        add di,linktab
        mov al,bl
        sub al,[cs:di+1]
        mov [cs:di+2],al
        inc byte [cs:nlinks]
        pop ax
        pop di
dc_end: ret

; AL = link, AH = attribute: the link's cells take it
paint:  cmp al,[nlinks]
        jb pt_go
        ret
pt_go:  mov bl,al
        xor bh,bh
        mov cl,3
        shl bx,cl
        add bx,linktab
        mov dx,[bx]
        xchg dl,dh
        mov cl,[bx+2]
        xor ch,ch
        call at
        jcxz pt_end
pt_one: inc di
        mov al,ah
        stosb
        loop pt_one
pt_end: ret

; AL = most lines the topic can be scrolled by
last_top:
        mov al,[nlines]
        sub al,BODY+1
        jnc lt_end
        xor al,al
lt_end: ret

; the whole screen
draw:   ; the sections
        xor dh,dh
        mov ah,BARAT
        call fill_row
        mov dx,1
        call at
        mov si,tabs
        xor bl,bl
dr_tab: mov ah,BARAT
        cmp bl,[section]
        jne dr_put
        mov ah,TABAT
dr_put: call puts
        mov ax,7020h
        stosw
        inc bl
        cmp bl,7
        jb dr_tab
        ; the title
        mov byte [noting],0
        xor bx,bx
        mov dh,1
        mov ah,TITLEAT
        call draw_line
        ; the text
        mov byte [nlinks],0
        mov byte [open],0
        mov byte [noting],1
        mov dh,2
        mov bl,[top]
        inc bl
dr_row: push bx
        push dx
        xor bh,bh
        mov ah,NORMAL
        call draw_line
        pop dx
        pop bx
        inc bl
        inc dh
        cmp dh,BODY+2
        jb dr_row
        ; more above, more below
        cmp byte [top],0
        je dr_dn
        mov dx,024Fh
        call at
        mov ax,1E18h
        stosw
dr_dn:  call last_top
        cmp [top],al
        jae dr_st
        mov dx,174Fh
        call at
        mov ax,1E19h
        stosw
dr_st:  mov si,m_keys
        cmp byte [pageno],0
        je dr_bar
        mov si,m_pkeys
dr_bar: call status
        mov al,[cur]
        cmp al,[nlinks]
        jb dr_sel
        xor al,al
        mov [cur],al
dr_sel: mov ah,SELAT
        jmp paint

; ---------------------------------------------------------------------------------------------
; reading

redraw: call draw
keys:   call getkey
        cmp ah,50h
        jne k1
        jmp k_down
k1:     cmp ah,48h
        jne k2
        jmp k_up
k2:     cmp ah,51h
        jne k3
        call last_top
        mov ah,[top]
        add ah,BODY-1
        cmp ah,al
        jbe k2s
        mov ah,al
k2s:    mov [top],ah
        mov byte [cur],0
        jmp redraw
k3:     cmp ah,49h
        jne k4
        mov al,[top]
        sub al,BODY-1
        jnc k3s
        xor al,al
k3s:    mov [top],al
        mov byte [cur],0
        jmp redraw
k4:     cmp ah,47h
        jne k5
        mov byte [top],0
        mov byte [cur],0
        jmp redraw
k5:     cmp ah,4Fh
        jne k6
        call last_top
        mov [top],al
        mov byte [cur],0
        jmp redraw
k6:     cmp ah,4Dh
        jne k7
        mov ax,[next]
        jmp k_turn
k7:     cmp ah,4Bh
        jne k8
        mov ax,[prev]
        jmp k_turn
k8:     cmp ah,0Fh
        jne k9
        cmp byte [nlinks],0
        je keys
        mov al,[cur]
        mov ah,LINKAT
        call paint
        mov al,[cur]
        inc al
        cmp al,[nlinks]
        jb k8s
        xor al,al
k8s:    mov [cur],al
        mov ah,SELAT
        call paint
        jmp keys
k9:     cmp ah,3Bh
        jb k10
        cmp ah,41h
        ja k10
        mov bl,ah
        sub bl,3Bh
        xor bh,bh
        shl bx,1
        mov ax,[bx+hdr_sect]
        xor cl,cl
        jmp open_doc
k10:    cmp al,13
        jne k11
        jmp follow
k11:    cmp al,8
        je k_back
        cmp al,27
        je k_back
        and al,0DFh
        cmp al,'P'
        jne keys
        jmp ask_page

k_down: mov al,[cur]
        inc al
        cmp al,[nlinks]
        jae kd_roll
        push ax
        dec al
        mov ah,LINKAT
        call paint
        pop ax
        mov [cur],al
        mov ah,SELAT
        call paint
        jmp keys
kd_roll:
        call last_top
        cmp [top],al
        jae keys
        inc byte [top]
        mov byte [cur],MAXLINK  ; the last link in sight
        call draw
        mov al,[nlinks]
        or al,al
        jz keys
        dec al
        push ax
        xor al,al
        mov ah,LINKAT
        call paint
        pop ax
        mov [cur],al
        mov ah,SELAT
        call paint
        jmp keys
k_up:   mov al,[cur]
        or al,al
        jz ku_roll
        mov ah,LINKAT
        call paint
        dec byte [cur]
        mov al,[cur]
        mov ah,SELAT
        call paint
        jmp keys
ku_roll:
        cmp byte [top],0
        je keys
        dec byte [top]
        jmp redraw

; AX = a topic to turn to, or FFFFh: no way back is kept
k_turn: cmp ax,0FFFFh
        je keys
        mov [doc],ax
        mov byte [top],0
        mov byte [cur],0
        call load_doc
        jmp redraw

k_back: mov bl,[depth]
        or bl,bl
        jz keys
        dec bl
        mov [depth],bl
        xor bh,bh
        shl bx,1
        shl bx,1
        mov ax,[bx+history]
        mov [doc],ax
        mov ax,[bx+history+2]
        mov [top],al
        mov [cur],ah
        call load_doc
        jmp redraw

; AX = the topic to open, CL = the line to show first: the place left is kept
open_doc:
        push ax
        push cx
        mov bl,[depth]
        cmp bl,16
        jb od_room
        ; the oldest place is forgotten
        push ds
        pop es
        mov si,history+4
        mov di,history
        mov cx,30
        rep movsw
        mov bl,15
od_room:
        xor bh,bh
        shl bx,1
        shl bx,1
        mov ax,[doc]
        mov [bx+history],ax
        mov al,[top]
        mov ah,[cur]
        mov [bx+history+2],ax
        shr bx,1
        shr bx,1
        inc bl
        mov [depth],bl
        pop cx
        pop ax
        mov [doc],ax
        mov [top],cl
        mov byte [cur],0
        push cx
        call load_doc
        pop cx
        call last_top
        cmp al,[top]
        jae redraw
        mov [top],al
        jmp redraw

follow: mov al,[cur]
        cmp al,[nlinks]
        jb fo_go
        jmp keys
fo_go:  mov bl,al
        xor bh,bh
        mov cl,3
        shl bx,cl
        add bx,linktab
        mov al,[bx+3]
        mov cl,[bx+6]
        mov bx,[bx+4]
        or al,al
        jnz fo_1
        mov ax,bx
        jmp open_doc
fo_1:   cmp al,1
        jne fo_2
        mov ax,bx
        call picture
        call text_mode
        jmp redraw
fo_2:   shl bx,1
        call [bx+tests]
        call text_mode
        call load_doc           ; a test may have used the topic's store
        jmp redraw

; "Pagina:" and a number: the page of the printed manual with that number, or the next there is
ask_page:
        mov si,m_goto
        call status
        xor bx,bx               ; BL the number so far, BH its digits
ap_key: push bx
        call getkey
        pop bx
        cmp al,27
        jne ap_1
        jmp redraw
ap_1:   cmp al,13
        je ap_do
        cmp al,'0'
        jb ap_key
        cmp al,'9'
        ja ap_key
        cmp bh,3
        jae ap_key
        mov ah,BARAT
        stosw
        inc bh
        sub al,'0'
        mov cl,al
        mov al,bl
        mov ch,10
        mul ch
        add al,cl
        mov bl,al
        jmp ap_key
ap_do:  or bh,bh
        jnz ap_find
        jmp redraw
ap_find:
        mov si,TABLE
        xor ax,ax
ap_look:
        cmp byte [si+5],0
        je ap_no
        cmp [si+5],bl
        jae ap_open
ap_no:  add si,ENTRY
        inc ax
        cmp ax,[hdr_docs]
        jb ap_look
        jmp redraw
ap_open:
        xor cl,cl
        jmp open_doc

; ---------------------------------------------------------------------------------------------
; a picture: 160x100 in sixteen colours. The text mode's rows are made two scan lines high, so that 100 of
; them fit; each cell shows character DEh, its left half in the background colour and its right half in
; the foreground colour. AX = the picture.
picture:
        mov bx,ax
        shl bx,1
        mov ax,[bx+hdr_pics]
        mov bx,PICSEG
        mov es,bx
        xor bx,bx
        mov cl,16
        call read
        mov ax,3
        int 10h
        mov dx,3D8h
        mov al,1
        out dx,al               ; the picture off while it is made
        mov si,pic_crtc
        mov cx,5
pi_reg: mov dx,3D4h
        lodsb
        out dx,al
        inc dx
        lodsb
        out dx,al
        loop pi_reg
        mov ax,VSEG
        mov es,ax
        xor di,di
        push ds
        mov ax,PICSEG
        mov ds,ax
        xor si,si
        mov cx,8000
pi_cell:
        lodsb
        mov ah,al
        mov al,0DEh
        stosw
        loop pi_cell
        pop ds
        mov dx,3D8h
        mov al,9
        out dx,al
        jmp getkey
pic_crtc: db 4,7Fh, 5,6, 6,64h, 7,70h, 9,1

; ---------------------------------------------------------------------------------------------
tabs:    db 'F1 Prezentare', 0, 'F2 Instalare', 0, 'F3 Hardware', 0, 'F4 Software', 0
         db 'F5 Anexe', 0, 'F6 Teste', 0, 'F7 Carte', 0
m_keys:  db 24, 25, ' Tab: legaturi   Enter: deschide   Backspace: inapoi   ', 27, 26, ': alt subiect', 0
m_pkeys: db 24, 25, ' PgUp PgDn: text   ', 27, 26, ': alta pagina   P: numar de pagina   Backspace: inapoi', 0
m_goto:  db 'Pagina (3-118), apoi Enter:  ', 0
m_error: db 'Eroare la citirea dischetei. Se apasa o tasta pentru reluare.', 0

drive:   db 0
dpt:     times 11 db 0
rd_lba:  dw 0
rd_n:    db 0
rd_now:  db 0
rd_try:  db 0
fl_n:    db 0
doc:     dw 0
prev:    dw 0
next:    dw 0
docoff:  dw 0
nlines:  db 0
section: db 0
pageno:  db 0
top:     db 0
cur:     db 0
nlinks:  db 0
noting:  db 0
open:    db 0
base:    db 0
keepat:  db 0
depth:   db 0
history: times 32 dw 0
linktab: times 160 dw 0
linetab: times 256 dw 0
