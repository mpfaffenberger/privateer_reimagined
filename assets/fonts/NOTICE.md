# assets/fonts — font assets

ImGui UI font bundled for the game's Debug panel and in-app UI.

## dosgame-pixel-font.ttf

- **Font:** "DOS Game Pixel Font" (embedded name table, `Copyright SFO 2026`)
- **Source / attribution:** **Friend Computer from the WCNews Discord**.
  Provided via the project owner's local download
  (`~/Downloads/dosgame-pixel-font.ttf`); no separate license file accompanied
  the download.
- **Use:** loaded by `src/debug_panel.cpp` as the ImGui default font via
  `AddFontFromFileTTF`, replacing the previous Inter-Regular.ttf. A pixel font
  chosen to match the game's DOS-era art style.
- **Note:** no redistributable open license was published with the file. It is
  included in this codebase at the owner's discretion; swap it out before any
  public redistribution if its license cannot be confirmed.
