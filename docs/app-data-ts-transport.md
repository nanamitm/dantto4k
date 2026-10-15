# 4K データ放送 (ARIB STD-B62) を MPEG-2 TS で受け渡す設計

BonDriver_dantto4k / dantto4k が MMT/TLV から MPEG-2 TS へ変換するとき、現在はデータ放送 (aapp アセットと
MH-AIT・DDMT・DAMT・DCCT) をすべて捨てている。TVTDataBroadcastingWV2 などの TS 側アプリケーションで 4K データ放送を
表示できるように、これらを TS 内の専用 PID に載せて渡す。

## 方針

- **TS に入れる** (名前付きパイプや共有フォルダなどの別経路にはしない)。
  - TVTest のストリームコールバックはすべての TS パケットを受け取るので、プラグインに新しい通信路が要らない。
  - PMT に載せておけば、TVTest/EDCB の「現在のサービスのみ」録画でも残り、録画ファイルの再生でも表示できる。
  - dantto4k (ファイル変換) でも同じ出力になる。
- **BonDriver 側の処理は最小限にする**。
  - MMT-SI のテーブルはセクションをそのまま転送する。
  - アイテムは MFU の断片を 1 アイテムに組み立てるところまでとし、zlib 展開やパス解決はしない。
  - パス解決と展開は受信側で行う。放送の情報を落とさず、受信側でバージョン管理やキャッシュを判断できる。
- **同じ内容は間引く**。放送ではカルーセルが非常に速く回っており、BS フジ 4K は 835 バイトの index.html を
  1 分間に約 11,700 回送っている。そのまま転送すると TS が 1〜2 Mbps 増えるため、内容が変わらないものは一定間隔ごとに
  再送する。

## PID と PMT

| 項目 | 値 |
|---|---|
| PID | `PMT の PID - 0x100` (PMT が 0x1000 + i なので 0x0F00 + i)。サービスごとに 1 本 |
| stream_type | `0x05` (ISO/IEC 13818-1 private sections) |
| ES 記述子 | registration_descriptor (tag 0x05)、format_identifier = `'D4KA'` (0x44344B41) |

- 既存の PID 割り当て (0x100 + component_tag、0x200 + index、PCR 0x01FF、PMT 0x1000〜) とは重ならない。
- `stream_type 0x0D` や data_component_descriptor は使わない。BML 用の受信機 (web-bml) が BML と誤認しないようにするため。
- そのサービスの MPT にアプリケーションサービス記述子 (後述) も字幕 (stpp) アセットもない間は、PMT にこの ES を載せない。
  字幕だけのサービス (BS-TBS 4K、BS 朝日 4K など) では字幕 (table_id 0xC1) だけが流れる。

## セクション形式

すべて section_syntax_indicator = 1 の長形式セクション (CRC_32 付き) で、最大 4096 バイトとする。

### 1. MMT-SI テーブルの転送 (table_id はそのまま)

MH-AIT (0x9C)、DDMT (0xA3)、DAMT (0xA4)、DCCT (0xA5) のセクションを、MMT 上のバイト列のまま 1 セクションずつ
載せる。いずれも MPEG-2 と同じセクション構文と CRC を持つので変換は不要。

- 受信したセクションのうち、CRC が直前と同じものは 1 秒に 1 回だけ送る。CRC が変わったら即座に送る。
- EMT は現在デマルチプレクサでハンドラに渡していないので、渡すようにする (イベントメッセージ用)。

### 2. アイテム (table_id = 0xC0、dantto4k 独自)

1 アイテムを組み立て終えたら、次の形式で送る。大きいアイテムは section_number で分割する。

```
table_id                      8   0xC0
section_syntax_indicator      1   1
private_indicator             1   1
reserved                      2
section_length               12
table_id_extension           16   アイテムの送信通番 (下位 16 ビット)
reserved                      2
version_number                5   送信通番の下位 5 ビット
current_next_indicator        1   1
section_number                8   分割番号
last_section_number           8   分割数 - 1
-- 先頭セクション (section_number == 0) のみ --
component_tag                16
mpu_sequence_number          32
item_id                      32
item_size                    32   組み立て後のアイテム全体のバイト数
-- 全セクション --
item_data_byte               N    アイテムの一部 (放送のまま。zlib 圧縮もそのまま)
CRC_32                       32
```

