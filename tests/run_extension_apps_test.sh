#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
g++ -std=c++17 -Wall -Wextra -Werror tests/subsonic_protocol_test.cpp -o /tmp/subsonic_protocol_test
/tmp/subsonic_protocol_test
g++ -std=c++17 -Wall -Wextra -Werror tests/wav_pcm_test.cpp -o /tmp/wav_pcm_test
/tmp/wav_pcm_test
python3 - <<'PY'
from pathlib import Path
import re
root = Path('arduino/08_LVGL_Clock')
menu = (root/'clock_app_menu.inc').read_text()
ext = (root/'clock_extensions.inc').read_text()
sketch = (root/'08_LVGL_Clock.ino').read_text()
power = (root/'clock_power.inc').read_text()
theme = (root/'clock_theme.inc').read_text()
control = (root/'clock_control_panel.inc').read_text()
assert 'APP_MENU_TILE_CAPACITY = 22' in menu
assert 'APP_MENU_PAGE_COUNT = 6' in (root / '08_LVGL_Clock.ino').read_text()
assert 'EXTENSION_APP_COUNT = 6' in menu
for name in ['subsonic','recorder','gameboy','wifi_hud','sampler','vault']:
    assert f'#include "clock_{name}.inc"' in ext
for hook in ['processExtensionTransitions();','serviceExtensionApps();']:
    assert sketch.count(hook) >= 2
assert 'suspendExtensionAppsForLowPower();' in power
assert 'lowPowerRestoreExtension = activeExtensionApp();' in power
assert 'refreshExtensionThemes();' in theme
assert 'if (extensionAppVisible() && !controlPanelVisible) return;' in control
assert 'extensionPageGestureEvent' in ext and 'stopMusicPlayback();' in ext
# Constructors are reached only through the lazy dispatch, never createAppMenu.
create_menu = menu.split('void createAppMenu() {',1)[1].split('void refreshAppMenuTheme()',1)[0]
for name in ['createSubsonicPage','createRecorderPage','createGameboyPage','createWifiHudPage','createSamplerPage','createVaultPage']:
    assert name not in create_menu
    assert f'{name}(); break;' in ext
assert 'heap_caps_malloc' not in ext  # heavy buffers belong to individual apps
capture = (root/'clock_capture.inc').read_text()
assert 'PIN_ES7210_DIN' in capture and 'i2s_new_channel(&channel, nullptr, &captureRx)' in capture
assert 'es7210_config_codec' in capture and 'stopMicrophoneCapture' in capture
assert (root / 'src' / 'es7210.c').exists()
assert 'loadSubsonicTracks' in (root / 'clock_subsonic.inc').read_text()
sampler = (root/'clock_sampler.inc').read_text()
assert 'SAMPLER_PADS = 16' in sampler and 'MALLOC_CAP_SPIRAM' in sampler
assert 'samplerPitch' in sampler and 'samplerCutoff' in sampler and 'velocity' in sampler
assert 'beginMicrophoneCapture(samplerCaptureSink)' in sampler
vault = (root/'clock_vault.inc').read_text()
crypto = (root/'src/vault_crypto.h').read_text()
assert 'mbedtls_gcm_auth_decrypt' in crypto and 'mbedtls_pkcs5_pbkdf2_hmac' in crypto
assert 'VAULT_KDF_ITERATIONS = 600000' in crypto
assert 'VaultCrypto::MAX_FILE' not in (root / 'clock_vault.inc').read_text()
assert 'mbedtls_pkcs5_pbkdf2_hmac_ext' in crypto
sketch = (root / '08_LVGL_Clock.ino').read_text()
assert sketch.index('#include <ArduinoJson.h>') < sketch.index('namespace {')
assert '#include <ArduinoJson.h>' not in (root / 'clock_subsonic.inc').read_text()
assert 'int status() { return http.connected()' in (root / 'clock_subsonic.inc').read_text()
assert 'bootloader_fill_random' in vault and 'vaultTypeConfirmed' in vault
assert 'Confirm USB typing' in vault and 'vaultStopTyping' in vault
assert 'vaultLock();' in ext and 'VaultCrypto::wipe(vaultRecords' in vault
assert '/vault/passwords.bin' in vault and 'FILE_WRITE' in vault
assert 'preferences.putString("vault' not in vault
hud = (root/'clock_wifi_hud.inc').read_text()
assert 'esp_wifi_set_promiscuous_rx_cb(nullptr)' in hud and 'esp_wifi_set_promiscuous(false)' in hud
assert 'payload[0]' in hud and 'payload[2]' not in hud
gb = (root/'clock_gameboy.inc').read_text()
assert 'gbRom[0x143] & 0x80' in gb and 'gbPersistSave' in gb and 'ENABLE_SOUND 0' in gb
print('extension apps: lazy lifecycle, six launcher tiles, standby/theme hooks, capture, sampler, authenticated vault, explicit HID, RF privacy, DMG saves OK')
PY