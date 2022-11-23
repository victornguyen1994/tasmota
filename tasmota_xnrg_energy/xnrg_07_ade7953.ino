/*
  xnrg_07_ade7953.ino - ADE7953 energy sensor support for Tasmota

  Copyright (C) 2021  Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef USE_I2C
#ifdef USE_ENERGY_SENSOR
#ifdef USE_ADE7953
/*********************************************************************************************\
 * ADE7953 - Energy used in Shelly 2.5 (model 1), Shelly EM (model 2) and Shelly Plus 2PM (model 3)
 *
 * {"NAME":"Shelly 2.5","GPIO":[320,0,32,0,224,193,0,0,640,192,608,225,3456,4736],"FLAG":0,"BASE":18}
 * {"NAME":"Shelly EM","GPIO":[0,0,0,0,0,0,0,0,640,3457,608,224,8832,1],"FLAG":0,"BASE":18}
 * {"NAME":"Shelly Plus 2PM PCB v0.1.5","GPIO":[320,0,192,0,0,0,1,1,225,224,0,0,0,0,193,0,0,0,0,0,0,608,3840,32,0,0,0,0,0,640,0,0,3458,4736,0,0],"FLAG":0,"BASE":1,"CMND":"AdcParam1 2,32000,40000,3350"}
 * {"NAME":"Shelly Plus 2PM PCB v0.1.9","GPIO":[320,0,0,0,32,192,0,0,225,224,0,0,0,0,193,0,0,0,0,0,0,608,640,3458,0,0,0,0,0,9472,0,4736,0,0,0,0],"FLAG":0,"BASE":1,"CMND":"AdcParam1 2,10000,10000,3350"}
 *
 * Based on datasheet from https://www.analog.com/en/products/ade7953.html
 *
 * Model differences:
 * Function                        Model1  Model2  Model3   Remark
 * ------------------------------  ------  ------  -------  -------------------------------------------------
 * Shelly                          2.5     EM      Plus2PM
 * Current measurement device      shunt   CT      shunt    CT = Current Transformer
 * Swapped channel A/B             Yes     No      No       Defined by hardware design - Fixed by Tasmota
 * Support Export Active           No      Yes     No       Only EM supports correct negative value detection
 * Show negative (reactive) power  No      Yes     No       Only EM supports correct negative value detection
 * Default phase calibration       0       200     0        CT needs different phase calibration than shunts
 * Default reset pin on ESP8266    -       16      -        Legacy support. Replaced by GPIO ADE7953RST
 *
 * I2C Address: 0x38
 *********************************************************************************************
 * Optionally allowing users to tweak calibration registers:
 * - In addition to possible rules add a rule containing the calib.dat string like:
 *   - rule3 on file#calib.dat do {"angles":{"angle0":180,"angle1":176}} endon
 *   - rule3 on file#calib.dat do {"rms":{"current_a":4194303,"current_b":4194303,"voltage":1613194},"angles":{"angle0":200,"angle1":200},"powers":{"totactive":{"a":2723574,"b":2723574},"apparent":{"a":2723574,"b":2723574},"reactive":{"a":2723574,"b":2723574}}} endon
 * - Restart Tasmota and obeserve that the results seem calibrated as Tasmota now uses the information from calib.dat
 * To restore standard calibration using commands like VoltSet remove above entry from rule3
\*********************************************************************************************/

#define XNRG_07                   7
#define XI2C_07                   7  // See I2CDEVICES.md

#define ADE7953_ADDR              0x38

/*********************************************************************************************/

//#define ADE7953_DUMP_REGS

#define ADE7953_PREF              1540       // 4194304 / (1540 / 1000) = 2723574 (= WGAIN, VAGAIN and VARGAIN)
#define ADE7953_UREF              26000      // 4194304 / (26000 / 10000) = 1613194 (= VGAIN)
#define ADE7953_IREF              10000      // 4194304 / (10000 / 10000) = 4194303 (= IGAIN, needs to be different than 4194304 in order to use calib.dat)

// Default calibration parameters can be overridden by a rule as documented above.
#define ADE7953_GAIN_DEFAULT      4194304    // = 0x400000 range 2097152 (min) to 6291456 (max)

#define ADE7953_PHCAL_DEFAULT     0          // = range -383 to 383 - Default phase calibration for Shunts
#define ADE7953_PHCAL_DEFAULT_CT  200        // = range -383 to 383 - Default phase calibration for Current Transformers (Shelly EM)

enum Ade7953Models { ADE7953_SHELLY_25, ADE7953_SHELLY_EM, ADE7953_SHELLY_PLUS_2PM };

