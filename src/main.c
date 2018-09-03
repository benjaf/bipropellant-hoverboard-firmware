/*
* This file is part of the hoverboard-firmware-hack project.
*
* Copyright (C) 2017-2018 Rene Hopf <renehopf@mac.com>
* Copyright (C) 2017-2018 Nico Stute <crinq@crinq.de>
* Copyright (C) 2017-2018 Niklas Fauth <niklas.fauth@kit.fail>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stm32f1xx_hal.h"
#include "defines.h"
#include "setup.h"
#include "config.h"
#include "comms.h"
#include "sensorcoms.h"
#include "flashaccess.h"
#include "protocol.h"
#include "bldc.h"
#include "hallinterrupts.h"
#include "softwareserial.h"
//#include "hd44780.h"
#include "pid.h"

#include <memory.h>

void SystemClock_Config(void);

extern TIM_HandleTypeDef htim_left;
extern TIM_HandleTypeDef htim_right;
extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;
extern volatile adc_buf_t adc_buffer;
//LCD_PCF8574_HandleTypeDef lcd;
extern I2C_HandleTypeDef hi2c2;
extern UART_HandleTypeDef huart2;

int cmd1;  // normalized input values. -1000 to 1000
int cmd2;
int cmd3;

typedef struct{
   int16_t steer;
   int16_t speed;
   //uint32_t crc;
} Serialcommand;
int scale[2] = {15, 15};

volatile Serialcommand command;

#ifdef READ_SENSOR
SENSOR_DATA last_sensor_data[2];
int sensor_control = 0;
int sensor_stabilise = 0;

#endif
int disablepoweroff = 0;
int powerofftimer = 0;


extern volatile unsigned int timerval;
extern volatile unsigned int ssbits;

uint8_t button1, button2;

int steer; // global variable for steering. -1000 to 1000
int speed; // global variable for speed. -1000 to 1000

extern volatile int pwml;  // global variable for pwm left. -1000 to 1000
extern volatile int pwmr;  // global variable for pwm right. -1000 to 1000
extern volatile int weakl; // global variable for field weakening left. -1000 to 1000
extern volatile int weakr; // global variable for field weakening right. -1000 to 1000

extern uint8_t buzzerFreq;    // global variable for the buzzer pitch. can be 1, 2, 3, 4, 5, 6, 7...
extern uint8_t buzzerPattern; // global variable for the buzzer pattern. can be 1, 2, 3, 4, 5, 6, 7...
int buzzerLen = 0;

extern uint8_t enable; // global variable for motor enable

extern volatile uint32_t timeout; // global variable for timeout
extern float batteryVoltage; // global variable for battery voltage

uint32_t inactivity_timeout_counter;
uint32_t debug_counter = 0;

extern uint8_t nunchuck_data[6];
#ifdef CONTROL_PPM
extern volatile uint16_t ppm_captured_value[PPM_NUM_CHANNELS+1];
#endif

int milli_vel_error_sum = 0;


void poweroff() {
    if (ABS(speed) < 20) {
        buzzerPattern = 0;
        enable = 0;
        for (int i = 0; i < 8; i++) {
            buzzerFreq = i;
            HAL_Delay(100);
        }
        HAL_GPIO_WritePin(OFF_PORT, OFF_PIN, 0);

        // if we are powered from sTLink, this bit allows the system to be started again with the button.
        while (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {}

        while (1){
          if (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)){
            HAL_NVIC_SystemReset();
          }
        }
    }
}

// actually 'power'
int pwms[2] = {0, 0};

int dirs[2] = {-1, 1};
int dspeeds[2] = {0,0};


// setup pid control for left and right speed.
pid_controller  PositionPid[2];
PID_FLOATS PositionPidFloats[2] = {
  { 0, 0, 0,   1.0, 0.5, 0.0 }, // 2nd 3 are kp, ki, kd
  { 0, 0, 0,   1.0, 0.5, 0.0 }
};
pid_controller  SpeedPid[2];
PID_FLOATS SpeedPidFloats[2] = {
  { 0, 0, 0,   1.0, 0.0, 0.0 }, // 2nd 3 are kp, ki, kd
  { 0, 0, 0,   1.0, 0.0, 0.0 }
};



int main(void) {
  char tmp[200];
  HAL_Init();
  __HAL_RCC_AFIO_CLK_ENABLE();
  HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
  /* System interrupt init*/
  /* MemoryManagement_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(MemoryManagement_IRQn, 0, 0);
  /* BusFault_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(BusFault_IRQn, 0, 0);
  /* UsageFault_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(UsageFault_IRQn, 0, 0);
  /* SVCall_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SVCall_IRQn, 0, 0);
  /* DebugMonitor_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DebugMonitor_IRQn, 0, 0);
  /* PendSV_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(PendSV_IRQn, 0, 0);
  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);

  SystemClock_Config();

  __HAL_RCC_DMA1_CLK_DISABLE();
  MX_GPIO_Init();
  MX_TIM_Init();
  MX_ADC1_Init();
  MX_ADC2_Init();

  #if defined(DEBUG_SERIAL_USART2) || defined(DEBUG_SERIAL_USART3)
    UART_Init();
  #endif

  memset((void*)&electrical_measurements, 0, sizeof(electrical_measurements));

  HAL_GPIO_WritePin(OFF_PORT, OFF_PIN, 1);

  HAL_ADC_Start(&hadc1);
  HAL_ADC_Start(&hadc2);

  #ifdef READ_SENSOR
  // initialise to 9 bit interrupt driven comms on USART 2 & 3
  sensor_USART_init();
  #endif

  for (int i = 8; i >= 0; i--) {
    buzzerFreq = i;
    HAL_Delay(100);
  }
  buzzerFreq = 0;

  HAL_GPIO_WritePin(LED_PORT, LED_PIN, 1);

  //int lastspeeds[2] = {0, 0};

  #ifdef READ_SENSOR
  // things we use in main loop for sensor control
  consoleLog("power on\n");
  int OnBoard = 0;
  int Center[2] = {0, 0};
  int Clamp[2] =  {600, 600};
  
  #endif

  #ifdef HALL_INTERRUPTS
    // enables interrupt reading of hall sensors for dead reconing wheel position.
    HallInterruptinit();
  #endif

  #ifdef SOFTWARE_SERIAL
    SoftwareSerialInit();
  #endif

  #ifdef CONTROL_PPM
    PPM_Init();
  #endif

  #ifdef CONTROL_NUNCHUCK
    I2C_Init();
    Nunchuck_Init();
  #endif

  #ifdef CONTROL_SERIAL_USART2
    UART_Control_Init();
    HAL_UART_Receive_DMA(&huart2, (uint8_t *)&command, 4);

  #endif

  #ifdef DEBUG_I2C_LCD
    I2C_Init();
    HAL_Delay(50);
    lcd.pcf8574.PCF_I2C_ADDRESS = 0x27;
      lcd.pcf8574.PCF_I2C_TIMEOUT = 5;
      lcd.pcf8574.i2c = hi2c2;
      lcd.NUMBER_OF_LINES = NUMBER_OF_LINES_2;
      lcd.type = TYPE0;

      if(LCD_Init(&lcd)!=LCD_OK){
          // error occured
          //TODO while(1);
      }

    LCD_ClearDisplay(&lcd);
    HAL_Delay(5);
    LCD_SetLocation(&lcd, 0, 0);
    LCD_WriteString(&lcd, "Hover V2.0");
    LCD_SetLocation(&lcd, 0, 1);
    LCD_WriteString(&lcd, "Initializing...");
  #endif

  float board_temp_adc_filtered = (float)adc_buffer.temp;
  float board_temp_deg_c;

  enable = 1;  // enable motors

  // ####### POWEROFF BY POWER-BUTTON #######
  int power_button_held = HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN);
  
  unsigned int startup_counter = 0;

  
  memset(&PositionPid, 0, sizeof(PositionPid));
  for (int i = 0; i < 2; i++){
    pid_create(&PositionPid[i], &PositionPidFloats[i].in, &PositionPidFloats[i].out, &PositionPidFloats[i].set, 
      PositionPidFloats[i].kp, PositionPidFloats[i].ki, PositionPidFloats[i].kd);
  	pid_limits(&PositionPid[i], -200, 200);
  	pid_auto(&PositionPid[i]);
  }
  memset(&SpeedPid, 0, sizeof(SpeedPid));
  for (int i = 0; i < 2; i++){
    pid_create(&SpeedPid[i], &SpeedPidFloats[i].in, &SpeedPidFloats[i].out, &SpeedPidFloats[i].set, 
      SpeedPidFloats[i].kp, SpeedPidFloats[i].ki, SpeedPidFloats[i].kd);
  	pid_limits(&SpeedPid[i], -600, 600);
  	pid_auto(&SpeedPid[i]);
  }

  while(1) {
    startup_counter++;
    HAL_Delay(DELAY_IN_MAIN_LOOP); //delay in ms
    cmd1 = 0;
    cmd2 = 0;

    #ifdef CONTROL_NUNCHUCK
      Nunchuck_Read();
      cmd1 = CLAMP((nunchuck_data[0] - 127) * 8, -1000, 1000); // x - axis. Nunchuck joystick readings range 30 - 230
      cmd2 = CLAMP((nunchuck_data[1] - 128) * 8, -1000, 1000); // y - axis

      button1 = (uint8_t)nunchuck_data[5] & 1;
      button2 = (uint8_t)(nunchuck_data[5] >> 1) & 1;
    #endif

    #ifdef CONTROL_PPM
      cmd1 = CLAMP((ppm_captured_value[0] - 500) * 2, -1000, 1000);
      cmd2 = CLAMP((ppm_captured_value[1] - 500) * 2, -1000, 1000);
      button1 = ppm_captured_value[5] > 500;
      float scale = ppm_captured_value[2] / 1000.0f;
    #endif

    #ifdef CONTROL_ADC
      // ADC values range: 0-4095, see ADC-calibration in config.h
      cmd1 = CLAMP(adc_buffer.l_tx2 - ADC1_MIN, 0, ADC1_MAX) / (ADC1_MAX / 1000.0f);  // ADC1
      cmd2 = CLAMP(adc_buffer.l_rx2 - ADC2_MIN, 0, ADC2_MAX) / (ADC2_MAX / 1000.0f);  // ADC2

      // use ADCs as button inputs:
      button1 = (uint8_t)(adc_buffer.l_tx2 > 2000);  // ADC1
      button2 = (uint8_t)(adc_buffer.l_rx2 > 2000);  // ADC2

      timeout = 0;
    #endif

    #ifdef CONTROL_SERIAL_USART2
      cmd1 = CLAMP((int16_t)command.steer, -1000, 1000);
      cmd2 = CLAMP((int16_t)command.speed, -1000, 1000);

      timeout = 0;
    #endif


    #ifdef INCLUDE_PROTOCOL
      // service input serial into out protocol parser
      short inputc = -1;
      while ((inputc = softwareserial_getrx()) >= 0){
        protocol_byte( (unsigned char) inputc );
      }
    #endif

    #ifdef READ_SENSOR
      if (!power_button_held){
        // read the last sensor message in the buffer
        sensor_read_data();

        // tapp one or other side twice in 2s, with at least 1/4s between to
        // enable hoverboard mode. 
        if (CONTROL_TYPE_NONE == control_type){
          if (sensor_data[0].doubletap || sensor_data[1].doubletap){
            sensor_control = 1;
            consoleLog("double tap -> hoverboard mode\r\n");
            sensor_data[0].doubletap = 0;
            sensor_data[1].doubletap = 0;
          }
        }

        if (electrical_measurements.charging){
          sensor_set_flash(0, 3);
        } else {
          sensor_set_flash(0, 0);
        }

        int rollhigh = 0;
        for (int i = 0; i < 2; i++){
          if  (sensor_data[i].sensor_ok){
            sensor_set_colour(i, SENSOR_COLOUR_GREEN);
            scale[i] = 3;
          } else {
            sensor_set_colour(i, SENSOR_COLOUR_RED);
            scale[i] = 3;
          }

          if  (ABS(sensor_data[i].Roll) > 2000){
            rollhigh = 1;
          }
          if  (ABS(sensor_data[i].Angle) > 9000){
            rollhigh = 1;
          }
        }


        // if roll is a large angle (>20 degrees)
        // then disable
    #ifdef CONTROL_SENSOR
        if (sensor_control){
          if (rollhigh){
            enable = 0;
          } else {
            if ((sensor_data[0].sensor_ok || sensor_data[1].sensor_ok) && !electrical_measurements.charging){
              if (!OnBoard){
                Center[0] = sensor_data[0].Angle;
                Center[1] = sensor_data[1].Angle;
                OnBoard = 1;
              }

              for (int i = 0; i < 2; i++){
                pwms[i] = CLAMP(dirs[i]*(sensor_data[i].Angle - Center[i])/3+dspeeds[i], -Clamp[i], Clamp[i]);
                if (sensor_data[i].sensor_ok){
                  sensor_set_colour(i, SENSOR_COLOUR_YELLOW);
                } else {
                  sensor_set_colour(i, SENSOR_COLOUR_GREEN);
                }
              }
              timeout = 0;
              enable = 1;
              inactivity_timeout_counter = 0;
            } else {
              OnBoard = 0;
              for (int i = 0; i < 2; i++){
                pwms[i] = CLAMP(dirs[i]*(sensor_data[i].Angle)/scale[i]+dspeeds[i], -80, 80);
              }
              timeout = 0;
              enable = 1;
            }
          }
        } 
    #endif // end if control_sensor
 
        if (!sensor_control){
          switch (control_type){
            case CONTROL_TYPE_POSITION:
              for (int i = 0; i < 2; i++){
                if (pid_need_compute(&PositionPid[i])) {
                  // Read process feedback
                  PositionPidFloats[i].set = PosnData.wanted_posn_mm[i];
                  PositionPidFloats[i].in = HallData[i].HallPosn_mm;
                  // Compute new PID output value
                  pid_compute(&PositionPid[i]);
                  //Change actuator value
                  int pwm = PositionPidFloats[i].out;
                  pwms[i] = pwm;
                  if (i == 0){
                    sprintf(tmp, "%d:%d\r\n", i, pwm);
                    consoleLog(tmp);
                  }
                }

#ifdef NONEWPID
                PosnData.posn_diff_mm[i] = PosnData.wanted_posn_mm[i] - HallData[i].HallPosn_mm;
                long abs_posn_diff = ABS(PosnData.posn_diff_mm[i]);
                float speed = CLAMP((float)PosnData.posn_diff_mm[i]*PosnData.posn_diff_mult, -PosnData.posn_max_speed, PosnData.posn_max_speed);
                PosnData.posn_speed_demand[i] = (int) speed;
                // assert minimum speeds
                if (speed > 0 && speed < PosnData.posn_min_speed) speed = PosnData.posn_min_speed;
                if (speed < 0 && speed > -PosnData.posn_min_speed) speed = -PosnData.posn_min_speed;

                // we raise/lower speed over 5 clicks
                pwms[i] += ((int)speed - pwms[i])/PosnData.posn_accelleration_factor;
                if (abs_posn_diff < PosnData.posn_max_diff_mm){
                  if (ABS(pwms[i]) < PosnData.posn_stop_speed) pwms[i] = 0;
                }
#endif              
              }
              break;
            case CONTROL_TYPE_SPEED:
              for (int i = 0; i < 2; i++){
                if (pid_need_compute(&SpeedPid[i])) {
                  // Read process feedback
                  SpeedPidFloats[i].set = SpeedData.wanted_speed_mm_per_sec[i];
                  SpeedPidFloats[i].in = HallData[i].HallSpeed_mm_per_s;
                  // Compute new PID output value
                  pid_compute(&SpeedPid[i]);
                  //Change actuator value
                  int pwm = SpeedPidFloats[i].out;
                  pwms[i] = pwm;
                  if (i == 0){
                    sprintf(tmp, "%d:%d\r\n", i, pwm);
                    consoleLog(tmp);
                  }
                }
              }

#ifdef DISBALEITFORNOW            
              for (int i = 0; i < 2; i++){
                SpeedData.speed_diff_mm_per_sec[i] = SpeedData.wanted_speed_mm_per_sec[i] - HallData[i].HallSpeed_mm_per_s;
                long abs_speed_diff = ABS(SpeedData.speed_diff_mm_per_sec[i]);
                float power = CLAMP((float)SpeedData.speed_diff_mm_per_sec[i]*SpeedData.speed_diff_mult, -SpeedData.speed_max_power, SpeedData.speed_max_power);
                SpeedData.speed_power_demand[i] = (int) power;
                // assert minimum speeds
                if (power > 0 && power < SpeedData.speed_min_power) power = SpeedData.speed_min_power;
                if (power < 0 && power > -SpeedData.speed_min_power) power = -SpeedData.speed_min_power;

                // we raise/lower speed over 5 clicks
                pwms[i] += ((int)power)/SpeedData.speed_accelleration_factor;
              }
#endif              
              break;
            case CONTROL_TYPE_PWM:
              for (int i = 0; i < 2; i++){
                pwms[i] = SpeedData.wanted_speed_mm_per_sec[i];
              }
              break;
          }
        }
      }

      // send twice to make sure each side gets it.
      // if we sent diagnositc data, it seems to need this.
      sensor_send_lights();
    #else


      // ####### LOW-PASS FILTER #######
      steer = steer * (1.0 - FILTER) + cmd1 * FILTER;
      speed = speed * (1.0 - FILTER) + cmd2 * FILTER;


      // ####### MIXER #######
      pwms[0] = CLAMP(speed * SPEED_COEFFICIENT -  steer * STEER_COEFFICIENT, -1000, 1000);
      pwms[1] = CLAMP(speed * SPEED_COEFFICIENT +  steer * STEER_COEFFICIENT, -1000, 1000);

    #endif

    #ifdef ADDITIONAL_CODE
      ADDITIONAL_CODE;
    #endif

    #ifdef INVERT_R_DIRECTION
      pwmr = pwms[1];
    #else
      pwmr = -pwms[1];
    #endif
    #ifdef INVERT_L_DIRECTION
      pwml = -pwms[0];
    #else
      pwml = pwms[0];
    #endif

//    for (int i = 0; i < 2; i++){
//      lastspeeds[i] = pwms[i];
//    }

    if ((debug_counter++) % 100 == 0) {
      // ####### CALC BOARD TEMPERATURE #######
      board_temp_adc_filtered = board_temp_adc_filtered * 0.99 + (float)adc_buffer.temp * 0.01;
      board_temp_deg_c = ((float)TEMP_CAL_HIGH_DEG_C - (float)TEMP_CAL_LOW_DEG_C) / ((float)TEMP_CAL_HIGH_ADC - (float)TEMP_CAL_LOW_ADC) * (board_temp_adc_filtered - (float)TEMP_CAL_LOW_ADC) + (float)TEMP_CAL_LOW_DEG_C;
      
      electrical_measurements.board_temp_raw = adc_buffer.temp;
      electrical_measurements.board_temp_filtered = board_temp_adc_filtered;
      electrical_measurements.board_temp_deg_c = board_temp_deg_c;
      electrical_measurements.charging = !(CHARGER_PORT->IDR & CHARGER_PIN);

      // ####### DEBUG SERIAL OUT #######
      #ifdef CONTROL_ADC
        setScopeChannel(0, (int)adc_buffer.l_tx2);  // 1: ADC1
        setScopeChannel(1, (int)adc_buffer.l_rx2);  // 2: ADC2
      #endif

      #ifdef CONTROL_SENSOR
        setScopeChannel(0, (int)sensor_data[0].Angle);  // 1: ADC1
        setScopeChannel(1, -(int)sensor_data[1].Angle);  // 2: ADC2
      #endif

      setScopeChannel(2, (int)pwms[1]);  // 3: output speed: 0-1000
      setScopeChannel(3, (int)pwms[0]);  // 4: output speed: 0-1000
      setScopeChannel(4, (int)adc_buffer.batt1);  // 5: for battery voltage calibration
      setScopeChannel(5, (int)(batteryVoltage * 100.0f));  // 6: for verifying battery voltage calibration
      setScopeChannel(6, (int)board_temp_adc_filtered);  // 7: for board temperature calibration
      setScopeChannel(7, (int)board_temp_deg_c);  // 8: for verifying board temperature calibration
      consoleScope();

//      SoftwareSerialReadTimer();
    }


    if (power_button_held){
      // highlight that the button has been helpd for >5s
      if (startup_counter > (5000/DELAY_IN_MAIN_LOOP)){
        #if defined CONTROL_SENSOR && defined FLASH_STORAGE
          sensor_set_flash(0, 2);
          sensor_set_flash(1, 2);
        #endif
      }

      if (!HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)){
        // if it was held for > 5 seconds
        if (startup_counter > (5000/DELAY_IN_MAIN_LOOP)){
#ifdef EXAMPLE_FLASH_ONLY
          #if defined CONTROL_SENSOR && defined FLASH_STORAGE
            calibrationdata[0] = sensor_data[0].Angle;
            calibrationdata[1] = sensor_data[1].Angle;
            calibrationread = 1;

            char tmp[40];
            sprintf(tmp, "\r\n*** Write Flash Calibration data");
            consoleLog(tmp);
            writeFlash((unsigned char *) calibrationdata, sizeof(calibrationdata));
            sensor_set_flash(0, 0);
            sensor_set_flash(1, 0);
          #endif
#endif
        }

        power_button_held = 0;
      }
    } else {
      // ####### POWEROFF BY POWER-BUTTON #######
      if (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN) && weakr == 0 && weakl == 0) {
        enable = 0;
        while (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {}
        poweroff();
      }
    }

    // if we plug in the charger, keep us alive
    // also if we have deliberately turned off poweroff over serial
    if (electrical_measurements.charging || disablepoweroff){
      inactivity_timeout_counter = 0;
    }

    // ####### BEEP AND EMERGENCY POWEROFF #######
    if ((TEMP_POWEROFF_ENABLE && board_temp_deg_c >= TEMP_POWEROFF && ABS(speed) < 20) || (batteryVoltage < ((float)BAT_LOW_DEAD * (float)BAT_NUMBER_OF_CELLS) && ABS(speed) < 20)) {  // poweroff before mainboard burns OR low bat 3
      poweroff();
    } else if (TEMP_WARNING_ENABLE && board_temp_deg_c >= TEMP_WARNING) {  // beep if mainboard gets hot
      buzzerFreq = 4;
      buzzerPattern = 1;
    } else if (batteryVoltage < ((float)BAT_LOW_LVL1 * (float)BAT_NUMBER_OF_CELLS) && batteryVoltage > ((float)BAT_LOW_LVL2 * (float)BAT_NUMBER_OF_CELLS) && BAT_LOW_LVL1_ENABLE) {  // low bat 1: slow beep
      buzzerFreq = 5;
      buzzerPattern = 42;
    } else if (batteryVoltage < ((float)BAT_LOW_LVL2 * (float)BAT_NUMBER_OF_CELLS) && batteryVoltage > ((float)BAT_LOW_DEAD * (float)BAT_NUMBER_OF_CELLS) && BAT_LOW_LVL2_ENABLE) {  // low bat 2: fast beep
      buzzerFreq = 5;
      buzzerPattern = 6;
    } else if (BEEPS_BACKWARD && speed < -50) {  // backward beep
      buzzerFreq = 5;
      buzzerPattern = 1;
    } else {  // do not beep
      if (buzzerLen > 0){
        buzzerLen--;
      } else {
        buzzerFreq = 0;
        buzzerPattern = 0;
      }
    }


    // ####### INACTIVITY TIMEOUT #######
    if (ABS(pwms[0]) > 50 || ABS(pwms[1]) > 50) {
      inactivity_timeout_counter = 0;
    } else {
      inactivity_timeout_counter ++;
    }

    // inactivity 10s warning; 1s bleeping
    if ((inactivity_timeout_counter > (INACTIVITY_TIMEOUT * 50 * 1000) / (DELAY_IN_MAIN_LOOP + 1)) &&
        (buzzerFreq == 0)) {
      buzzerFreq = 3;
      buzzerPattern = 1;
      buzzerLen = 1000;
    }

    // inactivity 5s warning; 1s bleeping
    if ((inactivity_timeout_counter > (INACTIVITY_TIMEOUT * 55 * 1000) / (DELAY_IN_MAIN_LOOP + 1)) &&
        (buzzerFreq == 0)) {
      buzzerFreq = 2;
      buzzerPattern = 1;
      buzzerLen = 1000;
    }

    // power off after ~60s of inactivity
    if (inactivity_timeout_counter > (INACTIVITY_TIMEOUT * 60 * 1000) / (DELAY_IN_MAIN_LOOP + 1)) {  // rest of main loop needs maybe 1ms
      inactivity_timeout_counter = 0;
      poweroff();
    }


    if (powerofftimer > 0){
      powerofftimer --;

      // spit a msg every 2 seconds
      if (!(powerofftimer % (2000/DELAY_IN_MAIN_LOOP))){
        char tmp[30];
        sprintf(tmp, "power off in %ds\r\n", (powerofftimer*DELAY_IN_MAIN_LOOP)/1000 );
        consoleLog(tmp);
      }

      if (powerofftimer <= 10000/DELAY_IN_MAIN_LOOP){
        buzzerFreq = 3;
        buzzerPattern = 1;
        buzzerLen = 1000;
      }

      if (powerofftimer <= 5000/DELAY_IN_MAIN_LOOP){
        buzzerFreq = 2;
        buzzerPattern = 1;
        buzzerLen = 1000;
      }

      if (powerofftimer <= 0){
        powerofftimer = 0;
        poweroff();
      }
    }
  }
}

/** System Clock Configuration
*/
void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct;
  RCC_ClkInitTypeDef RCC_ClkInitStruct;
  RCC_PeriphCLKInitTypeDef PeriphClkInit;

  /**Initializes the CPU, AHB and APB busses clocks
    */
  RCC_OscInitStruct.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState            = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = 16;
  RCC_OscInitStruct.PLL.PLLState        = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource       = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL          = RCC_PLL_MUL16;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  /**Initializes the CPU, AHB and APB busses clocks
    */
  RCC_ClkInitStruct.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection    = RCC_ADCPCLK2_DIV8;  // 8 MHz
  HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit);

  /**Configure the Systick interrupt time
    */
  HAL_SYSTICK_Config(HAL_RCC_GetHCLKFreq() / 1000);

  /**Configure the Systick
    */
  HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK);

  /* SysTick_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}