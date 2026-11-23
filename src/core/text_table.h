/* The string table (text.h): one row for each text the window shows, with
 * its English and its Romanian wording. X(ID, english, romanian); the id is
 * MX_T_<ID>. The Romanian is kept in UTF-8 with its diacritics and folded
 * when the text is used (text.c): the Junior's character ROM holds only the
 * accented a and i of code page 437, so the letters are drawn without
 * diacritics, as the Junior's own manual does.
 *
 * The Romanian follows the manual of the Junior (Carte tehnica, 1989): its
 * words, its impersonal
 * "se" sentences without any address to the reader, noun phrases for
 * labels, option values and keys' functions, the decimal comma, "Kocteti",
 * and its 1953 orthography (i with a circumflex inside a word: sint, pina).
 *
 * A printf format keeps the same conversions in the same order in both
 * languages (checked by test_text): a sentence that needs another word
 * order is worded so that it still takes its values in the same order.
 *
 * MezinXT's own. */

#define EN_RESTART "Applies when you leave setup; the machine restarts."
#define RO_RESTART "Se aplică la ieșire; microcalculatorul se reinițializează."
#define EN_NOW "Applies at once."
#define RO_NOW "Se aplică imediat."
#define EN_LEAVE "Applies when you leave setup."
#define RO_LEAVE "Se aplică la ieșire."

/* --- the setup screen: pages and bars -------------------------------------------------- */
X(PAGE_DRIVES, "Drives", "Unități")
X(PAGE_MACHINE, "Machine", "Microcalculator")
X(PAGE_DISPLAY, "Display", "Ecran")
X(PAGE_SOUND, "Sound", "Sunet")
X(PAGE_PORTS, "Ports", "Interfețe")
X(PAGE_CONTROLLERS, "Controllers", "Joystick")
X(SETUP_TITLE, "MezinXT %s - Setup", "MezinXT %s - Configurare")

X(KEY_LEFT_RIGHT, "Left/Right", "Stg/Dr")
X(KEY_UP_DOWN, "Up/Down", "Sus/Jos")
X(KEY_PAGE_UPDOWN, "PgUp/PgDn", "PgUp/PgDn")
X(KEY_ENTER, "Enter", "Enter")
X(KEY_ESC, "Esc", "Esc")
X(KEY_ANY, "Any key", "Orice tastă")
X(KEY_BACKSPACE, "Backspace", "Backspace")
X(ACT_PAGE, "Page", "Pagină")
X(ACT_SELECT, "Select", "Selecție")
X(ACT_CHANGE, "Change", "Valoare")
X(ACT_CHOOSE, "Choose", "Alegere")
X(ACT_DONE, "Exit", "Ieșire")
X(ACT_BACK, "Back", "Înapoi")
X(ACT_ACCEPT, "Accept", "Acceptare")
X(ACT_CANCEL, "Cancel", "Anulare")
X(ACT_OPEN, "Open", "Deschidere")
X(ACT_UP_FOLDER, "Up a folder", "Nivel superior")
X(ACT_NEXT, "Next", "Continuare")
X(ACT_ASSIGN, "Assign", "Atribuire")

/* --- the Drives page -------------------------------------------------------------------- */
X(DRIVE_HEADING, "Drive %c:", "Unitate %c:")
X(DRIVE_DISK, "Diskette", "Dischetă")
X(DRIVE_EMPTY, "(empty)", "(fără dischetă)")
X(DRIVE_SIZE, "Capacity", "Capacitate")
X(DRIVE_SIZE_KIB, "%u KiB", "%u Kocteți")
X(DRIVE_FILE_NOT_FOUND, "file not found", "fișier negăsit")
X(DRIVE_INSERT, "Insert image...", "Introducere imagine...")
X(DRIVE_EJECT, "Eject diskette", "Scoatere dischetă")
X(DRIVE_PROTECT, "Write protect", "Protecție la scriere")
X(DRIVE_NEW_BLANK, "New blank diskette...", "Dischetă nouă goală...")
X(HINT_DRIVE_INSERT, "Chooses a diskette image file to put in the drive.",
  "Se alege fișierul imagine de dischetă care se introduce în unitate.")
X(HINT_DRIVE_EJECT, "Takes the diskette out of the drive.", "Discheta se scoate din unitate.")
X(HINT_DRIVE_EMPTY, "The drive is empty.", "Unitatea este fără dischetă.")
X(HINT_DRIVE_PROTECT, "Programs cannot write to a write-protected diskette. " EN_NOW,
  "Programele nu pot scrie pe o dischetă protejată la scriere. " RO_NOW)
X(L_FAST_ACCESS, "Fast access", "Acces rapid")
X(HINT_FAST_ACCESS, "Not original: the machine runs at full speed while the drive works, so loading is quick. " EN_LEAVE,
  "Nu există la original: microcalculatorul lucrează cu frecvență maximă cît timp unitatea lucrează, deci încărcarea este rapidă. " RO_LEAVE)
X(HINT_DRIVE_NEW_BLANK, "Makes an empty diskette image file of 160 to 360 KiB, or 720 KiB for an 80-track drive.",
  "Se creează un fișier imagine de dischetă goală de la 160 la 360 Kocteți, sau de 720 Kocteți pentru o unitate cu 80 de piste.")
X(L_TRACKS, "Tracks", "Piste")
X(V_TRACKS_40, "40 tracks", "40 piste")
X(V_TRACKS_80, "80 tracks (reads 720 KiB diskettes)", "80 piste (citește dischete de 720 Kocteți)")
X(HINT_TRACKS_EJECT, "Eject the diskette to change the drive.", "Pentru schimbarea unității se scoate discheta.")
X(HINT_TRACKS, "An 80-track drive reads 720 KiB diskettes. " EN_RESTART, "O unitate cu 80 de piste citește dischete de 720 Kocteți. " RO_RESTART)
X(BLANK_160, "160 KiB  single sided, 8 sectors", "160 Kocteți  simplă față, 8 sectoare")
X(BLANK_180, "180 KiB  single sided, 9 sectors", "180 Kocteți  simplă față, 9 sectoare")
X(BLANK_320, "320 KiB  double sided, 8 sectors", "320 Kocteți  dublă față, 8 sectoare")
X(BLANK_360, "360 KiB  double sided, 9 sectors", "360 Kocteți  dublă față, 9 sectoare")
X(HARD_HEADING, "Fixed disk %u (%c:)", "Disc fix %u (%c:)")
X(HARD_IMAGE, "Image", "Imagine")
X(HARD_NONE, "(none)", "(fără imagine)")
X(HARD_SIZE_VALUE, "%u.%u MB (%u x %u x 17)", "%u,%u MB (%u x %u x 17)")
X(HARD_NOT_IMAGE, "not a fixed disk image", "nu este imagine de disc fix")
X(HARD_ATTACH, "Attach image...", "Conectare imagine...")
X(HARD_DETACH, "Detach", "Deconectare")
X(HARD_NEW_BLANK, "New blank fixed disk...", "Disc fix nou gol...")
X(HINT_HARD_ATTACH, "Chooses a fixed disk image: 17 sectors a track, 20 to 70 MB. " EN_RESTART,
  "Se alege o imagine de disc fix: 17 sectoare pe pistă, de la 20 la 70 MB. " RO_RESTART)