enum Ade7953_8BitRegisters {
  // Register Name                    Addres  R/W  Bt  Ty  Default     Description
  // ----------------------------     ------  ---  --  --  ----------  --------------------------------------------------------------------
  ADE7953_SAGCYC = 0x000,          // 0x000   R/W  8   U   0x00        Sag line cycles
  ADE7953_DISNOLOAD,               // 0x001   R/W  8   U   0x00        No-load detection disable (see Table 16)
  ADE7953_RESERVED_0X002,          // 0x002
  ADE7953_RESERVED_0X003,          // 0x003
  ADE7953_LCYCMODE,                // 0x004   R/W  8   U   0x40        Line cycle accumulation mode configuration (see Table 17)
  ADE7953_RESERVED_0X005,          // 0x005
  ADE7953_RESERVED_0X006,          // 0x006
  ADE7953_PGA_V,                   // 0x007   R/W  8   U   0x00        Voltage channel gain configuration (Bits[2:0])
  ADE7953_PGA_IA,                  // 0x008   R/W  8   U   0x00        Current Channel A gain configuration (Bits[2:0])
  ADE7953_PGA_IB                   // 0x009   R/W  8   U   0x00        Current Channel B gain configuration (Bits[2:0])
};

enum Ade7953_16BitRegisters {
  // Register Name                    Addres  R/W  Bt  Ty  Default     Description
  // ----------------------------     ------  ---  --  --  ----------  --------------------------------------------------------------------
  ADE7953_ZXTOUT = 0x100,          // 0x100   R/W  16  U   0xFFFF      Zero-crossing timeout
  ADE7953_LINECYC,                 // 0x101   R/W  16  U   0x0000      Number of half line cycles for line cycle energy accumulation mode
  ADE7953_CONFIG,                  // 0x102   R/W  16  U   0x8004      Configuration register (see Table 18)
  ADE7953_CF1DEN,                  // 0x103   R/W  16  U   0x003F      CF1 frequency divider denominator. When modifying this register, two sequential write operations must be performed to ensure that the write is successful.
  ADE7953_CF2DEN,                  // 0x104   R/W  16  U   0x003F      CF2 frequency divider denominator. When modifying this register, two sequential write operations must be performed to ensure that the write is successful.
  ADE7953_RESERVED_0X105,          // 0x105
  ADE7953_RESERVED_0X106,          // 0x106
  ADE7953_CFMODE,                  // 0x107   R/W  16  U   0x0300      CF output selection (see Table 19)
  ADE7943_PHCALA,                  // 0x108   R/W  16  S   0x0000      Phase calibration register (Current Channel A). This register is in sign magnitude format.
  ADE7943_PHCALB,                  // 0x109   R/W  16  S   0x0000      Phase calibration register (Current Channel B). This register is in sign magnitude format.
  ADE7943_PFA,                     // 0x10A   R    16  S   0x0000      Power factor (Current Channel A)
  ADE7943_PFB,                     // 0x10B   R    16  S   0x0000      Power factor (Current Channel B)
  ADE7943_ANGLE_A,                 // 0x10C   R    16  S   0x0000      Angle between the voltage input and the Current Channel A input
  ADE7943_ANGLE_B,                 // 0x10D   R    16  S   0x0000      Angle between the voltage input and the Current Channel B input
  ADE7943_Period                   // 0x10E   R    16  U   0x0000      Period register
};

enum Ade7953_32BitRegisters {
  // Register Name                    Addres  R/W  Bt  Ty  Default     Description
  // ----------------------------     ------  ---  --  --  ----------  --------------------------------------------------------------------
  ADE7953_ACCMODE = 0x301,         // 0x301   R/W  24  U   0x000000    Accumulation mode (see Table 21)

