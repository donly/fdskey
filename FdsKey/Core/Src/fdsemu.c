#include <stdlib.h>
#include <string.h>
#include "main.h"
#include "fdsemu.h"
#include "settings.h"
#include "ff.h"

static char fds_filename[FF_MAX_LFN + 1];
static uint8_t fds_side;
// loaded FDS data
#ifdef FDS_USE_DYNAMIC_MEMORY
static uint8_t * volatile fds_raw_data;
#else
static uint8_t volatile fds_raw_data[FDS_MAX_SIDE_SIZE];
#endif
static volatile uint8_t fds_read_buffer[FDS_READ_BUFFER_SIZE];
static volatile int fds_used_space = 0;
static volatile int fds_block_count = 0;
static volatile int fds_block_offsets[FDS_MAX_BLOCKS];
static volatile uint16_t fds_write_buffer[FDS_WRITE_BUFFER_SIZE];

// state machine variables
static volatile FDS_STATE fds_state = FDS_OFF;
static volatile uint8_t fds_clock = 0;
static volatile int fds_current_byte = 0;
static volatile uint8_t fds_current_bit = 0;
static volatile uint8_t fds_last_value = 0;
static volatile uint32_t fds_not_ready_time = 0;
static volatile uint8_t fds_write_carrier = 0;
static volatile uint16_t fds_last_write_impulse = 0;
static volatile uint32_t fds_current_block_end = 0;
static volatile uint16_t fds_write_gap_skip = 0;
static volatile uint8_t fds_changed = 0;
static volatile uint32_t fds_last_action_time = 0;
static volatile uint8_t fds_readonly = 0;

static void fds_start_reading();
static void fds_start_writing();
static void fds_reset_writing();
static void fds_stop_reading();
static void fds_stop_writing();
static void fds_reset_reading();
static void fds_stop();

// calculate block CRC
// source: https://forums.nesdev.org/viewtopic.php?p=194867#p194867
static uint16_t fds_crc(uint8_t *data, unsigned size)
{
  //Do not include any existing checksum, not even the blank checksums 00 00 or FF FF.
  //The formula will automatically count 2 0x00 bytes without the programmer adding them manually.
  //Also, do not include the gap terminator (0x80) in the data.
  //If you wish to do so, change sum to 0x0000.
  uint16_t sum = 0x8000;
  uint16_t byte_index;
  uint8_t bit_index;
  uint8_t byte;

  for (byte_index = 0; byte_index < size + 2; byte_index++)
  {
    byte = byte_index < size ? data[byte_index] : 0x00;
    for (bit_index = 0; bit_index < 8; bit_index++)
    {
      uint8_t bit = (byte >> bit_index) & 1;
      uint8_t carry = sum & 1;
      sum = (sum >> 1) | (bit << 15);
      if (carry)
        sum ^= 0x8408;
    }
  }
  return sum;
}

// calculate block size
static uint16_t fds_get_block_size(int i, uint8_t include_gap, uint8_t include_crc)
{
  if (i == 0)
    return (include_gap ? FDS_FIRST_GAP_READ_BITS / 8 : 0) + 56 + (include_crc ? 2 : 0); // disk info block
  if (i == 1)
    return (include_gap ? FDS_NEXT_GAPS_READ_BITS / 8 : 0) + 2 + (include_crc ? 2 : 0);  // file amount block
  if (i % 2 == 0)
    return (include_gap ? FDS_NEXT_GAPS_READ_BITS / 8 : 0) + 16 + (include_crc ? 2 : 0); // file header block
  // file data block - size stored in previous block
  return (include_gap ? FDS_NEXT_GAPS_READ_BITS / 8 : 0) + 1
      + (fds_raw_data[fds_block_offsets[i - 1] + FDS_NEXT_GAPS_READ_BITS / 8 + 0x0D] | (fds_raw_data[fds_block_offsets[i - 1] + FDS_NEXT_GAPS_READ_BITS / 8 + 0x0E] << 8)) + (include_crc ? 2 : 0);
}

