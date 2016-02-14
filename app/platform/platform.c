// Platform-dependent functions and includes

#include "platform.h"
#include "common.h"
#include "c_stdio.h"
#include "c_string.h"
#include "c_stdlib.h"
#include "llimits.h"
#include "gpio.h"
#include "user_interface.h"
#include "driver/gpio16.h"
#include "driver/i2c_master.h"
#include "driver/spi.h"
#include "driver/uart.h"

#ifdef  GPIO_SAFE_NO_INTR_ENABLE
#define NO_INTR_CODE ICACHE_RAM_ATTR __attribute__ ((noinline))
#else
#define NO_INTR_CODE inline
#endif

#ifdef GPIO_INTERRUPT_ENABLE
typedef struct _GPIO_HOOK {
  uint32_t gpio_bits;
  void (*callback)(uint32_t bits);
  struct _GPIO_HOOK *next;
} GPIO_HOOK;

static GPIO_HOOK* gpio_hooks;
#endif

static void pwms_init();

int platform_init()
{
  // Setup PWMs
  pwms_init();

  cmn_platform_init();
  // All done
  return PLATFORM_OK;
}

// ****************************************************************************
// KEY_LED functions
uint8_t platform_key_led( uint8_t level){
  uint8_t temp;
  gpio16_output_set(1);   // set to high first, for reading key low level
  gpio16_input_conf();
  temp = gpio16_input_get();
  gpio16_output_conf();
  gpio16_output_set(level);
  return temp;
}

// ****************************************************************************
// GPIO functions

/*
 * Set GPIO mode to output. In RAM helper because interrupts are dsabled 
 */
static void NO_INTR_CODE set_gpio_no_interrupt(uint8 pin) {
  unsigned pnum = pin_num[pin];
  ETS_GPIO_INTR_DISABLE();
#ifdef GPIO_INTERRUPT_ENABLE
  pin_trigger[pin] = false;
  pin_int_type[pin] = GPIO_PIN_INTR_DISABLE;
#endif
  PIN_FUNC_SELECT(pin_mux[pin], pin_func[pin]);
  //disable interrupt
  gpio_pin_intr_state_set(GPIO_ID_PIN(pnum), GPIO_PIN_INTR_DISABLE);
  //clear interrupt status
  GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, BIT(pnum));
  GPIO_REG_WRITE(GPIO_PIN_ADDR(GPIO_ID_PIN(pnum)), 
                 GPIO_REG_READ(GPIO_PIN_ADDR(GPIO_ID_PIN(pnum))) & 
                 (~ GPIO_PIN_PAD_DRIVER_SET(GPIO_PAD_DRIVER_ENABLE))); //disable open drain; 
  ETS_GPIO_INTR_ENABLE();
}

/*
 * Set GPIO mode to interrupt. In RAM helper because interrupts are dsabled 
 */
#ifdef GPIO_INTERRUPT_ENABLE
static void NO_INTR_CODE set_gpio_interrupt(uint8 pin) {
  ETS_GPIO_INTR_DISABLE();
  PIN_FUNC_SELECT(pin_mux[pin], pin_func[pin]);
  GPIO_DIS_OUTPUT(pin_num[pin]);
  gpio_register_set(GPIO_PIN_ADDR(GPIO_ID_PIN(pin_num[pin])), 
                    GPIO_PIN_INT_TYPE_SET(GPIO_PIN_INTR_DISABLE)
                    | GPIO_PIN_PAD_DRIVER_SET(GPIO_PAD_DRIVER_DISABLE)
                    | GPIO_PIN_SOURCE_SET(GPIO_AS_PIN_SOURCE));
  pin_trigger[pin] = true;
  ETS_GPIO_INTR_ENABLE();
}
#endif

