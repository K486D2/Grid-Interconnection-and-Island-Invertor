/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2023 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "adc.h"
#include "dac.h"
#include "dma.h"
#include "hrtim.h"
#include "i2c.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "stdarg.h"

#include "arm_math.h"
#include "arm_const_structs.h"

#include "spll_sogi.h"
#include "u8g2.h"

// �������򱣻�
#define USE_Current_Backfilling_PROTECT    // �������򱣻��ر�
#define PROTECT_Current_Backfilling_W (-20) // �������򱣻���-1w

// ռ�ձȱ���
#define USE_DUTYrms_ERROR
#define USE_DUTYrms_ERROR_Value 0.85 // �ο�ֵ0.707105
int error_cnt=0;

#define _DEBUG_DUTY_SENSOR_ 2 // ��ռ�ձȲ⴫��������
#define _DEBUG_ConstVol_ 2    // ��ѹ
#define _DEBUG_ConstCur_ 3    // ����
#define _DEBUG_SPGI_ 4        // ����������
#define _DEBUG_MAIN_FSM_ 5    // ������״̬��

//#define __DEBUG _DEBUG_ConstCur_ // shit

// �����Ϊ1ʱȡ1��Ϊ�����2��ȡ2
#define INV_MACHINE_NUM 2
// #define INV_MACHINE_NUM 2
#if INV_MACHINE_NUM != 1 && INV_MACHINE_NUM != 2
#error INV_MACHINE_NUM must be between 1 and 2.
#endif
#ifndef INV_MACHINE_NUM
#error INV_MACHINE_NUM must be refered.
#endif //

#if INV_MACHINE_NUM == 1
#define MACHINE_Info ("INV_MACHINE_NUM:1\n")
#endif
#if INV_MACHINE_NUM == 2
#define MACHINE_Info ("INV_MACHINE_NUM:2\n")
#endif

// ��������
#include <math.h>
#define SYSTickFreq (170000000UL)
#define Vol_SoftStart_STEP (1.0f)
#define Cur_SoftStart_STEP (0.01f)
#define Vol_SoftStart_aimOverRate 0.05  // 5%
#define Cur_SPGI_beforeSoftStart (2.0f) // ���裺2A

#define MAIN_FSM_POWERON_NoVol_Value (5.0f)   // ����5Vrms��Ϊû�е�ѹ
#define MAIN_FSM_ConstVol_VALUE (24)          // ����Ҫ��24Vrms
#define MAIN_FSM_POWERON_AVERAGE_number (100) // 100�ξ�ֵ�˲����ж�

// fsm_UI_Com
#define CON_CUR_Io_initial Cur_SPGI_beforeSoftStart*2 //should be 2 bei
#define CON_CUR_k_initial 1

#define HRTIM_MAB_PERIOD (54400)

extern SPLL_1PH_SOGI spll1;

void error();

typedef enum ref_sine_src
{
  ref_sine_src_enum_Internal_sine = 0,
  ref_sine_src_enum_PLL_sine
} ref_sine_src_enum;

typedef enum loop_Controller
{
  V_loop = 0,
  I_loop
} loop_switch_enum;

typedef enum loopShutdown
{
  loopCtrl_on,
  loopCtrl_off
} loopShutdown_enum;
typedef struct loopCtrl
{
  ref_sine_src_enum ref_sine_src;

  loop_switch_enum loop_switch_enum;

  // ��·��ʼ״̬����
  loopShutdown_enum loopShutdown; // ��ʼֵռ�ձȱ�������Ϊ0.5f

  float V_rms_aim; // ��λΪVrms���жϵĴ����п������������зŴ�/��С
  float I_rms_aim; // ��λΪArms���жϵĴ����п������������зŴ�/��С
} loopCtrl_t;

loopCtrl_t Loop_Controller;

typedef enum fsm_UI_mode
{
  fsm_UI_mode_idle,
  fsm_UI_mode_Cur,
#if INV_MACHINE_NUM == 1
  fsm_UI_mode_Vol,
#endif
} fsm_UI_mode_enum;

typedef struct Con_Cur
{
  float Io;
  float k;
  float en_flag;
} Con_Cur_t;
typedef struct fsm_UI_com
{
  fsm_UI_mode_enum fsm_UI_mode;
  Con_Cur_t Con_Cur_info;
} fsm_UI_com_t;
fsm_UI_com_t fsm_UI_communication = {fsm_UI_mode_idle, {CON_CUR_Io_initial, CON_CUR_k_initial, 1}};

GPIO_PinState MYHAL_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
  return (GPIOx->IDR & GPIO_Pin) ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

void MYHAL_GPIO_SetPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
  GPIOx->BSRR = GPIO_Pin;
}

void MYHAL_GPIO_ResetPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
  GPIOx->BSRR = (uint32_t)GPIO_Pin << 16U;
}

void MYHAL_GPIO_TogglePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
  uint32_t odr = GPIOx->ODR;
  GPIOx->BSRR = ((odr & GPIO_Pin) << 16U) | (~odr & GPIO_Pin);
}

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#define CLAMP(x, max, min) ((x < max) ? ((x > min) ? x : min) : max)
float o_Vol, i_Vol, o_Cur, o_Cur_ref, o_Vol_ref, k_Vol, k_Cur;
float o_Vol_Vp_N, o_Vol_Vp_P, o_Vol_Vp, o_Vol_Vpp, o_Vol_rms, o_Vol_rms_sum;
float o_Cur_rms, o_Cur_rms_sum;
float acBusVol, acBusVol_rms, acBusVol_rms_sum;
float o_Power; // o_Power_rms; //o_Power=o_Vol*o_Cur

float duty_Virtual, duty_Virtual_rms, duty_Virtual_rms_sum; //-1~1

uint16_t Caculate_50k_to_50_index;

float HB_duty, ref_sine;
uint16_t ADC_Value[4] = {0};
uint16_t DAC_Value = 0;
float y0;
float y1;
float sinf_out;

float sin_Phase_det = 0; // -2.65;

float pll_sine, internal_sine;

uint32_t ARR;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

uint16_t ADC_buffer_1[1];
uint16_t ADC_buffer_2[3];
uint16_t DAC_buffer[1];

void MY_HRTIM_SetDuty_MAB(float duty_to_set, float duty_min, float duty_max); // 0<=duty_min<=duty_to_set<=duty_max<=1

// KEYBOARD
#define Keyboard_X 4
#define Keyboard_Y 4
__IO int i_Keyboard, j_Keyboard;
__IO GPIO_PinState Keyboard_state_temp;
__IO uint16_t Keyboard_state, Keyboard_state_last;
__IO uint8_t Keyboard_state_4x4[4][4];

GPIO_TypeDef *Keyboard_X_output_ports[Keyboard_X] = {KEYBOARD_X_0_GPIO_Port, KEYBOARD_X_1_GPIO_Port, KEYBOARD_X_2_GPIO_Port, KEYBOARD_X_3_GPIO_Port};
uint16_t Keyboard_X_output_pins[Keyboard_X] = {KEYBOARD_X_0_Pin, KEYBOARD_X_1_Pin, KEYBOARD_X_2_Pin, KEYBOARD_X_3_Pin};

