/*
* This file is part of the hoverboard-firmware-hack project.
*
* Copyright (C) 2017-2018 Rene Hopf <renehopf@mac.com>
* Copyright (C) 2017-2018 Nico Stute <crinq@crinq.de>
* Copyright (C) 2017-2018 Niklas Fauth <niklas.fauth@kit.fail>
* Copyright (C) 2019-2020 Emanuel FERU <aerdronix@gmail.com>
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

#include <stdlib.h> // for abs()
#include "stm32f1xx_hal.h"
#include "defines.h"
#include "setup.h"
#include "config.h"
#include "comms.h"

#if defined(DEBUG_I2C_LCD) || defined(SUPPORT_LCD)
  #include "hd44780.h"
#endif

#ifdef TRANSPOTTER
  #include "eeprom.h"
#endif

// Matlab includes and defines - from auto-code generation
// ###############################################################################
#include "BLDC_controller.h"      /* Model's header file */
#include "rtwtypes.h"

RT_MODEL rtM_Left_;               /* Real-time model */
RT_MODEL rtM_Right_;              /* Real-time model */
RT_MODEL *const rtM_Left    = &rtM_Left_;
RT_MODEL *const rtM_Right   = &rtM_Right_;

P     rtP_Left;                   /* Block parameters (auto storage) */
DW    rtDW_Left;                  /* Observable states */
ExtU  rtU_Left;                   /* External inputs */
ExtY  rtY_Left;                   /* External outputs */

P     rtP_Right;                  /* Block parameters (auto storage) */
DW    rtDW_Right;                 /* Observable states */
ExtU  rtU_Right;                  /* External inputs */
ExtY  rtY_Right;                  /* External outputs */

extern uint8_t errCode_Left;      /* Global variable to handle Motor error codes */
extern uint8_t errCode_Right;     /* Global variable to handle Motor error codes */
// ###############################################################################

void SystemClock_Config(void);
void poweroff(void);

extern TIM_HandleTypeDef htim_left;
extern TIM_HandleTypeDef htim_right;
extern ADC_HandleTypeDef hadc1;
extern ADC_HandleTypeDef hadc2;
extern volatile adc_buf_t adc_buffer;
#if defined(DEBUG_I2C_LCD) || defined(SUPPORT_LCD)
  LCD_PCF8574_HandleTypeDef lcd;
#endif
extern I2C_HandleTypeDef hi2c2;
#ifndef TRANSPOTTER
  extern UART_HandleTypeDef huart2;
  extern UART_HandleTypeDef huart3;
  static UART_HandleTypeDef huart;
#endif

#if defined(DEBUG_I2C_LCD) || defined(SUPPORT_LCD)
  extern uint8_t LCDerrorFlag;
#endif

#ifdef TRANSPOTTER
  uint8_t nunchuck_connected = 0;
  float steering;
  int feedforward;

  void saveConfig(void);

  /* Virtual address defined by the user: 0xFFFF value is prohibited */
  uint16_t VirtAddVarTab[NB_OF_VAR] = {0x1337};
  uint16_t VarDataTab[NB_OF_VAR] = {0};
  uint16_t VarValue = 0;
  uint16_t saveValue = 0;

  uint16_t counter = 0;
#else
  uint8_t nunchuck_connected = 1;
#endif

#if defined(CONTROL_SERIAL_USART2) || defined(CONTROL_SERIAL_USART3)
typedef struct{
  uint16_t  start;
  int16_t   steer;
  int16_t   speed;
  uint16_t  checksum;
} Serialcommand;
static volatile Serialcommand command;
static int16_t timeoutCnt   = 0;  // Timeout counter for Rx Serial command
#endif
static uint8_t timeoutFlag  = 0;  // Timeout Flag for Rx Serial command: 0 = OK, 1 = Problem detected (line disconnected or wrong Rx data)

#if defined(FEEDBACK_SERIAL_USART2) || defined(FEEDBACK_SERIAL_USART3)
typedef struct{
  uint16_t  start;
  int16_t   cmd1;
  int16_t   cmd2;
  int16_t   speedR;
  int16_t   speedL;
  int16_t   speedR_meas;
  int16_t   speedL_meas;
  int16_t   batVoltage;
  int16_t   boardTemp;
  uint16_t  checksum;
} SerialFeedback;
static SerialFeedback Feedback;
#endif
static uint8_t serialSendCounter; // serial send counter

#if defined(CONTROL_NUNCHUCK) || defined(SUPPORT_NUNCHUCK) || defined(CONTROL_PPM) || defined(CONTROL_ADC)
static uint8_t button1, button2;
#endif