  ADE7953_AVA = 0x310,             // 0x310   R    24  S   0x000000    Instantaneous apparent power (Current Channel A)
  ADE7953_BVA,                     // 0x311   R    24  S   0x000000    Instantaneous apparent power (Current Channel B)
  ADE7953_AWATT,                   // 0x312   R    24  S   0x000000    Instantaneous active power (Current Channel A)
  ADE7953_BWATT,                   // 0x313   R    24  S   0x000000    Instantaneous active power (Current Channel B)
  ADE7953_AVAR,                    // 0x314   R    24  S   0x000000    Instantaneous reactive power (Current Channel A)
  ADE7953_BVAR,                    // 0x315   R    24  S   0x000000    Instantaneous reactive power (Current Channel B)
  ADE7953_IA,                      // 0x316   R    24  S   0x000000    Instantaneous current (Current Channel A)
  ADE7953_IB,                      // 0x317   R    24  S   0x000000    Instantaneous current (Current Channel B)
  ADE7953_V,                       // 0x318   R    24  S   0x000000    Instantaneous voltage (voltage channel)
  ADE7953_RESERVED_0X319,          // 0x319
  ADE7953_IRMSA,                   // 0x31A   R    24  U   0x000000    IRMS register (Current Channel A)
  ADE7953_IRMSB,                   // 0x31B   R    24  U   0x000000    IRMS register (Current Channel B)
  ADE7953_VRMS,                    // 0x31C   R    24  U   0x000000    VRMS register
  ADE7953_RESERVED_0X31D,          // 0x31D
  ADE7953_AENERGYA,                // 0x31E   R    24  S   0x000000    Active energy (Current Channel A)
  ADE7953_AENERGYB,                // 0x31F   R    24  S   0x000000    Active energy (Current Channel B)
  ADE7953_RENERGYA,                // 0x320   R    24  S   0x000000    Reactive energy (Current Channel A)
  ADE7953_RENERGYB,                // 0x321   R    24  S   0x000000    Reactive energy (Current Channel B)
  ADE7953_APENERGYA,               // 0x322   R    24  S   0x000000    Apparent energy (Current Channel A)
  ADE7953_APENERGYB,               // 0x323   R    24  S   0x000000    Apparent energy (Current Channel B)
  ADE7953_OVLVL,                   // 0x324   R/W  24  U   0xFFFFFF    Overvoltage level
  ADE7953_OILVL,                   // 0x325   R/W  24  U   0xFFFFFF    Overcurrent level
  ADE7953_VPEAK,                   // 0x326   R    24  U   0x000000    Voltage channel peak
  ADE7953_RSTVPEAK,                // 0x327   R    24  U   0x000000    Read voltage peak with reset
  ADE7953_IAPEAK,                  // 0x328   R    24  U   0x000000    Current Channel A peak
  ADE7953_RSTIAPEAK,               // 0x329   R    24  U   0x000000    Read Current Channel A peak with reset
  ADE7953_IBPEAK,                  // 0x32A   R    24  U   0x000000    Current Channel B peak
  ADE7953_RSTIBPEAK,               // 0x32B   R    24  U   0x000000    Read Current Channel B peak with reset
  ADE7953_IRQENA,                  // 0x32C   R/W  24  U   0x100000    Interrupt enable (Current Channel A, see Table 22)
  ADE7953_IRQSTATA,                // 0x32D   R    24  U   0x000000    Interrupt status (Current Channel A, see Table 23)
  ADE7953_RSTIRQSTATA,             // 0x32E   R    24  U   0x000000    Reset interrupt status (Current Channel A)
  ADE7953_IRQENB,                  // 0x32F   R/W  24  U   0x000000    Interrupt enable (Current Channel B, see Table 24)
  ADE7953_IRQSTATB,                // 0x330   R    24  U   0x000000    Interrupt status (Current Channel B, see Table 25)
  ADE7953_RSTIRQSTATB,             // 0x331   R    24  U   0x000000    Reset interrupt status (Current Channel B)

  ADE7953_CRC = 0x37F,             // 0x37F   R    32  U   0xFFFFFFFF  Checksum
  ADE7953_AIGAIN,                  // 0x380   R/W  24  U   0x400000    Current channel gain (Current Channel A)
  ADE7953_AVGAIN,                  // 0x381   R/W  24  U   0x400000    Voltage channel gain
  ADE7953_AWGAIN,                  // 0x382   R/W  24  U   0x400000    Active power gain (Current Channel A)
  ADE7953_AVARGAIN,                // 0x383   R/W  24  U   0x400000    Reactive power gain (Current Channel A)
  ADE7953_AVAGAIN,                 // 0x384   R/W  24  U   0x400000    Apparent power gain (Current Channel A)
  ADE7953_RESERVED_0X385,          // 0x385
  ADE7953_AIRMSOS,                 // 0x386   R/W  24  S   0x000000    IRMS offset (Current Channel A)
  ADE7953_RESERVED_0X387,          // 0x387
  ADE7953_VRMSOS,                  // 0x388   R/W  24  S   0x000000    VRMS offset
  ADE7953_AWATTOS,                 // 0x389   R/W  24  S   0x000000    Active power offset correction (Current Channel A)
  ADE7953_AVAROS,                  // 0x38A   R/W  24  S   0x000000    Reactive power offset correction (Current Channel A)
  ADE7953_AVAOS,                   // 0x38B   R/W  24  S   0x000000    Apparent power offset correction (Current Channel A)
  ADE7953_BIGAIN,                  // 0x38C   R/W  24  U   0x400000    Current channel gain (Current Channel B)
  ADE7953_BVGAIN,                  // 0x38D   R/W  24  U   0x400000    This register should not be modified.
  ADE7953_BWGAIN,                  // 0x38E   R/W  24  U   0x400000    Active power gain (Current Channel B)
  ADE7953_BVARGAIN,                // 0x38F   R/W  24  U   0x400000    Reactive power gain (Current Channel B)
  ADE7953_BVAGAIN,                 // 0x390   R/W  24  U   0x400000    Apparent power gain (Current Channel B)
  ADE7953_RESERVED_0X391,          // 0x391
  ADE7953_BIRMSOS,                 // 0x392   R/W  24  S   0x000000    IRMS offset (Current Channel B)
  ADE7953_RESERVED_0X393,          // 0x393
  ADE7953_RESERVED_0X394,          // 0x394
  ADE7953_BWATTOS,                 // 0x395   R/W  24  S   0x000000    Active power offset correction (Current Channel B)
  ADE7953_BVAROS,                  // 0x396   R/W  24  S   0x000000    Reactive power offset correction (Current Channel B)
  ADE7953_BVAOS                    // 0x397   R/W  24  S   0x000000    Apparent power offset correction (Current Channel B)
};