GPIO_TypeDef *Keyboard_Y_output_ports[Keyboard_Y] = {KEYBOARD_Y_0_GPIO_Port, KEYBOARD_Y_1_GPIO_Port, KEYBOARD_Y_2_GPIO_Port, KEYBOARD_Y_3_GPIO_Port};
uint16_t Keyboard_Y_output_pins[Keyboard_Y] = {KEYBOARD_Y_0_Pin, KEYBOARD_Y_1_Pin, KEYBOARD_Y_2_Pin, KEYBOARD_Y_3_Pin};

char KB_input_char[32];
float KB_input_float;
uint8_t point_num;

// OLED
char OLED_I2C_Str[6][32];
u8g2_t OLED_I2C1;
uint8_t u8x8_OLED_I2C1_gpio_and_delay(U8X8_UNUSED u8x8_t *u8x8, U8X8_UNUSED uint8_t msg, U8X8_UNUSED uint8_t arg_int, U8X8_UNUSED void *arg_ptr);
uint8_t u8x8_OLED_I2C1_hw(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr);

// UI
uint8_t flash_bit;
uint8_t to_set_or_set = 0;
uint8_t Parameter_adj_enable = 0;
uint8_t value_set_now;

// �������ʹ������
#define Parameter_list_num 5
float Pl_test_0, Pl_test_1, Pl_test_2, Pl_test_3, Pl_test_4;
// Parameter_list
float *Parameter_list_pointer[Parameter_list_num] = {&Pl_test_0, &Pl_test_1, &Pl_test_2, &Pl_test_3, &Pl_test_4};
char Parameter_list_name[Parameter_list_num][32] = {"Pl_test_0", "Pl_test_1", "Pl_test_2", "Pl_test_3", "Pl_test_4"};

uint8_t Parameter_list_use_now;

float y0;
float y1;
float sinf_out;

/* ���ƴ��� */

typedef struct
{
  float A0;       /**< The derived gain, A0 = Kp + Ki + Kd . */
  float A1;       /**< The derived gain, A1 = -Kp - 2Kd. */
  float A2;       /**< The derived gain, A2 = Kd . */
  float state[3]; /**< The state array of length 3. */
  float Kp;       /**< The proportional gain. */
  float Ki;       /**< The integral gain. */
  float Kd;       /**< The derivative gain. */
} PID_t;
PID_t pid_o_Cur = {
    .Kp = 0.1,
    .Ki = 0.6};
PID_t pid_o_Vol = {
    .Kp = 0.02,
    .Ki = 0.01,
    .Kd = 0.00};
void pid_init(PID_t *S)
{
  /* Derived coefficient A0 */
  S->A0 = S->Kp + S->Ki + S->Kd;
  /* Derived coefficient A1 */
  S->A1 = (-S->Kp) - ((float)2.0f * S->Kd);
  /* Derived coefficient A2 */
  S->A2 = S->Kd;
}
float pid_process_o_Cur(PID_t *S, float aim, float current, float out_max, float out_min)
{
  pid_init(S);
  float out;
  float in = aim - current;

  /* u[t] = u[t - 1] + A0 * e[t] + A1 * e[t - 1] + A2 * e[t - 2]  */
  out = (S->A0 * in) +
        (S->A1 * S->state[0]) + (S->A2 * S->state[1]) + (S->state[2]);

  /* Update state */
  S->state[1] = S->state[0];
  S->state[0] = in;
  S->state[2] = out;

  out = CLAMP(out, out_max, out_min);
  /* return to application */
  return (out);
}
float pid_process_o_Vol(PID_t *S, float aim, float current, float out_max, float out_min)
{
  pid_init(S);
  float out;
  float in = aim - current;

  /* u[t] = u[t - 1] + A0 * e[t] + A1 * e[t - 1] + A2 * e[t - 2]  */
  out = (S->A0 * in) +
        (S->A1 * S->state[0]) + (S->A2 * S->state[1]) + (S->state[2]);

  /* Update state */
  S->state[1] = S->state[0];
  S->state[0] = in;
  S->state[2] = out;

  out = CLAMP(out, out_max, out_min);

  /* return to application */
  return (out);
}

// PR_t
#define pr_PI 3.141592654f
typedef struct
{
  float Kp;
  float Kr;
  float wo;
  float wc;
  float Ts;
  float A0, A1, A2, B0, B1, B2;
  float vo, vo_1, vo_2;
  float vi, vi_1, vi_2;
  float temp;
} PR_t;

// PR_t pr_o_Cur = {
//     .Kp = 10,
//     .Kr = 3000,
//     .wo = 50 * pr_PI * 2,
//     .wc = pr_PI,
//     .Ts = 0.00002f};
PR_t pr_o_Cur = {
    .Kp = 10,
    .Kr = 3000,
    .wo = 50 * pr_PI * 2,
    .wc = pr_PI,
    .Ts = 0.00002f};
PR_t pr_o_Vol = {
    .Kp = 10,
    .Kr = 3000,
    .wo = 50 * pr_PI * 2,
    .wc = pr_PI,
    .Ts = 0.00002};

void PR_init(PR_t *p)
{
  float temp = 0;
  // p->Ts = Ts;
  // p->Kp = Kp;
  // p->Kr = Kr;
  // p->wc = wc;
  // p->wo = wo;
  temp = 4 / p->Ts / p->Ts + 4 * p->wc / p->Ts + p->wo * p->wo;

  p->B0 = (4 * p->Kp / p->Ts / p->Ts + 4 * p->wc * (p->Kp + p->Kr) / p->Ts + p->Kp * p->wo * p->wo) / temp;
  p->B1 = (-8 * p->Kp / p->Ts / p->Ts + 2 * p->Kp * p->wo * p->wo) / temp;
  p->B2 = (4 * p->Kp / p->Ts / p->Ts - 4 * p->wc / p->Ts * (p->Kp + p->Kr) + p->Kp * p->wo * p->wo) / temp;
  p->A1 = (-8 / p->Ts / p->Ts + 2 * p->wo * p->wo) / temp;
  p->A2 = (4 / p->Ts / p->Ts - 4 * p->wc / p->Ts + p->wo * p->wo) / temp;
}
void PR_process_o_Cur_init(PR_t *p, float Kp, float Kr, float Ts, float wc, float wo)
{
  float temp = 0;
  // p->Ts = Ts;
  // p->Kp = Kp;
  // p->Kr = Kr;
  // p->wc = wc;
  // p->wo = wo;
  temp = 4 / p->Ts / p->Ts + 4 * p->wc / p->Ts + p->wo * p->wo;

  p->B0 = (4 * p->Kp / p->Ts / p->Ts + 4 * p->wc * (p->Kp + p->Kr) / p->Ts + p->Kp * p->wo * p->wo) / temp;
  p->B1 = (-8 * p->Kp / p->Ts / p->Ts + 2 * p->Kp * p->wo * p->wo) / temp;
  p->B2 = (4 * p->Kp / p->Ts / p->Ts - 4 * p->wc / p->Ts * (p->Kp + p->Kr) + p->Kp * p->wo * p->wo) / temp;
  p->A1 = (-8 / p->Ts / p->Ts + 2 * p->wo * p->wo) / temp;
  p->A2 = (4 / p->Ts / p->Ts - 4 * p->wc / p->Ts + p->wo * p->wo) / temp;
  /*PRpr;
   * ????
   * y[n]+A1[n-1]+A2[n-2]=B0x[n]+B1x[n-1]+B2[n-2]
   */
}

