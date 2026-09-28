# PopSG — SHARP Brain (Windows CE) 移植版

**PopSG は非商用に限ります。** 販売すること、商用の製品や活動に使うこと、お金を払った人だけに配ることはできません(PicoDrive と DrZ80 のライセンスによる。詳しくは [`LICENSING.md`](LICENSING.md))。

## 概要

- セガのゲーム機のエミュレータ [PicoDrive](https://github.com/irixxxx/picodrive) の libretro 版を、SHARP の電子辞書 **Brain PW-G5200**(Windows CE / ARMv5TE、ハードウェア整数除算なし)向けに移植したものです
- 別機種 `pw-gc610` でも動作を確認しています
- `platform/libretro/libretro.c` をそのままコンパイルし、Win32 のフロントエンド(`CE/` 以下)を新しく書いて繋いでいます。コア側の変更は、32X を外したときの定義の補い(`pico/pico_int.h`)と、この端末で止まる1命令の修正(`pico/draw_arm.S`)だけです
- ゲームの ROM・BIOS は同梱していません。利用者が合法的に用意したものを使ってください
- SHARP・セガとは関係のない、非公式のファンプロジェクトです

### 対応システム

- **16bit**: メガドライブ / ジェネシス
- **8bit**: マスターシステム / セガ・マーク III、ゲームギア、SG-1000、SC-3000
- **メガ CD**: 起動はしますが、実用速度に届きません。68000 を 2 個と CD 用の回路を動かすため、この端末ではエミュレーションが実時間に追いつかず、音も途切れます。生の BIN/CUE のみ対応(CHD、圧縮した音声トラックには非対応)。それでも試すなら、Sound Config の Rate を 22000、Video Config の Frame Skip を 1 にすると、実機では一番ましでした
- **32X には対応していません**(`-DNO_32X` でビルドから外しています)
- Sega Pico は未確認です

CPU コアは Cyclone(68000)と DrZ80(Z80)です。どちらも手書き・自動生成の ARM アセンブリで、実行時にコードを作る JIT ではありません。

### 主な機能

- 4 つの表示倍率(1:1 / Full Screen / Wide / Expand)、GDI で直接描画
- 日本語 / 英語の UI(Galmuri14 と東雲 16 ドットのビットマップフォントを `AppMain.exe` に内蔵)
- 日本語 UI のときは、ゲーム機の地域を日本(Japan NTSC)に切り替え
- ステートセーブ / ロード(セーブ前に確認)、SRAM の自動セーブ
- 画面の保存(スクリーンショット。`AppMain.exe` と同じフォルダの `Screenshots` に BMP で保存)
- 日本語のファイル名・フォルダ名に対応した ROM 選択画面(前回開いたフォルダから始まります)
- Video Config: スプライト数の制限をなくす / フレームスキップ / UI の言語 / デバッグログ
- Sound Config: 音量 / レート / ビット数 / 音質 / バッファの大きさ

## ダウンロード

ビルド済みの `AppMain.exe` は [Releases](../../../releases) に置きます。

## ビルド方法

必要なもの:

- WSL(Windows 上の Linux)などに入れた cegcc のクロスコンパイラ(`/opt/cegcc/bin/arm-mingw32ce-*`。GCC 9.3.0、binutils 2.34 で確認)
- サブモジュール `cpu/cyclone`(Cyclone 68000)と `pico/sound/emu2413`(YM2413 音源)

**クローンするときは `--recursive` を付けてください。** 付けないと、サブモジュールのフォルダが空のままになり、ビルドが失敗します。GitHub の「Download ZIP」にもサブモジュールの中身は入らないので、ZIP では取得しないでください。

```sh
git clone --recursive https://github.com/daizu-coder/PopSG.git
cd PopSG/CE
make clean && make && make strip
```

`--recursive` を付けずにクローンしてしまった場合は、リポジトリの中で次を実行してください(PopSG が使うサブモジュール2つだけを取得します)。

```sh
git submodule update --init cpu/cyclone pico/sound/emu2413
```

`--recursive` を付けると、PopSG では使わないサブモジュール(`platform/libpicofe`、`pico/cd/libchdr`、`platform/common/dr_libs`)も一緒に取得されますが、ビルドには影響しません。

- できあがるのは `CE/AppMain.exe` です(依存する DLL は `COREDLL.dll` だけ)
- 既定のフォントは Galmuri14 です。`make CE_FONT=shinonome` で東雲 16 ドット版(`AppMain_shinonome.exe`)、`make CE_FONT=galmuri11` で GalmuriMono11 版(`AppMain_galmuri11.exe`)も作れます
- `draw_arm.S` が使う `pico/pico_int_offs.h` は、ビルドの途中で `tools/mkoffsets_ce.sh` が作ります
- Cyclone の `Cyclone.s` は作成済みのものを `CE/cyclone/` に置いているので、ホストの C++ コンパイラは要りません。作り直す方法は [`cyclone/README.txt`](cyclone/README.txt) にあります
- 設計の理由やつまずいた点は、`CE/` の各ソース(特に `Makefile`、`ce_main.c`、`ce_display.c`)の冒頭のコメントにあります

## 使用方法

SHARP Brain(PW-G5200 など)を PC にリムーバブルディスクとしてつなぎ、ドライブの直下に次のように置きます(メニューの名前は機種によって違うことがあります)。

```
<ドライブ直下>/
  アプリ/
    <好きなアプリ名>/
      AppMain.exe    ← ビルドしたもの
      index.din      ← 中身は空でよいファイル
```

- `index.din` を置くと、そのフォルダが [追加アプリ・動画] の一覧に出ます
- ROM は SD カードに置き、アプリのメニューの「ROMを開く」から選びます。拡張子が `.bin` のメガドライブのゲームは一覧に出ないので(`.bin` はメガCD の BIOS 用として分けています)、`.md` などに変えてください
- 設定ファイル `popsg.cfg` は、初めて起動したときに同じフォルダに作られます
- `popsg_debug.log` は、Video Config で「デバッグログを有効にする」をオンにしたときだけ作られます(既定はオフ)
- メガ CD の BIOS は、`AppMain.exe` と同じフォルダ(直下のみ)に置いておけば、ファイル名に関係なく中身を見て自動で読み込みます。見つからないときだけ、初めてメガ CD のゲームを開いたときにファイル選択画面が出ます(選んだフォルダは `popsg.cfg` に記憶)。地域の違う BIOS が複数あるときは、UI が日本語なら 日本→欧州→米国、英語なら 欧州→米国→日本 の順で選び、同じ地域の中では日付の新しいものを選びます。ファイルのコピーや名前の変更はしません

## 動作確認環境

- SHARP Brain PW-G5200
- SHARP Brain PW-GC610

## クレジット

- **PicoDrive** — エミュレーションコア。**notaz**(Gražvydas Ignotas)氏、**irixxxx** 氏ほか。MAME 系ライセンス(非商用)
  <https://github.com/irixxxx/picodrive>、libretro 版 <https://github.com/libretro/picodrive>
- **Cyclone 68000** — **FinalDave** 氏、**notaz** 氏。GPLv2 または MAME 系(MAME 系を選択)
- **Genesis Plus GX** の一部(メガCD の CD コントローラ・CD ドライブ・グラフィック回路、SPI の EEPROM)— **Eke-Eke** 氏。非商用のライセンス
- **SVP** — **notaz** 氏。BSD 3条項
- **YM2612 音源** — **Jarek Burczynski** 氏、**Tatsuyuki Satoh** 氏(MAME の fm.c)
- **DrZ80** — **Reesy** 氏、**irixxxx** 氏。非商用なら無料
- **emu2413**(YM2413 音源)— **Mitsutaka Okazaki** 氏(Digital Sound Antiques)。MIT
- 音声のリサンプラーのフィルタ(**blipper**)— **Hans-Kristian Arntzen** 氏。MIT
- **libretro API** のヘッダ・**libretro-common** の一部 — **The RetroArch team**。MIT
- **zlib** — **Jean-loup Gailly** 氏、**Mark Adler** 氏。zlib ライセンス
- **Galmuri** ビットマップフォント(メニューの既定の文字)— **Lee Minseo**(quiple)氏。SIL Open Font License 1.1
  <https://github.com/quiple/galmuri>
- **東雲(しののめ)16 ドットビットマップフォント** — メインデザイン **古川 泰之** 氏ほか、**The Electronic Font Open Laboratory(/efont/)**。実質パブリックドメイン
  <https://github.com/code4fukui/shinonome-font>
- **CeGCC** — Windows CE / ARM 向けクロスコンパイラ。**Danny Backx** 氏ほか、モダン版の **Max Kellermann** 氏
- **SHARP Brain homebrew コミュニティ** — 端末の情報を残してくださった皆さん
- Windows CE フロントエンド(`CE/`)は本プロジェクトで作成

コンポーネントごとの出所とライセンスの詳細は [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) と [`LICENSING.md`](LICENSING.md) をご覧ください。

## 制作について

コードとマスコットの絵はAI(Claude)で作りました。製作者はプログラムを読めません。

マスコットの絵とアイコン(`CE/icon/`)は、Pop シリーズのマスコットをもとに AI(Claude)で作りました。CC0 1.0(パブリックドメイン)です。各ライセンスについては、[`LICENSING.md`](LICENSING.md) をご覧ください。

## ライセンス

PopSG 全体は、PicoDrive のライセンス([`COPYING`](../COPYING)、古い MAME のライセンスと同じ文面)で配布します。

- **非商用に限ります。** 販売、商用の製品や活動での利用、有料での配布はできません(PicoDrive の `COPYING` と `cpu/DrZ80/DrZ80.txt` による)
- 改変したものを配るときは、ソース一式を付けてください
- 著作権表示・条件・免責の文を残してください
- 保証はありません

PopSG の自作部分(`SPDX-License-Identifier: MIT` と書いてあるファイル)は MIT です([`LICENSE`](LICENSE))。バイナリを配るときは、[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) も一緒に配ってください。

## 商標・免責

PopSG は非公式のファンプロジェクトです。シャープ株式会社、株式会社セガとは関係がなく、許諾・後援も受けていません。

- 「SHARP」「Brain」はシャープ株式会社の商標です
- 「SEGA」「セガ」「メガドライブ」「Mega Drive」「Genesis」「マスターシステム」「Master System」「ゲームギア」「Game Gear」「メガ CD」「Mega-CD」「Sega CD」「SG-1000」は株式会社セガ(またはその関連会社)の商標です

ゲームの ROM・BIOS は含みません。利用者が合法的に入手したものを用意してください。
