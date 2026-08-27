# ビルド手順

## 前提

| | 必要なもの |
|---|---|
| 共通 | CMake 3.22+ / C++20 対応コンパイラ / 初回ビルド時のネットワーク接続 |
| Windows | Visual Studio 2022 (MSVC v143, "Desktop development with C++") |
| macOS | Xcode 14+ / Command Line Tools |
| Linux | 非対応(仕様書4章:対象はWin/Macのみ) |

JUCE 8.0.6 は CMake の FetchContent が自動で取得する。取得先は https://github.com/juce-framework/JUCE.git 。

## 手順

> **重要: フォルダ名の `&` を避けて `E:\MIDIDAW` からビルドすること。**
> `E:\MIDI&DAW` を直接指定するとJUCE自身のビルド機構が壊れる。MSBuildが生成するバッチでパスが引用符なしに展開され、cmd.exe が `&` をコマンド区切りとして解釈するため。
> `E:\MIDIDAW` は `E:\MIDI&DAW` を指すディレクトリジャンクションで、実体は同じ。編集はどちらのパスからでも同じファイルに届く。
>
> 消えていたら作り直す(管理者権限は不要):
> ```powershell
> New-Item -ItemType Junction -Path "E:\MIDIDAW" -Target "E:\MIDI&DAW"
> ```

```powershell
cmake -S "E:/MIDIDAW" -B "E:/MIDIDAW/build" -G "Visual Studio 17 2022" -A x64
cmake --build "E:/MIDIDAW/build" --config Debug --parallel      # 開発用(LTO無しで速い)
cmake --build "E:/MIDIDAW/build" --config Release --parallel    # 配布用
```

生成物:

- Debug: `build/ScoreSmith_artefacts/Debug/ScoreSmith.exe` (約27 MB)
- Release: `build/ScoreSmith_artefacts/Release/ScoreSmith.exe` (約7.7 MB)

## テスト

GUIサブシステムのバイナリなので標準出力が無い。レポートはexeの隣の `test-results.txt` に書かれ、合否は終了コードで返る。

```powershell
& "E:/MIDIDAW/build/ScoreSmith_artefacts/Debug/ScoreSmith.exe" --run-tests
echo $LASTEXITCODE          # 0 = 全通過
Get-Content "E:/MIDIDAW/build/ScoreSmith_artefacts/Debug/test-results.txt"
```

カテゴリを絞る場合は `--run-tests=ScoreSmith` のように渡す。現在1,295アサーション。

## ログ

`%APPDATA%\ScoreSmith\ScoreSmith.log`。起動時に開いたオーディオデバイス・サンプルレート・バッファ・レイテンシを記録する。音が出ないときは最初にここを見る。

## オプション

### ローカルのJUCEを使う(オフラインビルド)

```bash
cmake -B build -DSCORESMITH_JUCE_PATH=/path/to/JUCE
```

### ASIO を有効にする(Windows)

Steinberg ASIO SDK は再配布不可のため同梱していない。[Steinberg のサイト](https://www.steinberg.net/developers/)から取得して展開し、パスを渡す:

```bash
cmake -B build -DSCORESMITH_ASIO_SDK_PATH="C:/SDKs/asiosdk_2.3.3"
```

未指定の場合、Windowsでは WASAPI(排他モード対応)が使われる。仕様書10.3はASIO推奨としているので、低レイテンシ録音を実運用するならSDKを入れること。

## 初回ビルドで詰まりやすいところ

**この実装はコンパイル検証されていない。** 開発環境にコンパイラが無い状態で書かれているため、初回は型の食い違いや include 漏れが出る前提で進める。

- **`Resources/lang/ja.txt` / `en.txt` が無いとCMakeが失敗する** — `juce_add_binary_data` が実ファイルを要求する
- **VST3ホスティングでリンクエラー** — `JUCE_PLUGINHOST_VST3=1` はJUCE同梱のVST3 SDKを使う。JUCEの取得が途中で切れていると出る
- **macOSでAUが見つからない** — `JUCE_PLUGINHOST_AU=1` は macOS のみ有効。`#if JUCE_MAC` のガードを確認
- **`juce::dsp` が無い** — `target_link_libraries` の `juce::juce_dsp` を確認

## 実行時の初期設定

1. 起動後 `Preferences → Audio` でデバイス・サンプルレート・バッファサイズを設定
2. `Preferences → Plugin Management` で VST3 のスキャンパスを追加して再スキャン(子プロセスで実行されるため、クラッシュするプラグインがあってもDAW本体は落ちない)
3. ステム分離を使う場合は `Preferences → Files/Paths` で Demucs 互換実行ファイルのパスを指定する。未設定でも採譜そのものは動く