enum Ade7953CalibrationRegisters {
  ADE7953_CAL_AVGAIN,
  ADE7953_CAL_BVGAIN,
  ADE7953_CAL_AIGAIN,
  ADE7953_CAL_BIGAIN,
  ADE7953_CAL_AWGAIN,
  ADE7953_CAL_BWGAIN,
  ADE7953_CAL_AVAGAIN,
  ADE7953_CAL_BVAGAIN,
  ADE7953_CAL_AVARGAIN,
  ADE7953_CAL_BVARGAIN,
  ADE7943_CAL_PHCALA,
  ADE7943_CAL_PHCALB
};

const uint16_t Ade7953CalibRegs[] {
  ADE7953_AVGAIN,
  ADE7953_BVGAIN,
  ADE7953_AIGAIN,
  ADE7953_BIGAIN,
  ADE7953_AWGAIN,
  ADE7953_BWGAIN,
  ADE7953_AVAGAIN,
  ADE7953_BVAGAIN,
  ADE7953_AVARGAIN,
  ADE7953_BVARGAIN,
  ADE7943_PHCALA,
  ADE7943_PHCALB
};

const uint16_t Ade7953Registers[] {
  ADE7953_IRMSA,   // IRMSA - RMS current channel A
  ADE7953_AWATT,   // AWATT - Active power channel A
  ADE7953_AVA,     // AVA - Apparent power channel A
  ADE7953_AVAR,    // AVAR - Reactive power channel A
  ADE7953_IRMSB,   // IRMSB - RMS current channel B
  ADE7953_BWATT,   // BWATT - Active power channel B
  ADE7953_BVA,     // BVA - Apparent power channel B
  ADE7953_BVAR,    // BVAR - Reactive power channel B
  ADE7953_VRMS,    // VRMS - RMS voltage (Both channels)
  ADE7943_Period,  // Period - 16-bit unsigned period register
  ADE7953_ACCMODE  // ACCMODE - Accumulation mode
};

struct Ade7953 {
  uint32_t voltage_rms = 0;
  uint32_t period = 0;
  uint32_t current_rms[2] = { 0, 0 };
  uint32_t active_power[2] = { 0, 0 };
  int32_t calib_data[sizeof(Ade7953CalibRegs)/sizeof(uint16_t)];
  uint8_t init_step = 0;
  uint8_t model = 0;                              // 0 = Shelly 2.5, 1 = Shelly EM, 2 = Shelly Plus 2PM
} Ade7953;

int Ade7953RegSize(uint16_t reg) {
  int size = 0;
  switch ((reg >> 8) & 0x0F) {
    case 0x03:  // 32-bit
      size++;
    case 0x02:  // 24-bit
      size++;
    case 0x01:  // 16-bit
      size++;
    case 0x00:  // 8-bit
    case 0x07:
    case 0x08:
      size++;
  }
  return size;
}

void Ade7953Write(uint16_t reg, uint32_t val) {
  int size = Ade7953RegSize(reg);
  if (size) {
    Wire.beginTransmission(ADE7953_ADDR);
    Wire.write((reg >> 8) & 0xFF);
    Wire.write(reg & 0xFF);
    while (size--) {
      Wire.write((val >> (8 * size)) & 0xFF);     // Write data, MSB first
    }
    Wire.endTransmission();
    delayMicroseconds(5);                         // Bus-free time minimum 4.7us
  }
}

int32_t Ade7953Read(uint16_t reg) {
	uint32_t response = 0;

  int size = Ade7953RegSize(reg);
  if (size) {
    Wire.beginTransmission(ADE7953_ADDR);
    Wire.write((reg >> 8) & 0xFF);
    Wire.write(reg & 0xFF);
    Wire.endTransmission(0);
    Wire.requestFrom(ADE7953_ADDR, size);
    if (size <= Wire.available()) {
      for (uint32_t i = 0; i < size; i++) {
        response = response << 8 | Wire.read();   // receive DATA (MSB first)
      }
    }
  }
	return response;
}

