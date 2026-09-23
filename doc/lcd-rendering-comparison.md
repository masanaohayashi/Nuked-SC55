# LCD・パネル描画と88Emuの比較

> 最終実装はTX81Zの矩形直接描画方式へ変更した（末尾参照）。
> 以下の画像補間案は調査・旧実装の記録であり、現在の描画方式ではない。


調査日: 2026-09-24。Nuked-SC55 `3e6d620`、gearmulator `2b25c85e` のローカルコードを比較。
対象は Plugins のJUCE画面と88emuplayer。SDLフロントエンドは別経路。
調査時点では製品コードを変更していない。続く実装結果は末尾に記載。
Windows/Linuxの実画面は未検証。

## 結論

文字・メーターの元のドット配置は同じ。大きな違いは最後の画像拡縮。
Nuked側は741×268の画像を標準サイズ344×124へ最近傍補間で縮小する。
元画像の文字の隙間1pxは約0.464pxとなり、サンプリング位置によって消える。
88Emuは固定解像度のLCDをテクスチャ化し、最後に線形補間で画面へ描画する。
88Emuの途中の `lowResamplingQuality` だけを真似すると、この違いを取り逃がす。

## コード上の根拠

| 項目 | Nuked-SC55 | 88Emu |
| --- | --- | --- |
| LCD画像 | 741×268 (`Plugins/Source/SC55Lcd.h`) | 741×268 (`88emuplayer/panel.hpp`) |
| 文字 | 5×5ドット、6pxピッチ | 同じ |
| メーター | 幅24×高さ9、横26/縦11pxピッチ | 同じ |
| 元画像生成 | `NukedSC55Emulator.cpp`: `renderStandardMask`, `renderLevelMask` | `panel.cpp`: `drawChar`, `drawLevel` |
| LCD画面寸法 | 344×124、さらに親Componentの変換 | CSS標準209×76dp、さらにUI倍率 |
| 最終補間 | `PluginEditor.cpp: LcdDisplay::paint`: low / 最近傍 | OpenGL/Metalのlinear |

88Emuの関連ファイルはgearmulatorの `source/ronaldo/88emu/88emuplayer/` 以下。
`Emu88EditorLcd.h` は `setFixedTextureSize` で解像度を固定する。
共通描画層 `source/framework/juce/juceRmlUi/rmlElemCanvas.h` は固定テクスチャを線形補間で画面サイズへ拡縮する意図を明記。
同ディレクトリの `RmlUi_Renderer_GL2.cpp` / `RmlUi_Renderer_GL3.cpp` はテクスチャのmin/magをGL_LINEARに設定（条件によってminにmipmap使用）。
`RmlUi_Renderer_Metal.mm` もmin/magをLinearに設定する。
`juceRmlComponent.cpp` にはMetal/OpenGLとソフトウェアへの分岐があるため、ユーザーが実行した88Emuのバックエンドまではこの調査では確定していない。

Nuked同梱JUCE 9.0.2ではlowの意味も確認した。
`juce_CoreGraphicsContext_mac.mm` は `kCGInterpolationNone`、
`juce_Direct2DGraphicsContext_windows.cpp` は `NEAREST_NEIGHBOR`、
`juce_RenderingHelpers.h` のソフトウェア描画もlowの場合は4画素補間を行わない。
したがってLinuxだけのドット配置バグではなく、各環境のDPI・画像サンプリングの影響を受ける構造。

## 数値確認と限界

実配置の16文字について各文字内4個の隙間を取り、出力画素中心で最近傍サンプリングする計算を実行した。
元画像の隙間の開始位置は `153 + 35*character + 6*dot + 5`、幅1。
倍率は `344 / 741 * DPI`。画面原点の追加の端数移動は与えていない。

| DPI倍率 | 消失した隙間 | 1画素になった隙間 |
| --- | ---: | ---: |
| 1 | 32 | 32 |
| 1.25 | 26 | 38 |
| 1.5 | 20 | 44 |
| 2 | 0 | 64 |

これは画像APIを実行した回帰テストでも実画面キャプチャでもない。
最近傍縮小の幾何的な欠落を確認したもの。実際の座標位相によって個数は変わる。
Retinaでマシという報告とは整合するが、OSの見た目の順位をこれだけで断定できない。
メーターの縦の隙間も標準1倍では `2 * 124 / 268 ≈ 0.925px` と整数にならない。

## 実装方針

1. 最小の比較実験は `LcdDisplay::paint` のlowをmediumへ変更すること。
   ソフトウェアではbilinear、Direct2Dではlinearになる。同梱CoreGraphicsではmedium指定になるため、全OSで同一結果になる保証はない。
   highへの変更もGPUのlinearを厳密に再現する指定ではない。
2. OS間で揃える本命は、741×268の固定元画像を維持し、最終物理ピクセルサイズへの拡縮を共通実装に集約すること。
   88Emuに忠実にするならGPUテクスチャのmin/magをlinearへ揃える。
   新しいGPU基盤を入れない案なら、共通CPUリサンプラーで物理サイズのキャッシュを作り、最終描画で再縮小しないようDPIと変換を扱う。
   親Componentの倍率とホストDPIの両方を含め、キャッシュを論理サイズで作ってから再拡縮しないこと。