static void fds_dma_fill_read_buffer(int pos, int length)
{
  // filling PWM DMA buffer with data
  uint8_t bit, value;
  switch (fds_state)
  {
  case FDS_READING:
  case FDS_READ_WAIT_READY:
    break;
  default:
    return;
  }
  while (length)
  {
    fds_clock ^= 1; // carrier state
    bit = (fds_raw_data[fds_current_byte] >> (fds_current_bit / 2)) & 1;
    value = bit ^ fds_clock;
    // send impulse when low to high transition
    if (value && !fds_last_value)
      fds_read_buffer[pos] = FDS_READ_IMPULSE_LENGTH - 1;
    else
      fds_read_buffer[pos] = 0;
    fds_last_value = value;
    fds_current_bit++;
    if (fds_current_bit > 15)
    {
      // next byte
      fds_current_bit = 0;
      fds_current_byte = (fds_current_byte + 1) % FDS_MAX_SIDE_SIZE;
      // check if drive is rewinded
      if ((fds_current_byte == 0) ||
          (fdskey_settings.rewind_speed == REWIND_SPEED_TURBO && fds_current_byte > fds_used_space + FDS_NOT_READY_BYTES))
      {
        // pause before ready
        HAL_GPIO_WritePin(FDS_READY_GPIO_Port, FDS_READY_Pin, GPIO_PIN_SET);
        fds_not_ready_time = HAL_GetTick();
        fds_state = FDS_READ_WAIT_READY_TIMER;
        fds_reset_reading();
      }
    }
    pos++;
    length--;
  }
}

static void fds_dma_read_half_callback(DMA_HandleTypeDef *hdma)
{
  fds_dma_fill_read_buffer(0, FDS_READ_BUFFER_SIZE / 2);
}

static void fds_dma_read_full_callback(DMA_HandleTypeDef *hdma)
{
  fds_dma_fill_read_buffer(FDS_READ_BUFFER_SIZE / 2, FDS_READ_BUFFER_SIZE / 2);
}

// add single bit of written data
static void fds_write_bit(uint8_t bit)
{
  fds_raw_data[fds_current_byte] = (fds_raw_data[fds_current_byte] >> 1) | (bit << 7);
  fds_current_bit++;
  if (fds_current_bit > 7)
  {
    // next byte
    fds_current_bit = 0;
    fds_current_byte = (fds_current_byte + 1) % FDS_MAX_SIDE_SIZE;

    if (fds_current_byte >= fds_current_block_end)
    {
      // end of block
      if (!HAL_GPIO_ReadPin(FDS_SCAN_MEDIA_GPIO_Port, FDS_SCAN_MEDIA_Pin))
      {
        fds_state = FDS_WRITING_STOPPING;
        // still spinning disk
        if (HAL_GPIO_ReadPin(FDS_WRITE_GPIO_Port, FDS_WRITE_Pin))
        {
          // reading
          fds_stop_writing();
          fds_start_reading(); // not writing anymore
        } else {
          // still writing but garbage data
          fds_write_gap_skip = 0;
          fds_state = FDS_WRITING_STOPPING;
        }
      } else {
        // not spinning
        fds_stop();
      }
    }
  }
}

// parsing single write impulse
// pulse - pause duration between previous and current impulses
static void fds_write_impulse(uint16_t pulse)
{
  switch (fds_state)
  {
  case FDS_WRITING_GAP:
  case FDS_WRITING:
    break;
  case FDS_WRITING_STOPPING:
    // some unlicensed software can write multiple blocks at once without /write toggling
    if (pulse < FDS_THRESHOLD_1)
      fds_write_gap_skip++;
    else
      fds_write_gap_skip = 0;
    if (fds_write_gap_skip >= FDS_MULTI_WRITE_UNLICENSED_BITS)
      // start writing of the next block
      fds_reset_writing();
    return;
  default:
    // invalid state, stop writing
    fds_stop_writing();
    return;
  }
  if (fds_state == FDS_WRITING_GAP)
  {
    // gap before actual data
    if (fds_write_gap_skip < FDS_WRITE_GAP_SKIP_BITS)
      fds_write_gap_skip++; // discard first bits
    else if (pulse >= FDS_THRESHOLD_1)
    {
      // gap terminated with start '1' bit (always 15us)
      fds_write_carrier = 0;
      fds_current_bit = 0;
      fds_state = FDS_WRITING;
    }
  } else if (fds_state == FDS_WRITING)
  {
    // some demodulation magic
    uint8_t l = fds_write_carrier;
    // there is three possible pause durations between impulses
    if (pulse < FDS_THRESHOLD_1)
      l |= 2; // 10us
    else if (pulse < FDS_THRESHOLD_2)
      l |= 3; // 15us
    else
      l |= 4; // 20us
    // data depends on pulse length and carrier state
    switch (l)
    {
    case 0x82:
      fds_write_bit(0);
      fds_write_carrier = 0x80;
      break;
    case 0x83:
      fds_write_bit(1);
      fds_write_carrier = 0;
      break;
    case 0x84:
      // invalid state
      break;
    case 0x02:
      fds_write_bit(1);
      fds_write_carrier = 0;
      break;
    case 0x03:
      fds_write_bit(0);
      fds_write_bit(0);
      fds_write_carrier = 0x80;
      break;
    case 0x04:
      fds_write_bit(0);
      fds_write_bit(1);
      fds_write_carrier = 0;
      break;
    }
  }
}

