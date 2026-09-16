# BQ25629 Battery Charger Driver

Driver สำหรับ Texas Instruments BQ25629 Battery Charge Management IC

## คุณสมบัติ

- **I2C Controlled**: สื่อสารผ่าน I2C (address 0x6A)
- **Single-cell Li-Ion/Li-polymer Charger**: รองรับแบตเตอรี่ 1 เซลล์
- **Current Limit**: 
  - Fast charge: 40mA - 2000mA (40mA steps)
  - Input current: 100mA - 3200mA (20mA steps)
- **Voltage Regulation**: 3.5V - 4.8V (10mV steps)
- **NVDC Power Path Management**: รักษาระดับแรงดันระบบเหนือแบตเตอรี่
- **Input Protection**: Over-voltage, under-voltage, and over-current protection
- **OTG Boost Mode**: Output 3.84V - 5.2V (80mV steps)
- **Integrated 12-bit ADC**: วัดค่า current, voltage และ temperature
- **USB BC1.2 Detection**: ตรวจจับ USB host และ charging port อัตโนมัติ
- **Temperature Monitoring**: JEITA-compliant temperature profile

## Hardware Configuration

ตามที่ระบุใน [HARDWARE_MAP.md](../../.docs/ARCHITECTURE/HARDWARE_MAP.md):

```
I2C Address: 0x6A
SCL: GPIO6
SDA: GPIO7
```

## การใช้งาน

### 1. เริ่มต้นใช้งาน

```cpp
#include "bq25629.h"

// สร้าง I2C bus
i2c_master_bus_config_t i2c_bus_config = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = GPIO_NUM_7,
    .scl_io_num = GPIO_NUM_6,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
    .flags = {
        .enable_internal_pullup = true,
    },
};

i2c_master_bus_handle_t i2c_bus;
ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

// สร้าง BQ25629 driver
drivers::BQ25629 charger(i2c_bus);

// กำหนดค่าเริ่มต้น
drivers::BQ25629_Config config = {
    .charge_voltage_mv = 4200,       // 4.2V (full charge)
    .charge_current_ma = 1000,       // 1A charging
    .input_current_limit_ma = 1500,  // 1.5A input limit
    .input_voltage_limit_mv = 4600,  // 4.6V VINDPM
    .min_system_voltage_mv = 3520,   // 3.52V minimum
    .precharge_current_ma = 30,      // 30mA pre-charge (10mA step)
    .term_current_ma = 20,           // 20mA termination (5mA step)
    .enable_charging = true,         // เปิดการชาร์จ
    .enable_otg = false,             // ปิด OTG mode
    .enable_bypass_otg = false,      // ปิด bypass OTG
    .enable_adc = true,              // เปิด ADC
};

ESP_ERROR_CHECK(charger.init(config));
```

### 2. ตรวจสอบสถานะการชาร์จ

```cpp
// ตรวจสอบว่ากำลังชาร์จอยู่หรือไม่
bool charging;
charger.is_charging(charging);
ESP_LOGI(TAG, "Charging: %s", charging ? "Yes" : "No");

// อ่านสถานะการชาร์จโดยละเอียด
drivers::ChargeStatus charge_status;
charger.get_charge_status(charge_status);

switch (charge_status) {
    case drivers::ChargeStatus::NOT_CHARGING:
        ESP_LOGI(TAG, "Not charging");
        break;
    case drivers::ChargeStatus::TRICKLE_PRECHARGE_FASTCHARGE:
        ESP_LOGI(TAG, "Charging (CC mode)");
        break;
    case drivers::ChargeStatus::TAPER_CHARGE:
        ESP_LOGI(TAG, "Charging (CV mode)");
        break;
    case drivers::ChargeStatus::TOPOFF_TIMER_ACTIVE:
        ESP_LOGI(TAG, "Top-off timer active");
        break;
}
```

### 3. ตรวจสอบ USB/Adapter Status

```cpp
drivers::VBusStatus vbus_status;
charger.get_vbus_status(vbus_status);

switch (vbus_status) {
    case drivers::VBusStatus::NO_ADAPTER:
        ESP_LOGI(TAG, "No adapter connected");
        break;
    case drivers::VBusStatus::USB_SDP:
        ESP_LOGI(TAG, "USB SDP (500mA)");
        break;
    case drivers::VBusStatus::USB_CDP:
        ESP_LOGI(TAG, "USB CDP (1.5A)");
        break;
    case drivers::VBusStatus::USB_DCP:
        ESP_LOGI(TAG, "USB DCP (1.5A)");
        break;
    case drivers::VBusStatus::NON_STANDARD:
        ESP_LOGI(TAG, "Non-standard adapter");
        break;
    case drivers::VBusStatus::OTG_MODE:
        ESP_LOGI(TAG, "OTG mode active");
        break;
}
```