float PR_process_o_Cur(PR_t *p, float aim, float cur)
{
  p->vi = aim - cur;
  p->vo = -p->A1 * p->vo_1 - p->A2 * p->vo_2 + p->B0 * p->vi + p->B1 * p->vi_1 + p->B2 * p->vi_2;

  // update and store
  p->vo_2 = p->vo_1;
  p->vo_1 = p->vo;
  p->vi_2 = p->vi_1;
  p->vi_1 = p->vi;

  //  float out = CLAMP(p->vo, 99, -99);
  //  return out;
  p->vo = CLAMP(p->vo, 99, -99);
  return p->vo;
}

void PR_process_o_Vol_init(PR_t *p, float Kp, float Kr, float Ts, float wc, float wo)
{
  float temp = 0;
  p->Ts = Ts;
  p->Kp = Kp;
  p->Kr = Kr;
  p->wc = wc;
  p->wo = wo;
  temp = 4 / p->Ts / p->Ts + 4 * p->wc / p->Ts + p->wo * p->wo;

  p->B0 = (4 * p->Kp / p->Ts / p->Ts + 4 * p->wc * (p->Kp + p->Kr) / p->Ts + p->Kp * p->wo * p->wo) / temp;
  p->B1 = (-8 * p->Kp / p->Ts / p->Ts + 2 * p->Kp * p->wo * p->wo) / temp;
  p->B2 = (4 * p->Kp / p->Ts / p->Ts - 4 * p->wc / p->Ts * (p->Kp + p->Kr) + p->Kp * p->wo * p->wo) / temp;
  p->A1 = (-8 / p->Ts / p->Ts + 2 * p->wo * p->wo) / temp;
  p->A2 = (4 / p->Ts / p->Ts - 4 * p->wc / p->Ts + p->wo * p->wo) / temp;
  /*PRpr;
   * ????
   * y[n]+A1[n-1]+A2[n-2]=B0x[n]+B1x[n-1]+B2[n-2]
   */
}

float PR_process_o_Vol(PR_t *p, float aim, float cur)
{
  p->vi = aim - cur;
  p->vo = -p->A1 * p->vo_1 - p->A2 * p->vo_2 + p->B0 * p->vi + p->B1 * p->vi_1 + p->B2 * p->vi_2;

  // update and store
  p->vo_2 = p->vo_1;
  p->vo_1 = p->vo;
  p->vi_2 = p->vi_1;
  p->vi_1 = p->vi;

  float out = CLAMP(p->vo, 99, -99);
  return out;
  //  p->vo = CLAMP(p->vo, 99, -99);
  //  return p->vo;
}

int UART_printf(UART_HandleTypeDef *huart, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int length;
  char buffer[128];
  length = vsnprintf(buffer, 128, fmt, ap);
  HAL_UART_Transmit(huart, (uint8_t *)buffer, length, HAL_MAX_DELAY); // HAL_MAX_DELAY
  //    CDC_Transmit_FS((uint8_t*)buffer,length);
  va_end(ap);
  return length;
}

/**
 * @brief ����ҵ���߼�����
 *
 * @author TT && FF
 */

typedef enum inv_state
{
  Vol_inv,
  Cur_inv,
  Cur_GI
} inv_state_enum;
typedef enum SPGI_state
{
  IDLE,
  PLL_stage,
  Voltage_Sync_softStart,
  Voltage_Sync_stage,

  SPGI_OK,
  SPGI_SoftStart,
  SPGI_Working,
  SPGI_Error
} SPGI_state_enum; // Single Phase Grid-connected Inverter

/**
 * @brief ��ҵ���߼�����  part1
 *
 * �������Ҫ��
 */

typedef enum PowerOn_acBusVol
{
  PowerOn_acBusVol_detected,
  PowerOn_acBusVol_NOT_detected,

} PowerOn_acBusVol_enum;
typedef enum main_fsm
{
  main_fsm_IDLE,
  main_fsm_Poweron_detect_stage,
#if INV_MACHINE_NUM == 1
  main_fsm_Vol_Cons,
#endif
  main_fsm_Cur_Cons
} main_fsm_enum;
typedef struct fsm
{
  main_fsm_enum main_fsm_state;
  inv_state_enum inv_state;
  SPGI_state_enum SPGI_state;

  PowerOn_acBusVol_enum PowerOn_acBusVol_state;
} fsm_t;

fsm_t fsm;

void SPGI_LED(SPGI_state_enum stageName, int LED_Value)
{
  if (stageName == IDLE)
  {
  }
  else if (stageName == PLL_stage)
  {
  }
  else if (stageName == Voltage_Sync_softStart)
  {
  }
  else if (stageName == Voltage_Sync_stage)
  {
  }
  else if (stageName == SPGI_OK)
  {
  }
  else if (stageName == SPGI_Working)
  {
  }
  else if (stageName == SPGI_Error)
  {
  }
}
void SPGI_LED_init()
{
  SPGI_LED(IDLE, 0);
  SPGI_LED(PLL_stage, 0);
  SPGI_LED(Voltage_Sync_softStart, 0);
  SPGI_LED(Voltage_Sync_stage, 0);
  SPGI_LED(SPGI_OK, 0);
  SPGI_LED(SPGI_Working, 0);
  SPGI_LED(SPGI_Error, 0);
}
unsigned int get_SysTick()
{
  return 100000000;
  // return HAL_GetTick();
}
/**
 * б�º��� ʵ������y=ax+b����
 * @param slewVal x
 * @param refVal  y
 * @param slewRate a
 * @return
 */