// parse data written by DMA
static void fds_dma_parse_write_buffer(int pos, int length)
{
  uint16_t pulse;
  while (length)
  {
    pulse = fds_write_buffer[pos] - fds_last_write_impulse;
    fds_write_impulse(pulse);
    fds_last_write_impulse = fds_write_buffer[pos];
    length--;
    pos++;
  }
}

static void fds_dma_write_half_callback(DMA_HandleTypeDef *hdma)
{
  fds_dma_parse_write_buffer(0, FDS_WRITE_BUFFER_SIZE / 2);
}

static void fds_dma_write_full_callback(DMA_HandleTypeDef *hdma)
{
  fds_dma_parse_write_buffer(FDS_WRITE_BUFFER_SIZE / 2, FDS_WRITE_BUFFER_SIZE / 2);
}

// start FDS reading: timer, PWM and DMA
static void fds_start_reading()
{
  fds_current_bit = 0;
  fds_dma_fill_read_buffer(0, FDS_READ_BUFFER_SIZE);
  HAL_DMA_RegisterCallback(&FDS_READ_DMA, HAL_DMA_XFER_HALFCPLT_CB_ID, fds_dma_read_half_callback);
  HAL_DMA_RegisterCallback(&FDS_READ_DMA, HAL_DMA_XFER_CPLT_CB_ID, fds_dma_read_full_callback);
  __HAL_TIM_ENABLE_DMA(&FDS_READ_PWM_TIMER, TIM_DMA_UPDATE);
  HAL_DMA_Start_IT(&FDS_READ_DMA, (uint32_t)&fds_read_buffer, (uint32_t)&FDS_READ_PWM_TIMER.Instance->FDS_READ_PWM_TIMER_CHANNEL_REG, FDS_READ_BUFFER_SIZE);
  HAL_TIM_PWM_Start(&FDS_READ_PWM_TIMER, FDS_READ_PWM_TIMER_CHANNEL_CONST);
  fds_state = FDS_READING;
}

// stop reading
static void fds_stop_reading()
{
  HAL_DMA_Abort_IT(&FDS_READ_DMA);
  HAL_TIM_PWM_Stop(&FDS_READ_PWM_TIMER, FDS_READ_PWM_TIMER_CHANNEL_CONST);
}

// reset reading state machine
void fds_reset_reading()
{
  fds_clock = 0;
  if (fdskey_settings.rewind_speed == REWIND_SPEED_TURBO)
    fds_current_byte = 0;
  fds_current_bit = 0;
  fds_last_value = 0;
}

// calculate current block for writing, update state variables
static void fds_reset_writing()
{
  int i;
  int gap_length;
  int fds_current_block = 0;

  // calculate current block
  for (i = 0;; i++)
  {
    if (i >= fds_block_count)
    {
      // add new block
      if (i == 0)
        fds_block_offsets[i] = 0;
      else
        fds_block_offsets[i] = fds_block_offsets[i - 1] + fds_get_block_size(i - 1, 1, 1);
      fds_current_block = fds_block_count;
      fds_block_count++;
      break;
    }
    uint16_t block_size = fds_get_block_size(i, 1, 1);
    if (fds_current_byte < fds_block_offsets[i] + block_size)
    {
      fds_current_block = i;
      break;
    }
  }
  // update used space
  fds_used_space = fds_block_offsets[fds_block_count - 1] + fds_get_block_size(fds_block_count - 1, 1, 1);
  if (fds_used_space > FDS_MAX_SIDE_SIZE)
  {
    fds_block_count--;
    fds_stop();
  }

  fds_current_byte = fds_block_offsets[fds_current_block];
  gap_length = fds_current_block == 0 ? FDS_FIRST_GAP_READ_BITS / 8 : FDS_NEXT_GAPS_READ_BITS / 8;
  fds_current_block_end = (fds_current_byte + gap_length + fds_get_block_size(fds_current_block, 0, 1)) % FDS_MAX_SIDE_SIZE;
  if (fds_current_block_end < fds_current_byte)
  {
    // this should not happen
    HAL_GPIO_WritePin(FDS_READY_GPIO_Port, FDS_READY_Pin, GPIO_PIN_SET);
    return;
  }
  if (fds_current_block + 1 < fds_block_count && fds_current_block_end != fds_block_offsets[fds_current_block + 1])
  {
    // oops, next block overwrited or disaligned
    // trimming and erasing
    fds_block_count = fds_current_block + 1;
    memset((uint8_t*)fds_raw_data + fds_block_offsets[fds_current_block + 1], 0, FDS_MAX_SIDE_SIZE - fds_block_offsets[fds_current_block + 1]);
  }
  // gap before data
  for (i = 0; i < gap_length - 1; i++)
    fds_raw_data[fds_current_byte++] = 0;
  fds_raw_data[fds_current_byte++] = 0x80; // gap terminator
  fds_write_gap_skip = 0;
  fds_changed = 1; // flag that ROM changed
}

