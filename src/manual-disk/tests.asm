; The manual application's tests: they run on the machine and show what they find. Each is entered in the
; 80x25 text mode and returns with any mode set; the application sets its own again.
; MezinXT's own; joined to app.asm by manual-disk/tools/build.py, which also makes `keytab`.

tests:  dw t_keys, t_video, t_system, t_disk

; SI = the title, BX = the bottom row's text: an empty screen between the two bars
t_screen:
        push bx
        push si
        mov dh,1
ts_row: mov ah,NORMAL
        call fill_row
        inc dh
        cmp dh,24
        jb ts_row
        xor dh,dh
        mov ah,BARAT
        call fill_row
        mov dx,1
        mov ah,BARAT
        mov si,m_tname
        call put_at
        pop si
        mov ah,TABAT
        call puts
        pop si
        jmp status

; SI = text, DH = row, DL = column, in the ordinary colour
say:    mov ah,NORMAL
        jmp put_at

; ---------------------------------------------------------------------------------------------
; the keyboard: the 83 keys drawn; a key pressed lights up, a key released stays marked. The keyboard's
; interrupt (level 1, vector 9) is taken for the time of the test, so that every code the keyboard sends
; is seen, releases too.

t_keys: mov si,m_ktitle
        mov bx,m_kbar
        call t_screen
        mov dx,0303h
        mov si,m_k1
        call say
        mov al,1
tk_all: push ax
        mov ah,BARAT
        call key_cap
        pop ax
        inc al
        cmp al,84
        jb tk_all
        mov word [kq_head],0
        mov byte [kesc],0
        xor ax,ax
        mov es,ax
        cli
        mov ax,[es:24h]
        mov [kold],ax
        mov ax,[es:26h]
        mov [kold+2],ax
        mov word [es:24h],kb_isr
        mov [es:26h],cs
        sti
tk_wait:
        mov bl,[kq_tail]
        cmp bl,[kq_head]
        jne tk_got
        call flush
        hlt
        jmp tk_wait
tk_got: xor bh,bh
        mov al,[bx+kq]
        inc bl
        and bl,15
        mov [kq_tail],bl
        mov [kcode],al
        ; the code, in figures
        mov dx,1203h
        mov si,m_k2
        call say
        mov al,[kcode]
        mov bh,1Bh
        call phex
        mov si,m_k3
        test byte [kcode],80h
        jz tk_say
        mov si,m_k4
tk_say: mov ah,NORMAL
        call puts
        mov al,[kcode]
        mov ah,TABAT
        test al,80h
        jz tk_cap
        mov ah,SELAT
tk_cap: and al,7Fh
        push ax
        call key_cap
        pop ax
        ; Esc pressed twice ends the test
        test byte [kcode],80h
        jnz tk_wait
        cmp al,1
        je tk_esc
        mov byte [kesc],0
        jmp tk_wait
tk_esc: inc byte [kesc]
        cmp byte [kesc],2
        jb tk_wait
        xor ax,ax
        mov es,ax
        cli
        mov ax,[kold]
        mov [es:24h],ax
        mov ax,[kold+2]
        mov [es:26h],ax
        sti
        ret

; AL = scan code (1 to 83), AH = attribute: the key's cap
key_cap:
        or al,al
        jz kc_end
        cmp al,83
        ja kc_end
        dec al
        mov bl,6
        push ax
        mul bl
        mov bx,ax
        pop ax
        add bx,keytab
        mov dh,[bx]
        shl dh,1
        add dh,6
        mov dl,[bx+1]
        call at
        mov cl,[bx+2]           ; the cap's width; the name is three characters in its middle
        sub cl,3
        shr cl,1
        xor ch,ch
        mov al,' '
        push cx
        rep stosw
        mov al,[bx+3]
        stosw
        mov al,[bx+4]
        stosw
        mov al,[bx+5]
        stosw
        pop cx
        mov al,[bx+2]
        sub al,3
        sub al,cl
        mov cl,al
        mov al,' '
        rep stosw
kc_end: ret

kb_isr: push ax
        push bx
        push ds
        push cs
        pop ds
        in al,60h
        mov bl,[kq_head]
        xor bh,bh
        mov [bx+kq],al
        inc bl
        and bl,15
        mov [kq_head],bl
        in al,61h
        mov ah,al
        or al,80h
        out 61h,al
        mov al,ah
        out 61h,al
        mov al,20h
        out 20h,al
        pop ds
        pop bx
        pop ax
        iret

