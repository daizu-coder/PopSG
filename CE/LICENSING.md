# PopSG のライセンス

PopSG は、セガのゲーム機のエミュレータ [PicoDrive](https://github.com/irixxxx/picodrive)(notaz 氏、irixxxx 氏ほか)の libretro 版を、SHARP Brain PW-G5200(Windows CE)向けに移植した、**非公式・非商用**の改変版です。PicoDrive の作者やメンテナーはこの移植に関わっていません。

ライセンスは2段になっています。

## 1. アプリ全体: 上流と同じ条件(PicoDrive の非商用ライセンス)

アプリ全体(`AppMain.exe`、およびこのリポジトリで公開しているソース全体)は、上流の PicoDrive と同じ条件、つまり PicoDrive のライセンスで配布します。本文はリポジトリ直下の [`COPYING`](../COPYING) にあります。古い MAME のライセンスと同じ文面で、条件は次の3つです。

- **販売しないこと。商用の製品や活動に使わないこと**
- 改変したものを配るときは、ソース一式を付けること(コンパイラや OS の部品は除く)
- 著作権表示・条件・免責の文を、説明書などに載せること

全体のライセンスを、これ以上はここで断定しません。上流はいくつもの部品からできていて、部品ごとにライセンスの条件が異なるためです。`AppMain.exe` に入るファイルには、次のものがあります。

- 多くのファイル(`pico/`、`platform/libretro/libretro.c` など)は「MAME license」で、`COPYING` を指しています
- メガCD の CD コントローラ・CD ドライブ・グラフィック回路(`pico/cd/cdc.c`、`pico/cd/cdd.c`、`pico/cd/gfx.c`)と SPI の EEPROM(`pico/carthw/eeprom_spi.c`)は、Eke-Eke 氏の Genesis Plus GX から来たもので、同じ内容の非商用のライセンスが各ファイルの先頭に書かれています
- CPU コアの DrZ80(`cpu/DrZ80/drz80.S`)は Reesy 氏、irixxxx 氏の作品で、非商用の利用に限り、無料で使うことを許していただいています
- Cyclone 68000、音声のリサンプラー(`pico/sound/resampler.c`)、画像の拡大(`platform/common/upscale.h`)は、GPL と MAME のライセンスから選べます。PopSG は MAME のライセンスを選んでいます
- SVP(`pico/carthw/svp/`)は BSD 3条項ライセンスです
- emu2413、VGM の読み込み(`pico/sound/vgm.c`)、リサンプラーの中のフィルタ(blipper)、libretro のヘッダと libretro-common の一部は MIT、zlib は zlib ライセンスです
- `pico/patch.c`、`pico/carthw_cfg.c`(自動で作られたデータ)、`pico/cd/megasd.c`、`pico/sound/sn76496.c`、`pico/sound/ym2612.c`、`pico/sound/ym2413.c`、`platform/common/mp3_sync.c`、`unzip/unzip.c`、`platform/libretro/libretro_core_options*.h` には、ライセンスの表記がありません(`ym2612.c` は MAME の fm.c から来たもので、著作権表示があります)

それぞれの表記と許諾文の本文は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) にまとめています。GPL だけのファイルは `AppMain.exe` に入っていません。

- PopSG を売ること、売っているものに付けること、お金を払った人だけに配ることはできません
- リポジトリへの寄付やスポンサーは、ソフトを売ることではないので通常は問題ありません。ただし「お金と引き換えに機能を作る」ような形は、商用の活動に近くなるので避けてください
- 「フリーソフト」「オープンソース」と書くときは、非商用に限ることも書いてください(OSI や FSF の定義するフリーなライセンスではありません。GPL でもありません)
- `AppMain.exe` を配布するときは、対応するソース(このリポジトリとサブモジュール)も入手できるようにしてください

非商用で配ることは PicoDrive のライセンス文そのものが認めているので、作者への個別の確認はしていません。

## 2. PicoDrive 本体(上流のファイル)