### 4. อ่านค่า ADC

```cpp
drivers::BQ25629_ADC_Data adc_data;
ESP_ERROR_CHECK(charger.read_adc(adc_data));

ESP_LOGI(TAG, "VBUS: %u mV", adc_data.vbus_mv);
ESP_LOGI(TAG, "VBAT: %u mV", adc_data.vbat_mv);
ESP_LOGI(TAG, "VSYS: %u mV", adc_data.vsys_mv);
ESP_LOGI(TAG, "IBAT: %d mA", adc_data.ibat_ma);  // positive = charging
ESP_LOGI(TAG, "IBUS: %d mA", adc_data.ibus_ma);
ESP_LOGI(TAG, "Die Temp: %d °C", adc_data.tdie_c);
```

### 5. ปรับค่าการชาร์จ

```cpp
// เปลี่ยนกระแสชาร์จ
charger.set_charge_current(800);  // 800mA

// เปลี่ยนแรงดันชาร์จ
charger.set_charge_voltage(4150); // 4.15V

// เปลี่ยน input current limit
charger.set_input_current_limit(1000); // 1A
```

### 6. เปิด/ปิดการทำงาน

```cpp
// เปิด/ปิดการชาร์จ
charger.enable_charging(true);   // เปิด
charger.enable_charging(false);  // ปิด

// เปิด/ปิด OTG mode
charger.enable_otg(true);   // เปิด
charger.enable_otg(false);  // ปิด
```

### 7. Watchdog Timer

```cpp
// ตั้งค่า watchdog timeout (รองรับ 50s/100s/200s/disable เท่านั้น)
charger.set_watchdog_timeout(drivers::WatchdogTimeout::Sec200);

// Reset watchdog timer (ควรทำก่อน timeout จะหมด)
charger.reset_watchdog();
```

## Register Map (สำคัญ)

| Register | Address | Description |
|----------|---------|-------------|
| CHARGE_CURRENT_LIMIT | 0x02-0x03 | Fast charge current (16-bit) |
| CHARGE_VOLTAGE_LIMIT | 0x04-0x05 | Battery voltage regulation (16-bit) |
| INPUT_CURRENT_LIMIT | 0x06-0x07 | Input current limit (16-bit) |
| INPUT_VOLTAGE_LIMIT | 0x08-0x09 | Input voltage limit (16-bit) |
| CHARGER_STATUS_1 | 0x1E | Charge และ VBUS status |
| VBAT_ADC | 0x30-0x31 | Battery voltage ADC (16-bit) |
| IBAT_ADC | 0x2A-0x2B | Battery current ADC (16-bit) |

## Power Path Management

BQ25629 ใช้ **NVDC (Narrow VDC) Architecture** โดย:
- VSYS จะถูก regulate ให้สูงกว่า VBAT ประมาณ 50mV
- ถ้า VBAT ต่ำกว่า VSYSMIN, ระบบจะถูก regulate ที่ VSYSMIN
- BATFET ทำงานในโหมด linear (LDO mode) เมื่อชาร์จ
- รองรับ supplement mode เมื่อ input source ไม่เพียงพอ

## Safety Features

- **Input Protection**:
  - VBUS OVP: 6.3V หรือ 18.5V (ปรับได้)
  - VBUS UVLO: 3.0V
  - Input current regulation
  
- **Battery Protection**:
  - Battery OVP: 103-105% of VREG
  - BATFET OCP: 6A หรือ 12A (ปรับได้)
  
- **Thermal Protection**:
  - Thermal regulation: 60°C หรือ 120°C
  - Thermal shutdown: 140°C

- **Safety Timers**:
  - Pre-charge timer: 0.62 หรือ 2.5 ชม.
  - Fast charge timer: 14.5 หรือ 28 ชม.

## ข้อควรระวัง

1. **I2C Communication**: ต้อง wait อย่างน้อย 90μs ระหว่าง START commands
2. **Watchdog Timer**: ตั้งค่าให้ reset ทุก 50 วินาที มิฉะนั้นจะ timeout
3. **Little-Endian Format**: การอ่าน/เขียน 16-bit registers ใช้ little-endian
4. **ADC Conversion Time**: ขึ้นอยู่กับ resolution (3.75ms - 30ms)

## อ้างอิง

- [BQ25629 Datasheet](../../.docs/REFERENCES/docs/pdf/bq25629.pdf)
- [Hardware Map](../../.docs/ARCHITECTURE/HARDWARE_MAP.md)
- [Texas Instruments Product Page](https://www.ti.com/product/BQ25629)

## License

ตาม license ของโปรเจกต์หลัก