; ---------------------------------------------------------------------------------------------
; the display: one picture after another, a key for the next, Esc to leave

t_video:
        ; the sixteen colours
        mov si,m_vtitle
        mov bx,m_vbar
        call t_screen
        mov dx,0204h
        mov si,m_v1
        call say
        xor bx,bx
tv_col: mov dh,bl
        add dh,4
        mov dl,4
        call at
        mov ah,bl
        mov al,0DBh
        mov cx,24
        rep stosw
        mov ax,1720h
        stosw
        stosw
        mov ah,bl
        mov cl,4
        shl ah,cl
        or ah,0Fh
        cmp bl,7
        jb tv_bg
        and ah,0F0h
tv_bg:  mov al,' '
        mov cx,2
        rep stosw
        push bx
        mov al,bl
        mov bh,ah
        call phex
        pop bx
        mov al,' '
        mov cx,2
        rep stosw
        mov ax,1720h
        stosw
        stosw
        push bx
        shl bx,1
        mov si,[bx+coltab]
        pop bx
        mov ah,NORMAL
        call puts
        inc bl
        cmp bl,16
        jb tv_col
        call tv_key
        ; the characters
        mov si,m_vtitle
        mov bx,m_vbar
        call t_screen
        mov dx,0204h
        mov si,m_v2
        call say
        xor bx,bx               ; BL the character
tv_chr: mov dh,bl
        mov cl,4
        shr dh,cl
        add dh,5
        mov dl,bl
        and dl,0Fh
        mov al,dl
        add dl,al
        add dl,al
        add dl,14
        call at
        mov al,bl
        mov ah,1Fh
        stosw
        inc bl
        jnz tv_chr
        xor bl,bl
tv_hex: mov dh,4                ; the low figure above, the high figure at the left
        mov dl,bl
        add dl,bl
        add dl,bl
        add dl,14
        call at
        mov al,bl
        mov bh,1Bh
        call ph_one
        mov dh,bl
        add dh,5
        mov dl,10
        call at
        mov al,bl
        call ph_one
        inc bl
        cmp bl,16
        jb tv_hex
        call tv_key
        ; forty columns
        mov ax,1
        int 10h
        mov dx,3D8h
        mov al,8
        out dx,al
        xor bx,bx
tv_40:  mov al,bl               ; forty cells a row here
        add al,4
        mov ah,40
        mul ah
        add ax,4
        shl ax,1
        mov di,ax
        mov ax,VSEG
        mov es,ax
        mov ah,bl
        or ah,ah
        jnz tv_40c
        mov ah,7
tv_40c: mov si,m_v3
        call puts
        inc bl
        cmp bl,16
        jb tv_40
        call tv_key
        ; 320x200, four colours, the two palettes and the intense one
        mov ax,4
        int 10h
        mov ax,VSEG
        mov es,ax
        xor di,di
        mov dx,2                ; the two halves of the store
tv_g4:  mov bx,100              ; rows of a half
tv_g4r: mov al,0
        mov cx,20
        rep stosb
        mov al,55h
        mov cx,20
        rep stosb
        mov al,0AAh
        mov cx,20
        rep stosb
        mov al,0FFh
        mov cx,20
        rep stosb
        dec bx
        jnz tv_g4r
        mov di,2000h
        dec dx
        jnz tv_g4
        mov si,m_v4
        call tv_tty
        xor al,al
        call tv_pal
        mov al,20h
        call tv_pal
        mov al,30h
        call tv_pal
        ; 640x200: fine lines, a chequer and a frame
        mov ax,6
        int 10h
        mov ax,VSEG
        mov es,ax
        xor di,di
        mov dx,2
tv_g6:  mov bx,100
tv_g6r: mov al,0AAh
        cmp bx,50
        ja tv_g6f
        mov al,0CCh
        test bl,4
        jz tv_g6f
        mov al,33h
tv_g6f: mov cx,80
        rep stosb
        dec bx
        jnz tv_g6r
        mov di,2000h
        dec dx
        jnz tv_g6
        mov si,m_v5
        call tv_tty
        call tv_key
        ; a pattern for setting the monitor: frame, grid and centre
        call text_mode
        mov dh,0