int platform_gpio_mode( unsigned pin, unsigned mode, unsigned pull )
{
  NODE_DBG("Function platform_gpio_mode() is called. pin_mux:%d, func:%d\n", pin_mux[pin], pin_func[pin]);
  if (pin >= NUM_GPIO)
    return -1;

  if(pin == 0){
    if(mode==PLATFORM_GPIO_INPUT)
      gpio16_input_conf();
    else
      gpio16_output_conf();
      
    return 1;
  }

  platform_pwm_close(pin);    // closed from pwm module, if it is used in pwm

  if (pull == PLATFORM_GPIO_PULLUP) {
    PIN_PULLUP_EN(pin_mux[pin]);
  } else {
    PIN_PULLUP_DIS(pin_mux[pin]);
  }

  switch(mode){

    case PLATFORM_GPIO_INPUT:
      GPIO_DIS_OUTPUT(pin_num[pin]);
      /* run on */ 
    case PLATFORM_GPIO_OUTPUT:
      set_gpio_no_interrupt(pin);
      break;

#ifdef GPIO_INTERRUPT_ENABLE
    case PLATFORM_GPIO_INT:
      set_gpio_interrupt(pin);
      break;
#endif

    default:
      break;
  }
  return 1;
}


int platform_gpio_write( unsigned pin, unsigned level )
{
  // NODE_DBG("Function platform_gpio_write() is called. pin:%d, level:%d\n",GPIO_ID_PIN(pin_num[pin]),level);
  if (pin >= NUM_GPIO)
    return -1;
  if(pin == 0){
    gpio16_output_conf();
    gpio16_output_set(level);
    return 1;
  }

  GPIO_OUTPUT_SET(GPIO_ID_PIN(pin_num[pin]), level);
}

int platform_gpio_read( unsigned pin )
{
  // NODE_DBG("Function platform_gpio_read() is called. pin:%d\n",GPIO_ID_PIN(pin_num[pin]));
  if (pin >= NUM_GPIO)
    return -1;

  if(pin == 0){
    // gpio16_input_conf();
    return 0x1 & gpio16_input_get();
  }

  // GPIO_DIS_OUTPUT(pin_num[pin]);
  return 0x1 & GPIO_INPUT_GET(GPIO_ID_PIN(pin_num[pin]));
}

#ifdef GPIO_INTERRUPT_ENABLE
static task_handle_t gpio_task_handle;

static void ICACHE_RAM_ATTR platform_gpio_intr_dispatcher (void *dummy){
  uint32 j=0;
  uint32 gpio_status = GPIO_REG_READ(GPIO_STATUS_ADDRESS);
  UNUSED(dummy);

  GPIO_HOOK *hooks = gpio_hooks;
  for (; hooks; hooks = hooks->next) {
    if (gpio_status & hooks->gpio_bits) {
      hooks->callback(gpio_status & hooks->gpio_bits);
    }
  }

  /*
   * gpio_status is a bit map where bit 0 is set if unmapped gpio pin 0 (pin3) has 
   * triggered the ISR. bit 1 if unmapped gpio pin 1 (pin10=U0TXD), etc.  Since this
   * in the ISR, it makes sense to optimize this by doing a fast scan of the status
   * and reverse mapping any set bits.
   */
   for (j = 0; gpio_status>0; j++, gpio_status >>= 1) {
    if (gpio_status&1) {
      int i = pin_num_inv[j];
      if (pin_int_type[i]) {
        //disable interrupt
        gpio_pin_intr_state_set(GPIO_ID_PIN(j), GPIO_PIN_INTR_DISABLE);
        //clear interrupt status
        GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, BIT(j));
        uint32 level = 0x1 & GPIO_INPUT_GET(GPIO_ID_PIN(j));
        if (pin_trigger[i]) {
          /* the task is only posted if a trigger callback is defined */
          pin_trigger[i] = false;
          task_post_high (gpio_task_handle, (i<<1) + level);
        }
        // Interrupts are re-enabled but any interrupt occuring before pin_trigger[i] is reset will be ignored.
        gpio_pin_intr_state_set(GPIO_ID_PIN(j), pin_int_type[i]);  
      }
    }
  }
}

void platform_gpio_init( task_handle_t gpio_task )
{
  int i;
  gpio_task_handle = gpio_task;

  get_pin_map();
  ETS_GPIO_INTR_ATTACH(platform_gpio_intr_dispatcher, NULL);
}
/*
 * Initialise GPIO interrupt mode. In RAM because interrupts are dsabled 
 */
