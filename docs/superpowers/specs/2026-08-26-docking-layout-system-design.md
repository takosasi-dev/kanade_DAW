# ドッキングレイアウトシステム 設計

日付: 2026-08-26
対象: ScoreSmith(`E:\MIDI&DAW`)

## 背景・目的

ユーザーからの要望: 「画面レイアウトをもっと自由にしたい」——具体的には (1) 複数画面を同時に見ながら作業したい、(2) マルチモニタを活用したい、(3) 自分の作業スタイルに合わせた配置を固定・保存したい、の3点すべて。

現状のScoreSmithは「タイムライン/ピアノロール/ミキサー/採譜/生成/楽譜/セッション/モジュラー」の8画面を、上部タブで1つずつ切り替えて表示する方式(`MainComponent::Impl::showView(View)` が対象コンポーネントの `setVisible()` を切り替えるだけの単純な実装)。8つのView自体はすべて起動時に生成済みで常に生きており、`workspace` という1つのコンテナの子として並び、非表示のものは `setVisible(false)` になっているだけ——この事実がドッキングシステムの実装コストを大きく下げている(後述)。

Cubase/Studio One的な「本格的なドッキングシステム」(パネルの分割・タブ合流・フローティングウィンドウ化・レイアウト保存)を新設する。

## ゴール

- 8画面すべてが初回バージョンからドッキング対象
- パネルをドラッグして分割(上下左右)・タブとして合流、の両方に対応
- パネルをメインウィンドウの外にドラッグすると独立した `juce::DocumentWindow` になり、そのままマルチモニタへ配置できる
- レイアウト(分割ツリーの形・比率・タブ構成・フローティングウィンドウの位置)を保存・復元できる。名前付きレイアウトを複数保持し、切り替えられる
- 起動時はデフォルトで「最後の状態」を復元する

## 非ゴール(明示的に対象外)

- 8つの既存View(`TimelineView` 等)自体の内部実装変更 — ドッキング層は純粋に「表示される場所」を差し替えるだけの外側のラッパー
- プロジェクトファイル(`.ssproj`)単位でのレイアウト保存 — レイアウトはアプリ全体の設定として保存する(プロジェクトを跨いで同じ配置で作業したい、という要望に合わせた判断)
- サードパーティプラグインのエディタウィンドウをドッキング対象にすること — 対象はScoreSmith自身の8画面のみ
- 任意のコンテンツ種別をドラッグ&ドロップで追加する汎用フレームワーク化 — 8画面という固定集合が前提
- 外部ドッキングライブラリの導入 — 本プロジェクトの既存方針(新規依存を増やさず自前実装する。UTAU連携でも徹底)を踏襲し、完全に自前実装する

## アーキテクチャ

`MainComponent` の `workspace`(8つのViewを並べて `setVisible` で切り替えるだけの単純なコンテナ)を、再帰的な分割ツリーのルートである `DockContainer` に置き換える。

