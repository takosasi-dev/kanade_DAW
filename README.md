# KANADE DAW

AI音楽生成・自動採譜を核に持つ統合DAW。仕様書 `scoresmith_spec.md` v0.8 の **Phase 0(素のDAWの土台)+ Phase 1(AIコア)** の実装。

- 対象: Windows / macOS スタンドアロンデスクトップアプリ
- 技術: C++20 / JUCE 8
- 配布: 非商用・無償(仕様書12章)。ライセンス管理・課金機構は一切持たない
- AI推論: **ローカル完結のみ**(仕様書v0.6確定)。クラウド送信のコードパスは存在しない

## ビルド

**2026-08-26にVisual Studio Build Tools 2022 + CMake 4.4.2でビルド環境を構築し、Debug/Releaseともビルド成功・ユニットテスト全通過を確認済み。** 手順・注意点(フォルダ名の`&`回避のためのジャンクション必須)は [docs/BUILD.md](docs/BUILD.md)。

```
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
```

JUCE 8.0.6 は FetchContent で自動取得する(初回ビルドのみネットワークが必要)。ローカルのJUCEを使う場合は `-DSCORESMITH_JUCE_PATH=/path/to/JUCE`。

## ディレクトリ構成

| パス | 内容 | 仕様書 |
|---|---|---|
| `Source/Core/` | プロジェクトモデル、テンポマップ、設定、`.ssproj` 永続化 | 7-5, 10.4 |
| `Source/Engine/` | オーディオデバイスI/O、トランスポート、録音、オフラインレンダリング | 7-2, 7-3, 8.4.3 |
| `Source/Mixer/` | チャンネルストリップ、内蔵エフェクト、ラウドネスメーター | 8.4.5, 8.4.6 |
| `Source/Plugins/` | VST3/AU スキャン(子プロセス隔離)・インスタンス化 | 8.4.2, 15.6 |
| `Source/AI/` | 音声→MIDI採譜、MIDI生成、音楽理論 | 8.1, 8.2, 10.2 |
| `Source/IO/` | MIDI / オーディオ / MusicXML の入出力 | 10.4 |
| `Source/UI/` | 全画面(タイムライン、ピアノロール、ミキサー、採譜、生成、設定) | 9章 |

## 実装済み / 未実装

実装状況と仕様書との対応は [docs/STATUS.md](docs/STATUS.md) を参照。

**P0のうち未達の2点**(仕様書11章の Must-Have に挙がっているが実装していない):

- **サウンドフォント/サンプルの取り込み** — .sf2 / SFZ ローダーが無い。音源は外部VST3経由のみ
- **MIDI録音** — オーディオ録音は動くが、MIDI入力のクリップ化は未実装

**その他の主な未実装**:

- ASIO — Steinberg SDKが再配布不可のため、`-DSCORESMITH_ASIO_SDK_PATH=...` を渡したときだけ有効化。未指定時はWASAPI排他モード
- テイク管理・コンピング、バッチ処理、タイムストレッチ
- バス→バスのルーティング(バスはマスターにのみ送れる)
- 譜面編集(8.5 / 9.8)は読み取り専用の五線譜ビューまで。編集はPhase 2
- 映像同期(9.9)、モジュラーパッチング(9.11)はプレースホルダ画面のみ。セッションビュー(9.10、track×sceneのクリップランチャー)は実装済み
- ステム分離は外部のDemucs互換実行ファイルを設定で指定する方式。モデルは同梱しない

設計時に固めたヘッダの不備は16点あったが、**2026-08-26に全16点を解消**(バス/センド、入力モニタリング、オートメーション編集UIと音声側の適用、テイク長、マスターFXの永続化、そして最後まで残っていたプラグインのランタイム別プロセス実行)。詳細は [docs/STATUS.md](docs/STATUS.md)。

ビルド・テスト状況: Debug/Release ともエラー0、ユニットテスト**2,694件全通過**(2026-08-29確認)。

**2026-08-26 実機テストで見つかった不具合を修正**: タイムラインの再生ヘッドをドラッグしても表示が動かない(`repaint()`呼び忘れ)を修正。また、内蔵音源が無く採譜結果が無音だった件への当面の対策として、ミキサーの「Add instrument → Built-in instruments」から挿せる簡易内蔵シンセ「KANADE DAW Basic Synth」(sine/saw/square + ADSR)を追加。

## 拡張機能(フォーマットプラグイン)の作り方

KANADE DAW は import/export フォーマットをサードパーティが追加できます。
1フォルダ = 1拡張機能で、`manifest.json` と実行ファイル(言語は問いません)
を同じフォルダに置くだけです。アプリ内では Help > 「拡張機能の作り方...」
でも同じ内容を確認できます。

### manifest.json

```json
{
  "id": "com.example.reason-export",
  "name": "Reason Project Export",
  "version": "1.0.0",
  "fileExtension": "reason",
  "direction": "export",
  "executable": "reason-export.exe"
}
```