void ICACHE_RAM_ATTR platform_gpio_intr_init( unsigned pin, GPIO_INT_TYPE type )
{
  if (pin < NUM_GPIO) {
    ETS_GPIO_INTR_DISABLE();
    //clear interrupt status
    GPIO_REG_WRITE(GPIO_STATUS_W1TC_ADDRESS, BIT(pin_num[pin]));
    pin_int_type[pin] = type;
    pin_trigger[pin] = true;
    //enable interrupt
    gpio_pin_intr_state_set(GPIO_ID_PIN(pin_num[pin]), type);
    ETS_GPIO_INTR_ENABLE();
  }
}

int platform_gpio_register_callback(uint32_t gpio_bits, void (*callback)(uint32_t)) 
{
  GPIO_HOOK *hook;

  for (hook = gpio_hooks; hook; hook = hook->next) {
    if (hook->callback == callback) {
      hook->gpio_bits = gpio_bits;
      return 1;
    }
  }

  hook = (GPIO_HOOK *) c_zalloc(sizeof(GPIO_HOOK));

  if (!hook) {
    return 0;
  }

  hook->gpio_bits = gpio_bits;
  hook->callback = callback;
  hook->next = gpio_hooks;
  gpio_hooks = hook;

  return 1;
}
#endif

// ****************************************************************************
// UART
// TODO: Support timeouts.

// UartDev is defined and initialized in rom code.
extern UartDevice UartDev;
uint32_t platform_uart_setup( unsigned id, uint32_t baud, int databits, int parity, int stopbits )
{
  switch( baud )
  {
    case BIT_RATE_300:
    case BIT_RATE_600:
    case BIT_RATE_1200:
    case BIT_RATE_2400:
    case BIT_RATE_4800:
    case BIT_RATE_9600:
    case BIT_RATE_19200:
    case BIT_RATE_38400:
    case BIT_RATE_57600:
    case BIT_RATE_74880:
    case BIT_RATE_115200:
    case BIT_RATE_230400:
    case BIT_RATE_460800:
    case BIT_RATE_921600:
    case BIT_RATE_1843200:
    case BIT_RATE_3686400:
      UartDev.baut_rate = baud;
      break;
    default:
      UartDev.baut_rate = BIT_RATE_9600;
      break;
  }

  switch( databits )
  {
    case 5:
      UartDev.data_bits = FIVE_BITS;
      break;
    case 6:
      UartDev.data_bits = SIX_BITS;
      break;
    case 7:
      UartDev.data_bits = SEVEN_BITS;
      break;
    case 8:
      UartDev.data_bits = EIGHT_BITS;
      break;
    default:
      UartDev.data_bits = EIGHT_BITS;
      break;
  }

  switch (stopbits)
  {
    case PLATFORM_UART_STOPBITS_1_5:
      UartDev.stop_bits = ONE_HALF_STOP_BIT;
      break;
    case PLATFORM_UART_STOPBITS_2:
      UartDev.stop_bits = TWO_STOP_BIT;
      break;
    default:
      UartDev.stop_bits = ONE_STOP_BIT;
      break;
  }

  switch (parity)
  {
    case PLATFORM_UART_PARITY_EVEN:
      UartDev.parity = EVEN_BITS;
      UartDev.exist_parity = STICK_PARITY_EN;
      break;
    case PLATFORM_UART_PARITY_ODD:
      UartDev.parity = ODD_BITS;
      UartDev.exist_parity = STICK_PARITY_EN;
      break;
    default:
      UartDev.parity = NONE_BITS;
      UartDev.exist_parity = STICK_PARITY_DIS;
      break;
  }

  uart_setup(id);

  return baud;
}

// if set=1, then alternate serial output pins are used. (15=rx, 13=tx)
void platform_uart_alt( int set )
{
    uart0_alt( set );
    return;
}