tv_p:   mov ah,NORMAL
        call fill_row
        xor dl,dl
tv_pc:  mov al,0C5h
        mov ah,13h
        test dh,3
        jz tv_pl
        test dl,7
        jz tv_pv
        jmp tv_pn
tv_pl:  test dl,7
        jz tv_pp
        mov al,0C4h
        jmp tv_pp
tv_pv:  mov al,0B3h
tv_pp:  call at
        stosw
tv_pn:  inc dl
        cmp dl,80
        jb tv_pc
        inc dh
        cmp dh,25
        jb tv_p
        mov dh,0
tv_fr:  xor dl,dl
        call at
        mov ax,1FDBh
        stosw
        mov dl,79
        call at
        stosw
        inc dh
        cmp dh,25
        jb tv_fr
        xor dx,dx
        call at
        mov cx,80
        rep stosw
        mov dx,1800h
        call at
        mov cx,80
        rep stosw
        mov dx,0C1Ch
        mov si,m_v6
        mov ah,TABAT
        call put_at
        call tv_key
        ret
; a key; Esc leaves the test from wherever it is
tv_key: call getkey
        cmp al,27
        jne tk_ret
        pop ax
tk_ret: ret
; AL = the colour register's value: set, and a key waited for
tv_pal: mov dx,3D9h
        out dx,al
        jmp tv_key
; SI = text, written by the BIOS at the top of a graphics screen
tv_tty: mov ah,2
        xor bx,bx
        xor dx,dx
        int 10h
tt_one: lodsb
        or al,al
        jz tt_end
        mov ah,0Eh
        mov bx,3
        int 10h
        jmp tt_one
tt_end: ret

; ---------------------------------------------------------------------------------------------
; the system: switches, memory, coprocessor, the BIOS's date, the timer, the speaker

t_system:
        mov si,m_stitle
        mov bx,m_sbar
        call t_screen
        ; the switches: port B bit 3 (61H) chooses the half, port C (62H) gives it; a 1 is an open switch
        in al,61h
        mov bh,al
        and al,0F7h
        out 61h,al
        in al,62h
        and al,0Fh
        mov bl,al
        mov al,bh
        or al,8
        out 61h,al
        in al,62h
        mov cl,4
        shl al,cl
        or bl,al
        mov al,bh
        out 61h,al
        mov [fsw],bl
        mov dx,0203h
        mov si,m_s1
        call say
        mov dx,0406h
        call at
        mov ch,1
        mov cl,'1'
sy_sw:  mov ax,1F53h            ; "SW-n" above each switch
        stosw
        mov al,'W'
        stosw
        mov al,'-'
        stosw
        mov al,cl
        stosw
        mov ax,1720h
        stosw
        stosw
        stosw
        inc cl
        shl ch,1
        jnc sy_sw
        mov dx,0506h
        call at
        mov ch,1
sy_st:  mov si,m_on
        mov ah,2Fh
        test [fsw],ch
        jz sy_one
        mov si,m_off
        mov ah,4Fh
sy_one: call puts
        mov ax,1720h
        stosw
        stosw
        stosw
        shl ch,1
        jnc sy_st
        mov dx,0703h
        mov si,m_sfpu
        call say
        mov si,m_no
        test byte [fsw],2
        jz sy_f
        mov si,m_yes
sy_f:   mov ah,1Bh
        call puts
        mov dx,0803h
        mov si,m_sram
        call say
        mov bl,[fsw]
        shr bl,1
        shr bl,1
        and bx,3
        shl bx,1
        mov si,[bx+ramtab]
        mov ah,1Bh
        call puts
        mov dx,0903h
        mov si,m_sdsp
        call say
        mov bl,[fsw]
        mov cl,4
        shr bl,cl
        and bx,3
        shl bx,1
        mov si,[bx+dsptab]
        mov ah,1Bh
        call puts
        mov dx,0A03h
        mov si,m_sdrv
        call say
        mov al,[fsw]
        mov cl,6
        shr al,cl
        add al,'1'
        mov ah,1Bh
        stosw
        ; what the BIOS and the machine say
        mov dx,0C03h
        mov si,m_smem
        call say
        int 12h
        mov [memk],ax
        mov bh,1Bh
        call pdec
        mov si,m_k
        mov ah,1Bh
        call puts
        mov dx,0D03h
        mov si,m_s87
        call say
        mov word [fpsw],0FFFFh
        db 0DBh, 0E3h           ; FNINIT
        db 0DDh, 3Eh            ; FNSTSW [fpsw]
        dw fpsw
        mov si,m_no
        cmp byte [fpsw],0
        jne sy_87
        mov si,m_yes
