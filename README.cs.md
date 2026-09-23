<div align="center">

<img src="docs/assets/logo.svg" alt="occ — Opus C Compiler" width="520">

**Překladač jazyka C23 pro x86-64 Linux, napsaný v C23, který přeloží i sám sebe.**<br>
Vlastní preprocesor, parser, optimalizátor i generátor kódu — a překlad, který vidíte.

[![CI](https://github.com/foxik38/opus-c-compiler/actions/workflows/ci.yml/badge.svg)](https://github.com/foxik38/opus-c-compiler/actions/workflows/ci.yml)
![C23](https://img.shields.io/badge/language-C23-5fafff)
![x86-64 Linux](https://img.shields.io/badge/target-x86--64%20Linux-5fd7d7)
![License: MIT](https://img.shields.io/badge/license-MIT-a6e3a1)
[![Built with Claude](https://img.shields.io/badge/built%20with-Claude-D97757?logo=claude&logoColor=white)](THINKPROC.md)

[English](README.md) · **Čeština** · [Magyar](README.hu.md) · [עברית](README.he.md)

<img src="docs/assets/demo.svg" alt="occ překládá program: každá fáze se svým stavem, časem CPU a výsledkem" width="860">

</div>

## Obsah

[Přednosti](#přednosti) · [Rychlý start](#rychlý-start) · [Použití](#použití) · [Diagnostika](#diagnostika) ·
[Jak to funguje](#jak-to-funguje) · [Výkon](#výkon) · [Ověřeno na skutečném kódu](#ověřeno-na-skutečném-kódu) ·
[Podpora C23](#podpora-c23) · [Testy](#testy) · [Vytvořeno Claudem](#vytvořeno-claudem)

## Přednosti

- 🧩 **Celý průběh překladu** — preprocesor, parser s typovou kontrolou, optimalizátor
  i generátor kódu pro x86-64 jsou vlastní; poslední krok obstarají GNU `as` a `ld`, které occ
  volá přímo.
- 📺 **Překlad, který vidíte** — každá fáze hlásí `OK`/`ERR`, čas CPU i reálný čas a co vytvořila.
  V terminálu se běžící fáze animují a souhrn ukáže, kolik času zabrala která fáze.
- 🩺 **Diagnostika, která pomáhá** — hlášení ve stylu GCC s ukázkou zdrojového kódu, nápověda
  `help:` u častých chyb a 27 pojmenovaných varování zaměřených na nedefinované chování a zastaralé
  konstrukce.
- 🚀 **Dvě úrovně optimalizace** — `-o none` pro zjevně správný kód, `-o prod` pro alokaci
  registrů, adresní režimy, skládání konstant, zjednodušování operací, přesun invariantního kódu
  ze smyček, skokové tabulky a peephole optimalizace. U kódu se smyčkami se blíží `gcc -O2`.
- ✨ **C23** — `constexpr`, `auto`, `typeof`, `nullptr`, `bool`, `#embed`, atributy,
  `<stdckdint.h>`, výčty s pevným typem, oddělovače číslic a další.
- 🧪 **Prověřeno na skutečných programech** — SQLite, Lua, zlib, Duktape a další se přeloží
  occem a projdou vlastními testy; occ přeloží sám sebe až do pevného bodu (bootstrap fixpoint).

## Rychlý start

K prvnímu sestavení potřebujete překladač C23 (nebo C2x; vyzkoušeno s GCC 13 a Clang 18),
GNU binutils a vývojové soubory glibc — tedy `build-essential` na Debianu/Ubuntu, `base-devel`
na Archu.

```sh
make                          # sestaví ./occ
make test                     # všechny testy s -o none i -o prod
sudo make install             # /usr/local/bin/occ (+ hlavičky v /usr/local/lib/occ)

occ examples/hello.c --run    # přeložit a spustit
occ -o prod examples/life.c && ./life 30
```

## Použití

```text
occ [volby] <soubor>...
```

Vstupem mohou být soubory `.c` (překládají se), `.s` (assemblují se), `.o` a `.a` (linkují se).

> [!NOTE]
> V occ **`-o` volí úroveň optimalizace** a **`-n` určuje jméno výstupu** — tedy obráceně
> než v GCC.

| Volba | Význam |
|---|---|
| `-o none` \| `-o prod` | Úroveň optimalizace. Výchozí je `none`; `prod` je kód připravený k nasazení (≈ GCC `-O1`/`-O2`). Jako aliasy fungují i `-O0`…`-O3`. |
| `-n <jméno>` | Jméno výstupního souboru. Výchozí: jméno prvního zdrojového souboru bez přípony. |
| `-E` / `-S` / `-c` | Skončit po preprocesoru / po překladu (`.s`) / po assembleru (`.o`). |
| `--run [-- argumenty]` | Po sestavení program spustit. |
| `--keep` | Ponechat mezivýsledky `.s` a `.o`. |
| `-I <adresář>`, `-iquote <adresář>` | Přidat adresář pro `#include <...>` / `"..."`. |
| `-D <jméno>[=hodnota]`, `-U <jméno>` | Definovat / zrušit makro. |
| `-l <knihovna>`, `-L <adresář>` | Přilinkovat knihovnu / přidat adresář knihoven (`libm` se přidá sám, jen je-li potřeba). |
| `--static`, `-rdynamic` | Linkovat staticky / exportovat všechny symboly pro pluginy načítané přes `dlopen()`. |
| `-w`, `-Werror`, `-W<jméno>`, `-Wno-<jméno>` | Ovládání varování; `--warnings` je vypíše všechna. |
| `-q`, `-v` | Tiše (jen diagnostika) / podrobně (vypíše i příkazy pro `as` a `ld`). |
| `--color`, `--no-color` | Vynutit nebo vypnout barvy (výchozí: když je stderr terminál, s ohledem na `NO_COLOR`). |

Proměnné prostředí: `OCC_AS`, `OCC_LD` a `OCC_CC` nahradí nástroje, které occ spouští,
`OCC_LINK_WITH_CC=1` linkuje přes `cc` místo přímého volání `ld` a `OCC_NO_ANIMATION=1` vypne
animace v terminálu. Když stderr není terminál, výstup je prostý text, jeden řádek na fázi.

## Diagnostika

Když se něco pokazí, fáze, která selhala, se změní na `ERR` a následuje diagnostika — každé
hlášení s umístěním, řádkem zdrojového kódu, jménem varování a tam, kde je oprava zřejmá,
i s nápovědou:

<div align="center">
<img src="docs/assets/diagnostics.svg" alt="Diagnostika occ pro soubor s pěti chybami" width="860">
</div>

První řádek každého hlášení zachovává formát GCC `soubor:řádek:sloupec: warning:`, takže mu
rozumějí editory i nástroje pro sestavení. Všechna varování jsou ve výchozím stavu zapnutá;
ta označená **UB** upozorňují na kód, jehož chování norma C nedefinuje.

<details>
<summary><b>Všech 27 varování</b> (<code>occ --warnings</code>)</summary>

| Varování | Co hlídá |
|---|---|
| `unused-variable` | lokální proměnná se nikdy nepoužije (nebo se do ní jen zapisuje) |
| `unused-function` | statická funkce se nikdy nepoužije |
| `unused-value` | výsledek výrazu se spočítá a zahodí |
| `unused-result` | výsledek funkce s `[[nodiscard]]` se ignoruje |
| `uninitialized` | proměnná se čte, ale nikdy se do ní nezapíše — **UB** |
| `parentheses` | přiřazení použité jako podmínka |
| `empty-body` | prázdné tělo `if`/`while`/`for` (zatoulaný středník) |
| `div-by-zero` | celočíselné dělení konstantní nulou — **UB** |
| `shift-count` | posun o zápornou hodnotu nebo o ≥ šířku typu — **UB** |
| `overflow` | konstanta přeteče nebo se při převodu změní |
| `array-bounds` | konstantní index mimo meze pole — **UB** |
| `null-dereference` | dereference nulového ukazatele — **UB** |
| `return-local-addr` | vrácení adresy lokální proměnné — **UB** |
| `return-type` | funkce vracející hodnotu skončí bez `return` — **UB** |
| `format` | formát `printf`/`scanf` neodpovídá argumentům — **UB** |
| `sign-compare` | porovnání celých čísel se znaménkem a bez znaménka |
| `int-conversion` | implicitní převod mezi ukazatelem a celým číslem |
| `incompatible-pointer-types` | implicitní převod mezi nekompatibilními ukazateli |
| `discarded-qualifiers` | implicitní převod zahodí `const`/`volatile` |
| `string-compare` | porovnání s řetězcovým literálem pomocí `==` nebo `!=` |
| `sizeof-array-argument` | `sizeof` použitý na parametr typu pole |
| `deprecated` | použití zastaralých nebo nebezpečných konstrukcí |
| `multichar` | znaková konstanta z více znaků |
| `main` | podezřelá deklarace `main` |
| `macro-redefined` | makro předefinované s jiným tělem |
| `cpp` | direktiva `#warning` |
| `unsupported` | konstrukce přijatá, ale occ ji jen aproximuje |

Odstraněné konstrukce jsou tvrdé chyby: implicitní `int` a implicitní deklarace funkcí (odstraněny
v C99), `gets()` (odstraněna v C11) a definice funkcí ve stylu K&R (odstraněny v C23).

</details>

## Jak to funguje

<div align="center">
<img src="docs/assets/pipeline.svg" alt="Průběh překladu v occ: preprocess, compile (parse, optimize, codegen), assemble, link" width="860">
</div>

Parser vytváří plně otypovaný AST, ve kterém je každý implicitní převod explicitním přetypováním,
takže generátor kódu nemusí znovu objevovat pravidla převodů jazyka C. `-o none` je jednoduchý
zásobníkový stroj s akumulátorem — zjevně správný a zároveň referenční, proti němuž se testuje
`-o prod`. `-o prod` zachovává jeho kostru a odstraňuje jeho náklady:

| | `-o none` | `-o prod` |
|---|---|---|
| Proměnné | na zásobníku | skaláry v `%rbx`, `%r12`–`%r15`; `double`/`float` v `%xmm12`–`%xmm15` |
| Mezivýsledky | `push`/`pop` | pomocné registry `%r8`–`%r11`, `%xmm8`–`%xmm11` |
| Přístup do paměti | adresa v `%rax`, pak načtení | operandy `disp(base, index, scale)`, čtení-úprava-zápis, přímé zápisy konstant |
| Řízení toku | test na začátku smyčky | smyčky s podmínkou na konci, přímé `cmp`+`jcc`, skokové tabulky pro husté `switch` |
| Průchody AST | — | skládání konstant, algebraické identity, zjednodušování operací, mrtvé větve, přesun invariantního kódu ze smyček |
| Úklid | — | peephole optimalizace |

```text
src/
├── support/   vektory, řetězce, hash mapa, zdrojové soubory, diagnostika, časovače
├── preproc/   lexer, expanze maker (Prosserovy hidesety), direktivy, vyhodnocení #if
├── parse/     deklarace, výrazy, příkazy, inicializátory, typy, rozsahy platnosti,
│              vyhodnocení konstant a statická kontrola, ze které pochází většina varování
├── opt/       průchody AST pro -o prod: skládání a zjednodušování, přesun kódu ze smyček
├── codegen/   výrazy, příkazy, volání a SysV ABI, data, registry, peephole
└── driver/    příkazová řádka, průběh překladu, živý výpis stavu, externí nástroje
include/       samostatné hlavičky, které occ dodává (stddef.h, stdarg.h, stdckdint.h, ...)
examples/      malé programy k vyzkoušení     tests/   běhové, diagnostické, výkonnostní a fuzz testy
```

Proč je occ postavený právě takhle — a co se cestou pokazilo — popisuje
[**THINKPROC.md**](THINKPROC.md) (anglicky).

## Výkon

Nejlepší ze 3 běhů benchmarků v [`tests/bench`](tests/bench) (`make bench`; všechna sestavení
vypíšou stejný kontrolní součet). Kratší je rychlejší.

<div align="center">
<img src="docs/assets/benchmarks.svg" alt="Časy benchmarků pro occ -o none, occ -o prod, gcc -O0 a gcc -O2" width="720">
</div>

`-o prod` je všude rychlejší než `gcc -O0` a ve čtyřech ze šesti benchmarků se od `gcc -O2`
liší nejvýš o 20 %. Zbývající rozdíl je v kódu s mnoha voláními (`fib`: occ nedělá inlining)
a ve smyčkách, které se dají vektorizovat (`matmul`). Na velkém skutečném programu je obrázek
podobný: `speedtest1` ze SQLite běží 1,74 s po překladu `occ -o prod`, 4,19 s s `-o none`
a 0,90 s s `gcc -O2`. Čísla pocházejí ze sdíleného stroje, počítejte se šumem kolem ±10 %.

## Ověřeno na skutečném kódu

Tyto projekty byly přeloženy occem na obou úrovních optimalizace a ověřeny vlastními testy
(nebo bajt po bajtu proti sestavení stejného kódu pomocí GCC):

| Projekt | Velikost | Výsledek s `-o none` i `-o prod` |
|---|---:|---|
| [SQLite](https://sqlite.org) 3.53 + shell | 307 tis. řádků | SQL úlohy dávají stejný výstup jako sestavení GCC; ověřovací hash `speedtest1` se shoduje |
| [Lua](https://www.lua.org) 5.4.9 | 30 tis. řádků | testovací skript: stejný výstup |
| [Duktape](https://duktape.org) 2.7 | 108 tis. řádků | testovací skript v JavaScriptu: stejný výstup |
| [MuJS](https://mujs.com) 1.3.9 | 20 tis. řádků | testovací skript v JavaScriptu: stejný výstup |
| [Jim Tcl](https://jim.tcl.tk) 0.83 | 43 tis. řádků | vlastní testy: projde 5551 z 5729 — stejně jako s GCC |
| [zlib](https://zlib.net) 1.3.2 | 25 tis. řádků | `example` projde; výstup `minigzip` bajt po bajtu shodný s GCC |
| [bzip2](https://sourceware.org/bzip2/) 1.0.8 | 8 tis. řádků | vzorky z `make test` projdou; výstup bajt po bajtu shodný s GCC |
| [LZ4](https://lz4.org) 1.10 | 18 tis. řádků | úrovně 1/9/12 shodné s GCC, komprese tam i zpět v pořádku |
| [xxHash](https://xxhash.com) 0.8.3 | 12 tis. řádků | sanity test: projde 49948 vektorů; všechny čtyři hashe se shodují |
| [Csmith](https://github.com/csmith-project/csmith) | náhodné | 500 vygenerovaných programů; jediný rozdíl oproti GCC byla chyba, už opravená |

Každý z těchto běhů něco našel nebo potvrdil: sestavení Luy odhalilo dvě falešná varování,
xxHash chybu v preprocesoru, Jim Tcl chybějící volbu `-rdynamic` a Csmith špatnou celočíselnou
promoci bitových polí — vše je opravené a pokryté regresními testy.

## Podpora C23

<details open>
<summary>Co podporuje</summary>

- objekty `constexpr`, odvození typu `auto`, `typeof` / `typeof_unqual`
- `nullptr` / `nullptr_t`, `bool` / `true` / `false` jako klíčová slova
- výčty s pevným podkladovým typem (`enum e : uint8_t`)
- atributy: `[[nodiscard]]`, `[[deprecated]]`, `[[fallthrough]]`, `[[maybe_unused]]`,
  `[[noreturn]]`, `[[unsequenced]]`, `[[reproducible]]`
- `#embed`, `#elifdef` / `#elifndef`, `#warning`, `__has_include`, `__has_embed`,
  `__has_c_attribute`, `__VA_OPT__`
- `static_assert` bez zprávy, prázdné inicializátory `= {}`
- binární literály a oddělovače číslic (`0b1010'0101`)
- návěští před deklaracemi a na konci bloků
- nepojmenované parametry v definicích, `f()` ve významu `f(void)`
- kontrolovaná aritmetika `<stdckdint.h>` (přesná pro každou kombinaci typů operandů),
  `unreachable()`, znakové konstanty `u8`
- vše, co čekáte z C99/C11: designované inicializátory, složené literály, flexibilní pole ve
  strukturách, anonymní struktury a uniony, `_Generic`, `_Alignas`/`_Alignof`, `_Thread_local`,
  variadické funkce, bitová pole, `setjmp`/`longjmp`, předávání a vracení struktur hodnotou
  (SysV ABI)

</details>

<details>
<summary>Co (zatím) ne</summary>

- pole proměnné délky (VLA) — odmítnutá s chybou (od C11 nepovinná)
- `_BitInt(N)`, `_Complex`, desítková plovoucí čárka
- `long double` se překládá jako `double`; `_Atomic` se přijme bez atomické sémantiky (obojí hlásí
  `-Wunsupported`)
- příkazové výrazy GNU a rozšířený `asm` (základní `asm("...")` funguje)
- jiné cíle než x86-64 Linux s glibc — záměrně

</details>

Ukázky novinek najdete v [`examples/c23_tour.c`](examples/c23_tour.c).

## Testy

```sh
make test                   # běhové a diagnostické testy s -o none i -o prod
tests/run.sh --reference    # ověří samotné běhové testy pomocí GCC
make selfhost               # occ přeloží occ; fáze 1 a 2 musí vygenerovat stejný assembler
make fuzz FUZZ_COUNT=1000   # náhodné programy bez UB, occ proti GCC
tests/fuzz/csmith.py        # programy z Csmith, occ proti GCC (vyžaduje csmith)
make bench                  # graf výše
```

Diagnostické testy jsou přísné oběma směry: každé očekávané varování se musí objevit a každé
varování, které occ vypíše, musí být očekávané. CI spouští vše výše uvedené při každém pushi.

## Vytvořeno Claudem

<img src="https://cdn.simpleicons.org/claude/D97757" alt="Claude" width="40" align="left">

occ navrhl, napsal, otestoval a zdokumentoval Claude (model umělé inteligence od Anthropicu),
který tento repozitář spravuje: každý commit, test i stránku dokumentace. Úvahy za návrhem,
chyby nalezené po cestě a to, jak se na ně přišlo, popisuje [THINKPROC.md](THINKPROC.md).

<br clear="left">

## Licence

[MIT](LICENSE)