// Send: version with and without mux
void platform_uart_send( unsigned id, u8 data ) 
{
  uart_tx_one_char(id, data);
}

// ****************************************************************************
// PWMs

static uint16_t pwms_duty[NUM_PWM] = {0};

static void pwms_init()
{
  int i;
  for(i=0;i<NUM_PWM;i++){
    pwms_duty[i] = DUTY(0);
  }
  pwm_init(500, NULL);
  // NODE_DBG("Function pwms_init() is called.\n");
}

// Return the PWM clock
// NOTE: Can't find a function to query for the period set for the timer,
// therefore using the struct.
// This may require adjustment if driver libraries are updated.
uint32_t platform_pwm_get_clock( unsigned pin )
{
  // NODE_DBG("Function platform_pwm_get_clock() is called.\n");
  if( pin >= NUM_PWM)
    return 0;
  if(!pwm_exist(pin))
    return 0;

  return (uint32_t)pwm_get_freq(pin);
}

// Set the PWM clock
uint32_t platform_pwm_set_clock( unsigned pin, uint32_t clock )
{
  // NODE_DBG("Function platform_pwm_set_clock() is called.\n");
  if( pin >= NUM_PWM)
    return 0;
  if(!pwm_exist(pin))
    return 0;

  pwm_set_freq((uint16_t)clock, pin);
  pwm_start();
  return (uint32_t)pwm_get_freq( pin );
}

uint32_t platform_pwm_get_duty( unsigned pin )
{
  // NODE_DBG("Function platform_pwm_get_duty() is called.\n");
  if( pin < NUM_PWM){
    if(!pwm_exist(pin))
      return 0;
    // return NORMAL_DUTY(pwm_get_duty(pin));
    return pwms_duty[pin];
  }
  return 0;
}

// Set the PWM duty
uint32_t platform_pwm_set_duty( unsigned pin, uint32_t duty )
{
  // NODE_DBG("Function platform_pwm_set_duty() is called.\n");
  if ( pin < NUM_PWM)
  {
    if(!pwm_exist(pin))
      return 0;
    pwm_set_duty(DUTY(duty), pin);
  } else {
    return 0;
  }
  pwm_start();
  pwms_duty[pin] = NORMAL_DUTY(pwm_get_duty(pin));
  return pwms_duty[pin];
}

uint32_t platform_pwm_setup( unsigned pin, uint32_t frequency, unsigned duty )
{
  uint32_t clock;
  if ( pin < NUM_PWM)
  {
    platform_gpio_mode(pin, PLATFORM_GPIO_OUTPUT, PLATFORM_GPIO_FLOAT);  // disable gpio interrupt first
    if(!pwm_add(pin)) 
      return 0;
    // pwm_set_duty(DUTY(duty), pin);
    pwm_set_duty(0, pin);
    pwms_duty[pin] = duty;
    pwm_set_freq((uint16_t)frequency, pin);
  } else {
    return 0;
  }
  clock = platform_pwm_get_clock( pin );
  pwm_start();
  return clock;
}

void platform_pwm_close( unsigned pin )
{
  // NODE_DBG("Function platform_pwm_stop() is called.\n");
  if ( pin < NUM_PWM)
  {
    pwm_delete(pin);
    pwm_start();
  }
}

void platform_pwm_start( unsigned pin )
{
  // NODE_DBG("Function platform_pwm_start() is called.\n");
  if ( pin < NUM_PWM)
  {
    if(!pwm_exist(pin))
      return;
    pwm_set_duty(DUTY(pwms_duty[pin]), pin);
    pwm_start();
  }
}

void platform_pwm_stop( unsigned pin )
{
  // NODE_DBG("Function platform_pwm_stop() is called.\n");
  if ( pin < NUM_PWM)
  {
    if(!pwm_exist(pin))
      return;
    pwm_set_duty(0, pin);
    pwm_start();
  }
}

// *****************************************************************************
// I2C platform interface

