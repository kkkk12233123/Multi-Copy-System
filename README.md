# Multi-Copy-System

[English](#english) · [日本語](#日本語)

## English

Multi-Copy-System is a lightweight Windows clipboard manager. It keeps up to ten recent text items, five images, five file selections, and three permanent slots. The keyboard picker and the settings app are included in the same Windows package.

### Download

1. Open [GitHub Releases](https://github.com/kkkk12233123/Multi-Copy-System/releases) and download `Multi-Copy-System-windows-x64.zip` from **Assets**. Use the build instructions below to create the executables from source.
2. Extract the ZIP to a folder you plan to keep. Both executables are directly inside the extraction folder. Keep them together.

   ```text
   <extraction folder>/
   ├── MultiCopySystem.exe
   ├── MultiCopySettings.exe
   └── README.md
   ```

3. Run `MultiCopySystem.exe`. It stays in the Windows notification area. Run `MultiCopySettings.exe` or open **Settings** (`設定を開く`) from the tray icon.

   If Microsoft Defender SmartScreen shows **Windows protected your PC**, check that you downloaded the ZIP from this project's GitHub Releases page. Click **More info**, confirm that the app name matches the EXE you opened, and select **Run anyway** if that button appears. If you do not see the SmartScreen prompt, start the app as usual.

4. To start automatically, select **Settings** (`設定`) → **Start at Windows sign-in** (`Windows サインイン時に常駐アプリを起動`) and save. The app runs under your current Windows account.

### Default keyboard shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+C` / `Ctrl+X` | Copy or cut as usual and add the resulting text, image, or file selection to its history. |
| `Ctrl+Shift+V` | Open the text and image picker near the insertion point, with the newest item selected. Press `V` again while holding `Ctrl+Shift` to move to the next item. |
| `Ctrl+Shift+F` | Open the picker for up to five recent file selections. Press `F` again while holding `Ctrl+Shift` to move to the next item. Focus the file list in the destination File Explorer folder before using the shortcut. |
| `Ctrl+1` / `Ctrl+2` / `Ctrl+3` | Paste from the corresponding permanent slot. |
| `Ctrl+V` | Paste from the Windows clipboard as usual. |

Both pickers support the arrow keys, mouse wheel, and pointer. Release the keys to paste or press `Esc` to cancel. The text and image picker shows the five most recent text items in a vertical list, with up to five numbered photo icons underneath. The **History** tab shows all ten recent text items. When the app cannot find the insertion point, it places the picker near the focused control or pointer.

A file-history entry stores the paths from one copy or cut operation, including multiple files or folders selected together. Keeping these paths in history uses only a small amount of memory. Pasting into File Explorer creates copies at the destination and uses disk space there. Keep the originals available at their saved paths. Pasting from file history always copies, including entries recorded with `Ctrl+X`.

Special pastes put the selected item on the Windows clipboard for subsequent `Ctrl+V` presses.

### Settings

- **Permanent slots** (`永久保存`) – Type text or register the current clipboard contents, including images, in slots 1–3. The slots are stored in `%LOCALAPPDATA%\MultiCopySystem` and survive restarting Windows.
- **History** (`履歴`) – Remove one item or clear all recent items. Recent items are held in memory and cleared when the background app exits.
- **Settings** (`設定`) – Change the popup position, preview length, shortcuts, and auto-start. You can pause history recording or clear the data stored by the app. The preview limit only affects what the picker shows.

Text and image items can be up to 16 MiB each. For file entries, the limit applies to the stored path list. Text uses Unicode plain text, and images use DIB, DIBV5, or PNG. Pasting to a Windows app running as administrator requires the same permission level.

### Build from source

Download the source with **Code → Download ZIP** on the [repository page](https://github.com/kkkk12233123/Multi-Copy-System), or clone it with Git. Run the following commands from the folder containing `CMakeLists.txt`, using a terminal with Visual Studio 2022 or later, the C++ desktop build tools, a Windows SDK, and CMake available.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build creates these paths relative to the folder containing `CMakeLists.txt`.

```text
build/Release/MultiCopySystem.exe
build/Release/MultiCopySettings.exe
```

The x64 build targets Windows 10 (version 1703 or later) and Windows 11.

To remove the app, turn off sign-in launch, clear its stored data in **Settings**, exit it from the notification area, and delete the extracted folder.

## 日本語

Multi-Copy-System は Windows 向けの軽量なクリップボード管理ツールです。テキスト履歴を最大10件、画像とファイル選択の履歴を各5件、永久保存を3枠まで保持します。履歴はキーボードで呼び出せます。永久保存への登録や表示の変更は専用の設定画面から行います。

### ダウンロード

1. [GitHub Releases](https://github.com/kkkk12233123/Multi-Copy-System/releases)の **Assets** から `Multi-Copy-System-windows-x64.zip` をダウンロードします。ソースから実行ファイルを作る方法は下記に記載しています。
2. ZIP を使い続けるフォルダーに展開します。2つのEXEは展開先フォルダーの直下にあります。そのまま同じフォルダーに置いて使います。

   ```text
   展開先フォルダー/
   ├── MultiCopySystem.exe
   ├── MultiCopySettings.exe
   └── README.md
   ```

3. `MultiCopySystem.exe` を起動すると、通知領域に常駐します。設定を開くには `MultiCopySettings.exe` を実行するか、通知領域のアイコンを右クリックして **設定を開く** を選びます。

   Microsoft Defender SmartScreen の **Windows によって PC が保護されました** が表示された場合は、このプロジェクトの GitHub Releases からZIPを入手したことを確認し、**詳細情報** をクリックします。表示されたアプリ名が開いたEXEと一致し、**実行** ボタンが表示されていれば、**実行** を選びます。この画面が出ない場合は、通常どおり起動できます。

4. 自動起動を使うには、**設定** タブで **Windows サインイン時に常駐アプリを起動** をオンにして **設定を保存** を押します。

### 初期設定のショートカット

| キー操作 | 動作 |
| --- | --- |
| `Ctrl+C` / `Ctrl+X` | 通常どおりコピーまたは切り取りをし、テキスト・画像・ファイル選択を履歴に追加します。 |
| `Ctrl+Shift+V` | テキスト・画像の履歴を、最新の候補を選んだ状態で入力位置の近くに表示します。`Ctrl+Shift` を押したまま `V` を押し直すと、次の候補に移動します。 |
| `Ctrl+Shift+F` | ファイル履歴を最大5件表示します。`Ctrl+Shift` を押したまま `F` を押し直すと、次の候補に移動します。使う前にエクスプローラーでコピー先を開き、ファイル一覧にフォーカスを置きます。 |
| `Ctrl+1` / `Ctrl+2` / `Ctrl+3` | 対応する永久保存枠の内容を貼り付けます。 |
| `Ctrl+V` | 通常どおり Windows のクリップボードから貼り付けます。 |

どちらの候補画面も矢印キー、ホイール、マウスで選べます。キーを離すと貼り付け、`Esc` で取り消します。テキスト・画像の候補には、最新のテキスト5件を縦に、画像最大5件を番号付きの写真アイコンで下段に表示します。テキスト履歴の10件すべては設定画面の **履歴** タブで確認できます。入力位置を取得できないときは、入力欄やマウスポインターの近くに候補を表示します。

ファイル履歴には、1回のコピーや切り取りで選んだファイル・フォルダーのパスを保存します。複数をまとめて選んだ操作も履歴1件です。履歴の保存で使うメモリはパスの分だけです。エクスプローラーに貼り付けるとコピー先に実ファイルが作られ、その分のディスク容量を使います。元のファイルは保存時の場所からアクセスできる状態にしてください。`Ctrl+X` で記録したファイルも、履歴から貼り付けるとコピーになります。

履歴や永久保存から貼り付けた内容は Windows のクリップボードにも入り、次の `Ctrl+V` で使えます。

### 設定画面

- **永久保存**。枠1〜3にテキストを入力するか、現在のクリップボードのテキスト・画像を登録できます。保存枠は `%LOCALAPPDATA%\MultiCopySystem` に保存され、Windows の再起動後も残ります。
- **履歴**。履歴を1件ずつ、またはまとめて削除できます。履歴はメモリに保存し、常駐アプリを終了すると消えます。
- **設定**。表示位置、文字数上限、ショートカット、自動起動を変更できます。履歴記録の一時停止と、アプリの保存データの全消去も行えます。文字数上限は候補表示に適用され、貼り付け時は全文を使います。

テキストと画像は1件あたり16 MiBまでです。ファイル履歴の上限は保存するパス一覧に適用されます。テキストはUnicodeのプレーンテキスト、画像はDIB・DIBV5・PNGに対応します。管理者として実行中のアプリへの貼り付けには、同じ権限が必要です。

### ソースからビルド

[リポジトリ](https://github.com/kkkk12233123/Multi-Copy-System)の **Code → Download ZIP** でソースを入手するか、Gitでクローンします。Visual Studio 2022以降、C++ デスクトップ開発ツール、Windows SDK、CMake が使える端末で、`CMakeLists.txt` のあるフォルダーから以下を実行します。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

ビルド後は、`CMakeLists.txt` のあるフォルダーを基準に以下の場所にEXEが出力されます。

```text
build/Release/MultiCopySystem.exe
build/Release/MultiCopySettings.exe
```

x64版の対象OSは Windows 10（バージョン1703以降）と Windows 11 です。

削除する場合は、設定で自動起動をオフにして保存データを全消去し、通知領域のメニューから終了して、展開先のフォルダーを削除します。
