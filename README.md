# Multi-Copy-System

[English](#english) · [日本語](#日本語)

## English

Multi-Copy-System is a lightweight Windows clipboard manager. It keeps up to five recent text items, five recent images, and three permanent slots. The keyboard picker and the settings app are included in the same Windows package.

### Download

1. Open [GitHub Releases](https://github.com/kkkk12233123/Multi-Copy-System/releases) and download the Windows ZIP from **Assets** when a release is available. Use the build instructions below to create the executables from source.
2. Extract the ZIP to a folder you plan to keep. Keep `MultiCopySystem.exe` and `MultiCopySettings.exe` together.
3. Run `MultiCopySystem.exe`. It stays in the Windows notification area. Run `MultiCopySettings.exe` or open **Settings** (`設定を開く`) from the tray icon.
4. To start automatically, select **Settings** (`設定`) → **Start at Windows sign-in** (`Windows サインイン時に常駐アプリを起動`) and save. The app runs under your current Windows account.

### Default keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+C` / `Ctrl+X` | Copy or cut as usual and add the resulting text or image to its history. |
| `Ctrl+Shift+V` | Open the picker with the newest item selected. Use the arrow keys, mouse wheel, or pointer, or press `V` again while holding `Ctrl+Shift`. Release the keys to paste. Press `Esc` to cancel. |
| `Ctrl+1` / `Ctrl+2` / `Ctrl+3` | Paste from the corresponding permanent slot. |
| `Ctrl+V` | Paste from the Windows clipboard as usual. |

Text appears in a vertical list. Copied images appear underneath as numbered photo icons. Special pastes put the selected item on the Windows clipboard for subsequent `Ctrl+V` presses.

### Settings

- **Permanent slots** (`永久保存`) – Type text or register the current clipboard contents, including images, in slots 1–3. The slots are stored in `%LOCALAPPDATA%\MultiCopySystem` and survive restarting Windows.
- **History** (`履歴`) – Remove one item or clear all recent items. Recent items are held in memory and cleared when the background app exits.
- **Settings** (`設定`) – Change the popup position, preview length, shortcuts, and auto-start. You can pause history recording or clear the data stored by the app. The preview limit only affects what the picker shows.

Each clipboard item can be up to 16 MiB. Supported items are plain Unicode text and Windows clipboard images in DIB, DIBV5, or PNG format. Pasting to a Windows app running as administrator requires the same permission level.

### Build from source

Download the source with **Code → Download ZIP** on the [repository page](https://github.com/kkkk12233123/Multi-Copy-System), or clone it with Git. From a terminal with Visual Studio 2022 or later, the C++ desktop build tools, a Windows SDK, and CMake available, run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Run the two executables from `build/Release`. The x64 build targets Windows 10 (version 1703 or later) and Windows 11.

To remove the app, turn off sign-in launch, clear its stored data in **Settings**, exit it from the notification area, and delete the extracted folder.

## 日本語

Multi-Copy-System は Windows 向けの軽量なクリップボード管理ツールです。テキストと画像の履歴をそれぞれ最大5件、永久保存を3枠まで保持します。履歴の呼び出しと貼り付けはキーボードで操作でき、登録や表示の変更は専用の設定画面で行います。

### ダウンロード

1. [GitHub Releases](https://github.com/kkkk12233123/Multi-Copy-System/releases)で、リリース公開後に **Assets** から Windows 用のZIPをダウンロードします。ソースから実行ファイルを作る方法は下記に記載しています。
2. ZIP を使い続けるフォルダーに展開します。`MultiCopySystem.exe` と `MultiCopySettings.exe` は同じフォルダーに置きます。
3. `MultiCopySystem.exe` を起動すると、通知領域に常駐します。設定を開くには `MultiCopySettings.exe` を実行するか、通知領域のアイコンを右クリックして **設定を開く** を選びます。
4. 自動起動を使うには、**設定** タブで **Windows サインイン時に常駐アプリを起動** をオンにして **設定を保存** を押します。

### 初期設定のショートカット

| キー操作 | 動作 |
| --- | --- |
| `Ctrl+C` / `Ctrl+X` | 通常どおりコピーまたは切り取りをし、取得したテキストや画像を履歴に追加します。 |
| `Ctrl+Shift+V` | 最新の候補を選択した状態で履歴を表示します。矢印キー、ホイール、マウスで選択できます。`Ctrl+Shift` を押したまま `V` を押し直しても移動できます。キーを離すと貼り付け、`Esc` で取り消します。 |
| `Ctrl+1` / `Ctrl+2` / `Ctrl+3` | 対応する永久保存枠の内容を貼り付けます。 |
| `Ctrl+V` | 通常どおり Windows のクリップボードから貼り付けます。 |

テキストは縦のリスト、画像は下段の番号付き写真アイコンで表示します。履歴や永久保存から貼り付けた内容は Windows のクリップボードにも入り、次の `Ctrl+V` で使えます。

### 設定画面

- **永久保存**。枠1〜3にテキストを入力するか、現在のクリップボードのテキスト・画像を登録できます。保存枠は `%LOCALAPPDATA%\MultiCopySystem` に保存され、Windows の再起動後も残ります。
- **履歴**。履歴を1件ずつ、またはまとめて削除できます。履歴はメモリに保存し、常駐アプリを終了すると消えます。
- **設定**。表示位置、文字数上限、ショートカット、自動起動を変更できます。履歴記録の一時停止と、アプリの保存データの全消去も行えます。文字数上限は候補表示に適用され、貼り付け時は全文を使います。

保存できるデータは1件あたり16 MiBまでです。Unicodeのプレーンテキストと、DIB・DIBV5・PNG形式のクリップボード画像に対応します。管理者として実行中のアプリへの貼り付けには、同じ権限が必要です。

### ソースからビルド

[リポジトリ](https://github.com/kkkk12233123/Multi-Copy-System)の **Code → Download ZIP** でソースを入手するか、Gitでクローンします。Visual Studio 2022以降、C++ デスクトップ開発ツール、Windows SDK、CMake が使える端末で実行します。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

`build/Release` に出力される2つの実行ファイルを使います。x64版の対象OSは Windows 10（バージョン1703以降）と Windows 11 です。

削除する場合は、設定で自動起動をオフにして保存データを全消去し、通知領域のメニューから終了して、展開先のフォルダーを削除します。
