# dantto4k

## 使用方法

### ACASのEMM更新（任意）

`dantto4k.ini` または `BonDriver_dantto4k.ini` の `[acas]` に
`enableEmm=true` を指定すると、放送で受信したEMMのうち接続中のACAS宛ての
契約情報更新をカードに送信します。既定値は `false` です。変更後はアプリを再起動してください。

```ini
[acas]
enableEmm=true
```

EMMの受信には、対象のEMMが含まれるMMT/TLV入力が必要です。入力に含まれない
更新を取得する機能はありません。CRC・長さ・宛先を検証し、更新用の `0x84` のみを扱います。
`--frontend-descrambled` と併用した場合は、ECMによる復号を行わずEMM更新だけにカードを使います。
メッセージ用の `0x85` はカードに送りません。カード通信はECMと直列化し、待機中のECMを優先します。
同一EMMは処理中の重複を除き、成功後30分、対象外・失敗後5秒は再処理しません。
チャンネル変更時は待機中のEMMを破棄します（すでにカードへ送信中の更新は取り消せません）。

結果とエラーは標準エラー出力に出します。成功時は `ACAS EMM update accepted`、
失敗時はPC/SCエラー・カードの応答コード・不正な応答長などを表示します。
カードIDやEMM本文はログに出しません。

### dantto4k.exe
MMTSから復号化およびMPEG-2 TSへの変換を行います。
dantto4k.exeと同じフォルダにdantto4k.iniがある場合、その設定を読み込みます。
```
Usage:
  dantto4k [OPTION...] input [output] ('-' for stdin/stdout)

      --listSmartCardReader     List available smart card readers
      --casProxyServer arg      Specify the address of a CasProxyServer
      --smartCardReaderName arg
                                Specify the smart card reader to use
      --customWinscardDLL arg   Specify the path to a winscard.dll
      --disableADTSConversion   Disable ADTS conversion
      --frontend-descrambled    Assume the input stream was already
                                descrambled by the frontend, and only
                                remux MMT/TLV to MPEG-2 TS
      --decode-mmts             Output ACAS-decrypted MMT/TLV instead of MPEG-2 TS
      --write-mmtsmap           Write an .mmtsmap sidecar for --decode-mmts output
      --write-mmtsmap-only      Scan input and write only an .mmtsmap sidecar
      --mmtsmap arg             Specify .mmtsmap output path
      --report                  Print what the conversion produced and left out
                                to stderr, and verify the TS as it is written
      --verify                  Check an MPEG-2 TS file; exits with 1 when it
                                finds problems
      --no-progress             Disable progress display
      --no-stats                Disable packet statistics
      --version                 Show the version
      --help                    Show help
```

フロントエンド側で既に復号済みの MMTS を MPEG-2 TS に変換するだけの場合は、
`--frontend-descrambled` を指定します。このモードではスマートカードを初期化しません。

復号済み MMTS/TLV をファイルへ書き出す場合は `--decode-mmts` を指定します。

```
dantto4k --decode-mmts input.mmts output.mmts
```

復号できないパケットは元の TLV パケットのまま出力を継続し、終了時に復号できなかったパケット数を stderr に表示します。

復号済み MMTS/TLV と同時にトラック構成やおおまかなシーク位置を記録する `.mmtsmap` を作成する場合は `--write-mmtsmap` を指定します。既定では `output.mmtsmap` に出力します。

```
dantto4k --decode-mmts --write-mmtsmap input.mmts output.mmts
```

出力先を明示する場合は `--mmtsmap` を指定します。

```
dantto4k --decode-mmts --mmtsmap output.mmtsmap input.mmts output.mmts
```

既存の MMTS/TLV から、メディアを再出力せず `.mmtsmap` だけ作成する場合は `--write-mmtsmap-only` を指定します。既定では `input.mmtsmap` に出力します。

```
dantto4k --write-mmtsmap-only input.mmts
```

出力先を明示する場合は `--mmtsmap` を指定します。

```
dantto4k --write-mmtsmap-only --mmtsmap output.mmtsmap input.mmts
```

#### 生成した TS の検証

`--verify` は TS ファイルを読み直して検査します。変換処理や TSDuck とは別に実装しているので、変換側の不具合も検出できます。問題がなければ終了コード 0、問題があれば 1、読めなければ 2 を返します。

```
dantto4k --verify output.ts
```

同期、transport_error、continuity counter、セクションの CRC、PAT/PMT の有無、PES のヘッダーと長さ、ES の先頭 (HEVC のスタートコード、ADTS と LOAS の同期ワード)、音声・映像の時刻の逆行、PCR の有無と逆行を確認し、末尾に `problems` と `warnings` の件数を出力します。PCR 間隔が 1 秒を超えると問題、100 ms を超えると警告です。1 秒を超える音声・映像の時刻の飛びも警告です。

#### 変換レポート

`--report` を付けて MPEG-2 TS へ変換すると、終了時に標準エラー出力へ次の内容を報告します。stdout へ出力する場合も有効です。

- TLV と MMT パケットの統計
- 字幕の変換件数と、音声・字幕・EIT などで捨てたものの件数
- ES として出力しなかったアセットとその理由
- TS へ変換しなかった記述子のタグと件数 (テーブル別)
- 出力した TS を `--verify` と同じ処理で検査した結果

```
dantto4k --frontend-descrambled --report input.mmts output.ts
```

#### データ放送ファイルの書き出し (実験的)

