/*
 * Copyright (c) 2016 Cesanta Software Limited & Espressif Systems (Shanghai) PTE LTD
 * All rights reserved
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
 * Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Spiffy flasher. Implements strong checksums (MD5) and can use higher
 * baud rates. Actual max baud rate will differ from device to device,
 * but 921K seems to be common.
 *
 * SLIP protocol is used for communication.
 * First packet is a single byte - command number.
 * After that, a packet with a variable number of 32-bit (LE) arguments,
 * depending on command.
 *
 * Then command produces variable number of packets of output, but first
 * packet of length 1 is the response code: 0 for success, non-zero - error.
 *
 * See individual command description below.
 */

#include "stub_flasher.h"
#include "rom_functions.h"
#include "slip.h"
#include "stub_commands.h"
#include "stub_write_flash.h"
#include "soc_support.h"

#define UART_RX_INTS (UART_RXFIFO_FULL_INT_ENA | UART_RXFIFO_TOUT_INT_ENA)


#ifdef ESP32
/* ESP32 register naming is a bit more consistent */
#define UART_INT_CLR(X) UART_INT_CLR_REG(X)
#define UART_INT_ST(X) UART_INT_ST_REG(X)
#define UART_INT_ENA(X) UART_INT_ENA_REG(X)
#define UART_STATUS(X) UART_STATUS_REG(X)
#define UART_FIFO(X) UART_FIFO_REG(X)
#endif

static uint32_t baud_rate_to_divider(uint32_t baud_rate) {
  uint32_t master_freq;
#ifdef ESP8266
  master_freq = 52*1000*1000;
#else
  master_freq = ets_get_detected_xtal_freq()<<4;
#endif
  master_freq += (baud_rate / 2);
  return master_freq / baud_rate;
}

/* Buffers for reading from UART. Data is read double-buffered, so
   we can read into one buffer while handling data from the other one
   (used for flashing throughput.) */
typedef struct {
  uint8_t buf_a[MAX_WRITE_BLOCK+64];
  uint8_t buf_b[MAX_WRITE_BLOCK+64];
  volatile uint8_t *reading_buf; /* Pointer to buf_a, or buf_b - which are we reading_buf? */
  uint16_t read; /* how many bytes have we read in the frame */
  slip_state_t state;
  esp_command_req_t *command; /* Pointer to buf_a or buf_b as latest command received */
} uart_buf_t;
static volatile uart_buf_t ub;

/* esptool protcol "checksum" is XOR of 0xef and each byte of
   data payload. */
static uint8_t calculate_checksum(uint8_t *buf, int length)
{
  uint8_t res = 0xef;
  for(int i = 0; i < length; i++) {
    res ^= buf[i];
  }
  return res;
}

static void uart_isr_receive(char byte)
{
  int16_t r = SLIP_recv_byte(byte, (slip_state_t *)&ub.state);
  if (r >= 0) {
    ub.reading_buf[ub.read++] = (uint8_t) r;
    if (ub.read == MAX_WRITE_BLOCK+64) {
      /* shouldn't happen unless there are data errors */
      r = SLIP_FINISHED_FRAME;
    }
  }
  if (r == SLIP_FINISHED_FRAME) {
    /* end of frame, set 'command'
       to be processed by main thread */
    if(ub.reading_buf == ub.buf_a) {
      ub.command = (esp_command_req_t *)ub.buf_a;
      ub.reading_buf = ub.buf_b;
    } else {
      ub.command = (esp_command_req_t *)ub.buf_b;
      ub.reading_buf = ub.buf_a;
    }
    ub.read = 0;
  }
}

void uart_isr(void *arg) {
  uint32_t int_st = READ_PERI_REG(UART_INT_ST(0));
  while (1) {
    uint32_t fifo_len = READ_PERI_REG(UART_STATUS(0)) & 0xff;
    if (fifo_len == 0) {
      break;
    }
    while (fifo_len-- > 0) {
      uint8_t byte = READ_PERI_REG(UART_FIFO(0)) & 0xff;
      uart_isr_receive(byte);
    }
  }
  WRITE_PERI_REG(UART_INT_CLR(0), int_st);
}

static esp_command_error verify_data_len(esp_command_req_t *command, uint8_t len)
{
  return (command->data_len == len) ? ESP_OK : ESP_BAD_DATA_LEN;
}