sy_87:  mov ah,1Bh
        call puts
        mov dx,0E03h
        mov si,m_stick
        call say
        mov [tickat],di
sy_key: ; the timer's count runs while a key is waited for
        mov ah,1
        int 16h
        jnz sy_got
        xor ah,ah
        int 1Ah
        cmp dx,[ticks]
        je sy_key
        mov [ticks],dx
        mov di,[tickat]
        push cs
        pop es
        mov ax,dx
        mov bh,1Bh
        call pdec
        mov ax,1B20h
        stosw
        call flush
        jmp sy_key
sy_got: call getkey
        cmp al,27
        jne sy_1
        ret
sy_1:   and al,0DFh
        cmp al,'M'
        jne sy_2
        call mem_test
        jmp sy_key
sy_2:   cmp al,'S'
        jne sy_key
        mov bx,2280             ; three tones: 523, 659 and 784 Hz
        call tone
        mov bx,1810
        call tone
        mov bx,1522
        call tone
        jmp sy_key

; BX = the timer's divider: the speaker sounds for a quarter of a second
tone:   mov al,0B6h
        out 43h,al
        mov al,bl
        out 42h,al
        mov al,bh
        out 42h,al
        in al,61h
        or al,3
        out 61h,al
        xor ah,ah
        int 1Ah
        add dx,5
        mov bx,dx
tn_w:   xor ah,ah
        int 1Ah
        cmp dx,bx
        jb tn_w
        in al,61h
        and al,0FCh
        out 61h,al
        ret

; the memory above this program, 4 K at a time: two patterns written and read back. The topic and picture
; stores are part of it (the application reads the topic again afterwards).
mem_test:
        mov dx,1003h
        mov si,m_mt
        call say
        mov [tickat+2],di
        mov word [merr],0
        mov bx,DOCSEG
mt_blk: mov ax,[memk]
        mov cl,6
        shl ax,cl               ; the first segment past the memory
        cmp bx,ax
        jae mt_end
        mov es,bx
        mov ax,55AAh
        call mt_pat
        mov ax,0AA55h
        call mt_pat
        add bx,100h
        ; how far, in K
        push bx
        mov ax,bx
        mov cl,6
        shr ax,cl
        mov di,[tickat+2]
        push cs
        pop es
        mov bh,1Bh
        call pdec
        mov si,m_k
        mov ah,1Bh
        call puts
        call flush
        pop bx
        jmp mt_blk
mt_end: mov dx,1103h
        mov si,m_mok
        cmp word [merr],0
        je mt_say
        mov si,m_mbad
mt_say: mov ah,2Fh
        cmp word [merr],0
        je mt_put
        mov ah,4Fh
mt_put: jmp put_at
; AX = pattern, ES = block: written to 4 K and read back
mt_pat: xor di,di
        mov cx,2048
        rep stosw
        xor di,di
        mov cx,2048
        repe scasw
        je mp_ok
        inc word [merr]
mp_ok:  ret

; ---------------------------------------------------------------------------------------------
; the diskette and the ports

t_disk: mov si,m_dtitle
        mov bx,m_dbar
        call t_screen
        mov dx,0203h
        mov si,m_d1
        call say
        mov dl,[drive]
        call verify
        ; the printer and the serial ports, as the BIOS reports them
        mov dx,0C03h
        mov si,m_d3
        call say
        mov ah,2
        xor dx,dx
        int 17h
        mov al,ah
        mov [pstat],al
        mov bh,1Bh
        call phex
        mov ax,1B48h
        stosw
        mov dx,0D06h
        mov si,m_p1
        test byte [pstat],10h
        jnz td_p1
        mov si,m_p0