float Slew_Func(float *slewVal, float refVal, float slewRate)
{
  static float diff = 0;
  diff = refVal - *slewVal;
  if (diff >= slewRate)
  {
    *slewVal += slewRate;
    return (1);
  }
  else if (-diff >= slewRate)
  {
    *slewVal -= slewRate;
    return (-1);
  }
  else
  {
    *slewVal = refVal;
    return (0);
  }
}
int SPGI_fsm(fsm_t *fsm,
             /*�����������*/
             float acBusVol_rms, float o_Vol_rms,
             float acBusVol, float o_Vol,
             /*���Ʋ���*/
             loopCtrl_t *ploopCtrl,
             /*�̵�������*/
             void SPGI_Connect_Func(), void SPGI_disConnect_Func())
{
  static unsigned int pllStage_t0 = 0;
  switch (fsm->SPGI_state)
  {
  case (IDLE):
  {
    fsm->SPGI_state = Voltage_Sync_softStart;
    // �л�PLLԴ
    ploopCtrl->ref_sine_src = ref_sine_src_enum_PLL_sine;
    // ��·����֤���ʼ��ȶ���
    (*SPGI_disConnect_Func)();
    // ��ʼ��ָʾ��
    SPGI_LED_init();
    pllStage_t0 = get_SysTick(); // ��תǰ��¼

    // �Ƚ������ѹ��ֵ��Ϊ��ǰ������
    ploopCtrl->V_rms_aim = o_Vol_rms;

    ploopCtrl->I_rms_aim = 0;
  }
  break;
  case (Voltage_Sync_softStart):
  {
    if (!Slew_Func(&(ploopCtrl->V_rms_aim), acBusVol_rms * (1 + Vol_SoftStart_aimOverRate), Vol_SoftStart_STEP))
    {
      fsm->SPGI_state = Voltage_Sync_stage;
      SPGI_LED(Voltage_Sync_softStart, 0);
    }
    else
    {
      fsm->SPGI_state = Voltage_Sync_softStart;
      SPGI_LED(Voltage_Sync_softStart, 1);
    }
  }
  break;

  case (Voltage_Sync_stage):
  {
    if (fabs(acBusVol_rms - o_Vol_rms) < acBusVol_rms * 0.05f)
    {
      fsm->SPGI_state = PLL_stage;
      SPGI_LED(Voltage_Sync_stage, 0);
    }
    else
    {
      fsm->SPGI_state = Voltage_Sync_stage;
      SPGI_LED(Voltage_Sync_stage, 1);
    }
  }
  break;

  case (PLL_stage):
  {
    //    float delta_tick = (float)(get_SysTick() - pllStage_t0);
    //    if (delta_tick < 0)
    //    {
    //      delta_tick = 2.0f * SYSTickFreq;
    //    }
    //    if ((fabsf(acBusVol - o_Vol) < acBusVol * 0.05) && (delta_tick > 1.0f * SYSTickFreq)) // �������:�Ҽ������1s
    //    {
    //      // ��ת��һ���׶�
    //      fsm->SPGI_state = SPGI_OK;
    //      // ָʾ�������
    //      SPGI_LED(PLL_stage, 0);
    //    }
    //    else
    //    {
    //      fsm->SPGI_state = PLL_stage;
    //      SPGI_LED(PLL_stage, 1);
    //    }

    if ((fabsf(acBusVol - o_Vol) < acBusVol * 0.05)) // �������:�Ҽ������1s
    {
      // ��ת��һ���׶�
      fsm->SPGI_state = SPGI_OK;
      // ָʾ�������
      SPGI_LED(PLL_stage, 0);
    }
    else
    {
      fsm->SPGI_state = PLL_stage;
      SPGI_LED(PLL_stage, 1);
    }

    // ����
    // fsm->SPGI_state = SPGI_OK;
  }
  break;

  case (SPGI_OK):
  {
    if (1)
    {
      fsm->SPGI_state = SPGI_SoftStart;
      SPGI_LED(SPGI_OK, 0);
    }
    else
    {
      fsm->SPGI_state = SPGI_OK;
      SPGI_LED(SPGI_OK, 1);
    }
  }
  break;

  case (SPGI_SoftStart):
  {
    if (!Slew_Func(&(ploopCtrl->I_rms_aim), Cur_SPGI_beforeSoftStart, Cur_SoftStart_STEP))
    {
      fsm->SPGI_state = SPGI_Working;
      SPGI_LED(SPGI_SoftStart, 0);
    }
    else
    {
      fsm->SPGI_state = SPGI_SoftStart;
      SPGI_LED(SPGI_SoftStart, 1);
    }
  }
  break;

  case (SPGI_Working):
  {

    // fsm->SPGI_state = SPGI_Working;
    SPGI_LED(SPGI_Working, 1);
  }
  break;

  default:
  {
    fsm->SPGI_state = IDLE;
  }
  break;
  }

  /*ִ��*/
  if (fsm->SPGI_state == Voltage_Sync_stage)
  {
    // �趨��ѹֵ,׷��ĸ�ߵ�ѹ
    ploopCtrl->V_rms_aim = acBusVol_rms * (1 + Vol_SoftStart_aimOverRate);
    // �л�����ѹ��
    ploopCtrl->loop_switch_enum = V_loop;
  }
  else if (fsm->SPGI_state == PLL_stage)
  {
    // �л�PLL
    ploopCtrl->ref_sine_src = ref_sine_src_enum_PLL_sine;
  }
  else if (fsm->SPGI_state == SPGI_OK)
  {
    // ����Ŀ�����
    ploopCtrl->I_rms_aim = Cur_SPGI_beforeSoftStart; // Ĭ��0.01A
    // ���̵���
    (*SPGI_Connect_Func)(); // ��·
    // �л�������
    ploopCtrl->loop_switch_enum = I_loop;
  }
  else if (fsm->SPGI_state == SPGI_Working)
  {
    return (0); // �������
  }
  return (-1); // ������δ���
}

/**
 * @brief ҵ���߼�����  part2
 *
 * �������Ҫ��
 */
void main_fsm(fsm_UI_com_t *pfsm_UI, fsm_t *fsm,
              /*�����������*/
              float acBusVol_rms, float o_Vol_rms,
              float acBusVol, float o_Vol,
              /*���Ʋ���*/
              loopCtrl_t *ploopCtrl,
              /*�̵�������*/
              void Relay_Connect_Func(), void Relay_disConnect_Func())
{
  /**
   * machine1:
   *        �ϵ磺mode machine1_Mode_acBusVol_on
   *        �޵磺mode machine1_Mode_acBusVol_off
   *
   */
  static int PowerOn_detect_cnt = 0;
  static float PowerOn_detect_acBusVol_rms_sum = 0, PowerOn_detect_acBusVol_rms_ave = 0;
  if (fsm->main_fsm_state == main_fsm_IDLE)
  {
    // ��ʼ����
    Relay_disConnect_Func();

    // ��ת���ϵ����ѹ���
    fsm->main_fsm_state = main_fsm_Poweron_detect_stage;
  }
  else if (fsm->main_fsm_state == main_fsm_Poweron_detect_stage)
  {
    PowerOn_detect_acBusVol_rms_sum += acBusVol_rms;
    PowerOn_detect_cnt++;
    if (PowerOn_detect_cnt > 100)
    {
      PowerOn_detect_acBusVol_rms_ave = PowerOn_detect_acBusVol_rms_sum / (float)PowerOn_detect_cnt;
      // �ָ���ʼ
      PowerOn_detect_acBusVol_rms_sum = 0;
      PowerOn_detect_cnt = 0;
      if (PowerOn_detect_acBusVol_rms_ave > MAIN_FSM_POWERON_NoVol_Value)
      {
        // 100��ƽ������Ϊ����0.1Vrms��ĸ���ϵ磬������ת
        // ��Ϊmachine1ʱ����ת����������ģʽ
        // ��Ϊmachine2ʱ��Ҳ��ת����������ģʽ
        fsm->main_fsm_state = main_fsm_Cur_Cons;
      }
      else
      {
        // 100��ƽ������ΪС����0.1Vrms��ĸ��δ�ϵ磬������ת
        // ��Ϊmachine1ʱ����ת����ѹģʽ
#if INV_MACHINE_NUM == 1
        fsm->main_fsm_state = main_fsm_Vol_Cons;
#endif
        // ��Ϊmachine2ʱ������ԭ״̬
#if INV_MACHINE_NUM == 2
        fsm->main_fsm_state = main_fsm_Poweron_detect_stage;
#endif
      }
    }
  }
#if INV_MACHINE_NUM == 1
  else if (fsm->main_fsm_state == main_fsm_Vol_Cons)
  {
        // ����״̬:��ѹ
    pfsm_UI->fsm_UI_mode = fsm_UI_mode_Vol;

    // ��ѹ����
    // �л����ڲ�sineԴ
    ploopCtrl->ref_sine_src = ref_sine_src_enum_Internal_sine;
    // ���ú�ѹ����

    ploopCtrl->V_rms_aim = MAIN_FSM_ConstVol_VALUE;

    // ѡ��ջ�������
    ploopCtrl->loop_switch_enum = V_loop;
    // ��·
    Relay_Connect_Func();


  }
#endif

  else if (fsm->main_fsm_state == main_fsm_Cur_Cons)
  {
      // ����״̬:��liu
      pfsm_UI->fsm_UI_mode = fsm_UI_mode_Cur;
    // �������񣺲���
    // ������ɺ����if�ڲ�
    if (!SPGI_fsm(fsm, acBusVol_rms, o_Vol_rms, acBusVol, o_Vol, ploopCtrl, Relay_Connect_Func, Relay_disConnect_Func))
    {

      // ���ú�������
      if (pfsm_UI->Con_Cur_info.en_flag == 1)
      {
        // machine1: I1=k*Io/(1+k)
#if INV_MACHINE_NUM == 1
        ploopCtrl->I_rms_aim = pfsm_UI->Con_Cur_info.Io * pfsm_UI->Con_Cur_info.k / (1 + pfsm_UI->Con_Cur_info.k);
#endif
        // machine2: I2=Io/(1+k)
#if INV_MACHINE_NUM == 2
        ploopCtrl->I_rms_aim = pfsm_UI->Con_Cur_info.Io / (1 + pfsm_UI->Con_Cur_info.k);
#endif
      }
    }
  }
}
void Relay_Connect_Func()
{
  HAL_GPIO_WritePin(relay_GPIO_Port, relay_Pin, 1);
}
void Relay_disConnect_Func()
{

#if __DEBUG == _DEBUG_DUTY_SENSOR_ || __DEBUG == _DEBUG_ConstVol_ || __DEBUG == _DEBUG_ConstCur_
  HAL_GPIO_WritePin(relay_GPIO_Port, relay_Pin, 1);
#else
  HAL_GPIO_WritePin(relay_GPIO_Port, relay_Pin, 0);
#endif
}

