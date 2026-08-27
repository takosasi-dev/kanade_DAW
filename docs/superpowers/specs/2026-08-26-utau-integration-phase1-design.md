# UTAU連携 Phase 1 設計 — 読み込み・レンダリング・再生

日付: 2026-08-26
対象: ScoreSmith(`E:\MIDI&DAW`)
関連: `E:\Obsidian\Claude\Claude\MDファイル\MIDI＆DAW編集ソフト\scoresmith_spec.md`(本体仕様書v0.8には含まれない、今回追加の拡張機能)

## 背景・目的

ユーザーはUTAU/OpenUtauおよびボイスバンク(モデル)をすでに保有している。ScoreSmithに `.ust`(クラシックUTAU形式)ファイルを読み込み、ボイスバンクを割り当てて歌声としてレンダリング・再生できるようにする。

編集(子音速度・オーバーラップ・ピッチベンドカーブ等、UST全パラメータ)は**Phase 2**で扱う。Phase 1のゴールは「読み込み→ボイスバンク設定→レンダリング→再生」が通しで動くところまで。

### Phase 1のゴール

- `.ust` ファイルをプロジェクトに読み込める
- 読み込んだノート列を、指定したボイスバンク(oto.ini)で外部リサンプラを使ってレンダリングできる
- レンダリング結果を通常のオーディオと同じ経路で再生できる
- プロジェクト保存・再読み込みでUTAUクリップのデータが失われない

### Phase 1の非ゴール(明示的に対象外)

- 専用の編集UI(子音速度・強弱・ピッチベンドカーブの操作画面) — Phase 2
- VCV/CVVCボイスバンクの前後接続を考慮したエイリアス自動選択(phonemizer相当の処理) — 歌詞とエイリアスの直接一致のみ対応。本格対応は将来の課題として`ponytail:`で明記する
- `.ustx`(OpenUtau固有形式)や、ニューラル系エンジンとの連携
- リアルタイム再生(UTAU本体と同様、「レンダリングしてから再生」方式のみ)

## データモデル

`Source/Core/Types.h` の `TrackType` に `utau` を追加(`audio` / `midi` / `utau` の3種)。

```cpp
enum class TrackType { audio, midi, utau };
```

新規: `UtauPitchBend`、`UtauNote`、`UtauClip`(配置先は `Source/Core/Types.h` または新規 `Source/Core/UtauTypes.h` — 既存の `Note`/`MidiClip` との依存関係を見て実装時に決定)。

```cpp
/** UST の PBS/PBW/PBY/PBM をそのまま保持するピッチベンド曲線。 */
struct UtauPitchBend
{
    double startMs        = 0.0;   // PBS の x (ノート開始からのオフセット, ms)
    double startSemitones = 0.0;   // PBS の y (開始時点のピッチオフセット, 半音)
    std::vector<double>       widthsMs;          // PBW
    std::vector<double>       heightsSemitones;  // PBY
    std::vector<juce::String> curveTypes;        // PBM ("" | "s" | "r" | "j")
};

/** UST の1ノート。UTAU本体のUSTと1:1に近い形で保持し、可逆的に書き出せるようにする。 */
struct UtauNote
{
    double startBeats  = 0.0;
    double lengthBeats = 1.0;
    int    pitch       = 60;      // NoteNum
    juce::String lyric  = "a";    // "R" は休符
    int    velocity     = 100;    // 子音速度 0-200 (100=標準)
    int    intensity    = 100;    // 音量 0-200
    int    modulation   = 0;      // ピッチのゆれ 0-100
    double preUtteranceMs = -1.0; // -1 = oto.ini の値をそのまま使う
    double voiceOverlapMs = -1.0; // 同上
    juce::String flags;           // リサンプラにそのまま渡す文字列 (例 "g-5B0")
    UtauPitchBend pitchBend;
    juce::String envelope;        // 5点エンベロープの生文字列。Phase 1では編集せず保持のみ
    /** 未知のUSTキー(将来の拡張・他ツール固有のタグ等)も欠落させず保持する。
        書き出し時にそのまま書き戻すことで、対応していないキーが原因で
        ファイルが壊れるのを防ぐ。 */
    juce::NamedValueSet extra;

    double endBeats() const noexcept { return startBeats + lengthBeats; }
};

/** UTAUトラック上の1クリップ。ノート列 + どのボイスバンクで歌わせるか + レンダリング
    結果のキャッシュを持つ。 */
struct UtauClip
{
    ClipId id = invalidClipId;
    juce::String name;
    double startBeats = 0.0;
    std::vector<UtauNote> notes;

    juce::String voicebankId;         // VoicebankLibrary が管理する識別子
    juce::File   renderedFile;        // レンダリング済みWAV。空 = 未レンダリング
    juce::int64  notesHashAtRender = 0; // 直近レンダリング時点のノート内容ハッシュ

    double endBeats() const noexcept;
    /** notes / voicebankId の現在の内容から計算したハッシュ。renderedFile が
        古くなっていないかの判定に使う(ハッシュが不一致なら要再レンダリング)。 */
    juce::int64 currentContentHash() const noexcept;
};
```

`Track` に `std::vector<UtauClip> utauClips;` を追加。既存の `audioClips` / `midiClips` と同じ並びで、`Track::endBeats()` にも組み込む。

## ファイルI/O

### `.ust` パーサー — `Source/IO/UstFile.h/.cpp`

- セクションベースのINI風フォーマット(`[#SETTING]`、`[#0000]`、`[#0001]`...、`[#TRACKEND]`)をパースし `std::vector<UtauNote>` を返す
- 認識しないキーは `UtauNote::extra` にそのまま保持し、書き出し時に復元する(可逆性の確保)
- **要注意**: 古い `.ust` は Shift-JIS エンコードのものが多い。JUCEに標準のSJISデコーダは無いため、UTF-8として読めなかった場合のフォールバック変換を実装する必要がある(実装時のリスクとして明記)
- 書き出し(`UtauClip` → `.ust` テキスト)も対称的に実装し、往復テストの対象にする