X(HINT_HARD_DETACH, "Takes the image off the coupler. " EN_RESTART, "Imaginea se deconectează de la cuplor. " RO_RESTART)
X(HINT_HARD_EMPTY, "No image is attached.", "Nu este conectată nici o imagine.")
X(HINT_HARD_NEW_BLANK, "Makes an empty fixed disk image of 20 to 70 MB; FDISK and FORMAT prepare it.",
  "Se creează o imagine goală de disc fix de la 20 la 70 MB; FDISK și FORMAT o pregătesc.")
X(HINT_HARD_NO_COUPLER, "The Winchester coupler is not fitted: it is fitted on the Machine page.",
  "Cuplorul Winchester este neinstalat: se instalează în pagina Microcalculator.")
X(HINT_HARD_NEEDS_FIRST, "Fixed disk 1 needs fixed disk 0.", "Discul fix 1 necesită discul fix 0.")
X(HINT_HARD_SECOND_FIRST, "Fixed disk 1 is detached first.", "Mai întîi se deconectează discul fix 1.")
X(BLANK_HARD, "%u MB  %u cylinders, %u heads", "%u MB  %u cilindri, %u capete")
X(BLANK_720, "720 KiB  double sided, 9 sectors, 80 tracks", "720 Kocteți  dublă față, 9 sectoare, 80 piste")

/* --- the Machine page ------------------------------------------------------------------- */
X(HEAD_BOARD, "Main board", "Placă logică de bază")
X(HEAD_MACHINE, "Machine", "Microcalculator")
X(L_BIOS, "BIOS", "BIOS")
X(L_CPU, "Processor", "Microprocesor")
X(L_SPEED, "Speed", "Frecvență")
X(L_RAM, "RAM capacity", "Capacitate memorie RAM")
X(L_DISPLAY, "Display coupler", "Cuplor de afișaj")
X(L_FPU, "8087 math coprocessor", "Coprocesor matematic 8087")
X(L_RESET, "System reset...", "Inițializare sistem...")
X(L_POWER, "Power off and on...", "Oprire și pornire...")
X(HINT_BIOS, "Version 4.0 uses the Junior's own serial adapter; 3.0 and 5.0 need 8250 ports. " EN_RESTART,
  "Versiunea 4.0 folosește cuplorul serial al Junior-ului; 3.0 și 5.0 necesită interfețe 8250. " RO_RESTART)
X(HINT_CPU, "The 8086 has a 16-bit bus, the 8088 an 8-bit one. " EN_RESTART,
  "Microprocesorul I8086 are magistrala de 16 biți, I8088 de 8 biți. " RO_RESTART)
X(HINT_SPEED, "7.16 and 9.54 MHz are not original; some programs run too fast. " EN_RESTART,
  "Frecvențele de 7,16 și 9,54 MHz nu sînt cele originale; unele programe rulează prea repede. " RO_RESTART)
X(HINT_RAM, "A 256 KiB board reaches 640 KiB with the 384 KiB expansion module. " EN_RESTART,
  "Placa de 256 Kocteți ajunge la 640 Kocteți cu modulul de extensie de 384 Kocteți. " RO_RESTART)
X(HINT_DISPLAY, "Sets the display coupler that the DIP switches announce to the BIOS. " EN_RESTART,
  "Se stabilește cuplorul de afișaj anunțat BIOS-ului prin microîntrerupătoare. " RO_RESTART)
X(HINT_FPU, "Fits the 8087 math coprocessor and sets its DIP switch. " EN_RESTART,
  "Coprocesorul matematic I8087 se instalează; se stabilește microîntrerupătorul. " RO_RESTART)
X(HINT_RESET, "Pulls the RESET line; memory is kept.", "Se activează linia de inițializare (RESET); memoria se păstrează.")
X(HINT_POWER, "Clears memory and starts again, as switching the machine on.",
  "Memoria se șterge și microcalculatorul este repus în funcțiune, ca la punerea sub tensiune.")
X(L_EXIT, "Exit MezinXT", "Ieșire din MezinXT")
X(HINT_EXIT, "Closes MezinXT at once, writing the images back and saving the settings; Shift+F12 does the same.",
  "MezinXT se închide imediat, imaginile se scriu înapoi și opțiunile se salvează; la fel acționează Shift+F12.")
X(V_SPEED_477, "4.77 MHz (original)", "4,77 MHz (originală)")
X(V_SPEED_716, "7.16 MHz", "7,16 MHz")
X(V_SPEED_954, "9.54 MHz", "9,54 MHz")
X(V_SPEED_FREE, "Unthrottled", "Nelimitată")
X(V_RAM_640, "640 KiB (with the 384 KiB module)", "640 Kocteți (cu modulul de 384 Kocteți)")
X(V_RAM_256, "256 KiB", "256 Kocteți")
X(V_RAM_192, "192 KiB", "192 Kocteți")
X(V_RAM_128, "128 KiB", "128 Kocteți")
X(V_RAM_64, "64 KiB", "64 Kocteți")
X(V_DISPLAY_80, "CGA coupler (80x25)", "Cuplor CGA (80x25)")
X(V_DISPLAY_40, "CGA coupler (40x25)", "Cuplor CGA (40x25)")
X(V_DISPLAY_MDA, "Monochrome MDA coupler (80x25)", "Cuplor MDA monocrom (80x25)")
X(V_NO_DISPLAY, "Not fitted", "Neinstalat")
X(HEAD_OPTIONS, "Options", "Opțiuni")
X(L_FLOPPIES, "Diskette drives", "Unități de dischetă")
X(L_WINCHESTER, "Winchester coupler", "Cuplor Winchester")
X(HINT_WINCHESTER, "Fits the fixed disk coupler (320h, IRQ 5, DMA 3, its BIOS at C8000h). " EN_RESTART,
  "Se instalează cuplorul de disc fix (320h, IRQ 5, DMA 3, BIOS-ul său la C8000h). " RO_RESTART)
