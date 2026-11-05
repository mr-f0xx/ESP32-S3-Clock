# ES7210 capture codec

Vendored from Espressif esp-bsp commit `67f41a169401d1f23e827188dd08fb351cb0febe`,
`components/es7210`, Apache-2.0 (license.txt).

Local change: register writes use `esp32-hal-i2c.h` / `i2cWrite`, matching the
existing ES8311 driver and sharing Arduino Wire rather than installing a second,
legacy I2C driver on the same bus. Register sequences are otherwise unchanged.