// start writing: timer, PWM and DMA
static void fds_start_writing()
{
  fds_reset_writing();
  // start and reset timer
  fds_state = FDS_WRITING_GAP;
  HAL_DMA_RegisterCallback(&FDS_WRITE_DMA, HAL_DMA_XFER_HALFCPLT_CB_ID, fds_dma_write_half_callback);
  HAL_DMA_RegisterCallback(&FDS_WRITE_DMA, HAL_DMA_XFER_CPLT_CB_ID, fds_dma_write_full_callback);
  __HAL_TIM_ENABLE_DMA(&FDS_WRITE_CAPTURE_TIMER, FDS_WRITE_CAPTURE_DMA_TRIGGER_CONST);
  HAL_DMA_Start_IT(&FDS_WRITE_DMA, (uint32_t)&(FDS_WRITE_CAPTURE_TIMER.Instance->FDS_WRITE_CAPTURE_TIMER_CHANNEL_REG), (uint32_t)&fds_write_buffer, FDS_WRITE_BUFFER_SIZE);
  HAL_TIM_IC_Start_IT(&FDS_WRITE_CAPTURE_TIMER, FDS_WRITE_CAPTURE_TIMER_CHANNEL_CONST);
}

// stop writing
static void fds_stop_writing()
{
  HAL_DMA_Abort_IT(&FDS_WRITE_DMA);
  HAL_TIM_IC_Stop_IT(&FDS_WRITE_CAPTURE_TIMER, FDS_WRITE_CAPTURE_TIMER_CHANNEL_CONST);
}

// full drive stop
static void fds_stop()
{
  fds_stop_reading();
  fds_stop_writing();
  HAL_GPIO_WritePin(FDS_READY_GPIO_Port, FDS_READY_Pin, GPIO_PIN_SET);
  fds_state = FDS_IDLE;
}

