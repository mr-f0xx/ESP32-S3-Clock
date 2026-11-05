# Device themes

The clock keeps the existing seven themes and adds three append-only choices:

| ID | Theme | Short pill | Background | Surface | Primary text | Accent |
|---:|---|---|---|---|---|---|
| 7 | **Matrix Terminal** | `MTX` | `#000000` | `#031006` | `#00FF41` | `#00FF41` |
| 8 | **Tokyo Night** (night variant) | `TKY` | `#1A1B26` | `#24283B` | `#C0CAF5` | `#7AA2F7` |
| 9 | **Dracula** | `DRC` | `#282A36` | `#44475A` | `#F8F8F2` | `#BD93F9` |

Select them in the device web page under **Appearance → Theme**, or cycle the
control-centre theme pill through `MTX`, `TKY`, and `DRC`. Existing saved theme
IDs are unchanged; the new entries only extend the schema-2 range.

## Visual treatment

- **Matrix Terminal** is a crisp green-on-black terminal look: true-black pages,
  dark-green panels, thin green rules, nearly square keys/tiles, and `> ` shell
  prompts on control labels and extension headings. It does not add animated
  code rain, scanlines, or a global CRT effect. The clock face still uses the
  separately selected clock font/size; the default “THM” clock color follows the
  theme text color.
- **Tokyo Night** uses the familiar night-blue editor background, storm-blue
  surfaces, soft foreground text, and blue accent. It is flat and halo-free.
- **Dracula** uses the classic background/current-line surfaces, foreground
  text, comment-colored borders, and purple accent. Secondary text is slightly
  lifted from the upstream comment color, and small accent-label controls use
  the darker page background, for small-screen readability.

Palette references: [Tokyo Night](https://github.com/folke/tokyonight.nvim) and
[Dracula](https://github.com/dracula/dracula-theme). These are color/palette
inspirations; no upstream editor runtime or assets are bundled.

## Related UI fixes

- The Game Boy ROM picker now keeps the shared **top-left back button** above
  the full-screen list, so an empty `/roms` folder is no longer a trap. The
  picker title starts to the right of the button.
- The control-centre Wi-Fi card uses two fixed single-line rows: SSID/status
  ellipsizes on the first row, and the IP address stays on the second row. Long
  network names can no longer wrap into the IP text.

Theme changes are applied through the existing full-ui refresh path. Extension
apps are also rebuilt on entry; the active Game Boy picker is recolor-safe and
keeps its back button raised. Physical AMOLED appearance still needs a quick
on-device check after flashing.