uint32_t platform_i2c_setup( unsigned id, uint8_t sda, uint8_t scl, uint32_t speed ){
  if (sda >= NUM_GPIO || scl >= NUM_GPIO)
    return 0;

  // platform_pwm_close(sda);
  // platform_pwm_close(scl);
  
  // disable gpio interrupt first
  platform_gpio_mode(sda, PLATFORM_GPIO_INPUT, PLATFORM_GPIO_PULLUP);   // inside this func call platform_pwm_close
  platform_gpio_mode(scl, PLATFORM_GPIO_INPUT, PLATFORM_GPIO_PULLUP);    // disable gpio interrupt first

  i2c_master_gpio_init(sda, scl);
  return PLATFORM_I2C_SPEED_SLOW;
}

void platform_i2c_send_start( unsigned id ){
  i2c_master_start();
}

void platform_i2c_send_stop( unsigned id ){
  i2c_master_stop();
}

int platform_i2c_send_address( unsigned id, uint16_t address, int direction ){
  // Convert enum codes to R/w bit value.
  // If TX == 0 and RX == 1, this test will be removed by the compiler
  if ( ! ( PLATFORM_I2C_DIRECTION_TRANSMITTER == 0 &&
           PLATFORM_I2C_DIRECTION_RECEIVER == 1 ) ) {
    direction = ( direction == PLATFORM_I2C_DIRECTION_TRANSMITTER ) ? 0 : 1;
  }

  i2c_master_writeByte( (uint8_t) ((address << 1) | direction ));
  // Low-level returns nack (0=acked); we return ack (1=acked).
  return ! i2c_master_getAck();
}

int platform_i2c_send_byte( unsigned id, uint8_t data ){
  i2c_master_writeByte(data);
  // Low-level returns nack (0=acked); we return ack (1=acked).
  return ! i2c_master_getAck();
}

int platform_i2c_recv_byte( unsigned id, int ack ){
  uint8_t r = i2c_master_readByte();
  i2c_master_setAck( !ack );
  return r;
}

// *****************************************************************************
// SPI platform interface
uint32_t platform_spi_setup( uint8_t id, int mode, unsigned cpol, unsigned cpha, uint32_t clock_div)
{
  spi_master_init( id, cpol, cpha, clock_div );
  return 1;
}

int platform_spi_send( uint8_t id, uint8_t bitlen, spi_data_type data )
{
  if (bitlen > 32)
    return PLATFORM_ERR;

  spi_mast_transaction( id, 0, 0, bitlen, data, 0, 0, 0 );
  return PLATFORM_OK;
}

spi_data_type platform_spi_send_recv( uint8_t id, uint8_t bitlen, spi_data_type data )
{
  if (bitlen > 32)
    return 0;

  spi_mast_set_mosi( id, 0, bitlen, data );
  spi_mast_transaction( id, 0, 0, 0, 0, bitlen, 0, -1 );
  return spi_mast_get_miso( id, 0, bitlen );
}

int platform_spi_set_mosi( uint8_t id, uint16_t offset, uint8_t bitlen, spi_data_type data )
{
  if (offset + bitlen > 512)
    return PLATFORM_ERR;

  spi_mast_set_mosi( id, offset, bitlen, data );

  return PLATFORM_OK;
}

spi_data_type platform_spi_get_miso( uint8_t id, uint16_t offset, uint8_t bitlen )
{
  if (offset + bitlen > 512)
    return 0;

  return spi_mast_get_miso( id, offset, bitlen );
}

int platform_spi_transaction( uint8_t id, uint8_t cmd_bitlen, spi_data_type cmd_data,
                              uint8_t addr_bitlen, spi_data_type addr_data,
                              uint16_t mosi_bitlen, uint8_t dummy_bitlen, int16_t miso_bitlen )
{
  if ((cmd_bitlen   >  16) ||
      (addr_bitlen  >  32) ||
      (mosi_bitlen  > 512) ||
      (dummy_bitlen > 256) ||
      (miso_bitlen  > 512))
    return PLATFORM_ERR;

  spi_mast_transaction( id, cmd_bitlen, cmd_data, addr_bitlen, addr_data, mosi_bitlen, dummy_bitlen, miso_bitlen );

  return PLATFORM_OK;
}