/**
 * @brief ҵ���߼�����  part2
 *
 * �������Ҫ��
 */

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_DAC1_Init();
  MX_HRTIM1_Init();
  MX_TIM6_Init();
  MX_USART1_UART_Init();
  MX_ADC4_Init();
  MX_I2C1_Init();
  /* USER CODE BEGIN 2 */
Soft_Reset:
  HAL_Delay(2000);
  Relay_disConnect_Func();

#ifdef __DEBUG
  UART_printf(&huart1, MACHINE_Info);
#endif

  HAL_ADCEx_Calibration_Start(&hadc1, ADC_DIFFERENTIAL_ENDED);
  HAL_ADCEx_Calibration_Start(&hadc4, ADC_DIFFERENTIAL_ENDED);

  HAL_Delay(100);

#if __DEBUG == _DEBUG_ConstVol_
  Loop_Controller.loop_switch_enum = V_loop;
  Loop_Controller.V_rms_aim = 15;
  Loop_Controller.ref_sine_src = ref_sine_src_enum_Internal_sine;
#endif
#if __DEBUG == _DEBUG_ConstCur_
  Loop_Controller.loop_switch_enum = I_loop;
  Loop_Controller.I_rms_aim = 0.5;
  Loop_Controller.ref_sine_src = ref_sine_src_enum_Internal_sine;
#endif
#if __DEBUG == _DEBUG_SPGI_
  Loop_Controller.loop_switch_enum = V_loop;
  Loop_Controller.V_rms_aim = 0;
  Loop_Controller.ref_sine_src = ref_sine_src_enum_PLL_sine;