- 1 セクションあたり最大約 4,080 バイト、256 分割で約 1 MB まで。これを超えるアイテムは送らず、統計で数える。
  実測の最大は NHK の top.js (圧縮後 約 64 KB) で、十分に収まる。
- インデックスアイテム (DAMT の index_item_id) も通常のアイテムとして送る。
- 同じ (component_tag, mpu_sequence_number, item_id) で内容が同じアイテムは、既定で 10 秒に 1 回だけ送る。
  内容が変わったらすぐに送る。時間は PCR (ストリーム時刻) で計るので、ファイル変換でも結果が毎回同じになる。

### 3. 字幕 (table_id = 0xC1、dantto4k 独自)

データ放送のアプリケーションは、`addCaptionListener` で字幕を自分で受け取って表示する
(NHK BSP4K の字幕ページなど)。そのため、字幕 (stpp アセット) の MFU も変換せずにそのまま送る。
受信側が B24 に変換していない字幕を直接表示できるよう、アプリケーションサービス記述子のないサービスでも送る。
分割の仕方はアイテムと同じ。

```
-- 先頭セクション (section_number == 0) のみ --
component_tag                16   0x30〜0x37 字幕、0x38〜0x3F 文字スーパー
data_type                     8   字幕 MFU の data_type (0: ARIB-TTML 文書、それ以外はリソース。6 は SVG フォント)
tmd                           8   追加字幕情報の TMD (下位 4 ビット)
pts_flag                      8   0x80: pts が有効
reserved                      7
pts                          33   文書の時刻 0 を提示する PTS (TS の時間軸)
data_size                    32
extension_length              8   以下の拡張部分のバイト数 (現在 15)。知らない部分は読み飛ばす
ISO_639_language_code        24   追加字幕情報の言語コード ("jpn" など)
resolution                    8   追加字幕情報の resolution (0: 1920x1080、1: 3840x2160)
reference_start_time_seconds 32   reference_start_time (NTP 形式の秒)
reference_start_time_fraction 32  同 (秒未満)
subtitle_sequence_number      8   字幕 MFU のヘッダー
subsample_number              8
last_subsample_number         8
-- 全セクション --
data_byte                     N   TTML 文書またはリソース (放送のまま)
```

拡張部分は、ARIB TR-B39 8.10.1.1 がアプリケーションの CaptionListener に渡すと定める項目である。

- PTS は B24 への変換と同じ規則で決める。TMD が 0010 で reference_start_time があればそれを使い、
  PCR より 30 秒以上先なら壊れているとみなして PCR にする。それ以外は受け取った時点の PCR。
  `setPtsOffset` のずらしも同じように掛ける。
- 受信側は、PTS に TTML の最初の begin を足した時刻にアプリケーションへ渡す。過ぎていればすぐに渡す。
  TMD が 1111 (時刻制御なし) のときは begin を足さない (ARIB TR-B39 8.5.9.2)。
- リソース (data_type が 0 以外) には PTS を付けず、受け取ったらすぐに渡す。
- 字幕は繰り返し送られないので、間引きはしない。

### 4. EMT (table_id = 0xC2、dantto4k 独自)

アプリケーションはイベントメッセージを、どの EMT から来たか (`event_message_tag` = EMT_tag) で選ぶ。
EMT_tag と packet_id の対応はアプリケーションサービス記述子 (MPT) にしかないので、EMT は
そのままではなく EMT_tag を付けて送る。分割の仕方はアイテムと同じ。

```
-- 先頭セクション (section_number == 0) のみ --
EMT_tag                       8
-- 全セクション --
emt_section_byte              N   EMT (table_id 0xA6) のセクション全体 (CRC_32 含む)
```

- 間引きは元の EMT のバイト列で行う (1 秒)。
- アプリケーションサービス記述子に載っていない packet_id の EMT は送らない。

## 受信側 (TVTDataBroadcastingWV2) の処理