X(HINT_WINCHESTER_ROM, "Its BIOS takes C8000h, where a ROM image is set: that socket is emptied first.",
  "BIOS-ul său ocupă adresa C8000h, unde este o imagine ROM: mai întîi se golește acel soclu.")
X(HINT_ROM_WINCHESTER, "The Winchester coupler's BIOS is at C8000h.", "La C8000h se află BIOS-ul cuplorului Winchester.")
X(V_FLOPPIES_1, "1 drive", "1 unitate")
X(V_FLOPPIES_2, "2 drives", "2 unități")
X(V_FLOPPIES_3, "3 drives", "3 unități")
X(V_FLOPPIES_4, "4 drives", "4 unități")
X(HINT_FLOPPIES, "Sets the number of diskette drives, 1 to 4 (DIP switches SW7 and SW8). " EN_RESTART,
  "Se stabilește numărul de unități de dischetă, de la 1 la 4 (SW7 și SW8). " RO_RESTART)
X(L_ROM, "ROM at %05Xh", "ROM la %05Xh")
X(HINT_ROM, "Chooses an EPROM image for the module's socket, up to %s. " EN_RESTART,
  "Se alege o imagine EPROM pentru soclul modulului, pînă la %s. " RO_RESTART)
X(ROM_LIMIT_16, "16 KiB (32 would cover the BIOS)", "16 Kocteți (32 ar acoperi BIOS)")
X(ROM_LIMIT_32, "32 KiB", "32 Kocteți")
X(HINT_ROM_NO_MODULE, "The EPROM sockets are on the 384 KiB module: set RAM capacity to 640 KiB.",
  "Soclurile EPROM sînt pe modulul de 384 Kocteți: capacitatea memoriei RAM se stabilește la 640 Kocteți.")
X(V_ROM_NONE, "(none)", "(fără imagine)")
X(M_ROM_NONE, "No image", "Fără imagine")
X(M_ROM_CHOOSE, "Choose image file...", "Alegere fișier imagine...")
X(PICK_ROM_TITLE, "ROM image for %05Xh", "Imagine ROM pentru %05Xh")

/* --- the Display page ------------------------------------------------------------------- */
X(L_LOOK, "Look", "Aspect")
X(L_MONITOR, "Monitor", "Monitor")
X(L_ASPECT, "Screen shape", "Formă ecran")
X(L_SCALE, "Scaling", "Scalare")
X(L_FILTER, "Filter", "Filtru")
X(L_FULLSCREEN, "Full screen", "Ecran complet")
X(L_VSYNC, "Vertical sync", "Sincronizare verticală")
X(L_SNOW, "CGA snow", "Paraziți CGA")
X(L_DRIVE_LIGHTS, "Drive lights", "Indicatoare unități")
X(HEAD_INTERFACE, "User interface", "Interfață utilizator")
X(L_LANGUAGE, "Language/Limbă", "Limbă/Language")
X(HINT_LOOK, "Shows the picture on a CRT monitor, with scanlines and glow, or as plain pixels. " EN_NOW,
  "Imaginea se afișează ca pe un monitor CRT, cu linii de explorare și strălucire, sau cu puncte simple. " RO_NOW)
X(HINT_MONITOR, "Chooses colour with intensity steps (9-pin), colour without (RCA jacks) or green phosphor monochrome (composite). " EN_NOW,
  "Se alege color cu trepte de intensitate (9 contacte), color fără trepte (mufe RCA) sau monocrom cu fosfor verde (complex). " RO_NOW)
X(HINT_MONITOR_MDA, "The monochrome coupler drives the monochrome monitor, in green phosphor; the setting returns with a CGA.",
  "Cuplorul monocrom comandă monitorul monocrom, cu fosfor verde; opțiunea se aplică din nou la cuplorul CGA.")
X(HINT_ASPECT, "Shows the picture in the shape 4:3 as the monitor did (640 x 200 pixels), or with square pixels. " EN_NOW,
  "Imaginea se afișează în raportul 4:3, ca pe monitor (640 x 200 puncte), sau cu puncte pătrate. " RO_NOW)
X(HINT_SCALE, "The picture fills the window, or uses whole multiples only. " EN_NOW,
  "Imaginea umple fereastra sau folosește numai multipli întregi. " RO_NOW)
X(HINT_FILTER, "Sets how the pixels are smoothed in the window. " EN_NOW,
  "Se stabilește modul în care se netezesc punctele în fereastră. " RO_NOW)
X(HINT_FILTER_CRT, "The CRT look smooths the picture itself, so the filter applies to plain pixels.",
  "Aspectul CRT netezește singur imaginea; filtrul se aplică la puncte simple.")
X(HINT_FULLSCREEN, "Alt+Enter switches full screen while the machine runs. " EN_NOW,
  "Alt+Enter comută afișarea pe tot ecranul în timpul funcționării microcalculatorului. " RO_NOW)
X(HINT_VSYNC, "Waits for the screen's refresh. " EN_NOW, "Se așteaptă reîmprospătarea ecranului. " RO_NOW)
X(HINT_SNOW, "The real card shows snow in 80-column text while a program uses the video memory. Not original: turning it off. " EN_NOW,
  "Cuplorul real produce paraziți în text pe 80 de coloane cînd un program folosește memoria de ecran. Nu există la original: oprirea. " RO_NOW)
X(HINT_SNOW_NO_CGA, "Only the CGA coupler shows snow; the setting returns with it.",
  "Numai cuplorul CGA produce paraziți; opțiunea se aplică din nou la acesta.")
X(HINT_DRIVE_LIGHTS, "Not original: shows a light in the picture's bottom row for each drive at work. " EN_NOW,
  "Nu există la original: se afișează un indicator în rîndul de jos al imaginii pentru fiecare unitate în funcțiune. " RO_NOW)