```
namespace ss
{
    /** ドッキングツリーの1ノード。DockSplit か DockTabGroup のどちらか。
        View自体には触れず、表示位置の管理だけを担う。 */
    class DockNode : public juce::Component
    {
    public:
        virtual ~DockNode() = default;
        /** このノード配下の状態をJSON化する(DockLayoutが呼ぶ)。 */
        virtual juce::var toVar() const = 0;
    };

    /** 子ノード2つを水平/垂直に分割する。中央のスプリッターをドラッグしてratioを変更できる。 */
    class DockSplit final : public DockNode
    {
    public:
        enum class Direction { horizontal, vertical };

        DockSplit (Direction, std::unique_ptr<DockNode> first, std::unique_ptr<DockNode> second, float ratio = 0.5f);

        void resized() override;
        juce::var toVar() const override;

    private:
        Direction direction;
        std::unique_ptr<DockNode> firstChild, secondChild;
        float splitRatio;
        // スプリッターのドラッグ処理は Source/UI/Dock/DockSplitter.h (内部部品) が担当
    };

    /** 1つ以上の DockPanel をタブとして持ち、アクティブな1つだけを表示する。 */
    class DockTabGroup final : public DockNode
    {
    public:
        void addPanel (std::unique_ptr<DockPanel>, int insertIndex = -1);
        std::unique_ptr<DockPanel> removePanel (int index);
        void setActivePanel (int index);

        int getNumPanels() const noexcept;
        DockPanel* getPanel (int index) const noexcept;

        void resized() override;
        juce::var toVar() const override;

    private:
        std::vector<std::unique_ptr<DockPanel>> panels;
        int activeIndex = 0;
        // タブ帯の描画・ドラッグ検出は Source/UI/Dock/DockTabBar.h (内部部品) が担当
    };

    /** 1画面分の中身。id は8画面の識別子("timeline"/"mixer"/...)で、
        DockLayout の保存・復元時にどのViewコンポーネントと紐付けるかに使う。
        content は既存の View コンポーネントそのものへの非所有ポインタ
        (所有権は引き続き MainComponent 側、生成/破棄のタイミングは今と変えない)。 */
    struct DockPanel
    {
        juce::String id;           // 例: "timeline", "mixer", "pianoRoll"
        juce::String displayName;  // タブに出す文字列(TRANS済み)
        juce::Component* content = nullptr;
    };

    /** メインウィンドウの外にドラッグされたパネルの受け皿。中に DockContainer を1つ持つ
        (再帰的に、フローティングウィンドウの中でさらに分割・タブ合流ができる)。 */
    class FloatingDockWindow final : public juce::DocumentWindow
    {
    public:
        explicit FloatingDockWindow (std::unique_ptr<DockNode> rootContent);
        void closeButtonPressed() override; // このウィンドウを閉じる = 中身を親レイアウトに戻すか、破棄するかは
                                             // ユーザーの選択(閉じるボタンは「パネルを破棄」、ドラッグで戻すのが「再結合」)
    };

    /** 分割ツリー全体(メインウィンドウ+全フローティングウィンドウ)をJSONとして
        保存/復元する。名前付きレイアウトを複数保持できる。 */
    class DockLayout
    {
    public:
        /** 現在の DockContainer 階層 + 全 FloatingDockWindow の位置/サイズ/モニタから
            var を構築する。 */
        static juce::var captureCurrentState (const DockContainer& main,
                                              const std::vector<FloatingDockWindow*>& floating);

        /** var から DockContainer/FloatingDockWindow 群を再構築する。壊れた/バージョン
            不整合な var を渡された場合は defaultLayout() の結果を返す(呼び出し側が
            警告ログを出す)。 */
        static std::unique_ptr<DockContainer> restore (const juce::var&,
                                                        const std::map<juce::String, juce::Component*>& panelsById);

        /** 8画面を「タイムライン+ミキサー+ピアノロール」を左右分割、残り5画面を
            右側にタブでまとめる、という初回起動時のデフォルト配置。 */
        static juce::var defaultLayout();

        /** 名前付きレイアウトの一覧・保存・削除。実体は Settings と同じ
            juce::PropertiesFile に保存する(プロジェクトファイルとは別)。 */
        static juce::StringArray getSavedLayoutNames (Settings&);
        static void saveNamedLayout (Settings&, const juce::String& name, const juce::var& state);
        static juce::var loadNamedLayout (Settings&, const juce::String& name);
    };
}
```

`MainComponent` が持つ「1画面だけ見せる」という `showView(View)` の概念は廃止し、「起動時デフォルトレイアウト」を `DockLayout::defaultLayout()` の初期値として持つ形に変える。既存のメニュー/キーボードショートカット(F2=タイムライン等)は「そのIDの `DockPanel` を含む `DockTabGroup` を探し、アクティブタブにしてフォーカスする」という意味に読み替える。

## ドラッグ操作

- タブ帯の1タブをつまんで、隣接パネルの上下左右いずれかの端(全体の外側20%程度の帯)にドロップ → その位置に新しい `DockSplit` を作り、パネルを移動する
- 同じタブ帯の中央にドロップ → 分割せず、そのタブ帯に合流(タブが増える)
- タブをタブ帯の外(ウィンドウの外)までドラッグ → `FloatingDockWindow` を新規生成してそこに移動する
- ドラッグ中は移動先候補を半透明の矩形でハイライトし、どちらの挙動になるか事前にわかるようにする
- ドロップ判定(「この座標にドロップしたらどの操作になるか」)はマウスイベントから切り離した純粋関数として実装し、テスト可能にする(後述)

## データフロー・既存コードとの関係

- `DockContainer` とその配下(`DockSplit`/`DockTabGroup`/`DockPanel`)は純粋に見た目・配置だけを扱う層で、`Project`/`Transport`/`AppContext` など既存のアプリ状態には一切触れない。各Viewは今まで通り `AppContext` 経由でデータを読み書きし、ドッキング層はその「表示される場所」を差し替えるだけ
- `MainComponent` は起動時に8つのViewを生成 → それぞれを `DockPanel` として `DockLayout` の初期木構造(または保存済みレイアウト)に登録 → `DockContainer` がその木を実際のコンポーネント階層として組み立てる、という一方向の流れ
- メインウィンドウを閉じる操作は、開いている全フローティングウィンドウも道連れに閉じる(逆はしない)