// ****************************************************************************
// Flash access functions

/*
 * Assumptions:
 * > toaddr is INTERNAL_FLASH_WRITE_UNIT_SIZE aligned
 * > size is a multiple of INTERNAL_FLASH_WRITE_UNIT_SIZE
 */
uint32_t platform_s_flash_write( const void *from, uint32_t toaddr, uint32_t size )
{
  SpiFlashOpResult r;
  const uint32_t blkmask = INTERNAL_FLASH_WRITE_UNIT_SIZE - 1;
  uint32_t *apbuf = NULL;
  uint32_t fromaddr = (uint32_t)from;
  if( (fromaddr & blkmask ) || (fromaddr >= INTERNAL_FLASH_MAPPED_ADDRESS)) {
    apbuf = (uint32_t *)c_malloc(size);
    if(!apbuf)
      return 0;
    c_memcpy(apbuf, from, size);
  }
  system_soft_wdt_feed ();
  r = flash_write(toaddr, apbuf?(uint32 *)apbuf:(uint32 *)from, size);
  if(apbuf)
    c_free(apbuf);
  if(SPI_FLASH_RESULT_OK == r)
    return size;
  else{
    NODE_ERR( "ERROR in flash_write: r=%d at %08X\n", ( int )r, ( unsigned )toaddr);
    return 0;
  }
}

/*
 * Assumptions:
 * > fromaddr is INTERNAL_FLASH_READ_UNIT_SIZE aligned
 * > size is a multiple of INTERNAL_FLASH_READ_UNIT_SIZE
 */
uint32_t platform_s_flash_read( void *to, uint32_t fromaddr, uint32_t size )
{
  if (size==0)
    return 0;

  SpiFlashOpResult r;
  system_soft_wdt_feed ();

  const uint32_t blkmask = (INTERNAL_FLASH_READ_UNIT_SIZE - 1);
  if( ((uint32_t)to) & blkmask )
  {
    uint32_t size2=size-INTERNAL_FLASH_READ_UNIT_SIZE;
    uint32* to2=(uint32*)((((uint32_t)to)&(~blkmask))+INTERNAL_FLASH_READ_UNIT_SIZE);
    r = flash_read(fromaddr, to2, size2);
    if(SPI_FLASH_RESULT_OK == r)
    {
      os_memmove(to,to2,size2);
      char back[ INTERNAL_FLASH_READ_UNIT_SIZE ] __attribute__ ((aligned(INTERNAL_FLASH_READ_UNIT_SIZE)));
      r=flash_read(fromaddr+size2,(uint32*)back,INTERNAL_FLASH_READ_UNIT_SIZE);
      os_memcpy((uint8_t*)to+size2,back,INTERNAL_FLASH_READ_UNIT_SIZE);
    }
  }
  else
    r = flash_read(fromaddr, (uint32 *)to, size);

  if(SPI_FLASH_RESULT_OK == r)
    return size;
  else{
    NODE_ERR( "ERROR in flash_read: r=%d at %08X\n", ( int )r, ( unsigned )fromaddr);
    return 0;
  }
}

int platform_flash_erase_sector( uint32_t sector_id )
{
  system_soft_wdt_feed ();
  return flash_erase( sector_id ) == SPI_FLASH_RESULT_OK ? PLATFORM_OK : PLATFORM_ERR;
}

uint32_t platform_flash_mapped2phys (uint32_t mapped_addr)
{
  uint32_t cache_ctrl = READ_PERI_REG(CACHE_FLASH_CTRL_REG);
  if (!(cache_ctrl & CACHE_FLASH_ACTIVE))
    return -1;
  bool b0 = (cache_ctrl & CACHE_FLASH_MAPPED0) ? 1 : 0;
  bool b1 = (cache_ctrl & CACHE_FLASH_MAPPED1) ? 1 : 0;
  uint32_t meg = (b1 << 1) | b0;
  return mapped_addr - INTERNAL_FLASH_MAPPED_ADDRESS + meg * 0x100000;
}