// check for /SCAN_MEDIA and /WRITE pins
// call it every pin state change and every ~100ms
void fds_check_pins()
{
  if (HAL_GPIO_ReadPin(FDS_SCAN_MEDIA_GPIO_Port, FDS_SCAN_MEDIA_Pin))
  {
    // motor stop
    // HAL_GPIO_WritePin(FDS_MOTOR_ON_GPIO_Port, FDS_MOTOR_ON_Pin, GPIO_PIN_RESET); // do i really need this?
    switch (fds_state)
    {
    case FDS_OFF:
    case FDS_WRITING: // waiting for FDS_WRITING_STOPPING (until buffer written by DMA)
      break;
    case FDS_IDLE:
      // schedule file saving if need and idle time exceeded
      if (fds_changed && (fds_last_action_time + FDS_AUTOSAVE_DELAY /*fdskey_settings.autosave_time * 1000*/ < HAL_GetTick()))
        fds_state = FDS_SAVE_PENDING;
      break;
    case FDS_SAVE_PENDING:
      // wait until file saved by the main thread
      if (!fds_changed)
        fds_state = FDS_IDLE;
      break;
    default:
      // just full stop
      fds_stop();
      if (fdskey_settings.rewind_speed == REWIND_SPEED_TURBO)
        fds_reset_reading();
    }
  } else
  {
    // motor on
    // HAL_GPIO_WritePin(FDS_MOTOR_ON_GPIO_Port, FDS_MOTOR_ON_Pin, GPIO_PIN_SET);
    // return from saving state if saved
    if ((fds_state == FDS_SAVE_PENDING) && !fds_changed) fds_state = FDS_IDLE;
    if (HAL_GPIO_ReadPin(FDS_WRITE_GPIO_Port, FDS_WRITE_Pin))
    {
      // reading
      switch (fds_state)
      {
      case FDS_IDLE:
        // start reading
        if (fdskey_settings.rewind_speed == REWIND_SPEED_TURBO || fds_current_byte == 0)
        {
          // wait some time before ready
          fds_not_ready_time = HAL_GetTick();
          fds_state = FDS_READ_WAIT_READY_TIMER;
          fds_reset_reading();
        } else
        {
          // start reading but waiting for drive to be rewinded
          fds_start_reading();
          fds_state = FDS_READ_WAIT_READY;
        }
        break;
      case FDS_READ_WAIT_READY_TIMER:
        // check if "not-ready" pause expired
        if (fds_not_ready_time + (fdskey_settings.rewind_speed == REWIND_SPEED_ORIGINAL ? FDS_NOT_READY_TIME_ORIGINAL : FDS_NOT_READY_TIME) < HAL_GetTick())
        {
          HAL_GPIO_WritePin(FDS_READY_GPIO_Port, FDS_READY_Pin, GPIO_PIN_RESET);
          fds_start_reading();
        }
        break;
      case FDS_WRITING_STOPPING:
        // time to stop writing and start reading
        fds_stop_writing();
        fds_start_reading();
        break;
      default:
        // ignore any other state
        break;
      }
    } else
    {
      // writing
      switch (fds_state)
      {
      case FDS_IDLE:
      case FDS_READING:
      case FDS_READ_WAIT_READY:
      case FDS_READ_WAIT_READY_TIMER:
        // start writing if not yet
        fds_stop_reading();
        fds_start_writing();
        break;
      default:
        // ignore any other state
        break;
      }
    }
    fds_last_action_time = HAL_GetTick();
  }
}

