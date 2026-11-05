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
assert 'SD.rename(temporary.c_str(), path.c_str())' in utils
assert 'UTILITY_NOTE_MAX_LENGTH = 4096' in utils
assert 'STR_NOTE_SD_SAVED' in i18n and 'STR_NOTE_LOCAL_SAVED' in i18n
assert 'void utilityReleaseSdAudio()' in utils
assert 'stopMusicPlayback();' in utils and 'stopAudiobookPlayback();' in utils
assert 'if (forceRemount) utilityReleaseSdAudio();' in utils

# One shared SD owner: SPI3/HSPI, never the QSPI LCD's FSPI/SPI2. Mount attempts
# retry at safer data clocks and verify the FAT root before reporting success.
assert 'SPIClass musicSdSpi(HSPI);' in music
assert 'SPIClass musicSdSpi(FSPI)' not in music
assert 'MUSIC_SD_MOUNT_FREQUENCIES[]' in music
assert re.search(r'MUSIC_SD_FREQUENCY,\s*4000000,\s*1000000', music)
mount = music.split('bool mountMusicSd() {', 1)[1].split('bool remountMusicSd()', 1)[0]
assert 'for (uint32_t frequency : MUSIC_SD_MOUNT_FREQUENCIES)' in mount
assert 'SD.begin(SD_CS_PIN, musicSdSpi, frequency, "/sd", 10, false)' in mount
assert 'File root = SD.open("/");' in mount and 'root.isDirectory()' in mount
assert 'digitalWrite(SD_CS_PIN, HIGH);' in mount
assert 'void unmountMusicSd()' in music and 'bool remountMusicSd()' in music
assert 'unmountMusicSd();' in music.split('void suspendMusicForLowPower', 1)[1]
assert 'bool mountMusicSd();' in sketch and 'bool remountMusicSd();' in sketch
assert sketch.index('bool mountMusicSd();') < sketch.index('#include "clock_utilities.inc"')
assert 'setMusicStatus(STR_SD_UNAVAILABLE);' in music
assert 'setAudiobookStatus(STR_SD_UNAVAILABLE);' in audiobook
print('sd/notes: reader-first Notes, /notes atomic SD storage, legacy migration, SPI3 retry mount OK')
PY
