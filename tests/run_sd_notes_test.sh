#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 - <<'PY'
from pathlib import Path
import re

root = Path('arduino/08_LVGL_Clock')
utils = (root / 'clock_utilities.inc').read_text()
music = (root / 'clock_music.inc').read_text()
audiobook = (root / 'clock_audiobook.inc').read_text()
sketch = (root / '08_LVGL_Clock.ino').read_text()
i18n = (root / 'clock_i18n.inc').read_text()

# Notes opens as a reader. The keyboard is constructed only by the explicit
# editor builder, never by the reader/list path.
assert 'void buildUtilityNotesReader(' in utils
assert 'void buildUtilityNotesEditor(' in utils
open_notes = utils.split('} else if (app == 1) {', 1)[1].split('} else {', 1)[0]
assert 'if (!utilityNotesPrepared) prepareUtilityNotes(false);' in open_notes
assert 'if (utilityNoteEditing) buildUtilityNotesEditor' in open_notes
assert 'else buildUtilityNotesReader' in open_notes
assert 'lv_keyboard_create' not in open_notes
reader = utils.split('void buildUtilityNotesReader(', 1)[1].split('void buildUtilityNotesEditor(', 1)[0]
assert 'lv_keyboard_create' not in reader
editor = utils.split('void buildUtilityNotesEditor(', 1)[1].split('void openUtilityApp(', 1)[0]
assert 'lv_keyboard_create' in editor
assert 'UTILITY_NOTE_DIRECTORY[] = "/notes"' in utils
assert 'UTILITY_NOTE_LEGACY_FILE[] = "/notes/legacy-note.txt"' in utils
assert 'utilityNoteMigrateLegacy' in utils and 'preferences.remove("utility_note")' in utils
assert 'utilityNoteWriteFileAtomic' in utils and '.tmp' in utils and '.bak' in utils
assert 'storageFs().rename(temporary.c_str(), path.c_str())' in utils
assert 'UTILITY_NOTE_MAX_LENGTH = 4096' in utils
assert 'STR_NOTE_SD_SAVED' in i18n and 'STR_NOTE_LOCAL_SAVED' in i18n
assert 'void utilityReleaseSdAudio()' in utils
assert 'stopMusicPlayback();' in utils and 'stopAudiobookPlayback();' in utils
assert 'if (forceRemount) utilityReleaseSdAudio();' in utils

# One shared SD owner with two transports. The TF slot is wired to the SDMMC
# host (1-bit CMD/CLK/D0 = GPIO1/2/3), so that is tried first; SPI3/HSPI (never
# the QSPI LCD's FSPI/SPI2) stays as the fallback. Both verify the FAT root
# before reporting success, and every caller reaches the card through
# storageFs() so the transports remain interchangeable.
assert 'SPIClass musicSdSpi(HSPI);' in music
assert 'SPIClass musicSdSpi(FSPI)' not in music
assert 'MUSIC_SD_SDMMC_FREQUENCIES_KHZ[]' in music
assert 'MUSIC_SD_MOUNT_FREQUENCIES[]' in music
assert re.search(r'MUSIC_SD_FREQUENCY,\s*4000000,\s*1000000', music)
assert 'fs::FS &storageFs()' in music and 'fs::FS &storageFs();' in sketch
assert sketch.index('fs::FS &storageFs();') < sketch.index('#include "clock_utilities.inc"')

native = music.split('bool mountMusicSdNative() {', 1)[1].split('bool mountMusicSdSpi() {', 1)[0]
assert 'SD_MMC.setPins(SD_SCLK_PIN, SD_MOSI_PIN, SD_MISO_PIN)' in native
assert 'SD_MMC.begin("/sd", true, false, frequencyKhz, 10)' in native
assert 'verifyMusicSdRoot()' in native

spi = music.split('bool mountMusicSdSpi() {', 1)[1].split('bool remountMusicSd()', 1)[0]
assert 'for (uint32_t frequency : MUSIC_SD_MOUNT_FREQUENCIES)' in spi
# format_if_empty must stay false: a dirty contact must never reformat a card.
assert 'SD.begin(SD_CS_PIN, musicSdSpi, frequency, "/sd", 10, false)' in spi
assert 'verifyMusicSdRoot()' in spi
assert 'digitalWrite(SD_CS_PIN, HIGH);' in spi
mount = music.split('bool mountMusicSd() {', 1)[1].split('bool mountMusicSdNative() {', 1)[0]
assert 'mountMusicSdNative()' in mount and 'mountMusicSdSpi()' in mount
assert 'storageFs().open("/")' in music and 'root.isDirectory()' in music

# No module may bypass storageFs() and touch a transport object directly; the
# inactive one would report an empty card.
assert not re.search(r'\bSD\.(open|exists|remove|rename|mkdir|rmdir)', music)
for other in (root / 'clock_utilities.inc', root / 'clock_vault.inc', root / 'clock_gameboy.inc',
              root / 'clock_weather.inc', root / 'clock_photo.inc', root / 'clock_recorder.inc',
              root / 'clock_sampler.inc', root / 'clock_subsonic.inc'):
    text = other.read_text()
    assert not re.search(r'\bSD\.(open|exists|remove|rename|mkdir|rmdir)', text), other.name
    assert not re.search(r'\bSD_MMC\.(open|exists|remove|rename|mkdir|rmdir)', text), other.name
assert 'AudioFileSourceSD' not in music and 'AudioFileSourceSD' not in audiobook

assert 'void unmountMusicSd()' in music and 'bool remountMusicSd()' in music
assert 'unmountMusicSd();' in music.split('void suspendMusicForLowPower', 1)[1]
assert 'bool mountMusicSd();' in sketch and 'bool remountMusicSd();' in sketch
assert sketch.index('bool mountMusicSd();') < sketch.index('#include "clock_utilities.inc"')
assert 'setMusicStatus(STR_SD_UNAVAILABLE);' in music
assert 'setAudiobookStatus(STR_SD_UNAVAILABLE);' in audiobook

# The user-facing docs must describe the same mount order the firmware uses,
# and must keep saying that exFAT is unsupported.
readme = Path('README.md').read_text()
apps = Path('docs/APPS.md').read_text()
assert 'exFAT' in readme and 'SDMMC' in readme
assert 'SDMMC' in apps and 'exFAT' not in apps
assert 'SD: card did not mount (SDMMC 1-bit: failed, SPI3: failed)' in readme
print('sd/notes: reader-first Notes, /notes atomic SD storage, legacy migration, SDMMC+SPI3 mount OK')
PY
