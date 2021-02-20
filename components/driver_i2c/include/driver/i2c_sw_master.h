#ifndef __I2C_SW_MASTER_H__
#define __I2C_SW_MASTER_H__

#ifdef CONFIG_IDF_TARGET_ESP32C3
#include "esp32c3/rom/ets_sys.h"
#else
#include "esp32/rom/ets_sys.h"
#endif

#define I2C_NUM_MAX 1

void i2c_sw_master_gpio_init(uint8 sda, uint8 scl);
void i2c_sw_master_init(void);

#define i2c_sw_master_wait ets_delay_us
void i2c_sw_master_stop(void);
void i2c_sw_master_start(void);
void i2c_sw_master_setAck(uint8_t level);
uint8_t i2c_sw_master_getAck(void);
uint8_t i2c_sw_master_readByte(void);
void i2c_sw_master_writeByte(uint8_t wrdata);

bool i2c_sw_master_checkAck(void);
void i2c_sw_master_send_ack(void);
void i2c_sw_master_send_nack(void);

uint8_t i2c_sw_master_get_pinSDA();
uint8_t i2c_sw_master_get_pinSCL();

#endif
