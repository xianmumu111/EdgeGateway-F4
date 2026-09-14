#ifndef MODBUS_SLAVE_H
#define MODBUS_SLAVE_H

#include "stdint.h"

void modbus_init(void);
void modbus_rx_byte(uint8_t byte);
void modbus_poll(void);
void modbus_tick(void);

#define MODBUS_SLAVE_ADDRESS 0x01

#endif