#endif

  // PR��������ʼ��
  PR_init(&pr_o_Vol);
  PR_init(&pr_o_Cur);

  // ��ʼ��SPLL
  spll_Start_wrapper();

  // ��ADC

  HAL_ADC_Start_DMA(&hadc1, (uint32_t *)ADC_buffer_1, 1);
  HAL_ADC_Start_DMA(&hadc4, (uint32_t *)ADC_buffer_2, 3);
  HAL_DAC_Start(&hdac1, DAC_CHANNEL_1);
  // HAL_DAC_Start_DMA(&hdac1,DAC_CHANNEL_1,(uint32_t *)DAC_buffer,1,DAC_ALIGN_12B_R);
  // HAL_DAC_Start(&hdac1,DAC_CHANNEL_1);

  MY_HRTIM_SetDuty_MAB(0.7, 0.1, 0.9);

  HAL_HRTIM_WaveformCounterStart(&hhrtim1, HRTIM_TIMERID_MASTER);
  HAL_HRTIM_WaveformOutputStart(&hhrtim1, HRTIM_OUTPUT_TA1 | HRTIM_OUTPUT_TA2); // ͨ����
  HAL_HRTIM_WaveformOutputStart(&hhrtim1, HRTIM_OUTPUT_TB1 | HRTIM_OUTPUT_TB2); // ͨ����
  HAL_HRTIM_WaveformCounterStart(&hhrtim1, HRTIM_TIMERID_TIMER_A);              // �����Ӷ�ʱ��A
  HAL_HRTIM_WaveformCounterStart(&hhrtim1, HRTIM_TIMERID_TIMER_B);              // �����Ӷ�ʱ��B

  HAL_TIM_Base_Start(&htim6);

  u8g2_Setup_ssd1306_i2c_128x64_noname_1(&OLED_I2C1, U8G2_R0, u8x8_OLED_I2C1_hw, u8x8_OLED_I2C1_gpio_and_delay);
  u8g2_InitDisplay(&OLED_I2C1);
  u8g2_SetPowerSave(&OLED_I2C1, 0);

  HAL_Delay(1000);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  // error();
  while (1)
  {
#ifdef __DEBUG
    // UART_printf(&huart1, MACHINE_Info);
#endif

#if __DEBUG == _DEBUG_DUTY_SENSOR_
    HB_duty = 0.7f;
UART_printf(&huart1, "value:%f,%f,%f,%f,%f\n", HB_duty, o_Vol, o_Vol_rms, o_Cur_rms, o_Cur);
#endif
#if __DEBUG == _DEBUG_ConstVol_
    UART_printf(&huart1, "value:%f,%f,%f,%f,%f\n", HB_duty, o_Vol, o_Vol_rms, o_Cur_rms, o_Cur);
#endif
#if __DEBUG == _DEBUG_ConstCur_
    UART_printf(&huart1, "value:%f,%d,%f,%f,%f\n", HB_duty, error_cnt, o_Cur_rms, k_Cur, o_Vol);
#endif
    // UART_printf(&huart1, "value:%f,%f,%f,%f,%f\n", HB_duty, o_Cur, o_Cur_rms, acBusVol, k_Cur);

#if __DEBUG == _DEBUG_SPGI_
    // ����ʹ��ʾ��
    SPGI_fsm(&fsm, acBusVol_rms, o_Vol_rms, acBusVol, o_Vol, &Loop_Controller, &Relay_Connect_Func, &Relay_disConnect_Func);
#endif
#if __DEBUG == _DEBUG_MAIN_FSM_
    //  ��fsm״̬�����
    main_fsm(&fsm_UI_communication, &fsm, acBusVol_rms, o_Vol_rms, acBusVol, o_Vol, &Loop_Controller, Relay_Connect_Func, Relay_disConnect_Func);
    UART_printf(&huart1, "value:%f,%f,%f,%f,%f,%f,%f\n", HB_duty, o_Vol, o_Vol_rms, o_Cur, o_Cur_rms, acBusVol, o_Power);
#endif
#ifndef __DEBUG

    //  ��fsm״̬�����
    main_fsm(&fsm_UI_communication, &fsm, acBusVol_rms, o_Vol_rms, acBusVol, o_Vol, &Loop_Controller, Relay_Connect_Func, Relay_disConnect_Func);
  
#endif // ! 
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
#ifndef __DEBUG
    for (i_Keyboard = 0; i_Keyboard < Keyboard_X; i_Keyboard++)
    {
      MYHAL_GPIO_SetPin(Keyboard_X_output_ports[i_Keyboard], Keyboard_X_output_pins[i_Keyboard]);
      for (j_Keyboard = 0; j_Keyboard < Keyboard_Y; j_Keyboard++)
      {
        Keyboard_state = Keyboard_state << 1;
        Keyboard_state_temp = MYHAL_GPIO_ReadPin(Keyboard_Y_output_ports[j_Keyboard], Keyboard_Y_output_pins[j_Keyboard]);
        Keyboard_state_4x4[i_Keyboard][j_Keyboard] = (uint8_t)Keyboard_state_temp;
        Keyboard_state |= Keyboard_state_temp;
      }
      MYHAL_GPIO_ResetPin(Keyboard_X_output_ports[i_Keyboard], Keyboard_X_output_pins[i_Keyboard]);
    }

    if (Keyboard_state ^ Keyboard_state_last) //(0,4)(0,5)(0,6)(0,7)(1,4)......(3,6)(3,7)
    {
      if (Keyboard_state & 0x8000) //.
      {
        if (point_num == 0)
        {
          strcat(KB_input_char, ".");
          point_num++;
        }
      }
      if (Keyboard_state & 0x4000) // 0
      {
        strcat(KB_input_char, "0");
      }
      if (Keyboard_state & 0x2000) // �趨/ȷ��
      {
        if (to_set_or_set == 1)
        {
          point_num = 0;
          if ((KB_input_char[0] == 0) || ((KB_input_char[0] == 0) && (KB_input_char[1] == '.')))
          { // �������
          }
          else
          {
            sscanf(KB_input_char, "%f", &KB_input_float);
            if (Parameter_adj_enable == 1) // ����ģʽ
            {
              *Parameter_list_pointer[Parameter_list_use_now] = KB_input_float;
            }
            else if (fsm_UI_communication.fsm_UI_mode == fsm_UI_mode_Cur)
            {
              if (value_set_now == 0) // k
              {
                fsm_UI_communication.Con_Cur_info.k = KB_input_float;
                fsm_UI_communication.Con_Cur_info.en_flag = 1;
              }
              else if (value_set_now == 1) // Io
              {
                fsm_UI_communication.Con_Cur_info.Io = KB_input_float;
                fsm_UI_communication.Con_Cur_info.en_flag = 1;
              }
            }
          }
          to_set_or_set = 0;
        }
        else
        {
          to_set_or_set = 1;
        }
        point_num = 0;
        memset(KB_input_char, 0, 32 * sizeof(char));
      }
      if (Keyboard_state & 0x1000) // ���
      {
        point_num = 0;
        memset(KB_input_char, 0, 32 * sizeof(char));
      }
      if (Keyboard_state & 0x0800) // 1
      {
        strcat(KB_input_char, "1");
      }
      if (Keyboard_state & 0x0400) // 2
      {
        strcat(KB_input_char, "2");
      }
      if (Keyboard_state & 0x0200) // 3
      {
        strcat(KB_input_char, "3");
      }
      if (Keyboard_state & 0x0100) // ��һ������
      {
        if (Parameter_adj_enable == 1)
        {
          if (Parameter_list_use_now >= 1)
            Parameter_list_use_now--;
        }
        else
        {
          if (value_set_now <= 2 - 1)
            value_set_now++;
        }
      }
      if (Keyboard_state & 0x0080) // 4
      {
        strcat(KB_input_char, "4");
      }
      if (Keyboard_state & 0x0040) // 5
      {
        strcat(KB_input_char, "5");
      }
      if (Keyboard_state & 0x0020) // 6
      {
        strcat(KB_input_char, "6");
      }
      if (Keyboard_state & 0x0010) // ��һ������
      {
        if (Parameter_adj_enable == 1)
        {
          if (Parameter_list_use_now <= Parameter_list_num - 1 - 1)
            Parameter_list_use_now++;
        }
        else
        {
          if (value_set_now >= 1)
            value_set_now--;
        }
      }
      if (Keyboard_state & 0x0008) // 7
      {
        strcat(KB_input_char, "7");
      }
      if (Keyboard_state & 0x0004) // 8
      {
        strcat(KB_input_char, "8");
      }
      if (Keyboard_state & 0x0002) // 9
      {
        strcat(KB_input_char, "9");
      }
      if (Keyboard_state & 0x0001) // ����ģʽ
      {
        Parameter_adj_enable = !Parameter_adj_enable;
      }
    }
    Keyboard_state_last = Keyboard_state;

    //    sprintf(OLED_I2C_Str[0], "KB:%4X", Keyboard_state);
    //    sprintf(OLED_I2C_Str[1], "%s", KB_input_char);
    //    sprintf(OLED_I2C_Str[2], "%f", KB_input_float);
    //    sprintf(OLED_I2C_Str[3], "KB:%4X", Keyboard_state);
    //    sprintf(OLED_I2C_Str[4], "%s", KB_input_char);
    //    sprintf(OLED_I2C_Str[5], "%f", KB_input_float);


    if ((Parameter_adj_enable == 0) && (fsm_UI_communication.fsm_UI_mode == fsm_UI_mode_Cur) && (to_set_or_set))
    {
      sprintf(OLED_I2C_Str[0], "%s", KB_input_char);
    }
    else
    {
      if (fsm_UI_communication.fsm_UI_mode == fsm_UI_mode_idle)
      {
        sprintf(OLED_I2C_Str[0], "controler_%d", (INV_MACHINE_NUM));
      }
      else
      {
        sprintf(OLED_I2C_Str[0], "controler_%d_in_%s_mode", (INV_MACHINE_NUM), (fsm_UI_communication.fsm_UI_mode == fsm_UI_mode_Cur) ? ("I") : ("V"));
      }
    }

    if (Parameter_adj_enable == 1) // ����ģʽ
    {
      // Parameter_list_pointer
      // Parameter_list_name
      // Parameter_list_use_now
      sprintf(OLED_I2C_Str[1], "%d", Parameter_list_use_now);
      sprintf(OLED_I2C_Str[2], "%s", Parameter_list_name[(Parameter_list_use_now < Parameter_list_num) ? (Parameter_list_use_now) : (Parameter_list_num - 1)]);
      sprintf(OLED_I2C_Str[3], "%f", *Parameter_list_pointer[(Parameter_list_use_now < Parameter_list_num) ? (Parameter_list_use_now) : (Parameter_list_num - 1)]);
      sprintf(OLED_I2C_Str[4], "%s", KB_input_char);
      sprintf(OLED_I2C_Str[5], "");
    }
    else if (fsm_UI_communication.fsm_UI_mode == fsm_UI_mode_idle) // IDLE����
    {
      sprintf(OLED_I2C_Str[1], "wait...");
      sprintf(OLED_I2C_Str[2], "");
      sprintf(OLED_I2C_Str[3], "");
      sprintf(OLED_I2C_Str[4], "");
      sprintf(OLED_I2C_Str[5], "");
    }
    else if (fsm_UI_communication.fsm_UI_mode == fsm_UI_mode_Cur) // ����ģʽ����
    {
      sprintf(OLED_I2C_Str[1], "K_seted    :%.5f", fsm_UI_communication.Con_Cur_info.k);
      sprintf(OLED_I2C_Str[2], "Io_seted   :%.5fA", fsm_UI_communication.Con_Cur_info.Io);
      sprintf(OLED_I2C_Str[3], "V_RMS      :%.5fV", o_Vol_rms);
      sprintf(OLED_I2C_Str[4], "I_RMS      :%.5fA", o_Cur_rms);
      sprintf(OLED_I2C_Str[5], "acBusV_rms :%.5fA", acBusVol_rms);
    }
#if INV_MACHINE_NUM == 1
    else if (fsm_UI_communication.fsm_UI_mode == fsm_UI_mode_Vol) // ��ѹģʽ����
    {
      sprintf(OLED_I2C_Str[1], "V_RMS :%.5fV", o_Vol_rms);
      sprintf(OLED_I2C_Str[2], "I_RMS :%.5fA", o_Cur_rms);
      sprintf(OLED_I2C_Str[3], "");
      sprintf(OLED_I2C_Str[4], "");
      sprintf(OLED_I2C_Str[5], "");
    }
#endif
    else
    {
      sprintf(OLED_I2C_Str[1], "%s", "fsm_UI_mode_ERROR");
      sprintf(OLED_I2C_Str[2], "");
      sprintf(OLED_I2C_Str[3], "");
      sprintf(OLED_I2C_Str[4], "");
      sprintf(OLED_I2C_Str[5], "");
    }

    /*
    sprintf(OLED_I2C_Str[1], "%s", KB_input_char);
    sprintf(OLED_I2C_Str[2], "%f", KB_input_float);
    sprintf(OLED_I2C_Str[3], "KB:%4X", Keyboard_state);
    sprintf(OLED_I2C_Str[4], "%s", KB_input_char);
    sprintf(OLED_I2C_Str[5], "%f", KB_input_float);
*/
    u8g2_FirstPage(&OLED_I2C1);
    do
    {
      u8g2_SetFont(&OLED_I2C1, u8g2_font_courR08_tf);
      u8g2_DrawStr(&OLED_I2C1, 2, 12, OLED_I2C_Str[0]);
      //			u8g2_DrawLine(&OLED_I2C1,0,13,127,13);
      u8g2_DrawStr(&OLED_I2C1, 2, 22, OLED_I2C_Str[1]);
      //			u8g2_DrawLine(&OLED_I2C1,0,23,127,23);
      u8g2_DrawStr(&OLED_I2C1, 2, 32, OLED_I2C_Str[2]);
      //			u8g2_DrawLine(&OLED_I2C1,0,33,127,33);
      u8g2_DrawStr(&OLED_I2C1, 2, 42, OLED_I2C_Str[3]);
      //			u8g2_DrawLine(&OLED_I2C1,0,43,127,43);
      u8g2_DrawStr(&OLED_I2C1, 2, 52, OLED_I2C_Str[4]);
      //			u8g2_DrawLine(&OLED_I2C1,0,53,127,53);
      u8g2_DrawStr(&OLED_I2C1, 2, 62, OLED_I2C_Str[5]);
      u8g2_DrawRFrame(&OLED_I2C1, 0, 0, 128, 64, 0);

      if ((to_set_or_set) && (flash_bit))
      {
        if (Parameter_adj_enable == 1)
        {
          u8g2_DrawRFrame(&OLED_I2C1, 1, 13 + 10 * 3, 126, 11, 0);
        }
        else
        {
          if (fsm_UI_communication.fsm_UI_mode == fsm_UI_mode_Cur)
          {
            u8g2_DrawRFrame(&OLED_I2C1, 1, 13 + 10 * value_set_now, 126, 11, 0);
          }
        }
      }
    } while (u8g2_NextPage(&OLED_I2C1));
    flash_bit = !flash_bit;

#endif

    //		MY_HRTIM_SetDuty_MAB(0.3,0.1,0.9);
    //		HAL_Delay(1000);
    //		MY_HRTIM_SetDuty_MAB(0.7,0.1,0.9);
    //		HAL_Delay(1000);
  }
  /* USER CODE END 3 */
}

