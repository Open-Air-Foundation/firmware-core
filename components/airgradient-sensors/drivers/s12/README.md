# SenseAir S12 Driver

I2C driver for SenseAir S12 CO2 sensors. It provides concentration reads,
manual target calibration, and automatic background calibration (ABC)
configuration through the shared `CO2Sensor` interface.

## EEPROM Configuration

ABC Period (`0x9A`/`0x9B`) and MeterControl (`0xA5`) are EEPROM-backed. Before
writing either field, the driver reads and logs its current semantic value and
skips the write when the requested configuration already matches.

For each changed field, the driver sends one write and treats `ESP_OK` as
acceptance by the sensor. It then waits 300 ms, exceeding the documented 180 ms
EEPROM update time. After all changed fields have settled, the driver writes
`0xFF` to SCR (`0xA3`) once and waits 60 ms for communication to resume.

The driver reads and logs the ABC values again after reset. These reads are
diagnostic only: their values and read status do not validate the accepted
EEPROM write or change the configuration result.