## 永続化

- レイアウトは `.ssproj` とは別のファイル(`Settings` と同じ `juce::PropertiesFile`)に保存する。プロジェクトごとではなく「作業スタイル」に紐づくものという判断
- 保存内容: 分割ツリーの形・比率(`DockSplit::splitRatio`)、各タブ帯のタブ構成とアクティブタブ、フローティングウィンドウの位置・サイズ・どのモニタにあったか
- 「名前付きレイアウト」を複数保存して切り替えられるようにする(仕事用/配信用、など)。デフォルトは常に「起動時に最後の状態を復元」(`"__last__"` のような予約名で自動保存)

## エラー処理

- 保存時にモニタ2枚だったレイアウトを、モニタ1枚の環境で復元する場合 → 画面外に出るフローティングウィンドウの座標は `juce::Desktop::getDisplays()` を参照し、プライマリモニタ内に収まるよう補正する
- 保存済みレイアウトの分割ツリーが壊れている(JSON破損・バージョン不整合・存在しないパネルIDへの参照等) → `DockLayout::defaultLayout()` にフォールバックし、警告ログ(`ScoreSmith.log`)を残す。ユーザーの作業を止めない

## 既存コードへの影響

- `Source/UI/MainComponent.cpp` の `workspace`(8つのViewを `setVisible` で切り替えるだけの単純なコンテナ)を `DockContainer` に置き換える
- `showView(View)` の呼び出し元(メニュー・ショートカット・トランスクライブ画面からの遷移等、`MainComponent.cpp` 内の十数箇所)は「該当パネルをアクティブタブにする」呼び出しに置き換える。呼び出し元の意図(「タイムラインを見せたい」)は変わらないので、置き換えは機械的
- 8つのView自体(`TimelineView.cpp` 等)は無改修。既存の `resized()` / `AppContext` 経由のデータアクセスはそのまま
- 新規ファイルは `Source/UI/Dock/` に集約: `DockNode.h`、`DockSplit.h/.cpp`、`DockTabGroup.h/.cpp`、`DockPanel.h`、`DockSplitter.h/.cpp`(スプリッターのドラッグ処理)、`DockTabBar.h/.cpp`(タブ帯の描画・ドラッグ検出)、`FloatingDockWindow.h/.cpp`、`DockLayout.h/.cpp`

## テスト方針

- ドラッグ操作やレイアウトの見た目そのものは、このコードベースの既存方針(Preferencesダイアログ等と同じ)に倣い自動テスト対象外——実機での目視確認になる
- `DockLayout` の木構造とJSONシリアライズ/デシリアライズは純粋なロジックなので、`.ust` パーサーや `ProjectPersistence` と同じようにユニットテストで固める:
  - 分割ツリーを組んで保存 → 読み込みで同じ木構造(分割方向・比率・タブ構成)に戻ることの往復テスト
  - 壊れたJSON/存在しないパネルIDへの参照を渡した際に `defaultLayout()` へ正しくフォールバックすること
  - 画面外に出るフローティングウィンドウ座標が、復元時にモニタ内へ補正されること
- ドロップ判定ロジック(「この座標にドロップしたらどの操作になるか」)もマウスイベントと切り離した純粋関数として書き、既知の座標パターン(端/中央/外側)に対する期待される判定結果をテストする

## 既知のリスク・将来課題

- **JUCEに標準のドッキングフレームワークが無いため、完全に自前実装になる**——分割ツリー・タブ帯・フローティングウィンドウ・ドラッグ判定のすべてを新規に書く必要があり、実装規模は今日のUTAU連携Phase1と同等かそれ以上になる見込み
- フローティングウィンドウのメニューバー扱い(macOSのメイン メニューは1つしか持てない等、`MainComponent::resized()` に既存の `hostOwnsMenuBar` 判定があるのと同種の考慮が必要)は実装時に詳細設計する
- パネルの最小サイズ(あまり小さく分割しすぎた場合の下限)は実装時に決める。極端な分割によるUI崩壊を防ぐガードは必要
- 内蔵エフェクトのアニメーションUI・XYパッド操作・複数トラック連動編集は、それぞれ別の設計として扱う(本ドキュメントの対象外)。ただし内蔵エフェクトUIの刷新は「エフェクトパネルがドッキング対象になる」ことを前提にレイアウトを組むと手戻りが少ない