/**
 * @brief System Clock Configuration
 * @retval None
 */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
   */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
   * in the RCC_OscInitTypeDef structure.
   */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV2;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

// uint16_t ADC_buffer[ADC1_CHANNEL_NUM];
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  float ac_value;

  //	if(hadc->Instance==ADC1)
  //	{
  //
  //	}
  if (hadc->Instance == ADC4)
  {

    /* ���� */
    // ������

#if INV_MACHINE_NUM == 1
    acBusVol = (ADC_buffer_2[1] * 3.3f / 4095.0f - 1.60f) * 30;
    ac_value = (ADC_buffer_1[0] * 3.3f / 4095.0f - 1.60f);
    o_Vol = ac_value * 30;
    // o_Cur = (ADC_buffer_2[0] * 3.3f / 4095.0f - 1.5995f) / 0.25;
    o_Cur = (ADC_buffer_2[0] - 2049.705882) / (-313.2526439) + 0.1f;
#endif
#if INV_MACHINE_NUM == 2
    acBusVol = (ADC_buffer_2[1] * 3.3f / 4095.0f - 1.60f) * 30;
    ac_value = (ADC_buffer_1[0] * 3.3f / 4095.0f - 1.60f);
    o_Vol = ac_value * 30;
    // o_Cur = (ADC_buffer_2[0] * 3.3f / 4095.0f - 1.5995f) / 0.25;
    o_Cur = (ADC_buffer_2[0] - 2049.705882) / (-313.2526439) + 0.05f;
#endif
    


o_Power = o_Cur * o_Vol;
#ifdef USE_Current_Backfilling_PROTECT
    

    // ����������
    if (o_Power < PROTECT_Current_Backfilling_W)
    {
      error();
    }
#endif

    /*ref sine ����*/
    if (Loop_Controller.ref_sine_src == ref_sine_src_enum_Internal_sine)
    {
      // �ڲ�sin��Ϊref
      ref_sine = ((float32_t)arm_sin_f32((Caculate_50k_to_50_index * 2 * PI / 1000.0f)));
    }
    else if (Loop_Controller.ref_sine_src == ref_sine_src_enum_PLL_sine)
    {
      // PLL��Ϊref
      spll_Outputs_wrapper(&acBusVol, &y0, &y1, &sinf_out);
      ref_sine = ((float32_t)arm_sin_f32(spll1.theta + (sin_Phase_det * 2 * PI / 360.0)));
#ifdef __DEBUG
      DAC_buffer[0] = (ref_sine + 1) / 2 * 4095.0f;
#endif
    }

    o_Vol_rms_sum += o_Vol * o_Vol;                      // ��ȡ����ֵ,����ƽ����
    o_Cur_rms_sum += o_Cur * o_Cur;                      // ��ȡ����ֵ,����ƽ����
    acBusVol_rms_sum += acBusVol * acBusVol;             // ��ȡ����ֵ,����ƽ����
    duty_Virtual_rms_sum += duty_Virtual * duty_Virtual; // ��ȡ����ֵ,����ƽ����
    if (Caculate_50k_to_50_index >= 1300)
    {
      Caculate_50k_to_50_index = 0; // 50Hz

      // ������ѹRMS����
      //  Vrms
      o_Vol_rms = (float)sqrtf(o_Vol_rms_sum / 1300); // ƽ����ȡƽ�����ٿ���
      o_Vol_rms_sum = 0;
      // Irms
      o_Cur_rms = (float)sqrtf(o_Cur_rms_sum / 1300); // ƽ����ȡƽ�����ٿ���
      o_Cur_rms_sum = 0;
      // acBusVolrms
      acBusVol_rms = (float)sqrtf(acBusVol_rms_sum / 1300); // ƽ����ȡƽ�����ٿ���
      acBusVol_rms_sum = 0;
      // dutyrms
      duty_Virtual_rms = (float)sqrtf(duty_Virtual_rms_sum / 1300); // ƽ����ȡƽ�����ٿ���
      duty_Virtual_rms_sum = 0;

if(o_Cur_rms>5.0f){
error();
}
#ifdef USE_DUTYrms_ERROR
      if (duty_Virtual_rms > 0.9) // �ο�ֵ0.707105
      {
        error();
      }

#endif // DEBUG

      if (Loop_Controller.loop_switch_enum == V_loop)
      {
        // ��ѹ��
        k_Vol = pid_process_o_Vol(&pid_o_Vol, Loop_Controller.V_rms_aim, o_Vol_rms, 1, 0.0001); // ��ѹ�ջ�
      }
      else if (Loop_Controller.loop_switch_enum == I_loop)
      {
        // ������Чֵ��
        k_Cur = pid_process_o_Cur(&pid_o_Cur, Loop_Controller.I_rms_aim, o_Cur_rms, 10, 0.000001); // ������Чֵ�ջ�
      }
    }
    else
    {
      Caculate_50k_to_50_index++;
    }

    if (Loop_Controller.loop_switch_enum == V_loop)
    {
      // ��ѹ�����
      o_Vol_ref = k_Vol * ref_sine;
      HB_duty = o_Vol_ref * 0.5 + 0.5;
    }
    else if (Loop_Controller.loop_switch_enum == I_loop)
    {
      // ������PR�����
      // k_Cur=0.89f;
      o_Cur_ref = k_Cur * ref_sine;                                             // ��ȡPRԤ��ֵ
      HB_duty = (PR_process_o_Cur(&pr_o_Cur, o_Cur_ref, o_Cur) + 100) / 200.0f; // ����˲ʱֵ�ջ�
      // HB_duty = (  ref_sine+ 1) / 2.2f;//����˲ʱֵ�ջ�
    }

    HB_duty = CLAMP(HB_duty,0.99,0.01);
    duty_Virtual = 2.0f * (HB_duty - 0.5f);
#if __DEBUG == _DEBUG_DUTY_SENSOR_
    //HB_duty = 0.6f;
#endif
    MY_HRTIM_SetDuty_MAB(HB_duty, 0.02, 0.98);
#ifdef __DEBUG
    HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, DAC_buffer[0]);