X(HINT_LANGUAGE, "Sets the language of the setup screen and of the messages. " EN_NOW,
  "Se stabilește limba ecranului de configurare și a mesajelor. " RO_NOW)
X(V_LOOK_CRT, "CRT monitor", "Monitor CRT")
X(V_LOOK_PLAIN, "Plain pixels", "Puncte simple")
X(V_MONITOR_RGBI, "Colour with intensity steps", "Color cu trepte de intensitate")
X(V_MONITOR_RGB, "Colour without intensity steps", "Color fără trepte de intensitate")
X(V_MONITOR_COMPOSITE, "Monochrome, composite video", "Monocrom cu semnal video complex")
X(V_MONITOR_MONO, "Monochrome, green phosphor", "Monocrom, fosfor verde")
X(V_ASPECT_ORIGINAL, "4:3, as the monitor", "4:3, ca monitorul")
X(V_ASPECT_SQUARE, "Square pixels", "Puncte pătrate")
X(V_SCALE_FIT, "Fit to window", "Potrivire la fereastră")
X(V_SCALE_INTEGER, "Whole multiples", "Multipli întregi")
X(V_FILTER_NEAREST, "Nearest", "Cel mai apropiat")
X(V_FILTER_LINEAR, "Linear", "Liniar")
X(V_FILTER_SHARP, "Sharp", "Clar")
X(V_FILTER_CRT, "Smoothing by the CRT look", "Netezire prin aspectul CRT")
X(V_LANGUAGE_EN, "English", "English")
X(V_LANGUAGE_RO, "Română", "Română")

/* --- the Sound page --------------------------------------------------------------------- */
X(L_SOUND, "Sound", "Sunet")
X(L_VOLUME, "Volume", "Volum")
X(HEAD_OUTPUT, "Output", "Ieșire")
X(L_SOURCE, "Source", "Sursă")
X(L_FORMAT, "Format", "Format")
X(V_SPEAKER, "PC speaker (timer 2, port B)", "Difuzor (generator de tonuri 2, port B)")
X(V_AUDIO_FORMAT, "48 kHz, 16 bit, stereo", "48 kHz, 16 biți, stereo")
X(L_SOURCE_DRIVES, "Drives", "Unități")
X(V_DRIVES_SOURCE, "Diskette, fixed disk (synthesised, not recorded)", "Dischetă, disc fix (imitat, nu înregistrat)")
X(HINT_SOUND, "When off, the speaker and the drives make no sound. " EN_LEAVE,
  "Cînd este oprit, difuzorul și unitățile nu produc sunet. " RO_LEAVE)
X(HINT_VOLUME, "Sets the overall loudness of the speaker and the drives. " EN_LEAVE,
  "Se stabilește intensitatea generală a sunetului difuzorului și a unităților. " RO_LEAVE)
X(HINT_SOUND_OFF, "Sound is off.", "Sunetul este oprit.")
X(L_DRIVE_SOUND, "Drive sound", "Sunet unități")
X(L_DRIVE_VOLUME, "Drive volume", "Volum unități")
X(HINT_DRIVE_SOUND, "Plays made-up drive sounds: diskette motors, steps and heads, fixed disk spindle and seeks; nothing is recorded. " EN_LEAVE,
  "Se redau sunete imitate: motoare, pași și capete la dischetă, rotație și deplasări la disc fix; nimic nu este înregistrat. " RO_LEAVE)
X(HINT_DRIVE_VOLUME, "Sets the loudness of the drives against the speaker's. " EN_LEAVE,
  "Se stabilește intensitatea sunetului unităților față de cea a difuzorului. " RO_LEAVE)
X(HINT_DRIVE_SOUND_OFF, "The drives make no sound.", "Unitățile nu produc sunet.")

/* --- the Ports page --------------------------------------------------------------------- */
X(L_SIO_A, "Serial channel A", "Canal serial A")
X(L_SIO_B, "Serial channel B", "Canal serial B")
X(L_LPT, "Parallel port", "Interfață paralelă")
X(L_LPT_MDA, "MDA parallel port", "Interfață paralelă MDA")
X(HEAD_MDA, "Monochrome display coupler (MDA)", "Cuplor pentru afișaj monocrom (MDA)")
X(HINT_LPT_MDA, "The monochrome coupler's parallel port is at 3BCh: the output is written to a file; with no file the printer reads off line. " EN_LEAVE,
  "Interfața paralelă a cuplorului monocrom este la 3BCh: ieșirea se scrie într-un fișier; fără fișier imprimanta apare neconectată. " RO_LEAVE)
X(L_TARGET_PORT, "Port", "Port")
X(L_TARGET_PIPE, "Pipe", "Pipe")
X(L_TARGET_CONNECT, "Connect to", "Conectare la")
X(L_TARGET_LISTEN, "Listen on", "Ascultare pe")
X(L_TARGET_FILE, "File", "Fișier")
X(L_TARGET, "Target", "Destinație")
X(HEAD_ADAPTER, "Serial and parallel printer adapter", "Cuplor pentru comunicația serială și imprimanta paralelă")
X(L_FITTED, "Fitted", "Instalat")
X(V_PORT_NONE, "Not connected", "Neconectat")
X(V_PORT_LOOPBACK, "Loopback plug (TxD to RxD)", "Buclă de test (TxD la RxD)")
X(V_PORT_SERIAL, "Host serial port", "Interfață serială gazdă")
X(V_PORT_PIPE, "Local pipe", "Pipe local")
X(V_PORT_TCP, "TCP connection", "Conexiune TCP")
X(V_PORT_TCP_LISTEN, "TCP server", "Server TCP")
X(V_PORT_FILE, "Output file", "Fișier de ieșire")
X(V_NONE_FOUND, "%s - none found", "%s - nu s-a găsit nici una")
X(V_NOT_IN_ROMS, "%s - not in roms/", "%s - lipsește din roms/")
X(L_COM1, "COM1", "COM1")
X(L_COM2, "COM2", "COM2")
X(V_PORT_MOUSE, "Serial mouse (host mouse)", "Mouse serial (mouse gazdă)")
X(V_PORT_ON, "%s - on %s", "%s - la %s")
X(V_PORT_MOUSE_PC, "Mouse - on a PC serial port (Controllers page)", "Mouse - la o interfață serială PC (pagina Joystick)")
X(L_PC_SERIAL_TITLE, "PC serial ports", "Interfețe seriale PC")
X(L_PC_PORTS, "Ports", "Interfețe")
X(HEAD_PC_SERIAL, "PC/XT asynchronous serial adapter", "Cuplor serial asincron PC/XT")
X(V_PC_SERIAL_NONE, "Not fitted", "Neinstalat")
X(V_PC_SERIAL_ONE, "One port (COM1)", "O interfață (COM1)")
X(V_PC_SERIAL_TWO, "Two ports (COM1, COM2)", "Două interfețe (COM1, COM2)")
X(HINT_PC_SERIAL, "Fits 8250 ports for BIOS 3.0, 5.0 and a mouse in place of the Junior's own adapter. " EN_RESTART,
  "Se instalează interfețe 8250 pentru BIOS 3.0, 5.0 și mouse, în locul cuplorului propriu. " RO_RESTART)