td_p1:  call say
        mov dx,0E06h
        mov si,m_p3
        test byte [pstat],20h
        jnz td_p2
        mov si,m_p2
td_p2:  call say
        mov dx,0F06h
        mov si,m_p5
        test byte [pstat],8
        jnz td_p3
        mov si,m_p4
td_p3:  call say
        mov dx,1103h
        mov si,m_d4
        call say
        mov ah,3
        xor dx,dx
        int 14h
        push ax
        mov al,ah
        mov bh,1Bh
        call phex
        mov ax,1B48h
        stosw
        mov ax,1720h
        stosw
        stosw
        pop ax
        call phex
        mov ax,1B48h
        stosw
        mov dx,1303h
        mov si,m_d5
        call say
        mov dx,201h
        in al,dx
        mov bh,1Bh
        call phex
        mov ax,1B48h
        stosw
td_key: call getkey
        cmp al,27
        jne td_1
        ret
td_1:   and al,0DFh
        cmp al,'A'
        jne td_2
        xor dl,dl
        call verify
        jmp td_key
td_2:   cmp al,'B'
        jne td_key
        mov dl,1
        call verify
        jmp td_key

; DL = drive: every track of both sides is read (INT 13H, AH=2), nine sectors at a time
verify: mov [vdrive],dl
        mov dx,0403h
        mov si,m_d2
        call say
        mov al,[vdrive]
        add al,'A'
        mov ah,1Bh
        stosw
        mov dh,6
vf_cls: mov ah,NORMAL
        push dx
        call fill_row
        pop dx
        inc dh
        cmp dh,10
        jb vf_cls
        mov dx,0603h
        mov si,m_side0
        call say
        mov dx,0703h
        mov si,m_side1
        call say
        mov word [verr],0
        xor ch,ch               ; the track
vf_trk: xor dh,dh               ; the side
vf_side:
        push cx
        push dx
        mov byte [vtry],2
vf_try: mov ax,PICSEG           ; read into the picture's store
        mov es,ax
        xor bx,bx
        mov ax,0209h
        mov cl,1
        mov dl,[vdrive]
        int 13h
        jnc vf_ok
        xor ah,ah
        mov dl,[vdrive]
        int 13h
        pop dx
        pop cx
        push cx
        push dx
        dec byte [vtry]
        jnz vf_try
        inc word [verr]
        mov ax,1CDBh            ; a red block: the track cannot be read
        jmp vf_mark
vf_ok:  mov ax,1ADBh
vf_mark:
        pop dx
        pop cx
        push dx
        push ax
        add dh,6
        mov dl,ch
        add dl,12
        call at
        pop ax
        stosw
        call flush
        pop dx
        ; a key stops the test
        push cx
        push dx
        mov ah,1
        int 16h
        pop dx
        pop cx
        jnz vf_end              ; stopped: nothing is said about the diskette
        inc dh
        cmp dh,2
        jb vf_side
        inc ch
        cmp ch,40
        jb vf_trk
        mov dx,0903h
        mov si,m_dok
        mov ah,2Fh
        cmp word [verr],0
        je vf_say
        mov si,m_dbad
        mov ah,4Fh
vf_say: call put_at
        cmp word [verr],0
        je vf_end
        mov ax,[verr]
        mov bh,4Fh
        call pdec
vf_end: ret