#ifdef ADE7953_DUMP_REGS
void Ade7953DumpRegs(void) {
  AddLog(LOG_LEVEL_DEBUG, PSTR("ADE:                  SAGCYC DISNOLD  Resrvd  Resrvd LCYCMOD  Resrvd  Resrvd    PGAV   PGAIA   PGAIB"));
  char data[200] = { 0 };
  for (uint32_t i = 0; i < 10; i++) {
    int32_t value = Ade7953Read(ADE7953_SAGCYC + i);
    snprintf_P(data, sizeof(data), PSTR("%s      %02X"), data, value);  // 8-bit regs
  }
  AddLog(LOG_LEVEL_DEBUG, PSTR("ADE: Regs 0x000..009%s"), data);
  AddLog(LOG_LEVEL_DEBUG, PSTR("ADE:                  ZXTOUT LINECYC  CONFIG  CF1DEN  CF2DEN  Resrvd  Resrvd  CFMODE  PHCALA  PHCALB     PFA     PFB  ANGLEA  ANGLEB  Period"));
  data[0] = '\0';
  for (uint32_t i = 0; i < 15; i++) {
    int32_t value = Ade7953Read(ADE7953_ZXTOUT + i);
    snprintf_P(data, sizeof(data), PSTR("%s    %04X"), data, value);  // 16-bit regs
  }
  AddLog(LOG_LEVEL_DEBUG, PSTR("ADE: Regs 0x100..10E%s"), data);
  AddLog(LOG_LEVEL_DEBUG, PSTR("ADE:                   IGAIN   VGAIN   WGAIN VARGAIN  VAGAIN  Resrvd  IRMSOS  Resrvd  VRMSOS  WATTOS   VAROS    VAOS"));
  data[0] = '\0';
  for (uint32_t i = 0; i < 12; i++) {
    int32_t value = Ade7953Read(ADE7953_AIGAIN + i);
    snprintf_P(data, sizeof(data), PSTR("%s  %06X"), data, value);  // 24-bit regs
  }
  AddLog(LOG_LEVEL_DEBUG, PSTR("ADE: Regs 0x380..38B%s"), data);
  data[0] = '\0';
  for (uint32_t i = 0; i < 12; i++) {
    int32_t value = Ade7953Read(ADE7953_BIGAIN + i);
    snprintf_P(data, sizeof(data), PSTR("%s  %06X"), data, value);  // 24-bit regs
  }
  AddLog(LOG_LEVEL_DEBUG, PSTR("ADE: Regs 0x38C..397%s"), data);
}
#endif  // ADE7953_DUMP_REGS

void Ade7953Init(void) {
#ifdef ADE7953_DUMP_REGS
  Ade7953DumpRegs();
#endif  // ADE7953_DUMP_REGS

  Ade7953Write(ADE7953_CONFIG, 0x0004);           // Locking the communication interface (Clear bit COMM_LOCK), Enable HPF
  Ade7953Write(0x0FE, 0x00AD);                    // Unlock register 0x120
  Ade7953Write(0x120, 0x0030);                    // Configure optimum setting

  for (uint32_t i = 0; i < sizeof(Ade7953CalibRegs)/sizeof(uint16_t); i++) {
    if (i >= ADE7943_CAL_PHCALA) {
      int16_t phasecal = Ade7953.calib_data[i];
      if (phasecal < 0) {
        phasecal = abs(phasecal) + 0x200;         // Add sign magnitude
      }
      Ade7953Write(Ade7953CalibRegs[i], phasecal);
    } else {
      Ade7953Write(Ade7953CalibRegs[i], Ade7953.calib_data[i]);
    }
  }
  int32_t regs[sizeof(Ade7953CalibRegs)/sizeof(uint16_t)];
  for (uint32_t i = 0; i < sizeof(Ade7953CalibRegs)/sizeof(uint16_t); i++) {
    regs[i] = Ade7953Read(Ade7953CalibRegs[i]);
    if (i >= ADE7943_CAL_PHCALA) {
      if (regs[i] >= 0x0200) {
        regs[i] &= 0x01FF;                        // Clear sign magnitude
        regs[i] *= -1;                            // Make negative
      }
    }
  }
  AddLog(LOG_LEVEL_DEBUG_MORE, PSTR("ADE: CalibRegs aV %d, bV %d, aI %d, bI %d, aW %d, bW %d, aVA %d, bVA %d, aVAr %d, bVAr %d, aP %d, bP %d"),
    regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7], regs[8], regs[9], regs[10], regs[11]);