X(D_ADAPTER_SWAP, "Serial adapter", "Cuplor serial")
X(D_ADAPTER_SWAP_TEXT, "The PC/XT serial adapter takes IRQ 4, which the Junior's own serial and printer adapter drives: that board is taken out. Fit it again on the Ports page.",
  "Cuplorul serial PC/XT folosește IRQ 4, pe care îl are cuplorul serial și imprimanta Junior-ului: acesta este scos. Se instalează din nou în pagina Interfețe.")
X(HINT_ADAPTER_IRQ, "The SIO adapter drives IRQ 4, which COM1 of the 8250 adapter has: that adapter is set to Not fitted first.",
  "Cuplorul SIO are IRQ 4, pe care îl are și COM1 al cuplorului 8250: acesta se stabilește mai întîi la Neinstalat.")
X(HINT_SIO_PC, "Connects to a host serial port, local pipe, TCP connection or server, loopback plug, file or the host mouse. " EN_LEAVE,
  "Se conectează la o interfață serială gazdă, pipe local, conexiune sau server TCP, buclă de test, fișier sau mouse gazdă. " RO_LEAVE)
X(HINT_MOUSE, "The host mouse is a Microsoft serial mouse: click the picture to capture it, Ctrl+F10 lets it go.",
  "Mouse-ul gazdei este un mouse serial Microsoft: un clic pe ecran îl captează, Ctrl+F10 îl eliberează.")
X(HINT_NO_ADAPTER, "The serial and parallel printer adapter is not fitted.",
  "Cuplorul pentru comunicația serială și imprimanta paralelă este neinstalat.")
X(HINT_LPT, "The printer output is written to a file; with no file the printer reads off line. " EN_LEAVE,
  "Ieșirea imprimantei se scrie într-un fișier; fără fișier imprimanta apare neconectată. " RO_LEAVE)
X(HINT_SIO, "Connects to a host serial port, local pipe, TCP connection or server, loopback plug or file. " EN_LEAVE,
  "Se conectează la o interfață serială gazdă, pipe local, conexiune sau server TCP, buclă de test sau fișier. " RO_LEAVE)
X(HINT_LOOPBACK, "The channel's transmitter is wired to its own receiver.", "Emițătorul canalului este conectat la receptorul propriu.")
X(HINT_NOTHING, "Nothing is connected.", "Nu este conectat nimic.")
X(HINT_PICK_SERIAL, "Enter picks one of the host's serial ports. " EN_LEAVE,
  "Cu Enter se alege una din interfețele seriale ale gazdei. " RO_LEAVE)
X(HINT_EDIT_NAME, "Enter edits the name. " EN_LEAVE, "Cu Enter se modifică numele. " RO_LEAVE)
X(HINT_ADAPTER, "The serial port is at 250h, the printer at 378h; without the adapter the BIOS finds neither. " EN_RESTART,
  "Interfața serială este la 250h, imprimanta la 378h; fără cuplor BIOS-ul nu le găsește. " RO_RESTART)
X(PROMPT_SERIAL, "Host serial port (COM1, /dev/ttyS0)", "Interfață serială gazdă (COM1, /dev/ttyS0)")
X(PROMPT_PIPE, "Local pipe name", "Nume pipe local")
X(PROMPT_TCP, "Host and port to connect to (127.0.0.1:2323)", "Adresă și port de conectare (127.0.0.1:2323)")
X(PROMPT_TCP_LISTEN, "Port to listen on (2323), or HOST:PORT", "Port de ascultare (2323) sau ADRESA:PORT")
X(PROMPT_OUTPUT, "Output file name", "Nume fișier de ieșire")
X(PORT_PROBLEM_HOST_PORT, "A TCP connection needs HOST:PORT", "O conexiune TCP necesită ADRESA:PORT")
X(PORT_PROBLEM_TCP_PORT, "The TCP port must be a number from 1 to 65535", "Portul TCP trebuie să fie un număr de la 1 la 65535")
X(PORT_PROBLEM_LISTEN, "A TCP server takes PORT or HOST:PORT", "Un server TCP primește PORT sau ADRESA:PORT")
X(PORT_PROBLEM_EMPTY, "The port's device, pipe or file is empty", "Nu este specificat dispozitivul, pipe-ul sau fișierul portului")

/* --- the Controllers page --------------------------------------------------------------- */
X(HEAD_JOYSTICK, "Joystick %u", "Joystick %u")
X(L_FED_BY, "Fed by", "Comandat de")
X(L_DEAD_ZONE, "Dead zone", "Zonă moartă")
X(L_RANGE, "Range", "Domeniu")
X(L_CENTRE, "Centre", "Centru")
X(L_CALIBRATE, "Calibrate...", "Calibrare...")
X(HEAD_PADS, "Game controllers connected", "Dispozitive de intrare conectate")
X(V_NO_PADS, "(none)", "(niciunul)")
X(V_STICK_NONE, "Not used", "Neutilizat")
X(V_STICK_KEYBOARD, "Keyboard (arrows and two keys)", "Tastatură (săgeți și două taste)")
X(V_PAD_GONE, "controller %u (not connected)", "dispozitiv %u (neconectat)")
X(L_JOYSTICK_FED_BY, "Joystick fed by", "Joystick comandat de")
X(HINT_FED_KEYBOARD, "The keyboard drives the joystick: %s move the stick, %s is button 1 and %s button 2; F11 switches the keys to the computer.",
  "Tastatura comandă joystick-ul: %s îl deplasează, %s este butonul 1 și %s butonul 2; F11 comută tastele la microcalculator.")
