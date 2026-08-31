#include "UI/ExtensionHelpDialog.h"

namespace ss
{
    namespace
    {
        // The manifest JSON, field table, and CLI syntax stay literal
        // English/code - this is the technical contract a third-party
        // extension author builds against, the same way manifest.json's own
        // keys and the DAWproject XSD are English-only. The surrounding
        // prose is Japanese, matching every other user-facing string in
        // this app (and README.md, which is entirely Japanese).
        juce::String helpBodyText()
        {
            return juce::String (juce::CharPointer_UTF8 (
R"RAW(1つのフォルダ = 1つの拡張機能です。manifest.json と、そこで名指しした
実行ファイルを同じフォルダに置いてください。

manifest.json:
{
  "id": "com.example.reason-export",
  "name": "Reason Project Export",
  "version": "1.0.0",
  "fileExtension": "reason",
  "direction": "export",
  "executable": "reason-export.exe"
}

フィールド:
  id            必須。他と重複しないID(逆ドメイン名推奨)
  name          必須。File メニューと設定画面に表示される名前
  version       任意(省略時は空)。表示用のみ
  fileExtension 必須。先頭のドット無し(例: "reason")
  direction     任意(省略時 "both")。"import" | "export" | "both"
  executable    必須。manifest.json と同じフォルダ内の実行ファイル名

呼び出し方(コマンドライン引数):
  <executable> --export <input.dawproject> <output-file>
  <executable> --import <input-file> <output.dawproject>

KANADE DAW は御社独自のフォーマットを一切解釈しません。DAWproject
(Studio One / Bitwig / Cubase 等が対応するオープンな相互運用フォーマット)
との相互変換だけを、拡張機能の実行ファイルに任せます。

終了コード 0 で成功、それ以外は失敗として扱われます。標準出力/標準エラー
出力に書いた内容はそのままエラーダイアログに表示されます。成功終了して
いても出力ファイルが実際に存在しなければ失敗として扱われます。

タイムアウトは120秒です。

最小サンプルと詳しい解説は README.md の
「拡張機能(フォーマットプラグイン)の作り方」を参照してください。)RAW"
            ));
        }
    }

    ExtensionHelpDialog::ExtensionHelpDialog()
    {
        titleLabel.setText (TRANS ("How to build a format extension"), juce::dontSendNotification);
        titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
        titleLabel.setColour (juce::Label::textColourId, palette().text);
        addAndMakeVisible (titleLabel);

        body.setMultiLine (true, true);
        body.setReadOnly (true);
        body.setCaretVisible (false);
        body.setScrollbarsShown (true);
        body.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f,
                                                      juce::Font::plain)));
        body.setColour (juce::TextEditor::backgroundColourId, palette().panelAltBg);
        body.setColour (juce::TextEditor::textColourId, palette().text);
        body.setColour (juce::TextEditor::outlineColourId, palette().outline);
        body.setText (helpBodyText(), false);
        addAndMakeVisible (body);

        okButton.setButtonText (TRANS ("OK"));
        okButton.onClick = [this]
        {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (0);
        };
        addAndMakeVisible (okButton);

        setSize (560, 480);
    }

    void ExtensionHelpDialog::paint (juce::Graphics& g)
    {
        g.fillAll (palette().windowBg);
    }

    void ExtensionHelpDialog::resized()
    {
        auto area = getLocalBounds().reduced (14, 12);
        titleLabel.setBounds (area.removeFromTop (28));
        area.removeFromTop (8);

        auto buttonRow = area.removeFromBottom (30);
        okButton.setBounds (buttonRow.removeFromRight (90));
        area.removeFromBottom (8);

        body.setBounds (area);
    }

    void ExtensionHelpDialog::launch()
    {
        juce::DialogWindow::LaunchOptions options;
        options.content.setOwned (new ExtensionHelpDialog());
        options.dialogTitle            = TRANS ("How to build a format extension");
        options.dialogBackgroundColour = palette().windowBg;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar      = true;
        options.resizable              = true;
        options.launchAsync();
    }
}