1. PMT から stream_type 0x05 かつ format_identifier `'D4KA'` の ES を探し、その PID のセクションを集める。
   web-bml の decodeTS には渡さず、ネイティブ (C++) 側で処理する。
2. 次の対応表を作る。今回の `--dump-app-data` (src/appDataDumper.cpp) と同じ処理で、コードはそのまま移植できる。
   - DDMT: ノードタグ → base_directory_path + ディレクトリパス
   - DAMT: (component_tag, mpu) → node_tag、index_item_id、アイテム一覧
   - インデックスアイテム: item_id → ファイル名、MIME タイプ、圧縮方式
3. アイテムを zlib 展開してパスに対応付け、メモリ上のファイルシステムに入れる。
4. WebView2 の仮想ホスト (WebResourceRequested) でこのファイルシステムを配信する。
   MH-AIT の transport_protocol_descriptor のセレクタ (例: `sh4/40/001/`) と simple_application_location の
   initial_path から起動 URL を組み立てる。
5. MH-AIT の control_code (AUTOSTART / KILL など) とバージョンの変化に応じて、アプリを起動・終了する。

## 確認が必要な点

- **イベントメッセージの時刻同期**。EMT や汎用イベントメッセージは NTP 時刻で発火時刻を指定する。TS の PTS とどう
  対応付けるかは、表示まで動いてから別途決める。

## 確認済みの点

- **テーブルとサービスの対応は、MPT のアプリケーションサービス記述子で決まる**
  (ARIB STD-B60 7.4.3.39。英語版 v1.14-E1 は ARIB のサイトから無料で入手できる)。
  - データ放送のあるサービスの MPT には、この記述子が必ず 1 つ以上入っている。中には次の場所が
    MMT_general_location_info の形で書かれている。
    - MH-AIT の場所 (AIT_location_info)
    - データ伝送メッセージ (DDMT/DAMT/DCCT) の場所 (DT_message_location_info)
    - EMT の場所 (EMT_tag ごとの EMT_location_info)
  - 記述子タグの実測値は **0x8034**。PDF からテキストを抜き出した記述子タグ表では 0x8036 の行に並んでいるが、
    この表は行がずれて抜き出されている (MH-Application 記述子も、dantto4k の実装と実測は 0x8029 だが、表では 0x802A の行にある)。
  - 実測のバイト列 (NHK BSP4K、service_id 0x0065):
    `80 34 2d | 1f 1f f9 | 00 ff a2 | 00 ff d2 | 40 00 fe e1 | 60 00 fe e2 | ... | ff 00 ff eb`
    - `1f`: application_format = 1 (ARIB-HTML5)、残りの 4 ビットは reserved
    - `1f`: document_resolution = 1 (3840x2160)、続く 4 ビットに default_AIT_flag = 1 と DT_message_flag = 1
    - `f9`: 上位 4 ビットは reserved、下位 4 ビットが EMT_num = 9
    - `00 ff a2`: MH-AIT は packet_id 0xFFA2
    - `00 ff d2`: データ伝送メッセージは packet_id 0xFFD2
    - その後に EMT が 9 個 (EMT_tag 0x40 → 0xFEE1、0x60 → 0xFEE2、…、0xFF → 0xFFEB)。
      EMT_tag は aapp アセットの component_tag と対応している。
  - BS フジ 4K (service_id 0x00B5) も同じ形で、MH-AIT が 0xFFA1、データ伝送メッセージが 0xFFD1、
    EMT が 8 個 (0x70〜0x77 → 0xFFE0〜0xFFE7)。
  - BS 日テレ 4K (service_id 0x008D) の MPT にはこの記述子がない。録画の間はデータ放送がなかったことと合っている。
  - 規格の表ではビット幅の列もずれて抜き出されているので、ビット配置は実測に合わせる。
  - dantto4k はこの記述子をまだ読んでいない。記述子クラスを追加し、packet_id → サービスの対応表を作って、
    テーブルを正しいサービスの PID に振り分ける。この記述子のないサービスには ES を作らない。