uint8_t ctrlModReqRaw = CTRL_MOD_REQ;
uint8_t ctrlModReq    = CTRL_MOD_REQ;   // Final control mode request
static int        cmd1;                 // normalized input value. -1000 to 1000
static int        cmd2;                 // normalized input value. -1000 to 1000
static int16_t    speed;                // local variable for speed. -1000 to 1000
#ifndef TRANSPOTTER
  static int16_t  steer;                // local variable for steering. -1000 to 1000
  static int16_t  steerFixdt;           // local fixed-point variable for steering low-pass filter
  static int16_t  speedFixdt;           // local fixed-point variable for speed low-pass filter
  static int16_t  steerRateFixdt;       // local fixed-point variable for steering rate limiter
  static int16_t  speedRateFixdt;       // local fixed-point variable for speed rate limiter
#endif

extern volatile int pwml;               // global variable for pwm left. -1000 to 1000
extern volatile int pwmr;               // global variable for pwm right. -1000 to 1000

extern uint8_t buzzerFreq;              // global variable for the buzzer pitch. can be 1, 2, 3, 4, 5, 6, 7...
extern uint8_t buzzerPattern;           // global variable for the buzzer pattern. can be 1, 2, 3, 4, 5, 6, 7...

extern uint8_t enable;                  // global variable for motor enable

extern volatile uint32_t timeout;       // global variable for timeout
extern int16_t batVoltage;              // global variable for battery voltage

static uint32_t inactivity_timeout_counter;

extern uint8_t nunchuck_data[6];
#ifdef CONTROL_PPM
extern volatile uint16_t ppm_captured_value[PPM_NUM_CHANNELS+1];
#endif


void poweroff(void) {
  //  if (abs(speed) < 20) {  // wait for the speed to drop, then shut down -> this is commented out for SAFETY reasons
        buzzerPattern = 0;
        enable = 0;
        consoleLog("-- Motors disabled --\r\n");
        for (int i = 0; i < 8; i++) {
            buzzerFreq = (uint8_t)i;
            HAL_Delay(100);
        }
        HAL_GPIO_WritePin(OFF_PORT, OFF_PIN, 0);
        while(1) {}
  //  }
}