リポジトリ直下のファイルは、上流の [libretro/picodrive](https://github.com/libretro/picodrive) のコミット `6248b51`(2026-07-29)を元にしています。libretro/picodrive は、irixxxx 氏の [picodrive](https://github.com/irixxxx/picodrive)(notaz 氏の [picodrive](https://github.com/notaz/picodrive) の後継)の libretro 版です。

PopSG が変えた上流のファイルは次のものだけです。どれも Windows CE 用のツール(cegcc)とこの端末に合わせるためのもので、ライセンスは変わりません。

- `pico/pico_int.h`: 32X を外してビルド(`-DNO_32X`)したときに使う代わりの定義を足した(32X を入れたビルドには影響しない)
- `pico/draw_arm.S`: 1バイトの値を4バイトで読んでいた1命令を、1バイトの読み込みに直した(この端末では、4の倍数でない番地からの4バイト読み込みで止まるため)
- `pico/sound/emu2413`(サブモジュール): 参照先を `a2dfc20` から `813cff6`(v1.5.9 の次の修正を含む版)に進めた
- `.gitignore`: PopSG のビルドの生成物などを除外するように書き足した

上流のソースの著作権表示は、変えずに残しています。

### サブモジュール

ビルドに使うサブモジュールは `cpu/cyclone` と `pico/sound/emu2413` の2つです。

- `cpu/cyclone`(Cyclone 68000、[irixxxx/cyclone68000](https://github.com/irixxxx/cyclone68000) の `3ac7cf1`)は、変えずに使っています。Cyclone から作った `Cyclone.s` と、手を入れた `idle.s` は `CE/cyclone/` に置いています(詳しくは [`cyclone/README.txt`](cyclone/README.txt))。Cyclone は GPL バージョン2と MAME のライセンスから選べます。PopSG は PicoDrive と組み合わせるため、MAME のライセンスを選んでいます
- `pico/sound/emu2413`(YM2413 音源、[digital-sound-antiques/emu2413](https://github.com/digital-sound-antiques/emu2413) の `813cff6`)は MIT です

`platform/libpicofe`、`pico/cd/libchdr`、`platform/common/dr_libs` のサブモジュールは使っていません。

## 3. PopSG の自作部分: MIT

PopSG の自作部分は、MIT ライセンスです。本文は [`LICENSE`](LICENSE) にあります。対象は、先頭に `SPDX-License-Identifier: MIT` と書いてある次のファイルです。

- `CE/` のフロントエンド(`ce_*.c`、`ce_*.h`、`ce_res.rc`、`Makefile`、`compat/`)。ただし次のものは除きます
  - フォントのデータ `ce_shinonome16.h`、`ce_galmuri14.h`、`ce_galmuri11.h`(下の「第三者のもの」を参照)
  - `CE/cyclone/` の `Cyclone.s`、`idle.s`(Cyclone のライセンス)
  - マスコットの画像とアプリのアイコン(下の「マスコットの絵とアイコン」を参照)
- `tools/mkoffsets_ce.sh`

自作部分だけを取り出して、ほかのプロジェクトで MIT として使うことができます。PicoDrive と組み合わせて配布する場合は、1 の条件も守る必要があります。

## 4. マスコットの絵とアイコン: CC0 1.0

メニューのマスコットの絵(`CE/icon/popsg_mascot.bmp`)、アプリのアイコン(`CE/icon/popsg.ico`、`AppMain.exe` に入っているもの)、README の先頭の絵(`.github/images/popsg_mascot_C_osanpo_4x.png`)は、[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)(パブリックドメイン)です。Pop シリーズのマスコットをもとに AI(Claude)で作りました。

README のスクリーンショット(`.github/screenshots/`)は、lunoka 氏のゲーム「Mai Nurse（Mega Drive 版）」を作者の許可を得て掲載しているもので、このリポジトリのライセンスの対象外です。

## 5. 第三者のもの

`AppMain.exe` に入っている第三者のものは次のとおりです。著作権表示と許諾文は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) にまとめています。

| もの | 作者 | ライセンス |
|---|---|---|
| PicoDrive 本体、libretro の接続部分(`platform/libretro/libretro.c`)、unzip | notaz 氏、irixxxx 氏ほか | PicoDrive のライセンス(MAME 系、非商用。表記のないファイルもあります) |
| メガCD の CD コントローラ・CD ドライブ・グラフィック回路、SPI の EEPROM | Eke-Eke 氏(Genesis Plus GX) | Genesis Plus GX の非商用ライセンス |
| Cyclone 68000 | FinalDave 氏、notaz 氏 | GPL バージョン2 または MAME 系(MAME 系を選択) |
| DrZ80 | Reesy 氏、irixxxx 氏 | 非商用の利用に限り、無料で使うことを許していただいています |
| SVP | notaz 氏 | BSD 3条項 |
| YM2612 音源(MAME の fm.c から) | Jarek Burczynski 氏、Tatsuyuki Satoh 氏 | ライセンスの表記はなく、著作権表示があります |
| emu2413 | Mitsutaka Okazaki 氏 | MIT |
| リサンプラーのフィルタ(blipper) | Hans-Kristian Arntzen 氏 | MIT |
| libretro API のヘッダ、libretro-common の一部 | The RetroArch team | MIT |
| zlib | Jean-loup Gailly 氏、Mark Adler 氏 | zlib ライセンス |
| Galmuri フォント(メニューの既定の文字) | Lee Minseo 氏 | SIL Open Font License 1.1 |
| 東雲 16 ドットフォント | 古川泰之 氏ほか、/efont/ | 実質パブリックドメイン |

MIT、BSD、zlib のライセンスは商用も認めていますが、PopSG 全体の非商用の条件をゆるめるものではありません。

## 6. 同梱していないもの

- ゲームの ROM
- メガCD の BIOS。セガの著作物なので、使う人が自分で用意してください

## 7. 上流のファイルについての注意

- `cpu/fame/`、`cpu/cz80/`、`cpu/musashi/`、`cpu/sh2/`、`cpu/drc/`、`pico/32x/`、`platform/` の libretro 以外の部分、`jni/`、`Makefile`、`Makefile.libretro`、`.github/workflows/` などは上流のファイルです。PopSG のビルドには使っていません(`AppMain.exe` には入っていません)。ビルドに使うファイルは `CE/Makefile` で決まります
- `platform/gp2x/warm_*.o`、`warm_*.ko` は上流が同梱している GP2X 用のバイナリで、PopSG では使っていません
- リポジトリ直下の `README.md` は上流の PicoDrive の説明で、PopSG の説明ではありません