#ifdef ADE7953_DUMP_REGS
  Ade7953DumpRegs();
#endif  // ADE7953_DUMP_REGS
}

void Ade7953GetData(void) {
  uint32_t acc_mode;
  int32_t reg[2][4];
  for (uint32_t i = 0; i < sizeof(Ade7953Registers)/sizeof(uint16_t); i++) {
    int32_t value = Ade7953Read(Ade7953Registers[i]);
    if (8 == i) {
      Ade7953.voltage_rms = value;                // RMS voltage (both channels)
    } else if (9 == i) {
      Ade7953.period = value;                     // Period
    } else if (10 == i) {
      acc_mode = value;                           // Accumulation mode
    } else {
      uint32_t reg_index = i >> 2;                // 0 or 1
      reg[(ADE7953_SHELLY_25 == Ade7953.model) ? !reg_index : reg_index][i &3] = value;  // IRMS, WATT, VA, VAR
    }
  }
  AddLog(LOG_LEVEL_DEBUG_MORE, PSTR("ADE: ACCMODE 0x%06X, VRMS %d, Period %d, IRMS %d, %d, WATT %d, %d, VA %d, %d, VAR %d, %d"),
    acc_mode, Ade7953.voltage_rms, Ade7953.period,
    reg[0][0], reg[1][0], reg[0][1], reg[1][1], reg[0][2], reg[1][2], reg[0][3], reg[1][3]);

  uint32_t apparent_power[2] = { 0, 0 };
  uint32_t reactive_power[2] = { 0, 0 };

  for (uint32_t channel = 0; channel < 2; channel++) {
    Ade7953.current_rms[channel] = reg[channel][0];
    if (Ade7953.current_rms[channel] < 2000) {    // No load threshold (20mA)
      Ade7953.current_rms[channel] = 0;
      Ade7953.active_power[channel] = 0;
    } else {
      Ade7953.active_power[channel] = abs(reg[channel][1]);
      apparent_power[channel] = abs(reg[channel][2]);
      reactive_power[channel] = abs(reg[channel][3]);
      if ((ADE7953_SHELLY_EM == Ade7953.model) && (bitRead(acc_mode, 18 +(channel * 3)))) {  // VARNLOAD
        reactive_power[channel] = 0;
      }
    }
  }

  if (Energy.power_on) {                          // Powered on
    float divider = (Ade7953.calib_data[ADE7953_CAL_AVGAIN] != ADE7953_GAIN_DEFAULT) ? 10000 : Settings->energy_voltage_calibration;
    Energy.voltage[0] = (float)Ade7953.voltage_rms / divider;
    Energy.frequency[0] = 223750.0f / ((float)Ade7953.period + 1);

    for (uint32_t channel = 0; channel < 2; channel++) {
      Energy.data_valid[channel] = 0;
      divider = (Ade7953.calib_data[ADE7953_CAL_AWGAIN + channel] != ADE7953_GAIN_DEFAULT) ? 44 : (Settings->energy_power_calibration / 10);
      Energy.active_power[channel] = (float)Ade7953.active_power[channel] / divider;
      divider = (Ade7953.calib_data[ADE7953_CAL_AVARGAIN + channel] != ADE7953_GAIN_DEFAULT) ? 44 : (Settings->energy_power_calibration / 10);
      Energy.reactive_power[channel] = (float)reactive_power[channel] / divider;
      if (ADE7953_SHELLY_EM == Ade7953.model) {
        if (bitRead(acc_mode, 10 +channel)) {     // APSIGN
          Energy.active_power[channel] *= -1;
        }
        if (bitRead(acc_mode, 12 +channel)) {     // VARSIGN
          Energy.reactive_power[channel] *= -1;
        }
      }
      divider = (Ade7953.calib_data[ADE7953_CAL_AVAGAIN + channel] != ADE7953_GAIN_DEFAULT) ? 44 : (Settings->energy_power_calibration / 10);
      Energy.apparent_power[channel] = (float)apparent_power[channel] / divider;
      if (0 == Energy.active_power[channel]) {
        Energy.current[channel] = 0;
      } else {
        divider = (Ade7953.calib_data[ADE7953_CAL_AIGAIN + channel] != ADE7953_GAIN_DEFAULT) ? 100000 : (Settings->energy_current_calibration * 10);
        Energy.current[channel] = (float)Ade7953.current_rms[channel] / divider;
        Energy.kWhtoday_delta[channel] += Energy.active_power[channel] * 1000 / 36;
      }
    }
    EnergyUpdateToday();
/*
  } else {  // Powered off
    Energy.data_valid[0] = ENERGY_WATCHDOG;
    Energy.data_valid[1] = ENERGY_WATCHDOG;
*/
  }
}

