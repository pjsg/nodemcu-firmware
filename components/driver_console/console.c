/*
 * Copyright 2016 Dius Computing Pty Ltd. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the
 *   distribution.
 * - Neither the name of the copyright holders nor the names of
 *   its contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @author Johny Mattsson <jmattsson@dius.com.au>
 */

#include "driver/console.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "soc/periph_defs.h"
#include "soc/uart_reg.h"
//#include "soc/dport_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <unistd.h>
#include "esp32c3/rom/libc_stubs.h"
#include "sys/reent.h"

#define UART_INPUT_QUEUE_SZ 0x100

// These used to be available in soc/uart_register.h:
#define UART_GET_RXFIFO_RD_BYTE(i)  GET_PERI_REG_BITS2(UART_FIFO_REG(i) , UART_RXFIFO_RD_BYTE_V, UART_RXFIFO_RD_BYTE_S)
#define UART_GET_RXFIFO_CNT(i)  GET_PERI_REG_BITS2(UART_STATUS_REG(i) , UART_RXFIFO_CNT_V, UART_RXFIFO_CNT_S)
//#define UART_SET_AUTOBAUD_EN(i,val)  SET_PERI_REG_BITS(UART_AUTOBAUD_REG(i) ,UART_AUTOBAUD_EN_V,(val),UART_AUTOBAUD_EN_S)
# define UART_SET_PARITY_EN(i,val)  SET_PERI_REG_BITS(UART_CONF0_REG(i) ,UART_PARITY_EN_V,(val),UART_PARITY_EN_S)
#define UART_SET_RX_TOUT_EN(i,val)  SET_PERI_REG_BITS(UART_CONF1_REG(i) ,UART_RX_TOUT_EN_V,(val),UART_RX_TOUT_EN_S)
#define UART_SET_RX_TOUT_THRHD(i,val)  SET_PERI_REG_BITS(UART_CONF1_REG(i) ,UART_RX_TOUT_THRHD_V,(val),UART_RX_TOUT_THRHD_S)
#define UART_SET_RXFIFO_FULL_THRHD(i,val)  SET_PERI_REG_BITS(UART_CONF1_REG(i) ,UART_RXFIFO_FULL_THRHD_V,(val),UART_RXFIFO_FULL_THRHD_S)
#define UART_SET_STOP_BIT_NUM(i,val)  SET_PERI_REG_BITS(UART_CONF0_REG(i) ,UART_STOP_BIT_NUM_V,(val),UART_STOP_BIT_NUM_S)
#define UART_SET_BIT_NUM(i,val)  SET_PERI_REG_BITS(UART_CONF0_REG(i) ,UART_BIT_NUM_V,(val),UART_BIT_NUM_S)
#define UART_SET_PARITY_EN(i,val)  SET_PERI_REG_BITS(UART_CONF0_REG(i) ,UART_PARITY_EN_V,(val),UART_PARITY_EN_S)
#define UART_SET_PARITY(i,val)  SET_PERI_REG_BITS(UART_CONF0_REG(i) ,UART_PARITY_V,(val),UART_PARITY_S)


#define BUF_SIZE 256


typedef int (*_read_r_fn) (struct _reent *r, int fd, void *buf, int size);

static _read_r_fn _read_r_pro, _read_r_app;

static task_handle_t input_task = 0;
static intr_handle_t intr_handle;

static QueueHandle_t uart0_queue;

// --- Syscall support for reading from STDIN_FILENO ---------------

static int console_read_r (struct _reent *r, int fd, void *buf, int size, _read_r_fn next)
{
  if (fd == STDIN_FILENO)
  {
    static _lock_t stdin_lock;
    _lock_acquire_recursive (&stdin_lock);
    char *c = (char *)buf;
    int i = 0;
    for (; i < size; ++i)
    {
      if (!console_getc (c++))
        break;
    }
    _lock_release_recursive (&stdin_lock);
    return i;
  }
  else if (next)
    return next (r, fd, buf, size);
  else
    return -1;
}

static int console_read_r_pro (struct _reent *r, int fd, void *buf, int size)
{
  return console_read_r (r, fd, buf, size, _read_r_pro);
}
static int console_read_r_app (struct _reent *r, int fd, void *buf, int size)
{
  return console_read_r (r, fd, buf, size, _read_r_app);
}

// --- End syscall support -------------------------------------------

