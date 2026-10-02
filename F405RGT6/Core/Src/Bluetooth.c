#include "main.h"
#include "usart.h"
#include "oled.h"

void Send_Next(){
	static const uint8_t Next[3] = {0x55, 0x44, 0xAA};
	USART3_SendDMA(Next, 3);
}

void Send_USART_TIMEOUT(){
	static const uint8_t Next[3] = {0x55, BT_INIT_TIMEOUT, 0xAA};
	USART3_SendDMA(Next, 3);
}

int8_t BT_Init(){
  RX_write_cusor = 0;
  
  for(int i = 0; i < 8; i++){
	  
	  RX_number = 0;
	  USART3_RxStart();
      USART3_SendDMA(ctrl_req, 3);
      req_sent_ms = HAL_GetTick();
	  
      while(RX_number != USART3_CMD_BUF_SIZE){
		  
		RX_number = USART3_RxPoll();
		  
		if(i == 0 && (HAL_GetTick() - req_sent_ms) > REPLY_TIMEOUT_MS * 10){
			USART3_StopReceiveDMA(); 
            char program_error[] = {"USART TIMEOUT!!!"};
            OLED_ShowString(0, 0, program_error, OLED_FONT_08, 1);
            OLED_Refresh();
			Send_USART_TIMEOUT();
			return BT_INIT_TIMEOUT;
		}
		
        if (i != 0 && (HAL_GetTick() - req_sent_ms) > REPLY_TIMEOUT_MS)
        {
			USART3_StopReceiveDMA(); 
            char program_error[] = {"USART TIMEOUT!!!"};
            OLED_ShowString(0, 0, program_error, OLED_FONT_08, 1);
            OLED_Refresh();
			Send_USART_TIMEOUT();
			return BT_INIT_TIMEOUT;
        }
	  }
	  
	  if(usart3_cmd_buf[0] == 0x55 && usart3_cmd_buf[68] == 0xAA){
		  memcpy(RX_data[RX_write_cusor], usart3_cmd_buf + 2, 64);
		  RX_write_cusor++;
	  
		  if(RX_write_cusor >= 8){
			  RX_write_cusor = 0;
		  }
	  }
	  
  }
  
	char program_error[] = {"BT OK!!!"};
	OLED_ShowString(0, 0, program_error, OLED_FONT_08, 1);
	OLED_Refresh();
	return 1;
}