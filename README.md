# dantto4k

## 使用方法

### dantto4k.exe
MMTSから復号化およびMPEG-2 TSへの変換を行います。
dantto4k.exeと同じフォルダにdantto4k.iniがある場合、その設定を読み込みます。
```
Usage:
  dantto4k [OPTION...] input output ('-' for stdin/stdout)

      --listSmartCardReader     List available smart card readers
      --casProxyServer arg      Specify the address of a CasProxyServer
      --smartCardReaderName arg
                                Specify the smart card reader to use
      --customWinscardDLL arg   Specify the path to a winscard.dll
      --disableADTSConversion   Disable ADTS conversion
      --decode-mmts             Output ACAS-decrypted MMT/TLV instead of MPEG-2 TS
      --no-progress             Disable progress display
      --no-stats                Disable packet statistics
      --help                    Show help
```

復号済み MMTS/TLV をファイルへ書き出す場合は `--decode-mmts` を指定します。

```
dantto4k --decode-mmts input.mmts output.mmts
```

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

録画セッション中は復号済み TLV/MMTS と raw shadow を並行保存します。復号できないパケットを検出した場合は raw fallback として、停止時に raw shadow を本ファイルへ昇格します。

#### Mirakurunでの動作
PT4Kで動作する場合、チャンネル再生まで15～20秒かかるため、Mirakurunのtimeout(20秒)を超える場合があります。
Mirakurunのソースコードを修正してtimeoutを30秒以上に変更する必要があります。

https://github.com/Chinachu/Mirakurun/blob/master/src/Mirakurun/Tuner.ts

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