3. 背景とドット層の座標・サンプリング規則を揃える。CPU案では元解像度で合成してから一度だけ拡縮すると管理しやすい。
   LCD内容更新と倍率/DPI変更でキャッシュを更新し、音声スレッドでは処理しない。
4. 線形補間でもサブピクセルの隙間が数学的に均等な整数画素になるわけではない。
   特に強い縮小では線形補間にもエイリアシングが残る。さらに安定させる場合は面積平均による縮小を比較する。
   完全な等幅の硬いドットを優先するなら物理画素の整数ピッチへ配置し直す必要があり、元のレイアウトと自由な拡縮とのトレードオフになる。

RmlUi全体の移植やフォントの差し替えは最初の対策として不要。
88Emuの「固定元画像＋最終段の線形補間」を再現することが先。

## パネル全体

Nukedの `Plugins/Images/Background.png` は2048×400で、標準表示は1024×200。
LCD以外の背景にはLCDのlow指定は及ばず、ボタンやノブも別々のJUCE描画経路。
そのためLCDの変更だけでパネル全体のOS差まで解消したとは言えない。
背景・ノブ・ラベルについても同じ物理サイズで比較し、素材の解像度不足と補間の差を別に判定する。

## 実装後の検証

同じLCDスナップショット（全点灯文字、通常文字、全点灯メーター、消灯ドット）を固定入力にする。
UI倍率100/125/150/200%、DPI 1/1.25/1.5/2で比較し、モニター移動でも確認する。
旧low・新方式・88Emuの画面を同じ物理寸法で比較する。
隙間の消失、濃さの周期的な変化、背景とのずれ、リサイズ時のちらつきを調べる。
Windows/Linux/macOSの実画面確認までは「全OSで解消」としない。

## 実装結果（2026-09-24）

`Plugins/Source/SC55LcdRenderer.h` を追加し、`LcdDisplay` から使用する。
固定解像度で背景とドットを合成し、明示的な `SoftwareImageType` 上でJUCEの
medium（bilinear）を使用する。画面のネイティブ描画バックエンドに縮小を任せない。
画面DPIと親Componentの倍率を `getPhysicalPixelScaleFactor()` から取得し、
物理解像度のキャッシュを作成。最終提示はlowとし、二度目の平滑化を避ける。
内容更新・物理サイズ変更時に再計算する。音声処理は変更していない。

88EmuのGPU/RmlUi自体は導入せず、固定元画像と最終線形補間という処理を共通CPU描画で再現した。
端数の位置での最終提示には依然ネイティブ画面描画が関わるため、OS間の最終画素の完全一致を保証するものではない。

`tools/lcd-rendering` に実際の提示クラスを使うROM不要の描画テストを追加。
旧最近傍経路では8条件が失敗し、新経路では全条件成功。
文字・メーターの境界濃淡、物理解像度での補間、キャッシュ再利用、表示OFF/ON、
同一サイズでのフレーム更新を確認した。描画画像も目視確認した。
端数倍率では最終提示の丸め差を考慮し、参照画像との誤差は各色2/255まで許容する。

macOS arm64のReleaseビルドでStandalone・VST3・AUv3が成功。
過去のarchive出力のsymlinkと衝突したため、出力先を `Plugins/build/LcdRenderingProducts` に指定した。
Windows/Linuxのビルド・実画面、プラグインホストへのロード、macOS実ウィンドウの目視確認は未実施。

## 最終実装: TX81Zと同じ矩形直接描画

ユーザー提示のTX81Zを調べたところ、実際のLCDは `Source/UI/CharacterLcdComponent.cpp`
の `paint()` が `RectangleList<float>` に点灯・消灯のドット矩形を作り、
`fillRectList` で直接描画していた。`Source/PluginEditor.cpp` は `Scaling::fit` を設定。
画像の拡縮ではなく、JUCEが最終解像度で矩形の被覆率を描画する方式。
「AffineTransformでは対応できない」という説明は、画像描画だけに議論を限定した誤りだった。

SC-55でも同じ方式を採用。既存エミュレータのマスクから文字・メーターの矩形を復元し、
点灯・消灯の `RectangleList<float>` にまとめる。`AffineTransform` で配置を変換し、
`fillRectList` で直接描画する。背景の静止画像のみ通常のJUCE画像描画を使う。
独自の面積平均・補間ウェイト・物理解像度画像キャッシュは削除した。
SC-55の文字形状・メーター配置・色は維持する。

追加の回帰テストでは、6画素周期の細い隙間を1画素へ縮小し、6種類の位相すべてで
平均輝度が保たれることを確認。以前の画像bilinear経路では全6条件が失敗した。
50〜400%の変換、繰り返し描画、消灯・再点灯・更新も確認。
実背景・実フォントのROM不要Piano 1プレビューを244×88で生成し、目視確認する。
Windows/Linuxの実ウィンドウ確認は別途必要。