// load .fds file and start drive emulation
FRESULT fds_load_side(char *filename, uint8_t side, uint8_t ro)
{
  int i;
  FRESULT fr;
  FIL fp;
  FSIZE_t f_size;
  int gap_length;
  int block_size;
  uint8_t block_type;
  UINT br;
  uint16_t crc;
  int min_blocks = 0;

  fds_close(0);
  fds_reset_reading();

  // not ready yet
  HAL_GPIO_WritePin(FDS_READY_GPIO_Port, FDS_READY_Pin, GPIO_PIN_SET);
  // but media is inserted
  HAL_GPIO_WritePin(FDS_MEDIA_SET_GPIO_Port, FDS_MEDIA_SET_Pin, GPIO_PIN_RESET);
  // writable maybe
  fds_readonly = ro;
  HAL_GPIO_WritePin(FDS_WRITABLE_MEDIA_GPIO_Port, FDS_WRITABLE_MEDIA_Pin, ro ? GPIO_PIN_SET : GPIO_PIN_RESET);
  // start ready state waiting before file loaded
  fds_not_ready_time = HAL_GetTick();

  strlcpy(fds_filename, filename, sizeof(fds_filename));
  fds_side = side;

  if (fdskey_settings.backup_original != SAVES_EVERDRIVE)
  {
    fr = f_open(&fp, filename, FA_READ);
  } else {
    // everdrive-style saves
    char alt_filename[FF_MAX_LFN + 1];
    FILINFO fno;
    char* filename_no_path = fds_filename + strlen(fds_filename);
    while (filename_no_path >= fds_filename)
    {
      if (*filename_no_path == '\\')
      {
        filename_no_path++;
        break;
      }
      if (filename_no_path > fds_filename)
        filename_no_path--;
    }
    strlcpy(alt_filename, "EDN8\\gamedata\\", sizeof(alt_filename));
    strlcat(alt_filename, filename_no_path, sizeof(alt_filename));
    strlcat(alt_filename, "\\bram.srm", sizeof(alt_filename));
    fr = f_stat(alt_filename, &fno);
    if (fr == FR_OK)
      fr = f_open(&fp, alt_filename, FA_READ);
    else
      fr = f_open(&fp, filename, FA_READ);
  }
  if (fr != FR_OK)
  {
    fds_close(0);
    return fr;
  }
  f_size = f_size(&fp);
  if (f_size % FDS_ROM_SIDE_SIZE != 0 && f_size % FDS_ROM_SIDE_SIZE != 16)
  {
    f_close(&fp);
    fds_close(0);
    return FDSR_INVALID_ROM;
  }
  fr = f_lseek(&fp, ((f_size % FDS_ROM_SIDE_SIZE == FDS_ROM_HEADER_SIZE) ? FDS_ROM_HEADER_SIZE : 0) + side * FDS_ROM_SIDE_SIZE);
  if (fr != FR_OK)
    return fr;

#ifdef FDS_USE_DYNAMIC_MEMORY
  fds_raw_data = malloc(FDS_MAX_SIDE_SIZE * sizeof(uint8_t));
  if (!fds_raw_data) return FDSR_OUT_OF_MEMORY;
#endif

  memset((uint8_t*)fds_raw_data, 0, FDS_MAX_SIDE_SIZE);

  while (1)
  {
    // calculate total number of blocks based on file amount block
    if (fds_block_count == 2)
      min_blocks = fds_raw_data[fds_block_offsets[1] + FDS_NEXT_GAPS_READ_BITS / 8 + 1] * 2 + 2; // files * 2 + header blocks;
    fds_block_offsets[fds_block_count] = fds_used_space;
    gap_length = fds_block_count == 0 ? FDS_FIRST_GAP_READ_BITS / 8 : FDS_NEXT_GAPS_READ_BITS / 8;
    if (fds_used_space + gap_length > FDS_MAX_SIDE_SIZE)
    {
      if (fds_block_count + 1 < min_blocks)
      {
        f_close(&fp);
        fds_close(0);
        return FDSR_ROM_TOO_LARGE;
      }
      break;
    }
    // gap before data
    for (i = 0; i < gap_length - 1; i++)
    {
      fds_raw_data[fds_used_space++] = 0;
      // check size
      if (fds_used_space - 1 >= FDS_MAX_SIDE_SIZE)
      {
        if (fds_block_count + 1 < min_blocks)
        {
          f_close(&fp);
          fds_close(0);
          return FDSR_ROM_TOO_LARGE;
        }
        fds_used_space -= i;
        break;
      }
    }
    fds_raw_data[fds_used_space++] = 0x80; // gap terminator

    if (fds_block_count == 0)
      // disk info block
      block_type = 1;
    else if (fds_block_count == 1)
      // file amount block
      block_type = 2;
    else if (fds_block_count % 2 == 0)
      // file header block
      block_type = 3;
    else
      // file data block
      block_type = 4;
    block_size = fds_get_block_size(fds_block_count, 0, 0);

    // check size
    if (fds_used_space + block_size + 2 /*CRC*/> FDS_MAX_SIDE_SIZE)
    {
      if (fds_block_count + 1 < min_blocks)
      {
        f_close(&fp);
        fds_close(0);
        return FDSR_ROM_TOO_LARGE;
      }
      fds_raw_data[fds_used_space - 1] = 0; // remove terminator
      fds_used_space -= gap_length; // rollback last gap
      break;
    }

    // reading
    fr = f_read(&fp, (uint8_t*) fds_raw_data + fds_used_space, block_size, &br);
    if (fr != FR_OK)
    {
      // SD card error?
      f_close(&fp);
      fds_close(0);
      return fr;
    }
    if (br != block_size)
    {
      // end of file?
      if (fds_block_count + 1 < min_blocks)
      {
        f_close(&fp);
        fds_close(0);
        return FDSR_INVALID_ROM;
      }
      fds_raw_data[fds_used_space - 1] = 0; // remove terminator
      fds_used_space -= gap_length; // rollback last gap
      break;
    }
    if (fds_raw_data[fds_used_space] != block_type)
    {
      // invalid block?
      if (fds_block_count + 1 < min_blocks)
      {
        f_close(&fp);
        fds_close(0);
        return FDSR_INVALID_ROM;
      }
      fds_raw_data[fds_used_space - 1] = 0; // remove terminator
      fds_used_space -= gap_length; // rollback last gap
      break;
    }
    if (fds_block_count == 0)
    {
      // check header
      const char signature[] = "*NINTENDO-HVC*";
      char verify[sizeof(signature)];
      memcpy(verify, fds_raw_data + fds_used_space + 1, sizeof(signature) - 1);
      verify[sizeof(signature) - 1] = 0;
      if (strcmp(verify, signature) != 0)
      {
        f_close(&fp);
        fds_close(0);
        return FDSR_INVALID_ROM;
      }
    }
    crc = fds_crc((uint8_t*) fds_raw_data + fds_used_space, block_size);
    fds_used_space += block_size;
    fds_raw_data[fds_used_space++] = crc & 0xFF;
    fds_raw_data[fds_used_space++] = (crc >> 8) & 0xFF;
    fds_block_count++;
  }
  f_close(&fp);

//  strcat(filename, ".good.bin");
//  fds_dump(filename);

  if (!HAL_GPIO_ReadPin(FDS_SCAN_MEDIA_GPIO_Port, FDS_SCAN_MEDIA_Pin) && (fdskey_settings.rewind_speed == REWIND_SPEED_TURBO))
    fds_state = FDS_READ_WAIT_READY_TIMER;
  else
    fds_state = FDS_IDLE;
  fds_check_pins();

  return FR_OK;
}