int main(void) {

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

  HAL_GPIO_WritePin(OFF_PORT, OFF_PIN, 1);

  HAL_ADC_Start(&hadc1);
  HAL_ADC_Start(&hadc2);

// Matlab Init
// ###############################################################################
  
  /* Set BLDC controller parameters */ 
  rtP_Right                     = rtP_Left;     // Copy the Left motor parameters to the Right motor parameters

  rtP_Left.b_selPhaABCurrMeas   = 1;            // Left motor measured current phases = {iA, iB} -> do NOT change
  rtP_Left.z_ctrlTypSel         = CTRL_TYP_SEL;
  rtP_Left.b_diagEna            = DIAG_ENA; 
  rtP_Left.i_max                = (I_MOT_MAX * A2BIT_CONV) << 4;        // fixdt(1,16,4)
  rtP_Left.n_max                = N_MOT_MAX << 4;                       // fixdt(1,16,4)
  rtP_Left.b_fieldWeakEna       = FIELD_WEAK_ENA; 
  rtP_Left.id_fieldWeakMax      = (FIELD_WEAK_MAX * A2BIT_CONV) << 4;   // fixdt(1,16,4)
  rtP_Left.a_phaAdvMax          = PHASE_ADV_MAX << 4;                   // fixdt(1,16,4)
  rtP_Left.r_fieldWeakHi        = FIELD_WEAK_HI << 4;                   // fixdt(1,16,4)
  rtP_Left.r_fieldWeakLo        = FIELD_WEAK_LO << 4;                   // fixdt(1,16,4)

  rtP_Right.b_selPhaABCurrMeas  = 0;            // Left motor measured current phases = {iB, iC} -> do NOT change
  rtP_Right.z_ctrlTypSel        = CTRL_TYP_SEL;
  rtP_Right.b_diagEna           = DIAG_ENA; 
  rtP_Right.i_max               = (I_MOT_MAX * A2BIT_CONV) << 4;        // fixdt(1,16,4)
  rtP_Right.n_max               = N_MOT_MAX << 4;                       // fixdt(1,16,4)
  rtP_Right.b_fieldWeakEna      = FIELD_WEAK_ENA; 
  rtP_Right.id_fieldWeakMax     = (FIELD_WEAK_MAX * A2BIT_CONV) << 4;   // fixdt(1,16,4)
  rtP_Right.a_phaAdvMax         = PHASE_ADV_MAX << 4;                   // fixdt(1,16,4)
  rtP_Right.r_fieldWeakHi       = FIELD_WEAK_HI << 4;                   // fixdt(1,16,4)
  rtP_Right.r_fieldWeakLo       = FIELD_WEAK_LO << 4;                   // fixdt(1,16,4)

  /* Pack LEFT motor data into RTM */
  rtM_Left->defaultParam        = &rtP_Left;
  rtM_Left->dwork               = &rtDW_Left;
  rtM_Left->inputs              = &rtU_Left;
  rtM_Left->outputs             = &rtY_Left;

  /* Pack RIGHT motor data into RTM */
  rtM_Right->defaultParam       = &rtP_Right;
  rtM_Right->dwork              = &rtDW_Right;
  rtM_Right->inputs             = &rtU_Right;
  rtM_Right->outputs            = &rtY_Right;

  /* Initialize BLDC controllers */
  BLDC_controller_initialize(rtM_Left);
  BLDC_controller_initialize(rtM_Right);

// ###############################################################################

  for (int i = 8; i >= 0; i--) {
    buzzerFreq = (uint8_t)i;
    HAL_Delay(100);
  }
  buzzerFreq = 0;

  HAL_GPIO_WritePin(LED_PORT, LED_PIN, 1);

  #ifdef TRANSPOTTER
    int  lastDistance = 0;
    enable = 1;
    uint8_t checkRemote = 0;

    HAL_FLASH_Unlock();

    /* EEPROM Init */
    EE_Init();

    EE_ReadVariable(VirtAddVarTab[0], &saveValue);

    HAL_FLASH_Lock();
    float setDistance = saveValue / 1000.0;
    if (setDistance < 0.2) {
      setDistance = 1.0;
    }
  #endif

  #ifdef CONTROL_PPM
    PPM_Init();
  #endif

  #ifdef CONTROL_NUNCHUCK
    I2C_Init();
    Nunchuck_Init();
  #endif

  #if defined(CONTROL_SERIAL_USART2) || defined(FEEDBACK_SERIAL_USART2) || defined(DEBUG_SERIAL_USART2)
    UART2_Init();
    huart = huart2;
  #endif
  #if defined(CONTROL_SERIAL_USART3) || defined(FEEDBACK_SERIAL_USART3) || defined(DEBUG_SERIAL_USART3)
    UART3_Init();
    huart = huart3;
  #endif
  #if defined(CONTROL_SERIAL_USART2) || defined(CONTROL_SERIAL_USART3)
    HAL_UART_Receive_DMA(&huart, (uint8_t *)&command, sizeof(command));
  #endif

  #if defined(DEBUG_I2C_LCD) || defined(SUPPORT_LCD)
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
    #ifdef TRANSPOTTER
      LCD_WriteString(&lcd, "TranspOtter V2.1");
    #else
      LCD_WriteString(&lcd, "Hover V2.0");
    #endif
    LCD_SetLocation(&lcd, 0, 1);
    LCD_WriteString(&lcd, "Initializing...");
  #endif

  #if defined(TRANSPOTTER) && defined(SUPPORT_LCD)
    LCD_ClearDisplay(&lcd);
    HAL_Delay(5);
    LCD_SetLocation(&lcd, 0, 1);
    LCD_WriteString(&lcd, "Bat:");
    LCD_SetLocation(&lcd, 8, 1);
    LCD_WriteString(&lcd, "V");

    LCD_SetLocation(&lcd, 15, 1);
    LCD_WriteString(&lcd, "A");

    LCD_SetLocation(&lcd, 0, 0);
    LCD_WriteString(&lcd, "Len:");
    LCD_SetLocation(&lcd, 8, 0);
    LCD_WriteString(&lcd, "m(");
    LCD_SetLocation(&lcd, 14, 0);
    LCD_WriteString(&lcd, "m)");
  #endif

  int16_t lastSpeedL = 0, lastSpeedR = 0;
  int16_t speedL = 0, speedR = 0;

  int16_t board_temp_adcFixdt = adc_buffer.temp << 4;  // Fixed-point filter output initialized with current ADC converted to fixed-point
  int16_t board_temp_adcFilt  = adc_buffer.temp;
  int16_t board_temp_deg_c;


  while(1) {
    HAL_Delay(DELAY_IN_MAIN_LOOP); //delay in ms

    #ifdef TRANSPOTTER
      if(HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {
        enable = 0;
        while(HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {
          HAL_Delay(10);
        }
        shortBeep(5);
        HAL_Delay(300);
        if (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {
          while(HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {
            HAL_Delay(10);
          }
          longBeep(5);
          HAL_Delay(350);
          poweroff();
        } else {
          setDistance += 0.25;
          if (setDistance > 2.6) {
            setDistance = 0.5;
          }
          shortBeep(setDistance / 0.25);
          saveValue = setDistance * 1000;
          saveConfig();
        }
      }

      #ifdef GAMETRAK_CONNECTION_NORMAL
        uint16_t distance = CLAMP((adc_buffer.l_rx2) - 180, 0, 4095);
        steering = (adc_buffer.l_tx2 - 2048) / 2048.0;
      #endif
      #ifdef GAMETRAK_CONNECTION_ALTERNATE
        uint16_t distance = CLAMP((adc_buffer.l_tx2) - 180, 0, 4095);
        steering = (adc_buffer.l_rx2 - 2048) / 2048.0;
      #endif

      feedforward = ((distance - (int)(setDistance * 1345)));

      if (nunchuck_connected == 0) {
        speedL = speedL * 0.8f + (CLAMP(feedforward +  ((steering)*((float)MAX(ABS(feedforward), 50)) * ROT_P), -850, 850) * -0.2f);
        speedR = speedR * 0.8f + (CLAMP(feedforward -  ((steering)*((float)MAX(ABS(feedforward), 50)) * ROT_P), -850, 850) * -0.2f);
        if ((speedL < lastSpeedL + 50 && speedL > lastSpeedL - 50) && (speedR < lastSpeedR + 50 && speedR > lastSpeedR - 50)) {
          if (distance - (int)(setDistance * 1345) > 0) {
            enable = 1;
          }
          if (distance - (int)(setDistance * 1345) > -300) {
            #ifdef INVERT_R_DIRECTION
              pwmr = -speedR;
            #endif
            #ifndef INVERT_R_DIRECTION
              pwmr = speedR;
            #endif

            #ifdef INVERT_L_DIRECTION
              pwml = -speedL;
            #endif
            #ifndef INVERT_L_DIRECTION
              pwml = speedL;
            #endif

            if (checkRemote) {
              if (!HAL_GPIO_ReadPin(LED_PORT, LED_PIN)) {
                //enable = 1;
              } else {
                enable = 0;
              }
            }
          } else {
            enable = 0;
          }
        }
        lastSpeedL = speedL;
        lastSpeedR = speedR;

        timeout = 0;
      }
    #endif

    #if defined(CONTROL_NUNCHUCK) || defined(SUPPORT_NUNCHUCK)
      if (nunchuck_connected != 0) {
        Nunchuck_Read();
        cmd1 = CLAMP((nunchuck_data[0] - 127) * 8, INPUT_MIN, INPUT_MAX); // x - axis. Nunchuck joystick readings range 30 - 230
        cmd2 = CLAMP((nunchuck_data[1] - 128) * 8, INPUT_MIN, INPUT_MAX); // y - axis

        button1 = (uint8_t)nunchuck_data[5] & 1;
        button2 = (uint8_t)(nunchuck_data[5] >> 1) & 1;
      }
    #endif

    #ifdef CONTROL_PPM
      cmd1 = CLAMP((ppm_captured_value[0] - INPUT_MID) * 2, INPUT_MIN, INPUT_MAX);
      cmd2 = CLAMP((ppm_captured_value[1] - INPUT_MID) * 2, INPUT_MIN, INPUT_MAX);
      button1 = ppm_captured_value[5] > INPUT_MID;
      float scale = ppm_captured_value[2] / 1000.0f;
    #endif

    #ifdef CONTROL_ADC
      // ADC values range: 0-4095, see ADC-calibration in config.h
      #ifdef ADC1_MID_POT
        cmd1 = CLAMP((adc_buffer.l_tx2 - ADC1_MID) * INPUT_MAX / (ADC1_MAX - ADC1_MID), 0, INPUT_MAX) 
              -CLAMP((ADC1_MID - adc_buffer.l_tx2) * INPUT_MAX / (ADC1_MID - ADC1_MIN), 0, INPUT_MAX);    // ADC1        
      #else
        cmd1 = CLAMP((adc_buffer.l_tx2 - ADC1_MIN) * INPUT_MAX / (ADC1_MAX - ADC1_MIN), 0, INPUT_MAX);    // ADC1
      #endif

      #ifdef ADC2_MID_POT
        cmd2 = CLAMP((adc_buffer.l_rx2 - ADC2_MID) * INPUT_MAX / (ADC2_MAX - ADC2_MID), 0, INPUT_MAX)  
              -CLAMP((ADC2_MID - adc_buffer.l_rx2) * INPUT_MAX / (ADC2_MID - ADC2_MIN), 0, INPUT_MAX);    // ADC2        
      #else
        cmd2 = CLAMP((adc_buffer.l_rx2 - ADC2_MIN) * INPUT_MAX / (ADC2_MAX - ADC2_MIN), 0, INPUT_MAX);    // ADC2
      #endif  

      // use ADCs as button inputs:
      button1 = (uint8_t)(adc_buffer.l_tx2 > 2000);  // ADC1
      button2 = (uint8_t)(adc_buffer.l_rx2 > 2000);  // ADC2

      timeout = 0;
    #endif

    #if defined CONTROL_SERIAL_USART2 || defined CONTROL_SERIAL_USART3

      // Handle received data validity, timeout and fix out-of-sync if necessary
      if (command.start == START_FRAME && command.checksum == (uint16_t)(command.start ^ command.steer ^ command.speed)) { 
        if (timeoutFlag) {                      // Check for previous timeout flag  
          if (timeoutCnt-- <= 0)                // Timeout de-qualification
            timeoutFlag   = 0;                  // Timeout flag cleared           
        } else {
          cmd1            = CLAMP((int16_t)command.steer, INPUT_MIN, INPUT_MAX);
          cmd2            = CLAMP((int16_t)command.speed, INPUT_MIN, INPUT_MAX);         
          command.start   = 0xFFFF;             // Change the Start Frame for timeout detection in the next cycle
          timeoutCnt      = 0;                  // Reset the timeout counter         
        }
      } else {
        if (timeoutCnt++ >= SERIAL_TIMEOUT) {   // Timeout qualification
          timeoutFlag     = 1;                  // Timeout detected
          timeoutCnt      = SERIAL_TIMEOUT;     // Limit timout counter value
        }
        // Check the received Start Frame. If it is NOT OK, most probably we are out-of-sync.
        // Try to re-sync by reseting the DMA
        if (command.start != START_FRAME && command.start != 0xFFFF) {
          HAL_UART_DMAStop(&huart);                
          HAL_UART_Receive_DMA(&huart, (uint8_t *)&command, sizeof(command));
        }
      }       

      if (timeoutFlag) {                        // In case of timeout bring the system to a Safe State
        ctrlModReq  = 0;                        // OPEN_MODE request. This will bring the motor power to 0 in a controlled way
        cmd1        = 0;
        cmd2        = 0;
      } else {
        ctrlModReq  = ctrlModReqRaw;            // Follow the Mode request
      }
      timeout = 0;

    #endif

    #ifndef TRANSPOTTER
      // ####### MOTOR ENABLING: Only if the initial input is very small (for SAFETY) #######
      if (enable == 0 && (cmd1 > -50 && cmd1 < 50) && (cmd2 > -50 && cmd2 < 50)){
        shortBeep(6);                     // make 2 beeps indicating the motor enable
        shortBeep(4); HAL_Delay(100);
        enable = 1;                       // enable motors
      }

      // ####### LOW-PASS FILTER #######
      rateLimiter16(cmd1, RATE, &steerRateFixdt);
      rateLimiter16(cmd2, RATE, &speedRateFixdt);
      filtLowPass16(steerRateFixdt >> 4, FILTER, &steerFixdt);
      filtLowPass16(speedRateFixdt >> 4, FILTER, &speedFixdt);
      steer = steerFixdt >> 4;  // convert fixed-point to integer
      speed = speedFixdt >> 4;  // convert fixed-point to integer    

      // ####### MIXER #######
      // speedR = CLAMP((int)(speed * SPEED_COEFFICIENT -  steer * STEER_COEFFICIENT), -1000, 1000);
      // speedL = CLAMP((int)(speed * SPEED_COEFFICIENT +  steer * STEER_COEFFICIENT), -1000, 1000);
      mixerFcn(speedFixdt, steerFixdt, &speedR, &speedL);   // This function implements the equations above

      #ifdef ADDITIONAL_CODE
        ADDITIONAL_CODE;
      #endif


      // ####### SET OUTPUTS (if the target change is less than +/- 50) #######
      if ((speedL > lastSpeedL-50 && speedL < lastSpeedL+50) && (speedR > lastSpeedR-50 && speedR < lastSpeedR+50) && timeout < TIMEOUT) {
        #ifdef INVERT_R_DIRECTION
          pwmr = speedR;
        #else
          pwmr = -speedR;
        #endif
        #ifdef INVERT_L_DIRECTION
          pwml = -speedL;
        #else
          pwml = speedL;
        #endif
      }
    #endif

    lastSpeedL = speedL;
    lastSpeedR = speedR;

    #ifdef TRANSPOTTER
      if (timeout > TIMEOUT) {
        pwml = 0;
        pwmr = 0;
        enable = 0;
        #ifdef SUPPORT_LCD
          LCD_SetLocation(&lcd, 0, 0);
          LCD_WriteString(&lcd, "Len:");
          LCD_SetLocation(&lcd, 8, 0);
          LCD_WriteString(&lcd, "m(");
          LCD_SetLocation(&lcd, 14, 0);
          LCD_WriteString(&lcd, "m)");
        #endif

        HAL_Delay(1000);

        nunchuck_connected = 0;
      }

      if ((distance / 1345.0) - setDistance > 0.5 && (lastDistance / 1345.0) - setDistance > 0.5) { // Error, robot too far away!
        enable = 0;
        longBeep(5);
        #ifdef SUPPORT_LCD
          LCD_ClearDisplay(&lcd);
          HAL_Delay(5);
          LCD_SetLocation(&lcd, 0, 0);
          LCD_WriteString(&lcd, "Emergency Off!");
          LCD_SetLocation(&lcd, 0, 1);
          LCD_WriteString(&lcd, "Keeper too fast.");
        #endif
        poweroff();
      }

      #ifdef SUPPORT_NUNCHUCK
        if (counter % 500 == 0) {
          if (nunchuck_connected == 0 && enable == 0) {
            if (Nunchuck_Ping()) {
              HAL_Delay(500);
              Nunchuck_Init();
              #ifdef SUPPORT_LCD
                LCD_SetLocation(&lcd, 0, 0);
                LCD_WriteString(&lcd, "Nunchuck Control");
              #endif
              timeout = 0;
              HAL_Delay(1000);
              nunchuck_connected = 1;
            }
          }
        }   
      #endif

      #ifdef SUPPORT_LCD
        if (counter % 100 == 0) {
          if (LCDerrorFlag == 1 && enable == 0) {

          } else {
            if (nunchuck_connected == 0) {
              LCD_SetLocation(&lcd, 4, 0);
              LCD_WriteFloat(&lcd,distance/1345.0,2);
              LCD_SetLocation(&lcd, 10, 0);
              LCD_WriteFloat(&lcd,setDistance,2);
            }
            LCD_SetLocation(&lcd, 4, 1);
            LCD_WriteFloat(&lcd,batVoltage, 1);
            LCD_SetLocation(&lcd, 11, 1);
            //LCD_WriteFloat(&lcd,MAX(ABS(currentR), ABS(currentL)),2);
          }
        }
      #endif

      counter++;
    #endif


    // ####### CALC BOARD TEMPERATURE #######
    filtLowPass16(adc_buffer.temp, TEMP_FILT_COEF, &board_temp_adcFixdt);
    board_temp_adcFilt  = board_temp_adcFixdt >> 4;  // convert fixed-point to integer
    board_temp_deg_c    = (TEMP_CAL_HIGH_DEG_C - TEMP_CAL_LOW_DEG_C) * (board_temp_adcFilt - TEMP_CAL_LOW_ADC) / (TEMP_CAL_HIGH_ADC - TEMP_CAL_LOW_ADC) + TEMP_CAL_LOW_DEG_C;

    serialSendCounter++;              // Increment the counter
    if (serialSendCounter > 20) {     // Send data every 100 ms = 20 * 5 ms, where 5 ms is approximately the main loop duration
      serialSendCounter = 0;          // Reset the counter

      // ####### DEBUG SERIAL OUT #######
      #if defined(DEBUG_SERIAL_USART2) || defined(DEBUG_SERIAL_USART3)
        #ifdef CONTROL_ADC
          setScopeChannel(0, (int16_t)adc_buffer.l_tx2);        // 1: ADC1
          setScopeChannel(1, (int16_t)adc_buffer.l_rx2);        // 2: ADC2
        #endif
        setScopeChannel(2, (int16_t)speedR);                    // 1: output command: [-1000, 1000]
        setScopeChannel(3, (int16_t)speedL);                    // 2: output command: [-1000, 1000]
        setScopeChannel(4, (int16_t)adc_buffer.batt1);          // 5: for battery voltage calibration
        setScopeChannel(5, (int16_t)(batVoltage * BAT_CALIB_REAL_VOLTAGE / BAT_CALIB_ADC)); // 6: for verifying battery voltage calibration
        setScopeChannel(6, (int16_t)board_temp_adcFilt);        // 7: for board temperature calibration
        setScopeChannel(7, (int16_t)board_temp_deg_c);          // 8: for verifying board temperature calibration
        consoleScope();

      // ####### FEEDBACK SERIAL OUT #######
      #elif defined(FEEDBACK_SERIAL_USART2) || defined(FEEDBACK_SERIAL_USART3)
        if(UART_DMA_CHANNEL->CNDTR == 0) {
          Feedback.start	        = (uint16_t)START_FRAME;
          Feedback.cmd1           = (int16_t)cmd1;
          Feedback.cmd2           = (int16_t)cmd2;
          Feedback.speedR	        = (int16_t)speedR;
          Feedback.speedL	        = (int16_t)speedL;
          Feedback.speedR_meas	  = (int16_t)rtY_Left.n_mot;
          Feedback.speedL_meas	  = (int16_t)rtY_Right.n_mot;
          Feedback.batVoltage	    = (int16_t)(batVoltage * BAT_CALIB_REAL_VOLTAGE / BAT_CALIB_ADC);
          Feedback.boardTemp	    = (int16_t)board_temp_deg_c;
          Feedback.checksum       = (uint16_t)(Feedback.start ^ Feedback.cmd1 ^ Feedback.cmd2 ^ Feedback.speedR ^ Feedback.speedL
                                    ^ Feedback.speedR_meas ^ Feedback.speedL_meas ^ Feedback.batVoltage ^ Feedback.boardTemp); 

          UART_DMA_CHANNEL->CCR  &= ~DMA_CCR_EN;
          UART_DMA_CHANNEL->CNDTR = sizeof(Feedback);
          UART_DMA_CHANNEL->CMAR  = (uint32_t)&Feedback;
          UART_DMA_CHANNEL->CCR  |= DMA_CCR_EN;          
        }
      #endif      
    }    

    HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
    // ####### POWEROFF BY POWER-BUTTON #######
    if (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {
      enable = 0;                                             // disable motors
      while (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN)) {}    // wait until button is released
      if(__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST)) {               // do not power off after software reset (from a programmer/debugger)
        __HAL_RCC_CLEAR_RESET_FLAGS();                        // clear reset flags
      } else {
        poweroff();                                           // release power-latch
      }
    }


    // ####### BEEP AND EMERGENCY POWEROFF #######
    if ((TEMP_POWEROFF_ENABLE && board_temp_deg_c >= TEMP_POWEROFF && abs(speed) < 20) || (batVoltage < BAT_LOW_DEAD && abs(speed) < 20)) {  // poweroff before mainboard burns OR low bat 3
      poweroff();
    } else if (TEMP_WARNING_ENABLE && board_temp_deg_c >= TEMP_WARNING) {  // beep if mainboard gets hot
      buzzerFreq = 4;
      buzzerPattern = 1;
    } else if (batVoltage < BAT_LOW_LVL1 && batVoltage >= BAT_LOW_LVL2 && BAT_LOW_LVL1_ENABLE) {  // low bat 1: slow beep
      buzzerFreq = 5;
      buzzerPattern = 42;
    } else if (batVoltage < BAT_LOW_LVL2 && batVoltage >= BAT_LOW_DEAD && BAT_LOW_LVL2_ENABLE) {  // low bat 2: fast beep
      buzzerFreq = 5;
      buzzerPattern = 6;
    } else if (errCode_Left || errCode_Right || timeoutFlag) {  // beep in case of Motor error or serial timeout - fast beep
      buzzerFreq = 12;
      buzzerPattern = 1;
    } else if (BEEPS_BACKWARD && speed < -50) {  // backward beep
      buzzerFreq = 5;
      buzzerPattern = 1;
    } else {  // do not beep
      buzzerFreq = 0;
      buzzerPattern = 0;
    }


    // ####### INACTIVITY TIMEOUT #######
    if (abs(speedL) > 50 || abs(speedR) > 50) {
      inactivity_timeout_counter = 0;
    } else {
      inactivity_timeout_counter ++;
    }
    if (inactivity_timeout_counter > (INACTIVITY_TIMEOUT * 60 * 1000) / (DELAY_IN_MAIN_LOOP + 1)) {  // rest of main loop needs maybe 1ms
      poweroff();
    }
  }
}

#ifdef TRANSPOTTER
  void saveConfig() {
    HAL_FLASH_Unlock();
    EE_WriteVariable(VirtAddVarTab[0], saveValue);
    HAL_FLASH_Lock();
  }
#endif

void longBeep(uint8_t freq){
    buzzerFreq = freq;
    HAL_Delay(500);
    buzzerFreq = 0;
}

void shortBeep(uint8_t freq){
    buzzerFreq = freq;
    HAL_Delay(100);
    buzzerFreq = 0;
}

// ===========================================================
  /* Low pass filter fixed-point 16 bits: fixdt(1,16,4)
  * Max:  2047.9375
  * Min: -2048
  * Res:  0.0625
  * 
  * Inputs:       u     = int16
  * Outputs:      y     = fixdt(1,16,4)
  * Parameters:   coef  = fixdt(0,16,16) = [0,65535U]
  * 
  * Example: 
  * If coef = 0.8 (in floating point), then coef = 0.8 * 2^16 = 52429 (in fixed-point)
  * filtLowPass16(u, 52429, &y);
  * yint = y >> 4; // the integer output is the fixed-point ouput shifted by 4 bits
  */
void filtLowPass16(int16_t u, uint16_t coef, int16_t *y)
{
  int32_t tmp;

  tmp = (((int16_t)(u << 4) * coef) >> 16) + 
        (((int32_t)(65535U - coef) * (*y)) >> 16);

  // Overflow protection
  tmp = CLAMP(tmp, -32768, 32767);

  *y = (int16_t)tmp;
}

// ===========================================================
  /* Low pass filter fixed-point 32 bits: fixdt(1,32,16)
  * Max:  32767.99998474121
  * Min: -32768
  * Res:  1.52587890625e-5
  * 
  * Inputs:       u     = int32
  * Outputs:      y     = fixdt(1,32,16)
  * Parameters:   coef  = fixdt(0,16,16) = [0,65535U]
  * 
  * Example: 
  * If coef = 0.8 (in floating point), then coef = 0.8 * 2^16 = 52429 (in fixed-point)
  * filtLowPass16(u, 52429, &y);
  * yint = y >> 16;  // the integer output is the fixed-point ouput shifted by 16 bits
  */
void filtLowPass32(int32_t u, uint16_t coef, int32_t *y)
{
  int32_t q0;
  int32_t q1;
  int32_t tmp;

  q0 = (int32_t)(((int64_t)(u << 16) * coef) >> 16);
  q1 = (int32_t)(((int64_t)(65535U - coef) * (*y)) >> 16);

  // Overflow protection
  if ((q0 < 0) && (q1 < MIN_int32_T - q0)) {
    tmp = MIN_int32_T;
  } else if ((q0 > 0) && (q1 > MAX_int32_T - q0)) {
    tmp = MAX_int32_T;
  } else {
    tmp = q0 + q1;
  }

  *y = tmp;
}

// ===========================================================
  /* mixerFcn(rtu_speed, rtu_steer, &rty_speedR, &rty_speedL); 
  * Inputs:       rtu_speed, rtu_steer                  = fixdt(1,16,4)
  * Outputs:      rty_speedR, rty_speedL                = int16_t
  * Parameters:   SPEED_COEFFICIENT, STEER_COEFFICIENT  = fixdt(0,16,14)
  */
void mixerFcn(int16_t rtu_speed, int16_t rtu_steer, int16_t *rty_speedR, int16_t *rty_speedL)
{
  int16_t prodSpeed;
  int16_t prodSteer;
  int32_t tmp;

  prodSpeed   = (int16_t)((rtu_speed * (int16_t)SPEED_COEFFICIENT) >> 14);
  prodSteer   = (int16_t)((rtu_steer * (int16_t)STEER_COEFFICIENT) >> 14);

  tmp         = prodSpeed - prodSteer;  
  tmp         = CLAMP(tmp, -32768, 32767);  // Overflow protection
  *rty_speedR = (int16_t)(tmp >> 4);        // Convert from fixed-point to int 
  *rty_speedR = CLAMP(*rty_speedR, INPUT_MIN, INPUT_MAX);

  tmp         = prodSpeed + prodSteer;
  tmp         = CLAMP(tmp, -32768, 32767);  // Overflow protection
  *rty_speedL = (int16_t)(tmp >> 4);        // Convert from fixed-point to int
  *rty_speedL = CLAMP(*rty_speedL, INPUT_MIN, INPUT_MAX);
}

// ===========================================================
  /* rateLimiter16(int16_t u, int16_t rate, int16_t *y);
  * Inputs:       u     = int16
  * Outputs:      y     = fixdt(1,16,4)
  * Parameters:   rate  = fixdt(1,16,4) = [0, 32767] Do NOT make rate negative (>32767)
  */
void rateLimiter16(int16_t u, int16_t rate, int16_t *y)
{
  int16_t q0;
  int16_t q1;

  q0 = (u << 4)  - *y;

  if (q0 > rate) {
    q0 = rate;
  } else {
    q1 = -rate;
    if (q0 < q1) {
      q0 = q1;
    }
  }

  *y = q0 + *y;
}

// ===========================================================

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
  RCC_ClkInitStruct.ClockType           = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource        = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider       = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider      = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider      = RCC_HCLK_DIV1;

  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);

  PeriphClkInit.PeriphClockSelection    = RCC_PERIPHCLK_ADC;
  // PeriphClkInit.AdcClockSelection    = RCC_ADCPCLK2_DIV8;  // 8 MHz
  PeriphClkInit.AdcClockSelection       = RCC_ADCPCLK2_DIV4;  // 16 MHz
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