`--dump-app-data` を指定すると、データ放送 (ARIB STD-B62) のアプリケーションファイルを指定ディレクトリに書き出します。
DDMT・DAMT・インデックスアイテムから元のパスを復元し、zlib 圧縮されたファイルは展開して保存します。
MH-AIT などの制御情報と各ファイルの対応は `manifest.txt` に出力されます。

```
dantto4k --frontend-descrambled --dump-app-data appdata input.mmts
```

#### データ放送の TS への受け渡し (実験的)

MPEG-2 TS に変換するとき、データ放送 (ARIB STD-B62) を TS のプライベートセクションとして出力します。dantto4k と BonDriver_dantto4k.dll の両方が対象です。
データ放送か字幕のあるサービスでは、PMT に stream_type 0x05、registration_descriptor `'D4KA'` の ES が PMT の PID - 0x100 の PID で追加されます。
MH-AIT・DDMT・DAMT・DCCT・EMT はそのまま、アプリケーションのファイルは組み立てたアイテム単位 (table_id 0xC0) で、字幕 (ARIB-TTML) は PTS 付きで (table_id 0xC1) 送られます。字幕はアプリケーションが自分で表示するためのものですが、データ放送の無いサービスでも送るので、受信側で B24 に変換していない字幕を直接表示することもできます。
内容が変わらないものは間引き、アイテムは既定で 10 秒ごとに再送します。形式の詳細は [docs/app-data-ts-transport.md](docs/app-data-ts-transport.md) を参照してください。

出力しない場合は `--no-app-data` を指定するか、ini の `[appdata]` で `enable=false` にします。

### BonDriver_dantto4k.dll
リアルタイムで復号化とMPEG-2 TSへの変換を行うBonDriverです。
BonDriver_dantto4k.iniで設定されたBonDriverをロードして、復号化とMPEG-2 TSへの変換を行います。
dantto4kは64bitで配布しており、BonRecTestおよびBonDriver_BDAは64bitである必要があります。

EDCB 連携向けに、復号済み TLV/MMTS を複数ファイルへ同時保存する export も提供します。

```cpp
extern "C" __declspec(dllexport) BOOL WINAPI StartMmtsRecording(const wchar_t* path, BOOL overwrite, DWORD* sessionId);
extern "C" __declspec(dllexport) void WINAPI StopMmtsRecording(DWORD sessionId);
extern "C" __declspec(dllexport) BOOL WINAPI GetMmtsRecordingStatus(DWORD sessionId, DWORD* actualMode, BOOL* failed, BOOL* fallbackUsed);
```

録画セッション中は単一の TLV/MMTS ファイルへ保存します。復号できるパケットは復号済みで書き込み、復号できないパケットは元の TLV パケットのまま書き込みを継続します。`fallbackUsed` は未復号のまま残したパケットがあった場合に true になります。

録画保存時は、保存先が `record.mmts` の場合に `record.mmtsmap` も同時に作成します。`.mmtsmap` にはトラック構成、MPT 変化点、RAP、約 5 秒間隔のシーク候補が記録されます。

#### Mirakurunでの動作
PT4Kで動作する場合、チャンネル再生まで15～20秒かかることがあります。
Mirakurunの既定 timeout のままだと、環境によってはストリーム開始前に切断される場合があります。

少なくとも以下の timeout を 30 秒以上に調整してください。

- `src/Mirakurun/Server.ts` の HTTP server timeout
- `src/Mirakurun/Tuner.ts` の `getServices()` timeout

このリポジトリで併用している Windows 向け Mirakurun fork では、上記 2 か所を 30 秒に調整済みです。

### CasProxyServer
スマートカードのプロキシサーバーが必要な場合、以下のリポジトリから構築できます。
https://github.com/nekohkr/casproxyserver

## ビルド
### Windows

TSDuck の DLL は不要です。ビルド時に tsduck のソースから静的ライブラリを自動生成してリンクします。

#### 依存ライブラリの準備

サブモジュール（asio・tsduck ソース）を取得します。

```
git submodule update --init --recursive
```

#### ビルド

Visual Studio 2022 以降で `msvc/dantto4k.sln` を開いてビルドします。

初回ビルド時に tsduck の静的ライブラリ（`tscorelib.lib` / `tsducklib.lib`）が自動的にビルドされます（10分程度）。2回目以降はライブラリが存在するためスキップされます。

ビルドの前に `tools/generate-version-header.ps1` が `git describe --tags --always --dirty` の結果 (例: `v20261015a-2-gce5bea5`) を `src/dantto4kVersion.h` に書き出します (コミットしないファイルです)。この版は `dantto4k --version` と、dantto4k.exe・BonDriver_dantto4k.dll のファイルのプロパティ (詳細タブ) で確認できます。git が無い環境では `unknown` になります。

> **Note**
> TSDuck インストーラーは不要です。`TSDUCK` 環境変数も使用しません。

### Ubuntu

```bash
sudo apt install make g++ libssl-dev libpcsclite-dev pcscd pkgconf

git clone https://github.com/nekohkr/dantto4k.git
cd dantto4k
git submodule update --init --recursive

cd thirdparty/tsduck
scripts/install-prerequisites.sh
make -j10
make install

cd ../..
make
make install
```

## References
- ARIB STD-B32
- ARIB STD-B60
- [superfashi/FFmpeg](https://github.com/superfashi/FFmpeg)
- b61decoder