uint8_t cmd_loop() {
  while(1) {
    /* Wait for a command */
    while(ub.command == NULL) { }
    esp_command_req_t *command = ub.command;
    ub.command = NULL;
    /* provide easy access for 32-bit data words */
    uint32_t *data_words = (uint32_t *)command->data_buf;

    /* Send command response header */
    esp_command_response_t resp = {
      .resp = 1,
      .op_ret = command->op,
      .len_ret = 0, /* esptool.py ignores this value */
      .value = 0,
    };

    /* ESP_READ_REG is the only command that needs to write into the
       'resp' structure before we send it back. */
    if (command->op == ESP_READ_REG && command->data_len == 4) {
      resp.value = REG_READ(data_words[0]);
    }

    /* Send the command response. */
    SLIP_send_frame_delimiter();
    SLIP_send_frame_data_buf(&resp, sizeof(esp_command_response_t));

    if(command->data_len > MAX_WRITE_BLOCK+16) {
      SLIP_send_frame_data(ESP_BAD_DATA_LEN);
      SLIP_send_frame_data(0xEE);
      SLIP_send_frame_delimiter();
      continue;
    }

    /* ... some commands will insert in-frame response data
       between here and when we send the end of the frame */

    esp_command_error error = ESP_CMD_NOT_IMPLEMENTED;
    int status = 0;

    /* First stage of command processing - before sending error/status */
    switch (command->op) {
    case ESP_ERASE_FLASH:
      error = verify_data_len(command, 0) || SPIEraseChip();
      break;
    case ESP_ERASE_REGION:
      /* Params for ERASE_REGION are addr, len */
      error = verify_data_len(command, 8) || handle_flash_erase(data_words[0], data_words[1]);
      break;
    case ESP_SET_BAUD:
      /* ESP_SET_BAUD sends two args, we ignore the second one */
      error = verify_data_len(command, 8);
      /* actual baud setting happens after we send the reply */
      break;
    case ESP_READ_FLASH:
      error = verify_data_len(command, 16);
      /* actual data is sent after we send the reply */
      break;
    case ESP_FLASH_VERIFY_MD5:
      /* unsure why the MD5 command has 4 params but we only pass 2 of them,
         but this is in ESP32 ROM so we can't mess with it.
      */
      error = verify_data_len(command, 16) || handle_flash_get_md5sum(data_words[0], data_words[1]);
      break;
    case ESP_FLASH_BEGIN:
      /* a number of parameters the ROM flasher uses are ignored here:
         0 - erase_size (ignored)
         1 - num_blocks (only used to get total size)
         2 - block_size (only used to get total size)
         3 - offset (used used)
       */
      error = verify_data_len(command, 16) || handle_flash_begin(data_words[1] * data_words[2], data_words[3]);
      break;
    case ESP_FLASH_DEFLATED_BEGIN:
      /* 0 - uncompressed size
         1 - num_blocks (based on compressed size)
         2 - block size (used to get total size)
         3 - offset (used as-is)
      */
      error = verify_data_len(command, 16) || handle_flash_deflated_begin(data_words[0], data_words[1] * data_words[2], data_words[3]);
      break;
    case ESP_FLASH_DATA:
    case ESP_FLASH_DEFLATED_DATA:
      /* ACK DATA commands immediately, then process them a few lines down,
         allowing next command to buffer */
      if(is_in_flash_mode()) {
        error = get_flash_error();
        int payload_len = command->data_len - 16;
        if (data_words[0] != payload_len) {
          /* First byte of data payload header is length (repeated) as a word */
          error = ESP_BAD_DATA_LEN;
        }
        uint8_t data_checksum = calculate_checksum(command->data_buf + 16, payload_len);
        if (data_checksum != command->checksum) {
          error = ESP_BAD_DATA_CHECKSUM;
        }
      }
      else {
        error = ESP_NOT_IN_FLASH_MODE;
      }
      break;
    case ESP_FLASH_END:
    case ESP_FLASH_DEFLATED_END:
      error = handle_flash_end();
      break;
    case ESP_SPI_SET_PARAMS:
      /* data params: fl_id, total_size, block_size, sector_Size, page_size, status_mask */
      error = verify_data_len(command, 24) || handle_spi_set_params(data_words, &status);
      break;
    case ESP_SPI_ATTACH:
      /* params are isHSPI, isLegacy */
      error = verify_data_len(command, 8) || handle_spi_attach(data_words[0], data_words[1] & 0xFF);
      break;
    case ESP_WRITE_REG:
      /* params are addr, value, mask (ignored), delay_us (ignored) */
      error = verify_data_len(command, 16);
      if (error == ESP_OK) {
        REG_WRITE(data_words[0], data_words[1]);
      }
      break;
    case ESP_READ_REG:
      /* actual READ_REG operation happens higher up */
      error = verify_data_len(command, 4);
      break;
    }

    SLIP_send_frame_data(error);
    SLIP_send_frame_data(status);
    SLIP_send_frame_delimiter();

    /* Some commands need to do things after after sending this response */
    if (error == ESP_OK) {
      switch(command->op) {
      case ESP_SET_BAUD:
        ets_delay_us(10000);
        uart_div_modify(0, baud_rate_to_divider(data_words[0]));
        ets_delay_us(1000);
        break;
      case ESP_READ_FLASH:
        /* args are: offset, length, block_size, max_in_flight */
        handle_flash_read(data_words[0], data_words[1], data_words[2],
                          data_words[3]);
        break;
      case ESP_FLASH_DATA:
        /* drop into flashing mode, discard 16 byte payload header */
        handle_flash_data(command->data_buf + 16, command->data_len - 16);
        break;
      case ESP_FLASH_DEFLATED_DATA:
        handle_flash_deflated_data(command->data_buf + 16, command->data_len - 16);
        break;
      case ESP_FLASH_END:
        /* passing 0 as parameter for ESP_FLASH_END means reboot now */
        if (data_words[0] == 0) {
          /* Flush the FLASH_END response before rebooting */
#ifdef ESP32
          uart_tx_flush(0);
#else
          ets_delay_us(10000);
#endif
          software_reset();
        }
        break;
      }
    }
  }
  return 0;
}