X(V_STICK_ARROWS, "arrow keys", "tastele cursor")
X(V_STICK_ARROWS_CAP, "Arrow keys", "Tastele cursor")
X(L_STAND_KEYS, "Joystick keys...", "Taste de joystick...")
X(HINT_STAND_KEYS, "Chooses the keys of the keyboard joystick: Enter on a function, then the key it is to have. " EN_LEAVE,
  "Se aleg tastele joystick-ului de pe tastatură: Enter pe o funcție, apoi tasta dorită. " RO_LEAVE)
X(HINT_KEYS_NEEDS, "Needs a joystick fed by the keyboard (Fed by, below).",
  "Este necesar un joystick comandat de tastatură (Comandat de, mai jos).")
X(D_KEYS_TITLE, "Joystick keys", "Taste de joystick")
X(L_KEY_UP, "Up", "Sus")
X(L_KEY_DOWN, "Down", "Jos")
X(L_KEY_LEFT, "Left", "Stînga")
X(L_KEY_RIGHT, "Right", "Dreapta")
X(L_KEY_FIRE1, "Button 1", "Buton 1")
X(L_KEY_FIRE2, "Button 2", "Buton 2")
X(V_KEY_WAIT, "Press a key...", "Se apasă o tastă...")
X(D_KEY_RESERVED, "Key %s is reserved for the window: choose another key", "Tasta %s este rezervată ferestrei: se alege altă tastă")
X(D_KEY_USED, "Key %s is already assigned to %s", "Tasta %s este deja atribuită pentru %s")
X(D_KEY_BAD, "That key cannot be used", "Tasta nu poate fi utilizată")
X(HEAD_GAME_ADAPTER, "Game control adapter", "Cuplor pentru Joystick")
X(HINT_GAME_ADAPTER, "Fits the joystick coupler at port 201h: two joysticks of two axes and two buttons. " EN_RESTART,
  "Se instalează cuplorul Joystick la 201h: două joystick-uri cu două axe și două butoane. " RO_RESTART)
X(HINT_NO_GAME_ADAPTER, "The game control adapter is not fitted.", "Cuplorul pentru Joystick este neinstalat.")
X(HEAD_MOUSE, "Serial mouse", "Mouse serial")
X(L_MOUSE, "Mouse", "Mouse")
X(HINT_MOUSE_ROW, "Needs the PC/XT serial adapter, which replaces the Junior's own serial and printer adapter. Click the picture to capture it; Ctrl+F10 releases it.",
  "Mouse-ul necesită cuplorul serial PC/XT, care înlocuiește cuplorul serial și imprimanta Junior-ului. Un clic pe ecran îl captează, Ctrl+F10 îl eliberează.")
X(HEAD_LIGHT_PEN, "Light pen", "Creion optic")
X(L_ENABLED, "Enabled", "Activat")
X(HINT_LIGHT_PEN, "The mouse over the picture is the pen (left button: its switch); it needs a lit spot; a captured serial mouse takes precedence. " EN_NOW,
  "Mouse-ul de pe ecran este creionul optic (butonul stîng îl comută); necesită punct luminos; mouse-ul serial captat are prioritate. " RO_NOW)
X(HINT_NO_LIGHT_PEN, "The pen is wired to the display coupler, which is not fitted.",
  "Creionul optic se cuplează la cuplorul de afișaj, care este neinstalat.")
X(HINT_NO_LIGHT_PEN_MDA, "The pen is wired to the CGA; the monochrome coupler has no pen connector.",
  "Creionul optic se cuplează la cuplorul CGA; cuplorul monocrom nu are conector pentru creion optic.")
X(HINT_FED_OTHER, "Chooses a host game controller (only connected ones are offered), the keyboard or nothing. " EN_LEAVE,
  "Se alege un dispozitiv de intrare al gazdei (sînt oferite numai cele conectate), tastatura sau nimic. " RO_LEAVE)
X(HINT_DEAD_ZONE, "Sets the part of the travel around the centre that counts as centred. " EN_LEAVE,
  "Se stabilește partea din cursă din jurul centrului care este considerată centrată. " RO_LEAVE)
X(HINT_RANGE, "Sets the part of the travel that gives full deflection. " EN_LEAVE,
  "Se stabilește partea din cursă care dă valoarea maximă. " RO_LEAVE)
X(HINT_FOR_PAD, "Only for a host game controller.", "Numai pentru un dispozitiv de intrare al gazdei.")
X(HINT_CALIBRATE, "Sets the centre and the range from the controller's own movement. " EN_LEAVE,
  "Centrul și domeniul se stabilesc după mișcarea dispozitivului. " RO_LEAVE)
X(HINT_CALIBRATE_NEEDS, "Needs a connected game controller assigned to this joystick.",
  "Este necesar un dispozitiv de intrare conectat și atribuit acestui joystick.")
X(CAL_TITLE, "Calibrate joystick %d", "Calibrare joystick %d")
X(CAL_CENTRE, "Let the stick rest in the middle,\nthen press Enter.", "Joystick-ul se lasă în poziția de mijloc,\napoi se apasă Enter.")
X(CAL_RANGE, "Move the stick to every limit of its\ntravel, then press Enter.", "Joystick-ul se deplasează la fiecare\nlimită a cursei, apoi se apasă Enter.")
X(CAL_REACH, "Reach so far: %d %%", "Valoare atinsă pînă acum: %d %%")