#endif
    // HAL_DAC_Start_DMA(&hdac1,DAC_CHANNEL_1,(uint32_t *)DAC_buffer,1,DAC_ALIGN_12B_R);
    // HAL_DAC_SetValue(&hdac1,DAC_CHANNEL_1,DAC_ALIGN_12B_R,DAC_buffer[0]);
    // HAL_GPIO_TogglePin(check_GPIO_Port,check_Pin);
  }
}

void MY_HRTIM_SetDuty_MAB(float duty_to_set, float duty_min, float duty_max) // 0<=duty_min<=duty_to_set<=duty_max<=1
{
  float processed_duty;
  uint32_t HRTIM_MAB_period = HRTIM_MAB_PERIOD, HRTIM_AB_duty;

  processed_duty = (duty_to_set >= duty_min) ? ((duty_to_set <= duty_max) ? (duty_to_set) : (duty_max)) : (duty_min);

  HRTIM_AB_duty = (uint32_t)((float)HRTIM_MAB_period * processed_duty);

  hhrtim1.Instance->sMasterRegs.MCMP2R = 48; // ADC_trigger
  hhrtim1.Instance->sMasterRegs.MCMP3R = 48; // TIMERA_trigger
  hhrtim1.Instance->sMasterRegs.MCMP4R = 48; // TIMERB_trigger
  hhrtim1.Instance->sTimerxRegs[0].CMP1xR = (HRTIM_AB_duty);
  hhrtim1.Instance->sTimerxRegs[1].CMP1xR = (HRTIM_AB_duty);
}
/*ԭ����ɾ
hhrtim1.Instance->sMasterRegs.MCMP1R=100;
hhrtim1.Instance->sMasterRegs.MCMP2R=100;
hhrtim1.Instance->sMasterRegs.MCMP4R=100+(54400/4);
hhrtim1.Instance->sTimerxRegs[0].CMP1xR=(54400/2);
hhrtim1.Instance->sTimerxRegs[1].CMP1xR=(54400/2);
*/

// OLED_I2C1
uint8_t u8x8_OLED_I2C1_gpio_and_delay(U8X8_UNUSED u8x8_t *u8x8, U8X8_UNUSED uint8_t msg, U8X8_UNUSED uint8_t arg_int, U8X8_UNUSED void *arg_ptr)
{
  switch (msg)
  {
  case U8X8_MSG_GPIO_AND_DELAY_INIT:
    //    HAL_Delay(1);
    break;
  case U8X8_MSG_DELAY_MILLI:
    //    HAL_Delay(arg_int);
    break;
  }
  return 1;
}

uint8_t u8x8_OLED_I2C1_hw(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
  static uint8_t buffer[32];
  static uint8_t buf_idx;
  uint8_t *data;

  switch (msg)
  {
  case U8X8_MSG_BYTE_SEND:
    data = (uint8_t *)arg_ptr;
    while (arg_int > 0)
    {
      buffer[buf_idx++] = *data;
      data++;
      arg_int--;
    }
    break;
  case U8X8_MSG_BYTE_START_TRANSFER:
    buf_idx = 0;
    break;
  case U8X8_MSG_BYTE_END_TRANSFER:
    HAL_I2C_Master_Transmit(&hi2c1, u8x8_GetI2CAddress(u8x8), buffer, buf_idx, 1000);
    break;
  default:
    return 0;
  }
  return 1;
}
/*
 *�������ܣ�STM32��λ����
 */
void Stm32_SoftReset(void)
{
  __set_FAULTMASK(1); // ��ֹ���еĿ������ж�
  while (1)
  {
  }
  NVIC_SystemReset(); // �����λ
}


void error()
{
  if(error_cnt==250){
  __disable_irq();
  Relay_disConnect_Func();
  Stm32_SoftReset();
  }else{
    error_cnt++;
  }

}
/* USER CODE END 4 */

/**
 * @brief  This function is executed in case of error occurrence.
 * @retval None
 */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param  file: pointer to the source file name
 * @param  line: assert_param error line source number
 * @retval None
 */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