extern uint32_t _bss_start;
extern uint32_t _bss_end;

void stub_main() {
  uint32_t greeting = 0x4941484f; /* OHAI */
  uint32_t last_cmd;

  /* zero bss */
  for(uint32_t *p = &_bss_start; p < &_bss_end; p++) {
    *p = 0;
  }

  SLIP_send(&greeting, 4);

  /* All UART reads come via uart_isr */
  ub.reading_buf = ub.buf_a;
  ets_isr_attach(ETS_UART0_INUM, uart_isr, NULL);
  SET_PERI_REG_MASK(UART_INT_ENA(0), UART_RX_INTS);
  ets_isr_unmask(1 << ETS_UART0_INUM);

#ifdef ESP8266
  /* This points at us right now, reset for next boot. */
  ets_set_user_start(NULL);
#else
  // TODO for ESP32
#endif

  /* Configure default SPI flash functionality.
     Can be changed later by esptool.py. */
#ifdef ESP8266
        SelectSpiFunction();
#else
        spi_flash_attach(0, 0);
#endif
        SPIParamCfg(0, 16*1024*1024, FLASH_BLOCK_SIZE, FLASH_SECTOR_SIZE,
                    FLASH_PAGE_SIZE, FLASH_STATUS_MASK);

  last_cmd = cmd_loop();

  ets_delay_us(10000);

  if (last_cmd == -1/*CMD_BOOT_FW*/) {
    /*
     * Find the return address in our own stack and change it.
     * "flash_finish" it gets to the same point, except it doesn't need to
     * patch up its RA: it returns from UartDwnLdProc, then from f_400011ac,
     * then jumps to 0x4000108a, then checks strapping bits again (which will
     * not have changed), and then proceeds to 0x400010a8.
     */
    volatile uint32_t *sp = &last_cmd;
    while (*sp != (uint32_t) 0x40001100) sp++;
    *sp = 0x400010a8;
    /*
     * The following dummy asm fragment acts as a barrier, to make sure function
     * epilogue, including return address loading, is added after our stack
     * patching.
     */
    __asm volatile("nop.n");
    return; /* To 0x400010a8 */
  } else {
    //_ResetVector();
  }
  /* Not reached */
}