; ---------------------------------------------------------------------------------------------
m_tname:  db 'JUNIOR-XT  Teste  ', 0
m_ktitle: db ' Tastatura ', 0
m_kbar:   db 'Se apasa pe rind fiecare tasta.   Esc de doua ori: inapoi', 0
m_k1:     db 'Tasta apasata se aprinde; tasta eliberata ramine marcata.', 0
m_k2:     db 'Cod de scanare: ', 0
m_k3:     db 'H  (apasare)   ', 0
m_k4:     db 'H  (eliberare) ', 0
m_vtitle: db ' Afisaj ', 0
m_vbar:   db 'O tasta: imaginea urmatoare.   Esc: inapoi', 0
m_v1:     db 'Nuantele de culoare: caracter, fond si denumirea din cartea tehnica', 0
m_v2:     db 'Setul de caractere al generatorului de caractere (00H - FFH)', 0
m_v3:     db 'Regim alfanumeric 40x25', 0
m_v4:     db '320x200, 4 culori: paletele', 0
m_v5:     db '640x200, 2 culori', 0
m_v6:     db ' Model pentru reglarea monitorului ', 0
m_stitle: db ' Sistem ', 0
m_sbar:   db 'M: verificarea memoriei   S: difuzor   Esc: inapoi', 0
m_s1:     db 'Microintrerupatoarele de configurare, citite acum (port-urile 61H si 62H):', 0
m_on:     db ' ON ', 0
m_off:    db ' OFF', 0
m_sfpu:   db 'Coprocesor matematic I8087 (SW-2):     ', 0
m_no:     db 'neinstalat', 0
m_yes:    db 'instalat', 0
m_sram:   db 'Memoria RAM de pe placa (SW-3, SW-4):  ', 0
m_sdsp:   db 'Tipul cuplorului de afisaj (SW-5, 6):  ', 0
m_sdrv:   db 'Unitati de disc flexibil (SW-7, 8):    ', 0
m_smem:   db 'Memoria gasita de BIOS (INT 12H):      ', 0
m_k:      db ' K  ', 0
m_s87:    db 'Coprocesorul raspunde:                 ', 0
m_stick:  db 'Tactul de timp (INT 1AH):              ', 0
m_mt:     db 'Verificarea memoriei, pina la:         ', 0
m_mok:    db ' Memoria verificata nu are erori. ', 0
m_mbad:   db ' Erori la verificarea memoriei! ', 0
r_0:      db '64 K', 0
r_1:      db '128 K', 0
r_2:      db '192 K', 0
r_3:      db '256 K', 0
ramtab:   dw r_0, r_1, r_2, r_3
d_0:      db 'cuplor de afisaj neinstalat', 0
d_1:      db 'cuplor CGA (40x25)', 0
d_2:      db 'cuplor CGA (80x25)', 0
d_3:      db 'cuplor MDA sau ambele', 0
dsptab:   dw d_0, d_1, d_2, d_3
c_0:      db 'negru', 0
c_1:      db 'albastru', 0
c_2:      db 'verde', 0
c_3:      db 'cyan', 0
c_4:      db 'rosu', 0
c_5:      db 'magenta', 0
c_6:      db 'maron', 0
c_7:      db 'alb', 0
c_8:      db 'gri', 0
c_9:      db 'albastru intens', 0
c_10:     db 'verde intens', 0
c_11:     db 'cyan intens', 0
c_12:     db 'rosu intens', 0
c_13:     db 'magenta intens', 0
c_14:     db 'galben intens', 0
c_15:     db 'alb intens', 0
coltab:   dw c_0, c_1, c_2, c_3, c_4, c_5, c_6, c_7, c_8, c_9, c_10, c_11, c_12, c_13, c_14, c_15
m_dtitle: db ' Discheta si port-uri ', 0
m_dbar:   db 'A, B: verificarea dischetei din unitate   Esc: inapoi', 0
m_d1:     db 'Fiecare pista a dischetei este citita, pe ambele fete (INT 13H, functia 2).', 0
m_d2:     db 'Unitatea ', 0
m_side0:  db 'Fata 0   ', 0
m_side1:  db 'Fata 1   ', 0
m_dok:    db ' Discheta a fost citita fara erori. ', 0
m_dbad:   db ' Piste cu erori: ', 0
m_d3:     db 'Imprimanta paralela (INT 17H), starea: ', 0
m_p0:     db 'neselectata', 0
m_p1:     db 'selectata  ', 0
m_p2:     db 'hirtie prezenta', 0
m_p3:     db 'lipsa hirtie   ', 0
m_p4:     db 'fara eroare', 0
m_p5:     db 'eroare     ', 0
m_d4:     db 'Comunicatia seriala (INT 14H), starea liniei si a modemului: ', 0
m_d5:     db 'Cuplorul pentru joystick (port 201H): ', 0

kq:       times 16 db 0
kq_head:  db 0
kq_tail:  db 0
kold:     dw 0, 0
kcode:    db 0
kesc:     db 0
fsw:      db 0
fpsw:     dw 0
memk:     dw 0
merr:     dw 0
tickat:   dw 0, 0
ticks:    dw 0
pstat:    db 0
vdrive:   db 0
vtry:     db 0
verr:     dw 0