void Ade7953EnergyEverySecond(void) {
	if (Ade7953.init_step) {
    if (1 == Ade7953.init_step) {
      Ade7953Init();
	  }
    Ade7953.init_step--;
	}	else {
		Ade7953GetData();
	}
}

/*********************************************************************************************/

bool Ade7953SetDefaults(const char* json) {
  // {"angles":{"angle0":180,"angle1":176}}
  // {"rms":{"current_a":4194303,"current_b":4194303,"voltage":1613194},"angles":{"angle0":0,"angle1":0},"powers":{"totactive":{"a":2723574,"b":2723574},"apparent":{"a":2723574,"b":2723574},"reactive":{"a":2723574,"b":2723574}}}
  uint32_t len = strlen(json) +1;
  if (len < 7) { return false; }                  // Too short

  char json_buffer[len];
  memcpy(json_buffer, json, len);                 // Keep original safe
  JsonParser parser(json_buffer);
  JsonParserObject root = parser.getRootObject();
  if (!root) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("ADE: Invalid JSON"));
    return false;
  }

  // All parameters are optional allowing for partial changes
  JsonParserToken val;
  JsonParserObject rms = root[PSTR("rms")].getObject();
  if (rms) {
    val = rms[PSTR("voltage")];
    if (val) {
      Ade7953.calib_data[ADE7953_CAL_AVGAIN] = val.getInt();
      Ade7953.calib_data[ADE7953_CAL_BVGAIN] = Ade7953.calib_data[ADE7953_CAL_AVGAIN];
    }
    val = rms[PSTR("current_a")];
    if (val) { Ade7953.calib_data[ADE7953_CAL_AIGAIN] = val.getInt(); }
    val = rms[PSTR("current_b")];
    if (val) { Ade7953.calib_data[ADE7953_CAL_BIGAIN] = val.getInt(); }
  }
  JsonParserObject angles = root[PSTR("angles")].getObject();
  if (angles) {
    val = angles[PSTR("angle0")];
    if (val) { Ade7953.calib_data[ADE7943_CAL_PHCALA] = val.getInt(); }
    val = angles[PSTR("angle1")];
    if (val) { Ade7953.calib_data[ADE7943_CAL_PHCALB] = val.getInt(); }
  }
  JsonParserObject powers = root[PSTR("powers")].getObject();
  if (powers) {
    JsonParserObject totactive = powers[PSTR("totactive")].getObject();
    if (totactive) {
      val = totactive[PSTR("a")];
      if (val) { Ade7953.calib_data[ADE7953_CAL_AWGAIN] = val.getInt(); }
      val = totactive[PSTR("b")];
      if (val) { Ade7953.calib_data[ADE7953_CAL_BWGAIN] = val.getInt(); }
    }
    JsonParserObject apparent = powers[PSTR("apparent")].getObject();
    if (apparent) {
      val = apparent[PSTR("a")];
      if (val) { Ade7953.calib_data[ADE7953_CAL_AVAGAIN] = val.getInt(); }
      val = apparent[PSTR("b")];
      if (val) { Ade7953.calib_data[ADE7953_CAL_BVAGAIN] = val.getInt(); }
    }
    JsonParserObject reactive = powers[PSTR("reactive")].getObject();
    if (reactive) {
      val = reactive[PSTR("a")];
      if (val) { Ade7953.calib_data[ADE7953_CAL_AVARGAIN] = val.getInt(); }
      val = reactive[PSTR("b")];
      if (val) { Ade7953.calib_data[ADE7953_CAL_BVARGAIN] = val.getInt(); }
    }
  }
  return true;
}

void Ade7953Defaults(void) {
  for (uint32_t i = 0; i < sizeof(Ade7953CalibRegs)/sizeof(uint16_t); i++) {
    if (i < sizeof(Ade7953CalibRegs)/sizeof(uint16_t) -2) {
      Ade7953.calib_data[i] = ADE7953_GAIN_DEFAULT;
    } else {
      Ade7953.calib_data[i] = (ADE7953_SHELLY_EM == Ade7953.model) ? ADE7953_PHCAL_DEFAULT_CT : ADE7953_PHCAL_DEFAULT;
    }
  }

#ifdef USE_RULES
  // rule3 on file#calib.dat do {"angles":{"angle0":180,"angle1":176}} endon
  String calib = RuleLoadFile("CALIB.DAT");
  if (calib.length()) {
//    AddLog(LOG_LEVEL_DEBUG, PSTR("ADE: File '%s'"), calib.c_str());
    Ade7953SetDefaults(calib.c_str());
  }
#endif  // USE_RULES
}

