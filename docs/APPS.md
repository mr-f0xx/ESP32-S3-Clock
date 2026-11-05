# Extension apps

The launcher now has six pages / 22 tiles. The six extension apps are constructed
only when opened and release their heavy resources when you leave them or enter
standby. They use the existing theme palette and the shared 56 px back button.

> **Hardware validation status:** the firmware is compile-tested in CI and the
> protocol/crypto/layout helpers have native tests. The ES7210 microphone path,
> USB HID composite enumeration, sampler latency and emulator frame rate still
> need verification on the physical ESP32-S3-Touch-AMOLED-2.16. No claim is made
> that a successful compile proves these hardware paths work.

## Subsonic / Gonic

- Compatible with the Subsonic API 1.16.1 (Gonic, Navidrome and compatible servers).
- Open **Setup**, enter `http://host:port` or `https://host`, username and password,
  then **Save**. **Random songs** loads up to 60 tracks; search uses `search3`.
- Authentication uses a fresh random salt and `MD5(password + salt)` token, as the
  Subsonic protocol requires. The password itself is not placed in the stream URL.
- Streams request `format=mp3&maxBitRate=192` and use the shared ES8311 output with
  a 256 KiB PSRAM buffer. Only one audio app plays at a time.
- **HTTPS certificate validation is mandatory:** put your server's PEM CA certificate
  at `/subsonic-ca.pem` on the TF card. Plain HTTP is allowed for a trusted LAN but
  carries replayable Subsonic tokens; do not expose the setup portal or server to the
  internet. Redirects are disabled so credentials/tokens are not forwarded to another
  host.
- Server URL, username and password are stored in device NVS for unattended playback;
  they are **not** part of the encrypted password vault. Use a dedicated server account.
- Pause stops the network stream; Play restarts the selected track from the beginning.

## Voice memos

- Records ES7210 MIC1/MIC2 as stereo 16 kHz / 16-bit PCM WAV into `/memos/`.
- Audio is streamed to TF in small blocks; a 30-minute safety limit and a 512 MiB
  per-file ceiling prevent unbounded recordings. The live meter shows input peak.
- Tap a memo to play it. **Delete** removes the selected memo. Files remain on the card.
- Recording stops safely on app exit, alarm, another audio app, or standby. A WAV
  interrupted by power loss may have an unfinished header and need repair.
- The ES7210 driver is Espressif's Apache-2.0 register sequence, adapted to share
  Arduino Wire with the existing ES8311/touch/PMU bus. Capture uses I2S RX on GPIO10,
  with the existing BCLK9 / LRCK45 / MCLK42 clocks. The expected I2C address is `0x40`.

## Sample studio (MPC-style)

- Put up to 16 PCM WAV files at `/samples/01.wav` … `/samples/16.wav`.
  Supported input: mono/stereo, 8/16/24/32-bit integer PCM or 32-bit float, 4–192 kHz.
- On entry, the app allocates up to 6 MiB of PSRAM (never more than free PSRAM minus
  a 1.5 MiB reserve), divided equally across the 16 pads. At 16 kHz mono this is about
  12 seconds per pad with the full budget. Longer files are truncated to the pad quota.
- Sixteen-voice polyphonic mixer; retriggering a pad chokes its previous voice. Pads
  read directly from PSRAM with no per-hit allocation. Hold duration maps to velocity
  (a capacitive screen cannot measure real pressure); the border ring decays after release.
- Swipe vertically in the **left edge lane** for pitch (± semitone steps, 0.5×–2×);
  swipe in the **right edge lane** for the one-pole low-pass filter. **Reset** restores
  1.0× pitch and full bandwidth.
- **Sample** records the dual microphones into the selected pad; press again to stop.
  **Save** writes that pad back to its numbered TF WAV (mono). RAM samples are lost on exit.
- Input is converted to a common 16 kHz mono engine. This is a performance instrument,
  not a sample-accurate DAW; hardware latency must be measured on the device.

## Game Boy