#if 0
static void uart0_rx_intr_handler (void *arg)
{
  (void)arg;
  bool received = false;
  uint32_t status = READ_PERI_REG(UART_INT_ST_REG(CONSOLE_UART));
  while (status)
  {
    if (status & UART_FRM_ERR_INT_ENA)
    {
      // TODO: report somehow?
      WRITE_PERI_REG(UART_INT_CLR_REG(CONSOLE_UART), UART_FRM_ERR_INT_ENA);
    }
    if ((status & UART_RXFIFO_TOUT_INT_ENA) ||
        (status & UART_RXFIFO_FULL_INT_ENA))
    {
      uint32_t fifo_len = UART_GET_RXFIFO_CNT(CONSOLE_UART);
      for (uint32_t i = 0; i < fifo_len; ++i)
      {
        received = true;
        char c = UART_GET_RXFIFO_RD_BYTE(CONSOLE_UART);
        if (uart0Q)
         xQueueSendToBackFromISR (uart0Q, &c, NULL);
      }
      WRITE_PERI_REG(UART_INT_CLR_REG(CONSOLE_UART),
        UART_RXFIFO_TOUT_INT_ENA | UART_RXFIFO_FULL_INT_ENA);
    }
    status = READ_PERI_REG(UART_INT_ST_REG(CONSOLE_UART));
  }
  if (received && input_task)
    task_post_low (input_task, false);
}

#endif

static void uart_event_task(void *pvParameters)
{
  uart_event_t event;
  for(;;) {
    //Waiting for UART event.
    if(xQueueReceive(uart0_queue, (void * )&event, (portTickType)portMAX_DELAY)) {
      switch(event.type) {
	//Event of UART receving data
	/*We'd better handler data event fast, there would be much more data events than
	other types of events. If we take too much time on data event, the queue might
	be full.*/
	case UART_DATA:
	  task_post_low (input_task, false);
	  break;
	default:
	  break;
      }
    }
  }
}

void console_setup (const ConsoleSetup_t *cfg)
{
  esp_rom_uart_tx_wait_idle (CONSOLE_UART);

/*
  esp_rom_uart_set_clock_baudrate(CONSOLE_UART, UART_CLK_FREQ, cfg->bit_rate);
  UART_SET_BIT_NUM(CONSOLE_UART, cfg->data_bits);
  UART_SET_PARITY_EN(CONSOLE_UART, cfg->parity != CONSOLE_PARITY_NONE);
  UART_SET_PARITY(CONSOLE_UART, cfg->parity & 0x1);
  UART_SET_STOP_BIT_NUM(CONSOLE_UART, cfg->stop_bits);

  // TODO: Make this actually work
  //UART_SET_AUTOBAUD_EN(CONSOLE_UART, cfg->auto_baud);
*/
    uart_driver_install(CONSOLE_UART, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart0_queue, 0);
    //uart_param_config(EX_UART_NUM, &uart_config);

    //Set UART log level
    //esp_log_level_set(TAG, ESP_LOG_INFO);
    //Set UART pins (using UART0 default pins ie no changes.)
    //uart_set_pin(EX_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    //Set uart pattern detect function.
    //uart_enable_pattern_det_baud_intr(EX_UART_NUM, '+', PATTERN_CHR_NUM, 9, 0, 0);
    //Reset the pattern queue length to record at most 20 pattern positions.
    //uart_pattern_queue_reset(EX_UART_NUM, 20);

    //Create a task to handler UART event from ISR
    xTaskCreate(uart_event_task, "uart_event_task", 2048, NULL, 12, NULL);
}


void console_init (const ConsoleSetup_t *cfg, task_handle_t tsk)
{
  input_task = tsk;
  console_setup (cfg);

  // Register our console_read_r_xxx functions to support stdin input
  _read_r_app = syscall_table_ptr->_read_r;
  syscall_table_ptr->_read_r = console_read_r_app;
#if !defined(CONFIG_IDF_TARGET_ESP32C3)
  _read_r_pro = syscall_table_ptr_pro->_read_r;
  syscall_table_ptr_pro->_read_r = console_read_r_pro;
#endif
}


bool console_getc (char *c)
{
  size_t data_len;
  int uart_num = CONSOLE_UART;
  if (uart_get_buffered_data_len(uart_num, &data_len) == ESP_OK && data_len > 0) {
    int ret = uart_read_bytes(uart_num, c, 1, 0);
    if (ret == 1) {
      return true;
    }
  }
  return false;
}