void Ade7953DrvInit(void) {
  if (PinUsed(GPIO_ADE7953_IRQ, GPIO_ANY)) {      // Irq on GPIO16 is not supported...
    uint32_t pin_irq = Pin(GPIO_ADE7953_IRQ, GPIO_ANY);
    pinMode(pin_irq, INPUT);                      // Related to resetPins() - Must be set to input
    Ade7953.model = GetPin(pin_irq) - AGPIO(GPIO_ADE7953_IRQ);  // 0 (1 = Shelly 2.5), 1 (2 = Shelly EM), 2 (3 = Shelly Plus 2PM)

    int pin_reset = Pin(GPIO_ADE7953_RST);        // -1 if not defined
#ifdef ESP8266
    if (ADE7953_SHELLY_EM == Ade7953.model) {
      if (-1 == pin_reset) {
        pin_reset = 16;
      }
    }
#endif
    if (pin_reset > -1) {
      pinMode(pin_reset, OUTPUT);                 // Reset pin ADE7953
      digitalWrite(pin_reset, 0);
      delay(1);
      digitalWrite(pin_reset, 1);
      pinMode(pin_reset, INPUT);
    }

    delay(100);                                   // Need 100mS to init ADE7953
    if (I2cSetDevice(ADE7953_ADDR)) {
      if (HLW_PREF_PULSE == Settings->energy_power_calibration) {
        Settings->energy_power_calibration = ADE7953_PREF;
        Settings->energy_voltage_calibration = ADE7953_UREF;
        Settings->energy_current_calibration = ADE7953_IREF;
      }
      I2cSetActiveFound(ADE7953_ADDR, "ADE7953");

      Ade7953Defaults();

      Ade7953.init_step = 2;
      Energy.phase_count = 2;                     // Handle two channels as two phases
      Energy.voltage_common = true;               // Use common voltage
      Energy.frequency_common = true;             // Use common frequency
      Energy.use_overtemp = true;                 // Use global temperature for overtemp detection
      if (ADE7953_SHELLY_EM == Ade7953.model) {
        Energy.local_energy_active_export = true;
      }
      TasmotaGlobal.energy_driver = XNRG_07;
    }
  }
}

bool Ade7953Command(void) {
  bool serviced = true;

  uint32_t channel = (2 == XdrvMailbox.index) ? 1 : 0;
  uint32_t value = (uint32_t)(CharToFloat(XdrvMailbox.data) * 100);  // 1.23 = 123

  if (CMND_POWERCAL == Energy.command_code) {
    if (1 == XdrvMailbox.payload) { XdrvMailbox.payload = ADE7953_PREF; }
    // Service in xdrv_03_energy.ino
  }
  else if (CMND_VOLTAGECAL == Energy.command_code) {
    if (1 == XdrvMailbox.payload) { XdrvMailbox.payload = ADE7953_UREF; }
    // Service in xdrv_03_energy.ino
  }
  else if (CMND_CURRENTCAL == Energy.command_code) {
    if (1 == XdrvMailbox.payload) { XdrvMailbox.payload = ADE7953_IREF; }
    // Service in xdrv_03_energy.ino
  }
  else if (CMND_POWERSET == Energy.command_code) {
    if (XdrvMailbox.data_len && Ade7953.active_power[channel]) {
      if ((value > 100) && (value < 200000)) {  // Between 1W and 2000W
        Settings->energy_power_calibration = (Ade7953.active_power[channel] * 1000) / value;  // 0.00 W
      }
    }
  }
  else if (CMND_VOLTAGESET == Energy.command_code) {
    if (XdrvMailbox.data_len && Ade7953.voltage_rms) {
      if ((value > 10000) && (value < 26000)) {  // Between 100V and 260V
        Settings->energy_voltage_calibration = (Ade7953.voltage_rms * 100) / value;  // 0.00 V
      }
    }
  }
  else if (CMND_CURRENTSET == Energy.command_code) {
    if (XdrvMailbox.data_len && Ade7953.current_rms[channel]) {
      if ((value > 2000) && (value < 1000000)) {  // Between 20mA and 10A
        Settings->energy_current_calibration = ((Ade7953.current_rms[channel] * 100) / value) * 100;  // 0.00 mA
      }
    }
  }
  else serviced = false;  // Unknown command

  return serviced;
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xnrg07(uint8_t function) {
  if (!I2cEnabled(XI2C_07)) { return false; }

  bool result = false;

  switch (function) {
    case FUNC_ENERGY_EVERY_SECOND:
      Ade7953EnergyEverySecond();
      break;
    case FUNC_COMMAND:
      result = Ade7953Command();
      break;
    case FUNC_PRE_INIT:
      Ade7953DrvInit();
      break;
  }
  return result;
}

#endif  // USE_ADE7953
#endif  // USE_ENERGY_SENSOR
#endif  // USE_I2C