| フィールド | 必須 | 説明 |
|---|---|---|
| `id` | ○ | 一意なID(逆ドメイン名推奨) |
| `name` | ○ | File メニュー・設定画面に表示される名前 |
| `version` | - | 表示用(省略可) |
| `fileExtension` | ○ | 先頭のドット無し(例: `"reason"`) |
| `direction` | - | `"import"` \| `"export"` \| `"both"`(省略時 `"both"`) |
| `executable` | ○ | 同じフォルダ内の実行ファイル名 |

**`executable`は必ずネイティブの実行ファイル(Win32 `.exe`)にしてください。**
KANADE DAW は `juce::ChildProcess`(Windows実装では `CreateProcess` を直接
呼び出す)で拡張機能を起動するため、シェルの解釈が一切挟まりません。
`.bat` / `.cmd` / `.ps1` のようなスクリプトを`executable`に直接指定しても
起動できません。また、`executable`は拡張機能自身のフォルダの中に実在する
ファイルでなければならないという制約もあるため、`powershell.exe`のような
外部インタプリタを直接指定して回避することもできません。スクリプトで
処理を書きたい場合は、そのスクリプトを呼び出すだけの小さなネイティブ
実行ファイルでラップしてください。

### 呼び出し方

```
<executable> --export <input.dawproject> <output-file>
<executable> --import <input-file> <output.dawproject>
```

KANADE DAW は御社独自のフォーマットを一切解釈しません。相互運用フォーマット
[DAWproject](https://github.com/bitwig/dawproject)(Studio One / Bitwig /
Cubase 等が対応)との変換だけを拡張機能の実行ファイルに任せます。

終了コード `0` で成功。それ以外は失敗として扱われ、標準出力/標準エラー
出力の内容がそのまま KANADE DAW 側のエラーダイアログに表示されます。
成功終了しても出力ファイルが実際に存在しなければ失敗扱いです。
タイムアウトは120秒です(後述の`customUI: true`の場合を除く)。

### 設定ダイアログ・独自GUI・進捗表示

`manifest.json`に`"settings"`配列(export専用)を書くと、実行前に
KANADE DAWが自動でダイアログを出し、選ばれた値を環境変数として拡張機能に
渡します。各エントリは`id` / `label` / `envVar`と、`"slider"`(min/max/
default) | `"checkbox"`(default) | `"dropdown"`(options/default) の
いずれかの`type`を持ちます。`envVar`名は`additionalInputs`のものも含めて
重複させられません(重複した`manifest.json`は読み込み時に弾かれます)。

`"customUI": true`(export専用、`"settings"`とは排他)を書くと、この
ダイアログを出さず、120秒のタイムアウトも適用しません。拡張機能が自分の
ウィンドウを好きなだけ出せる代わりに、中断手段はKANADE DAW側のキャンセル
ボタンのみになります。キャンセルで終了するのは拡張機能自身のプロセスだけ
で、拡張機能が起動した子プロセスまでは終了しないため、必要なら拡張機能側
で後始末してください。

標準出力(または標準エラー出力)に`PROGRESS:42`のような行を書くと、0-100
の進捗率としてKANADE DAWの進捗バーに反映されます。それ以外の行は従来どおり
失敗時のエラー表示用に蓄積されます。

### 最小サンプル(C言語、恒等変換)

入力ファイルをそのまま出力ファイルへコピーするだけの最小サンプルです。
実運用では`fread`/`fwrite`ループの部分を実際のフォーマット変換処理に
差し替えてください。

`identity.c`:
```c
#include <stdio.h>

int main (int argc, char** argv)
{
    /* argv[1] が --export/--import、argv[2] が入力パス、argv[3] が出力パス
       (前述の「呼び出し方」を参照)。 */
    if (argc < 4)
        return 1;

    FILE* in  = fopen (argv[2], "rb");
    FILE* out = fopen (argv[3], "wb");
    if (in == NULL || out == NULL)
        return 1;

    char buffer[4096];
    size_t bytesRead;
    while ((bytesRead = fread (buffer, 1, sizeof (buffer), in)) > 0)
        fwrite (buffer, 1, bytesRead, out);

    fclose (in);
    fclose (out);
    return 0;
}
```

コンパイルして`identity.exe`を作り、`manifest.json`と同じフォルダに
置いてください:
```
cl identity.c                    (MSVCの場合。identity.exeが生成されます)
gcc identity.c -o identity.exe   (MinGWなどGCC系ツールチェインの場合)
```

`manifest.json`の`executable`には、ソースファイルではなく
コンパイル後の`"identity.exe"`を指定します。

### 発見のされ方

設定 > Extensions タブでスキャン対象フォルダを登録すると、その直下の
各サブフォルダが1拡張機能として走査されます。`manifest.json` が壊れて
いる場合は警告付きでスキップされ、他の拡張機能の発見をブロックしません。