/* --- pop-ups ------------------------------------------------------------------------------- */
X(PICK_TITLE, "Insert diskette image - drive %c:", "Introducere imagine de dischetă - unitate %c:")
X(PICK_HARD_TITLE, "Attach fixed disk image - fixed disk %u", "Conectare imagine de disc fix - disc fix %u")
X(PICK_DRIVES, "Drives", "Unități")
X(PICK_CANNOT_READ, "Cannot read this folder", "Nu se poate citi directorul")
X(PICK_NO_DRIVES, "No drives found", "Nu s-a găsit nici o unitate")
X(PICK_FILES, "Files: %s", "Fișiere: %s")
X(PICK_ALL_FILES, "All files", "Toate fișierele")
X(PICK_DIR, "<DIR>", "<DIR>")
X(MENU_CHOOSE, "Choose", "Alegere")
X(BUTTON_OK, "OK", "OK")
X(D_NO_MEMORY, "Out of memory", "Memorie insuficientă")
X(D_NO_MEMORY_TEXT, "The file list could not be made.", "Lista de fișiere nu a putut fi construită.")
X(D_CANNOT_SAVE, "Cannot save", "Nu se poate salva")
X(D_CANNOT_READ_FILE, "Cannot read %s", "Nu se poate citi %s")
X(D_CANNOT_WRITE_FILE, "Cannot write %s", "Nu se poate scrie %s")
X(D_DISK_REFUSED, "Cannot use the image", "Imaginea nu poate fi utilizată")
X(D_DRIVE_REFUSED, "The drive does not accept the image.", "Unitatea nu acceptă imaginea.")
X(D_FILE_NOT_WRITTEN, "Cannot write the file", "Nu se poate scrie fișierul")
X(D_NEW_BLANK, "New blank diskette", "Dischetă nouă goală")
X(D_BLANK_NAME, "New diskette image file name", "Nume fișier imagine dischetă nouă")
X(D_BLANK_WRITTEN, "Diskette image written (%u KiB).\nInsert it in drive %c:?", "Imaginea de dischetă a fost scrisă (%u Kocteți).\nSe introduce în unitatea %c:?")
X(D_BLANK_BUTTONS, "Insert|Cancel", "Introducere|Anulare")
X(D_NEW_HARD, "New blank fixed disk", "Disc fix nou gol")
X(D_HARD_NAME, "New fixed disk image file name", "Nume fișier imagine disc fix nou")
X(D_HARD_WRITTEN, "Fixed disk image created (%u MB).\nAttach it as fixed disk %u?", "Imaginea de disc fix a fost creată (%u MB).\nSe conectează ca discul fix %u?")
X(D_HARD_BUTTONS, "Attach|Cancel", "Conectare|Anulare")
X(D_HARD_EXISTS_TEXT, "The file exists. Replace it with an empty fixed disk?", "Fișierul există. Se înlocuiește cu un disc fix gol?")
X(D_EMPTY_NAME, "Missing name", "Nume lipsă")
X(D_TARGET_NEEDS_NAME, "The target needs a name.", "Pentru destinație trebuie specificat un nume.")
X(D_IMAGE_NEEDS_NAME, "The image needs a file name.", "Pentru imagine trebuie specificat un nume de fișier.")
X(D_CANNOT_USE_THAT, "Invalid value", "Valoare incorectă")
X(D_FILE_EXISTS, "Existing file", "Fișier existent")
X(D_FILE_EXISTS_TEXT, "The file exists. Replace it with an empty diskette?", "Fișierul există. Se înlocuiește cu o dischetă goală?")
X(D_FILE_EXISTS_BUTTONS, "Replace|Back", "Înlocuire|Înapoi")
X(D_LEAVING, "Leaving setup", "Ieșire din configurare")
X(D_LEAVING_TEXT, "The machine restarts to apply the new settings.", "Pentru aplicarea opțiunilor noi microcalculatorul se reinițializează.")
X(D_RESTART_BUTTONS, "Restart|Back", "Reîncărcare|Înapoi")
X(D_RESET, "System reset", "Inițializare sistem")
X(D_RESET_TEXT, "Pull the RESET line? Memory is kept.", "Se activează linia de inițializare (RESET)? Memoria se păstrează.")
X(D_RESET_BUTTONS, "Reset|Back", "Inițializare|Înapoi")
X(D_POWER, "Power off and on", "Oprire și pornire")
X(D_POWER_TEXT, "Clear the memory and start again?", "Se șterge memoria și se reia punerea în funcțiune?")

/* --- the messages over the machine's picture (the status cells) ---------------------------- */
X(SAY_PAUSED, "Paused: the Pause key goes on", "Pauză: tasta Pause continuă lucrul")
X(SAY_SCREENSHOT, "Screenshot saved: %s", "Ecran salvat: %s")
X(SAY_SCREENSHOT_FAILED, "Cannot save the screenshot", "Nu se poate salva ecranul")
X(SAY_IMAGE_IN_DRIVE, "The image is already in drive %c", "Imaginea se află deja în unitatea %c")
X(SAY_NO_DRIVE, "There is no drive %c", "Nu există unitatea %c")
X(SAY_NOT_A_DISK, "%s: cannot be used as a diskette", "%s: nu poate fi utilizat ca dischetă")
X(SAY_DRIVE, "Drive %c: %s", "Unitate %c: %s")
X(SAY_IMAGE_ON_HARD, "The image is already fixed disk %u", "Imaginea este deja discul fix %u")
X(SAY_DRIVE_KIB, "Drive %c: %s (%u KiB)", "Unitate %c: %s (%u Kocteți)")
X(SAY_DRIVE_KIB_PROTECTED, "Drive %c: %s (%u KiB, write protected)", "Unitate %c: %s (%u Kocteți, protejată la scriere)")
X(SAY_SPEED_477, "Speed: 4.77 MHz", "Frecvență: 4,77 MHz")
X(SAY_SPEED_716, "Speed: 7.16 MHz (not original)", "Frecvență: 7,16 MHz (nu este cea originală)")
X(SAY_SPEED_954, "Speed: 9.54 MHz (not original)", "Frecvență: 9,54 MHz (nu este cea originală)")
X(SAY_SPEED_FREE, "Speed: unthrottled (not original)", "Frecvență: nelimitată (nu este cea originală)")
X(SAY_CANNOT_REBUILD, "Cannot rebuild the machine", "Nu se poate reconstitui microcalculatorul")
X(SAY_CANNOT_PORTS, "Cannot open the ports", "Nu se pot deschide interfețele")
X(SAY_MOUSE_CAPTURED, "Mouse captured: Ctrl+F10 lets it go", "Mouse captat: Ctrl+F10 îl eliberează")
X(SAY_MOUSE_RELEASED, "Mouse released", "Mouse eliberat")
X(SAY_STICK_KEYS, "Joystick: %s, buttons %s / %s", "Joystick: %s, butoane %s / %s")
X(SAY_STICK_OFF, "%s control the computer", "%s comandă microcalculatorul")
X(SAY_STICK_NONE, "A joystick must be assigned to the keyboard on the Controllers page", "Un joystick trebuie atribuit tastaturii în pagina Joystick")
X(SAY_STICK_NO_ADAPTER, "The game control adapter is not fitted (Controllers page)", "Cuplorul pentru Joystick este neinstalat (pagina Joystick)")
X(SAY_POWER_CYCLE, "Power off and on", "Oprire și pornire")
X(SAY_RESET, "System reset", "Inițializare sistem")