- Open-source **Peanut-GB** (MIT), original **Game Boy / DMG only**. Game Boy Color-only
  ROMs are rejected. No PICO-8 runtime, Nintendo boot ROM, or game ROMs are bundled.
- Put legally owned/homebrew `.gb` files in `/roms/` (up to 8 MiB each). Battery-backed
  saves are written to the matching `.sav`, with a `.bak` previous-save copy and periodic
  autosave while playing.
- Touch D-pad, A/B, Start/Select, Pause, Save and ROM picker. The shared **top-left
  back button stays above the ROM picker**, so you can return to the launcher even
  when `/roms` is empty. The LCD is doubled to 320×288 and targets 30 fps on the
  QSPI panel. **Sound is disabled** in this first frontend; compatibility and frame
  rate depend on the cartridge and hardware.

## WiFi / RF HUD

- Normal Wi-Fi scan lists nearby SSIDs, RSSI and channels. The monitor shows aggregate
  management/data/control frame counts, frames/s history and RSSI buckets.
- When disconnected it hops channels 1–13; when connected it stays on the AP channel.
  **Ch- / Ch+** manually tune only while disconnected. **Pause** stops the monitor.
- Privacy boundary: the callback inspects only the frame-type bits and RSSI. It does
  not store or display packet payloads, MAC addresses, handshakes or credentials, and
  never uploads or writes captures. This is a local diagnostic tool, not a deauth or
  credential-capture tool.
- The promiscuous callback is removed and the radio restored on exit/standby.

## Password vault + USB typing

- Create/unlock with a **10–32 digit master PIN**. There is no recovery path.
- The TF file `/vault/passwords.bin` is a versioned container encrypted with
  **AES-256-GCM**. The key is derived with **PBKDF2-HMAC-SHA256, 600,000 iterations**,
  a 16-byte hardware-random salt and a fresh 12-byte nonce per write. The header is
  authenticated as AAD; a 16-byte tag detects tampering. No plaintext database, PIN or
  derived key is written to TF. `.bak` is also encrypted.
- Up to 24 entries. Passwords are never displayed in the list/detail view. Keys and
  records are wiped on lock, exit and standby; auto-lock is two minutes idle. Sensitive
  textarea buffers are cleared before release. Five failed attempts start a cooldown.
- Connect Type-C to the target computer, focus its password field, select an entry,
  press **Type password**, then explicitly confirm **Type now**. The clock enumerates
  as a USB keyboard alongside CDC serial. **US / FR** selects QWERTY or AZERTY. No Enter
  is sent. **Cancel typing** aborts and releases all keys. Printable ASCII only.
- The HID interface must be registered before TinyUSB starts; the keyboard object is
  therefore constructed at boot, but the vault UI, keys and records remain lazy/locked.
  The Arduino core must have `CONFIG_TINYUSB_HID_ENABLED=y` (the CI build checks this).

### Security limits

This is authenticated encryption at rest, not a secure element. An attacker who
captures the device **while unlocked** can read RAM or use the unlocked UI. A weak PIN
is still vulnerable to offline guessing despite the KDF; use the full PIN length and
keep encrypted backups. FAT rename/backup is not a power-fail-proof journal: keep a
copy of the encrypted file. USB typing trusts the connected host—malware or the wrong
focused field can receive the password. The vault does not protect against a compromised
host, DMA attack, or physical extraction while powered and unlocked.

## Native tests

```sh
bash tests/run_extension_apps_test.sh
bash tests/run_vault_crypto_test.sh   # fetches pinned mbedtls 3.6.4 if MBEDTLS_DIR is unset
bash tests/run_launcher_icons_test.sh
bash tests/run_ui_regressions_test.sh
bash tests/run_clock_layout_test.sh
bash tests/run_clock_fonts_test.sh
```

The vault test covers a PBKDF2 known-answer vector, GCM round-trip, wrong PIN,
header/ciphertext/tag tampering, truncation and capacity bounds. Firmware compilation
and artifact size are checked by the existing GitHub Actions workflow.