// save disk changes to file
FRESULT fds_save()
{
  FRESULT fr;
  FIL fp, fp_backup;
  FILINFO fno;
  uint8_t buff[4096];
  UINT br, bw;
  int i;

  if (!fds_changed)
    return FR_OK;

  if (fds_readonly)
    return FDSR_READ_ONLY;

  // check CRC of every block
  for (i = 0; i < fds_block_count; i++)
  {
    int block_size = fds_get_block_size(i, 0, 0);
    uint16_t valid_crc = fds_crc((uint8_t*)(fds_raw_data + fds_block_offsets[i] + (i == 0 ? FDS_FIRST_GAP_READ_BITS : FDS_NEXT_GAPS_READ_BITS) / 8), block_size);
    uint8_t* crc = (uint8_t*)(fds_raw_data + fds_block_offsets[i] + (i == 0 ? FDS_FIRST_GAP_READ_BITS : FDS_NEXT_GAPS_READ_BITS) / 8 + block_size);
    if (valid_crc != (*crc | (*(crc + 1) << 8)))
      return FDSR_WRONG_CRC;
  }

  char alt_filename[FF_MAX_LFN + 1];
  if (fdskey_settings.backup_original == SAVES_REWRITE_BACKUP || fdskey_settings.backup_original == SAVES_EVERDRIVE)
  {
    // combine backup filename
    if (fdskey_settings.backup_original == SAVES_REWRITE_BACKUP)
    {
      // just add ".bak" to the filename
      strlcpy(alt_filename, fds_filename, sizeof(alt_filename));
      strlcat(alt_filename, ".bak", sizeof(alt_filename));
    } else {
      // get filename without path
      char* filename_no_path = fds_filename + strlen(fds_filename);
      while (filename_no_path >= fds_filename)
      {
        if (*filename_no_path == '\\')
        {
          filename_no_path++;
          break;
        }
        if (filename_no_path > fds_filename)
          filename_no_path--;
      }
      // create directories
      fr = f_mkdir("EDN8");
      if (fr != FR_OK && fr != FR_EXIST)
      {
        fds_state = FDS_IDLE;
        return fr;
      }
      fr = f_mkdir("EDN8\\gamedata");
      if (fr != FR_OK && fr != FR_EXIST)
      {
        fds_state = FDS_IDLE;
        return fr;
      }
      // this directory name contains filename
      strlcpy(alt_filename, "EDN8\\gamedata\\", sizeof(alt_filename));
      strlcat(alt_filename, filename_no_path, sizeof(alt_filename));
      fr = f_mkdir(alt_filename);
      if (fr != FR_OK && fr != FR_EXIST)
      {
        fds_state = FDS_IDLE;
        return fr;
      }
      // add save filename
      strlcat(alt_filename, "\\bram.srm", sizeof(alt_filename));
    }
    // check if exists
    fr = f_stat(alt_filename, &fno);
    if (fr == FR_NO_FILE)
    {
      // need to copy original ROM to this file
      fr = f_open(&fp, fds_filename, FA_READ);
      if (fr != FR_OK)
      {
        fds_state = FDS_IDLE;
        return fr;
      }
      fr = f_open(&fp_backup, alt_filename, FA_CREATE_NEW | FA_WRITE);
      if (fr != FR_OK)
      {
        f_close(&fp);
        fds_state = FDS_IDLE;
        return fr;
      }
      if (fdskey_settings.backup_original == SAVES_EVERDRIVE)
      {
        fr = f_stat(fds_filename, &fno);
        if (fr != FR_OK)
        {
          f_close(&fp);
          f_close(&fp_backup);
          fds_state = FDS_IDLE;
          return fr;
        }
        if (fno.fsize % FDS_ROM_SIDE_SIZE == FDS_ROM_HEADER_SIZE)
        {
          // skip header if any for everdrive save
          fr = f_lseek(&fp, FDS_ROM_HEADER_SIZE);
          if (fr != FR_OK)
          {
            f_close(&fp);
            f_close(&fp_backup);
            fds_state = FDS_IDLE;
            return fr;
          }
        }
      }
      // copy file
      do
      {
        fr = f_read(&fp, buff, sizeof(buff), &br);
        if (fr != FR_OK)
        {
          fds_state = FDS_IDLE;
          return fr;
        }
        fr = f_write(&fp_backup, buff, br, &bw);
        if (bw != br)
        {
          f_close(&fp);
          f_close(&fp_backup);
          fds_state = FDS_IDLE;
          return FR_DENIED;
        }
      } while (br > 0);
      f_close(&fp);
      fr = f_close(&fp_backup);
      if (fr != FR_OK)
      {
        fds_state = FDS_IDLE;
        return fr;
      }
    }
  }

  // open file
  if (fdskey_settings.backup_original != SAVES_EVERDRIVE)
    fr = f_open(&fp, fds_filename, FA_WRITE);
  else
    fr = f_open(&fp, alt_filename, FA_WRITE);
  if (fr != FR_OK)
  {
    fds_state = FDS_IDLE;
    return fr;
  }
  // calculating size offset
  if (fdskey_settings.backup_original != SAVES_EVERDRIVE)
    fr = f_stat(fds_filename, &fno);
  else
    fr = f_stat(alt_filename, &fno);
  if (fr != FR_OK)
  {
    fds_state = FDS_IDLE;
    return fr;
  }
  int header_offset = fno.fsize % FDS_ROM_SIDE_SIZE;
  fr = f_lseek(&fp, header_offset + fds_side * FDS_ROM_SIDE_SIZE);
  if (fr != FR_OK)
  {
    fds_state = FDS_IDLE;
    return fr;
  }
  // save every block
  for (i = 0; i < fds_block_count; i++)
  {
    fr = f_write(&fp, (uint8_t*) fds_raw_data + fds_block_offsets[i] + (i == 0 ? FDS_FIRST_GAP_READ_BITS : FDS_NEXT_GAPS_READ_BITS) / 8, fds_get_block_size(i, 0, 0), &bw);
    if (fr != FR_OK)
    {
      fds_state = FDS_IDLE;
      return fr;
    }
    if (bw != fds_get_block_size(i, 0, 0))
    {
      fds_state = FDS_IDLE;
      return FR_DISK_ERR;
    }
  }
  fr = f_close(&fp);
  if (fr != FR_OK)
  {
    fds_state = FDS_IDLE;
    return fr;
  }

  // clear changed flag
  fds_changed = 0;
  // resume idle state
  fds_check_pins();

  return FR_OK;
}

