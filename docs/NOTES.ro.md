# Note

Discheta cu cartea tehnică. Tastele, ecranul de configurare și opțiunile microcalculatorului sunt în [README](../README.ro.md); toate setările și limitările, în [notele în engleză](NOTES.md).

## Discheta cu cartea tehnică

`disks/manual.img` este o dischetă de 360 Kocteți care pornește singură, cu un program care prezintă cartea tehnică a
microcalculatorului (*Microcalculatorul JUNIOR-XT, Carte tehnica*, FEPER, 1989) chiar pe Junior, în limba română:
subiecte cu legături și desene în modul text color 80x25, o imagine de început cu emblema FEPER și o fotografie a
microcalculatorului în șaisprezece culori, verificări care rulează pe microcalculator și cartea tipărită, pagină cu pagină.
Pe dischetă nu se află niciun sistem de operare: sectorul de încărcare aduce programul, care folosește numai BIOS-ul.
Schemele (anexa 4) nu sunt pe dischetă. Programul rulează pe I8086 și pe I8088, cu 256 Kocteți și o unitate. Imaginea de
început se trece cu orice tastă.

### Taste

| Tastă | Efect |
|---|---|
| Sus, Jos | legătura de deasupra sau de dedesubt; după ultima vizibilă textul se deplasează cu o linie |
| Tab | următoarea legătură vizibilă |
| Enter | deschide legătura: un subiect, o pagină sau o verificare |
| Backspace, Esc | înapoi la locul de unde s-a deschis legătura (se păstrează șaisprezece locuri) |
| PgUp, PgDn, Home, End | textul cu un ecran, începutul și sfârșitul lui |
| Stânga, Dreapta | subiectul sau pagina dinainte și de după, în aceeași secțiune |
| F1 până la F7 | secțiunile |
| P | „Pagina”: numărul unei pagini din cartea tipărită, apoi Enter |

O eroare de citire se reîncearcă de trei ori, apoi este afișată pe ultima linie; o tastă încearcă din nou.

### Verificări

| Verificare | Arată |
|---|---|
| Tastatura | cele 83 de taste desenate; o tastă apăsată se aprinde și rămâne marcată după eliberare, cu codul trimis de tastatură (întreruperea tastaturii este preluată pe durata verificării); Esc de două ori încheie |
| Afisaj | imagini una după alta: cele șaisprezece culori cu numele din cartea tehnică, setul de caractere, modul cu 40 de coloane, 320x200 cu paletele lui, 640x200, o imagine pentru reglarea monitorului |
| Sistem | comutatoarele de configurare așa cum le dă placa (porturile 61h și 62h) și ce înseamnă, memoria găsită de BIOS, coprocesorul, contorul ceasului; M verifică memoria de deasupra programului, S face să sune difuzorul |
| Discheta si port-uri | toate pistele dischetei citite pe ambele fețe (A și B aleg unitatea), starea imprimantei și a portului serial așa cum le raportează BIOS-ul, portul de Joystick |
