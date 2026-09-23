<div align="center">

<img src="docs/assets/logo.svg" alt="occ — Opus C Compiler" width="520">

**C23 fordító x86-64 Linuxra, C23-ban írva, amely önmagát is le tudja fordítani.**<br>
Saját előfeldolgozó, elemző, optimalizáló és kódgenerátor — és egy fordítás, amelyet végig lehet követni.

[![CI](https://github.com/foxik38/opus-c-compiler/actions/workflows/ci.yml/badge.svg)](https://github.com/foxik38/opus-c-compiler/actions/workflows/ci.yml)
![C23](https://img.shields.io/badge/language-C23-5fafff)
![x86-64 Linux](https://img.shields.io/badge/target-x86--64%20Linux-5fd7d7)
![License: MIT](https://img.shields.io/badge/license-MIT-a6e3a1)
[![Built with Claude](https://img.shields.io/badge/built%20with-Claude-D97757?logo=claude&logoColor=white)](THINKPROC.md)

[English](README.md) · [Čeština](README.cs.md) · **Magyar** · [עברית](README.he.md)

<img src="docs/assets/demo.svg" alt="Az occ lefordít egy programot: minden fázis az állapotával, a CPU-idővel és az eredményével" width="860">

</div>

## Tartalom

[Főbb jellemzők](#főbb-jellemzők) · [Gyors kezdés](#gyors-kezdés) · [Használat](#használat) · [Diagnosztika](#diagnosztika) ·
[Hogyan működik](#hogyan-működik) · [Teljesítmény](#teljesítmény) · [Valódi kódon tesztelve](#valódi-kódon-tesztelve) ·
[C23-támogatás](#c23-támogatás) · [Tesztek](#tesztek) · [Claude készítette](#claude-készítette)

## Főbb jellemzők

- 🧩 **A teljes fordítási folyamat** — az előfeldolgozó, az elemző a típusellenőrzéssel, az
  optimalizáló és az x86-64 kódgenerátor az occ saját munkája; az utolsó lépést a GNU `as` és `ld`
  végzi, amelyeket az occ közvetlenül hív meg.
- 📺 **Követhető fordítás** — minden fázis jelenti, hogy `OK` vagy `ERR`, mennyi CPU- és valós
  időt vett igénybe, és mit állított elő. Terminálban a futó fázisok animálva jelennek meg, az
  összesítés pedig megmutatja, melyik fázis mennyi időt vitt el.
- 🩺 **Segítőkész diagnosztika** — GCC-stílusú üzenetek forráskód-részlettel, `help:` tipp a
  gyakori hibákhoz, és 27 névvel ellátott figyelmeztetés, amelyek a nem definiált viselkedésre és
  az elavult szerkezetekre összpontosítanak.
- 🚀 **Két optimalizálási szint** — `-o none` a nyilvánvalóan helyes kódhoz, `-o prod`
  regiszterkiosztással, címzési módokkal, konstanskiértékeléssel, műveletegyszerűsítéssel, a
  ciklusinvariáns kód kiemelésével, ugrótáblákkal és peephole optimalizálással. Ciklusokkal teli
  kódon megközelíti a `gcc -O2` szintjét.
- ✨ **C23** — `constexpr`, `auto`, `typeof`, `nullptr`, `bool`, `#embed`, attribútumok,
  `<stdckdint.h>`, rögzített típusú felsorolások, számjegy-elválasztók és még sok más.
- 🧪 **Valódi programokon bizonyítva** — az SQLite, a Lua, a zlib, a Duktape és társaik
  lefordulnak az occ-vel, és átmennek a saját tesztjeiken; az occ önmagát is lefordítja, egészen a
  fixpontig (bootstrap fixpoint).

## Gyors kezdés

Az első fordításhoz C23 (vagy C2x) fordító kell (GCC 13-mal és Clang 18-cal kipróbálva), valamint a
GNU binutils és a glibc fejlesztői fájljai — Debianon/Ubuntun a `build-essential`, Archon a
`base-devel` csomag.

```sh
make                          # lefordítja a ./occ programot
make test                     # minden teszt -o none és -o prod szinten
sudo make install             # /usr/local/bin/occ (+ fejlécfájlok: /usr/local/lib/occ)

occ examples/hello.c --run    # fordítás és futtatás
occ -o prod examples/life.c && ./life 30
```

## Használat

```text
occ [kapcsolók] <fájl>...
```

A bemenet lehet `.c` (lefordítja), `.s` (assemblálja), `.o` és `.a` fájl (összelinkeli).

> [!NOTE]
> Az occ-ben **a `-o` az optimalizálási szintet adja meg**, **a `-n` pedig a kimenet nevét** —
> tehát éppen fordítva, mint a GCC-ben.

| Kapcsoló | Jelentés |
|---|---|
| `-o none` \| `-o prod` | Optimalizálási szint. Alapértelmezés a `none`; a `prod` éles használatra kész kódot ad (≈ GCC `-O1`/`-O2`). A `-O0`…`-O3` álnévként is működik. |
| `-n <név>` | A kimeneti fájl neve. Alapértelmezés: az első forrásfájl neve kiterjesztés nélkül. |
| `-E` / `-S` / `-c` | Megállás az előfeldolgozás / a fordítás (`.s`) / az assemblálás (`.o`) után. |
| `--run [-- argumentumok]` | A program futtatása a fordítás után. |
| `--keep` | A köztes `.s` és `.o` fájlok megtartása. |
| `-I <könyvtár>`, `-iquote <könyvtár>` | Könyvtár hozzáadása az `#include <...>` / `"..."` kereséséhez. |
| `-D <név>[=érték]`, `-U <név>` | Makró definiálása / törlése. |
| `-l <könyvtár>`, `-L <könyvtár>` | Programkönyvtár linkelése / könyvtár hozzáadása a keresési úthoz (a `libm` magától hozzáadódik, de csak akkor, ha szükség van rá). |
| `--static`, `-rdynamic` | Statikus linkelés / minden szimbólum exportálása a `dlopen()`-nel betöltött bővítményeknek. |
| `-w`, `-Werror`, `-W<név>`, `-Wno-<név>` | A figyelmeztetések szabályozása; a `--warnings` mindet kilistázza. |
| `-q`, `-v` | Csendes (csak diagnosztika) / részletes (az `as` és `ld` parancsokat is kiírja). |
| `--color`, `--no-color` | Színek kényszerítése vagy kikapcsolása (alapértelmezés: ha a stderr terminál, a `NO_COLOR` figyelembevételével). |

Környezeti változók: az `OCC_AS`, `OCC_LD` és `OCC_CC` felülírja az occ által futtatott
eszközöket, az `OCC_LINK_WITH_CC=1` a `cc`-n keresztül linkel az `ld` közvetlen hívása helyett, az
`OCC_NO_ANIMATION=1` pedig kikapcsolja az animációt a terminálban. Ha a stderr nem terminál, a
kimenet egyszerű szöveg, fázisonként egy sor.

## Diagnosztika

Ha valami elromlik, a hibás fázis `ERR`-re vált, és jönnek a diagnosztikai üzenetek — mindegyik a
helyével, a forráskód sorával, a figyelmeztetés nevével és, ahol a javítás kézenfekvő, egy tippel:

<div align="center">
<img src="docs/assets/diagnostics.svg" alt="Az occ diagnosztikája egy öt hibát tartalmazó fájlra" width="860">
</div>

Minden üzenet első sora megtartja a GCC `fájl:sor:oszlop: warning:` formátumát, így a
szerkesztők és a buildeszközök is megértik. Alapértelmezés szerint minden figyelmeztetés be van
kapcsolva; az **UB** jelűek olyan kódra figyelmeztetnek, amelynek viselkedését a C szabvány nem
definiálja.

<details>
<summary><b>Mind a 27 figyelmeztetés</b> (<code>occ --warnings</code>)</summary>

| Figyelmeztetés | Mit észlel |
|---|---|
| `unused-variable` | a lokális változót sosem használják (vagy csak értéket kap) |
| `unused-function` | a statikus függvényt sosem használják |
| `unused-value` | a kifejezés eredményét kiszámolják, majd eldobják |
| `unused-result` | egy `[[nodiscard]]` függvény eredményét figyelmen kívül hagyják |
| `uninitialized` | a változót olvassák, de sosem kap értéket — **UB** |
| `parentheses` | értékadás feltételként |
| `empty-body` | üres `if`/`while`/`for` törzs (elkóborolt pontosvessző) |
| `div-by-zero` | egész osztás konstans nullával — **UB** |
| `shift-count` | negatív vagy a típus szélességénél nem kisebb léptetés — **UB** |
| `overflow` | a konstans túlcsordul vagy az átalakítás megváltoztatja |
| `array-bounds` | konstans index a tömb határain kívül — **UB** |
| `null-dereference` | nullpointer-konstans dereferálása — **UB** |
| `return-local-addr` | lokális változó címének visszaadása — **UB** |
| `return-type` | értéket visszaadó függvény `return` nélkül ér véget — **UB** |
| `format` | a `printf`/`scanf` formátuma nem illik az argumentumokhoz — **UB** |
| `sign-compare` | előjeles és előjel nélküli egészek összehasonlítása |
| `int-conversion` | implicit átalakítás pointer és egész szám között |
| `incompatible-pointer-types` | implicit átalakítás nem kompatibilis pointerek között |
| `discarded-qualifiers` | az implicit átalakítás elhagyja a `const`/`volatile` minősítőt |
| `string-compare` | összehasonlítás sztringliterállal `==` vagy `!=` operátorral |
| `sizeof-array-argument` | `sizeof` tömb típusú paraméterre |
| `deprecated` | elavult, kivezetés alatt álló vagy nem biztonságos szerkezetek |
| `multichar` | több karakterből álló karakterkonstans |
| `main` | gyanús `main`-deklaráció |
| `macro-redefined` | eltérő törzzsel újradefiniált makró |
| `cpp` | `#warning` direktíva |
| `unsupported` | az occ elfogadja a szerkezetet, de csak közelítőleg valósítja meg |

Az eltávolított szerkezetek kemény hibának számítanak: az implicit `int` és az implicit
függvénydeklaráció (a C99 óta nem része a nyelvnek), a `gets()` (a C11 óta) és a K&R stílusú
függvénydefiníció (a C23 óta).

</details>

## Hogyan működik

<div align="center">
<img src="docs/assets/pipeline.svg" alt="Az occ fordítási folyamata: preprocess, compile (parse, optimize, codegen), assemble, link" width="860">
</div>

Az elemző teljesen típusos AST-t épít, amelyben minden implicit átalakítás explicit típuskényszerítés,
így a kódgenerátornak nem kell újra kitalálnia a C átalakítási szabályait. A `-o none` egy
egyszerű akkumulátoros veremgép — nyilvánvalóan helyes, és ez a referencia, amelyhez a `-o prod`
szintet tesztelik. A `-o prod` megtartja a vázát, és megszünteti a költségeit:

| | `-o none` | `-o prod` |
|---|---|---|
| Változók | a vermen | skalárok a `%rbx`, `%r12`–`%r15` regiszterekben; `double`/`float` a `%xmm12`–`%xmm15`-ben |
| Részeredmények | `push`/`pop` | segédregiszterek: `%r8`–`%r11`, `%xmm8`–`%xmm11` |
| Memóriaelérés | cím a `%rax`-ben, majd betöltés | `disp(base, index, scale)` operandusok, olvasás-módosítás-írás, közvetlen konstansírás |
| Vezérlés | feltétel a ciklus elején | hátul tesztelő ciklusok, közvetlen `cmp`+`jcc`, ugrótáblák sűrű `switch`-hez |
| AST-menetek | — | konstanskiértékelés, algebrai azonosságok, műveletegyszerűsítés, halott ágak, ciklusinvariáns kód kiemelése |
| Utómunka | — | peephole optimalizálás |

```text
src/
├── support/   vektorok, sztringek, hash tábla, forrásfájlok, diagnosztika, időzítők
├── preproc/   lexer, makrókifejtés (Prosser-féle hidesetek), direktívák, #if kiértékelése
├── parse/     deklarációk, kifejezések, utasítások, inicializálók, típusok, hatókörök,
│              konstansok kiértékelése és a legtöbb figyelmeztetés mögötti statikus ellenőrzés
├── opt/       AST-menetek a -o prod szinthez: összevonás és egyszerűsítés, kódkiemelés ciklusokból
├── codegen/   kifejezések, utasítások, hívások és a SysV ABI, adatok, regiszterek, peephole
└── driver/    parancssor, fordítási folyamat, élő állapotkijelzés, külső eszközök
include/       az occ saját, önálló fejlécfájljai (stddef.h, stdarg.h, stdckdint.h, ...)
examples/      kipróbálható kis programok     tests/   futási, diagnosztikai, teljesítmény- és fuzz-tesztek
```

Hogy miért épül így fel az occ — és mi romlott el menet közben —, azt a
[**THINKPROC.md**](THINKPROC.md) írja le (angolul).

## Teljesítmény

A [`tests/bench`](tests/bench) benchmarkjainak legjobb eredménye 3 futásból (`make bench`; minden
változat ugyanazt az ellenőrzőösszeget írja ki). A rövidebb a gyorsabb.

<div align="center">
<img src="docs/assets/benchmarks.svg" alt="A benchmarkok futásideje occ -o none, occ -o prod, gcc -O0 és gcc -O2 esetén" width="720">
</div>

A `-o prod` mindenhol gyorsabb a `gcc -O0`-nál, és hat benchmarkból négyben legfeljebb 20%-kal marad
el a `gcc -O2`-től. A megmaradt különbség a sok függvényhívást tartalmazó kódban (`fib`: az occ nem
inline-ol) és a vektorizálható ciklusokban (`matmul`) van. Egy nagy, valódi programon is hasonló a
kép: az SQLite `speedtest1` tesztje 1,74 s alatt fut le `occ -o prod`-dal, 4,19 s alatt
`-o none`-nal és 0,90 s alatt `gcc -O2`-vel. A mérések megosztott gépen készültek, nagyjából ±10%
zajjal kell számolni.

## Valódi kódon tesztelve

Az alábbi projekteket az occ mindkét optimalizálási szinten lefordította, és a saját tesztjeikkel
(vagy bájtról bájtra, ugyanazon kód GCC-s fordításával összevetve) ellenőriztük őket:

| Projekt | Méret | Eredmény `-o none` és `-o prod` szinten |
|---|---:|---|
| [SQLite](https://sqlite.org) 3.53 + shell | 307 ezer sor | az SQL-feladatok kimenete megegyezik a GCC-s változatéval; a `speedtest1` ellenőrző hash-e azonos |
| [Lua](https://www.lua.org) 5.4.9 | 30 ezer sor | tesztszkript: azonos kimenet |
| [Duktape](https://duktape.org) 2.7 | 108 ezer sor | JavaScript tesztszkript: azonos kimenet |
| [MuJS](https://mujs.com) 1.3.9 | 20 ezer sor | JavaScript tesztszkript: azonos kimenet |
| [Jim Tcl](https://jim.tcl.tk) 0.83 | 43 ezer sor | saját tesztcsomag: 5729-ből 5551 sikeres — ugyanannyi, mint GCC-vel |
| [zlib](https://zlib.net) 1.3.2 | 25 ezer sor | az `example` sikeres; a `minigzip` kimenete bájtra egyezik a GCC-sével |
| [bzip2](https://sourceware.org/bzip2/) 1.0.8 | 8 ezer sor | a `make test` mintái átmennek; a kimenet bájtra egyezik a GCC-sével |
| [LZ4](https://lz4.org) 1.10 | 18 ezer sor | az 1/9/12-es szint kimenete egyezik a GCC-sével, oda-vissza tömörítés rendben |
| [xxHash](https://xxhash.com) 0.8.3 | 12 ezer sor | sanity teszt: mind a 49948 vektor sikeres; mind a négy hash egyezik |
| [Csmith](https://github.com/csmith-project/csmith) | véletlen | 500 generált program; az egyetlen eltérés a GCC-hez képest hiba volt, már javítva |

Mindegyik futás talált vagy megerősített valamit: a Lua fordítása két téves figyelmeztetést
leplezett le, az xxHash egy előfeldolgozó-hibát, a Jim Tcl egy hiányzó `-rdynamic` kapcsolót, a Csmith
pedig a bitmezők hibás egész-előléptetését — mindet javítottuk, és regressziós tesztek védik őket.

## C23-támogatás

<details open>
<summary>Ami benne van</summary>

- `constexpr` objektumok, `auto` típuskikövetkeztetés, `typeof` / `typeof_unqual`
- `nullptr` / `nullptr_t`, `bool` / `true` / `false` kulcsszóként
- rögzített alaptípusú felsorolások (`enum e : uint8_t`)
- attribútumok: `[[nodiscard]]`, `[[deprecated]]`, `[[fallthrough]]`, `[[maybe_unused]]`,
  `[[noreturn]]`, `[[unsequenced]]`, `[[reproducible]]`
- `#embed`, `#elifdef` / `#elifndef`, `#warning`, `__has_include`, `__has_embed`,
  `__has_c_attribute`, `__VA_OPT__`
- `static_assert` üzenet nélkül, üres inicializáló (`= {}`)
- bináris literálok és számjegy-elválasztók (`0b1010'0101`)
- címkék deklarációk előtt és blokkok végén
- névtelen paraméterek definíciókban, `f()` mint `f(void)`
- ellenőrzött aritmetika az `<stdckdint.h>`-val (pontos eredmény az operandustípusok bármely
  kombinációjára), `unreachable()`, `u8` karakterkonstansok
- minden, amit a C99-től és C11-től vár az ember: kijelölt inicializálók, összetett literálok,
  rugalmas tömbtagok, névtelen struktúrák és uniók, `_Generic`, `_Alignas`/`_Alignof`,
  `_Thread_local`, változó argumentumszámú függvények, bitmezők, `setjmp`/`longjmp`, struktúrák
  érték szerinti átadása és visszaadása (SysV ABI)

</details>

<details>
<summary>Ami (még) nincs</summary>

- változó hosszúságú tömbök (VLA) — hibával elutasítva (a C11 óta opcionálisak)
- `_BitInt(N)`, `_Complex`, decimális lebegőpontos számok
- a `long double` `double`-ként fordul; az `_Atomic` atomi szemantika nélkül elfogadott (mindkettőt
  jelzi a `-Wunsupported`)
- a GNU utasításkifejezések és a kiterjesztett `asm` (az egyszerű `asm("...")` működik)
- az x86-64 Linuxon (glibc) kívüli célplatformok — szándékosan

</details>

Az újdonságok bemutatója az [`examples/c23_tour.c`](examples/c23_tour.c) fájlban található.

## Tesztek

```sh
make test                   # futási és diagnosztikai tesztek -o none és -o prod szinten
tests/run.sh --reference    # maguknak a futási teszteknek az ellenőrzése GCC-vel
make selfhost               # az occ lefordítja az occ-t; az 1. és 2. fázisnak azonos assemblert kell adnia
make fuzz FUZZ_COUNT=1000   # véletlen, UB-mentes programok, occ kontra GCC
tests/fuzz/csmith.py        # Csmith által generált programok, occ kontra GCC (csmith kell hozzá)
make bench                  # a fenti grafikon
```

A diagnosztikai tesztek mindkét irányban szigorúak: minden várt figyelmeztetésnek meg kell
jelennie, és minden figyelmeztetésnek, amelyet az occ kiír, vártnak kell lennie. A CI minden push
után lefuttatja mindezt.

## Claude készítette

<img src="https://cdn.simpleicons.org/claude/D97757" alt="Claude" width="40" align="left">

Az occ-t Claude (az Anthropic mesterségesintelligencia-modellje) tervezte, írta, tesztelte és
dokumentálta, és ő kezeli ezt a repozitóriumot is: minden commitot, tesztet és dokumentációs oldalt.
A tervezés mögötti gondolatmenetet, a menet közben talált hibákat és azt, hogyan derültek ki, a
[THINKPROC.md](THINKPROC.md) írja le.

<br clear="left">

## Licenc

[MIT](LICENSE)