// stop drive emulation
FRESULT fds_close(uint8_t save)
{
  FRESULT fr = FR_OK;

  // remove disk
  HAL_GPIO_WritePin(FDS_MEDIA_SET_GPIO_Port, FDS_MEDIA_SET_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(FDS_WRITABLE_MEDIA_GPIO_Port, FDS_WRITABLE_MEDIA_Pin, GPIO_PIN_SET);

  // save if need
  if (save)
    fr = fds_save();

  // stop
  fds_stop();
  fds_state = FDS_OFF;

  // reset state variables
  fds_used_space = 0;
  fds_block_count = 0;
  fds_changed = 0;
#ifdef FDS_USE_DYNAMIC_MEMORY
  // free memory if need
  if (fds_raw_data)
    free(fds_raw_data);
  fds_raw_data = 0;
#endif

  return fr;
}

// return current state
FDS_STATE fds_get_state()
{
  return fds_state;
}

// return non-zero if disk content is changed and should be saved
uint8_t fds_is_changed()
{
  return fds_changed;
}

// calculate and return current block number
int fds_get_block()
{
  int i;

  // calculate current block
  for (i = 0;; i++)
  {
    if (i >= fds_block_count)
    {
      return -1;
    }
    uint16_t block_size = fds_get_block_size(i, 1, 1);
    if (fds_current_byte < fds_block_offsets[i] + block_size)
    {
      return i;
    }
  }
}

// return current amount of blocks
int fds_get_block_count()
{
  return fds_block_count;
}

// return virtual head position (in bytes)
int fds_get_head_position()
{
  return fds_current_byte;
}

// return maximum disk capacity in bytes
int fds_get_max_size()
{
  return FDS_MAX_SIDE_SIZE;
}

// return actually used disk space in bytes
int fds_get_used_space()
{
  return fds_used_space;
}