/* --- messages about a disk image ---------------------------------------------------------- */
X(DISK_PATH_TOO_LONG, "The path is too long", "Calea este prea lungă")
X(DISK_OTHER_KIND, "%s is %s: MezinXT reads raw 160, 180, 320, 360 and 720 KiB diskette images",
  "%s este %s: MezinXT citește imagini brute de dischetă de 160, 180, 320, 360 și 720 Kocteți")
X(DISK_BAD_SIZE, "%s: %zu bytes is not a 160, 180, 320, 360 or 720 KiB diskette image",
  "%s: %zu octeți nu reprezintă o imagine de dischetă de 160, 180, 320, 360 sau 720 Kocteți")
X(DISK_SIZES, "A diskette holds 160, 180, 320, 360 or 720 KiB", "O dischetă are capacitatea de 160, 180, 320, 360 sau 720 Kocteți")
X(DISK_EXISTS, "%s exists already", "%s există deja")
X(DISK_DRIVE_REASON, "Drive %c: %s", "Unitate %c: %s")
X(DISK_KIND_12, "a 1.2 MB (AT, 5.25-inch high density) diskette", "o dischetă de 1,2 MB (AT, 5,25 inch, densitate mare)")
X(DISK_KIND_144, "a 1.44 MB (3.5-inch) diskette", "o dischetă de 1,44 MB (3,5 inch)")
X(DISK_KIND_288, "a 2.88 MB (3.5-inch) diskette", "o dischetă de 2,88 MB (3,5 inch)")
X(DISK_KIND_IMD, "an ImageDisk (IMD) file", "un fișier ImageDisk (IMD)")
X(DISK_KIND_TD0, "a Teledisk (TD0) file", "un fișier Teledisk (TD0)")
X(DISK_KIND_HFE, "an HxC (HFE) file", "un fișier HxC (HFE)")
X(DISK_KIND_86F, "an 86Box (86F) file", "un fișier 86Box (86F)")
X(DISK_KIND_BPB, "an image whose boot sector describes %u KiB (%u sectors)",
  "o imagine al cărei sector de încărcare descrie %u Kocteți (%u sectoare)")
X(DISK_HARD_BAD_SIZE, "%s: %llu bytes is not a fixed disk image of 17-sector tracks (20 MB: 21411840 bytes)",
  "%s: %llu octeți nu reprezintă o imagine de disc fix cu piste de 17 sectoare (20 MB: 21411840 octeți)")
X(DISK_HARD_SIZES, "A new fixed disk holds 20, 30, 40, 60 or 70 MB", "Un disc fix nou are capacitatea de 20, 30, 40, 60 sau 70 MB")
X(DISK_HARD_WRITE, "Cannot write to %s", "Nu se poate scrie în %s")
X(DISK_NOT_HARD, "The image is not a fixed disk image", "Imaginea nu este o imagine de disc fix")
X(DISK_HARD_GEOMETRY, "The coupler cannot address this fixed disk", "Cuplorul nu poate adresa acest disc fix")
X(DISK_HARD_DESCRIBE, "%u.%u MB: %u cylinders, %u heads, %u sectors", "%u,%u MB: %u cilindri, %u capete, %u sectoare")
X(DISK_HARD_REASON, "Fixed disk %u: %s", "Disc fix %u: %s")

/* --- the test screen and the missing-ROM screen (the stub machine) -------------------------- */
X(STUB_TITLE, "MezinXT %s - test screen", "MezinXT %s - ecran de probă")
X(STUB_NAME, "FEPER JUNIOR-XT (stub machine)", "FEPER JUNIOR-XT (microcalculator de probă)")
X(STUB_GLYPHS_ROM, "Glyphs: the Junior's character ROM (CHAR_RF5)", "Caractere: memoria ROM de caractere a Junior-ului (CHAR_RF5)")
X(STUB_GLYPHS_FALLBACK, "Glyphs: MezinXT's fallback font - the character ROM is not in roms/",
  "Caractere: fontul de rezervă MezinXT - ROM-ul de caractere lipsește din roms/")
X(STUB_MISSING, "Missing ROMs: run  python tools/fetch_roms.py  to fetch them", "ROM-uri lipsă: se rulează  python tools/fetch_roms.py  pentru a le obține")
X(STUB_CONFIGURED, "Configured: BIOS %s, %s at %s MHz, %u KiB; no CPU",
  "Configurat: BIOS %s, %s la %s MHz, %u Kocteți; fără microprocesor")
X(STUB_CONFIGURED_FREE, "Configured: BIOS %s, %s at full speed, %u KiB; no CPU",
  "Configurat: BIOS %s, %s la frecvență maximă, %u Kocteți; fără microprocesor")
X(STUB_PAPER, "Paper", "Fond")
X(STUB_INK, "Ink", "Text")
X(STUB_ROMS, "ROMs in roms/ (known by their SHA-256):", "ROM-uri în roms/ (recunoscute după SHA-256):")
X(STUB_ROMS_ONE_OTHER, "ROMs in roms/ (known by their SHA-256; %u other file):", "ROM-uri în roms/ (recunoscute după SHA-256; alte fișiere: %u):")
X(STUB_ROMS_OTHERS, "ROMs in roms/ (known by their SHA-256; %u other files):", "ROM-uri în roms/ (recunoscute după SHA-256; alte fișiere: %u):")
X(STUB_FOUND, "found", "găsit")
X(STUB_MISSING_ROM, "missing", "lipsă")
X(STUB_HELP, "Type to echo keys (each one beeps)    Alt+Enter full screen", "Prin tastare se afișează tastele (fiecare dă un semnal)    Alt+Enter ecran complet")
X(STUB_KEYS, "Keys:", "Taste:")
X(STUB_MAKE, "make", "apăsat")
X(STUB_BREAK, "break", "eliberat")

#undef EN_RESTART
#undef RO_RESTART
#undef EN_NOW
#undef RO_NOW
#undef EN_LEAVE
#undef RO_LEAVE