- **TVTest はこの PID をプラグインに渡す** (TVTest / LibISDB のソースで確認)。
  - フィルタの順番は BonDriverSource → TSPacketParser → Analyzer → Tee → EPG → LogoDownloader → Grabber → Recorder
    (TVTest/src/CoreEngine.cpp の `BuildEngine`)。プラグインのストリームコールバックは Grabber に登録される。
  - Grabber より前で捨てられるのは、TSPacketParser で落とすヌルパケット (PID 0x1FFF) とエラーパケットだけ
    (LibISDB/Filters/TSPacketParserFilter.cpp の `ProcessPacket`)。PID による選別はない。
  - ただし TSProcessor プラグインを Recorder より前に挟んでいる場合は、その設定次第。
- **TVTest の「現在のサービスのみ」録画でも残る**。
  - StreamSelector (LibISDB/TS/StreamSelector.cpp) は、PMT に載っている ES の PID を録画対象にする。
  - stream_type による除外は、映像・音声・字幕 (0x06)・データカルーセル (0x0D) などの決まった種類だけが対象で、
    0x05 は常に残る。逆にいうと、録画設定の「データ放送を保存する」を切っても 0x05 は残る。この設定に連動させたい
    場合は stream_type を 0x0D にする方法もあるが、BML と誤認されないことを別途確認する必要がある。
- **web-bml はこの ES を無視する**。
  - web-bml の decode_ts.js は、PMT の ES のうち stream_identifier_descriptor (component_tag) を持つものだけを扱う。
  - この ES には stream_identifier_descriptor を付けないので、無視される。付けてはいけない。

## 実装 (dantto4k 側)

| 部分 | ファイル |
|---|---|
| アプリケーションサービス記述子 (0x8034) | `src/applicationServiceDescriptor.*` |
| アイテムの組み立て (`--dump-app-data` と共通) | `src/appItemAssembler.*` |
| サービスへの振り分け・間引き・アイテムのセクション化 | `src/appDataTransport.*` |
| 元のセクションをハンドラへ渡す (`onApplicationTableSection`) | `src/mmtTlvDemuxer.cpp`、`src/demuxerHandler.h` |
| PMT への ES 追加と TS パケット化 | `src/remuxerHandler.cpp` |
| 設定 | ini の `[appdata]` (`enable`、`itemResendInterval`)、dantto4k の `--no-app-data` |

- 振り分けは MPT ごとに作り直す。アプリケーションサービス記述子にある MH-AIT・データ伝送メッセージ・EMT の
  packet_id と、aapp・stpp アセットの packet_id を、その MPT のサービスに対応付ける。対応のない packet_id のものは捨てる。
- 間引きは内容のハッシュで行う。テーブルはサービスとセクションのバイト列、アイテムはサービス・component_tag・
  mpu_sequence_number・item_id・内容をキーにする。同じキーは、テーブルは 1 秒、アイテムは `itemResendInterval` 秒
  (既定 10) 経つまで送らない。時刻は PCR から取る。PCR が戻った場合は経過したものとして送る。
- 再送間隔は BonDriver の ini を読んだ後の値を使うため、アイテムを受け取るたびに設定から読み直している。

### 確認した結果

2026-10-14 の BS 4K の録画 (各 1〜2 分) を dantto4k で TS に変換して確認した。

| 録画 | アプリ用 PID | ビットレート | セクション (table_id 別) |
|---|---|---|---|
| NHK BSP4K | 0x0F00 | 約 227 kbps | 0x9C 67、0xA3 483、0xA4 324、0xC0 936 |
| BS フジ 4K | 0x0F00 | 約 4.7 kbps | 0x9C 58、0xA3 53、0xA4 46、0xC0 12 |
| BS 日テレ 4K | なし (アプリケーションサービス記述子がない) | – | – |

- PMT に stream_type 0x05 と `'D4KA'` の ES が載ることを `tstables` で確認した。
- TS からアイテムを組み立て直し (Python の検証スクリプト)、`--dump-app-data` が書き出したファイルと照合した。
  NHK の 59 個、フジの 2 個がすべて一致し、CRC エラーのセクションはなかった。
- 同じ入力を 2 回変換した結果は同一だった。`--no-app-data` では PID も PMT の ES も出ない。
- 回帰テストに `appdata` を追加した (`tools/app_data_transport_test.cpp`)。