### `oto.ini` パーサー — `Source/IO/OtoIni.h/.cpp`

- ボイスバンクのフォルダ内(サブフォルダ含め再帰的に)にある `oto.ini` を読み込み、`alias -> { サンプルファイル, offset, consonant, cutoff, preutterance, overlap }` のマップを構築
- こちらも伝統的にShift-JISのことが多い。`UstFile.h` と同じデコード処理を共有する

## ボイスバンク設定 — `Source/Vocal/VoicebankLibrary.h/.cpp`

- `Settings::getUtauVoicebankFolders()` / `setUtauVoicebankFolders()` を追加(複数フォルダ指定、既存の `getSampleLibraryFolders()` と同じ形)
- `VoicebankLibrary` が設定フォルダをスキャンし、各フォルダを1ボイスバンクとして oto.ini を読み込みキャッシュする
- `Settings::getUtauResamplerExecutable()` / `setUtauResamplerExecutable()` を追加(既存の `getStemSeparatorExecutable()` と同じ形の単一exeパス)
- Preferencesダイアログに設定欄を新設(既存の "Files" タブに追記するか、新規 "Vocal" タブを作るかは実装時に判断)

## レンダリング処理 — `Source/Vocal/UtauRenderer.h/.cpp`

`UtauClip` を受け取り、`renderedFile` に書き出すまでの一連の処理:

1. **エイリアス解決**: 各 `UtauNote::lyric` を、割り当てられたボイスバンクの oto.ini エイリアスと**直接一致**で照合する。一致しない場合はそのノートを無音扱いにし、警告を記録する(将来的なVCV/CVVC自動接続は非対応 — `ponytail:` で明記)
2. **リサンプラCLI引数の構築**: UTAU標準のリサンプラCLIプロトコル(`resampler.exe <入力wav> <出力wav> <音高> <子音速度> <フラグ> <オフセットms> <長さms> <子音長ms> <カットオフ> <音量> <モジュレーション> [ピッチベンド文字列]`)に従って引数を組み立てる。ピッチベンド文字列はUTAU標準のBase64風RLEエンコードを実装する
3. **プロセス起動**: `juce::ChildProcess` で1ノートずつ同期実行(`Source/AI/StemSeparator.cpp` と同一パターン)。タイムアウト・失敗時はそのノートを無音として扱い処理を継続する(1ノートの失敗で全体を止めない)
4. **結合(wavtool相当)**: 各ノートの断片WAVを、ノートのオーバーラップ設定に基づき**ScoreSmith内でクロスフェード結合**する。外部の `wavtool.exe` には依存しない
5. **書き出し**: 結合結果を `Project::getMediaFolder()` 配下にWAVとして保存し、`UtauClip::renderedFile` と `notesHashAtRender` を更新する

呼び出しは明示的な「レンダリング」操作(UIのコマンド/ボタン)からのみ行う。編集の度に自動実行はしない(ユーザーの選択どおり)。

**テスト容易性のため**、実際のプロセス起動部分は差し替え可能なインターフェースにする(例: `std::function` またはstrategy インターフェースで注入)。ユニットテストでは疑似リサンプラ(既知の入力に対して既知の出力を返すスタブ)を使い、実行環境に依存しない形でエイリアス解決・CLI引数構築・クロスフェード結合のロジックを検証する。

## 再生

`UtauClip::renderedFile` が存在する場合、既存の `AudioClip` 再生経路をそのまま利用して `startBeats` の位置で鳴らす。新しい再生パスは作らない(実装時の具体的な共有方法— 内部的に `AudioClip` 相当のリーダーを流用するか、専用の薄いラッパーを作るか — は実装計画時に決定)。

`renderedFile` が空、または `notesHashAtRender` が現在のノート内容と一致しない場合は無音として扱い、UI側で「レンダリングが必要」であることが分かるようにする(バッジ/色分け等の具体的なUIはPhase 2の範囲とし、Phase 1では最低限のログ出力のみでもよい)。

## 永続化

`.ssproj` の `ProjectPersistence.cpp` に `utauClips` の読み書きを追加(`audioClips`/`midiClips` と同じ形)。`TrackType::utau` を `toString(TrackType)` / `trackTypeFromString()` に追加。

## テスト方針

- `.ust` 往復テスト(既知のサンプルファイルを読み込み→書き出し→再読み込みで一致することを確認)
- `oto.ini` 解析テスト
- ピッチベンド文字列のエンコード/デコード往復テスト
- クロスフェード結合の数値的検証(既知の2断片を既知のオーバーラップで結合した際の出力振幅)
- リサンプラCLI引数構築のテスト(既知の `UtauNote` から期待される引数文字列が組み立てられること)
- 実際の `resampler.exe` を呼ぶテストは書かない(環境依存のため上記の差し替え可能インターフェースで代替)

## 既知のリスク・Phase 2以降への持ち越し事項

- **Shift-JISデコード**: 実装時に想定より手間がかかる可能性がある
- **VCV/CVVCエイリアス自動選択**: 直接一致のみのv1では、CV形式以外のボイスバンクは正しく歌わない可能性が高い。ユーザーが持っているボイスバンクの種類次第では優先度が上がる
- **編集UI一式**: 子音速度・強弱・ピッチベンドカーブ・エンベロープの編集画面はPhase 2
- **自動再レンダリング**: 編集後に自動でレンダリングし直すか、明示操作のみにするかはPhase 1では「明示操作のみ」で確定しているが、UXとして再検討の余地がある
