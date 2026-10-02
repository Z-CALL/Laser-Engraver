#include "main.h"
#include "usart.h"
#include "oled.h"

void Send_Next(){
	static const uint8_t Next[3] = {0x55, TX_Next, 0xAA};
	USART3_SendDMA(Next, 3);
}

void Send_Ctrl(){
	static const uint8_t Ctrl[3] = {0x55, TX_Ctrl, 0xAA};
	USART3_SendDMA(Ctrl, 3);
}

void Send_USART_TIMEOUT(){
	static const uint8_t Next[3] = {0x55, BT_INIT_TIMEOUT, 0xAA};
	USART3_SendDMA(Next, 3);
}

int8_t BT_Send_Ctrl(){
	
	  RX_number = 0;
	  USART3_RxStart();
      Send_Ctrl();
      req_sent_ms = HAL_GetTick();
	
      while(RX_number != USART3_CMD_BUF_SIZE){
		  
		RX_number = USART3_RxPoll();
        if ((HAL_GetTick() - req_sent_ms) > REPLY_TIMEOUT_MS * 10)
        {
			USART3_StopReceiveDMA(); 
            char program_error[] = {"USART TIMEOUT!!!"};
            OLED_ShowString(0, 0, program_error, OLED_FONT_08, 1);
            OLED_Refresh();
			Send_USART_TIMEOUT();
			return BT_INIT_TIMEOUT;
        }
	  }
	  
	  if(usart3_cmd_buf[0] == 0x55 && usart3_cmd_buf[1] == RX_Ctrl && usart3_cmd_buf[68] == 0xAA){
		  
		  if(usart3_cmd_buf[2] == 0x44){
			x.all_point = usart3_cmd_buf[3]<<8|usart3_cmd_buf[4];
		  }
		  if(usart3_cmd_buf[5] == 0x55){
			y.all_point = usart3_cmd_buf[6]<<8|usart3_cmd_buf[7];
		  }
		  if(usart3_cmd_buf[8] == 0x66){
			  //先不做处理
		  }
		return 1;
		  
	  }else{
		  return -1;
	  }
}

int8_t BT_Init(){
  RX_write_cusor = 0;
  
  for(int i = 0; i < 8; i++){
	  
	  RX_number = 0;
	  USART3_RxStart();
      Send_Next();
      req_sent_ms = HAL_GetTick();
	  
      while(RX_number != USART3_CMD_BUF_SIZE){
		  
		RX_number = USART3_RxPoll();
		  
		if((HAL_GetTick() - req_sent_ms) > REPLY_TIMEOUT_MS){
			USART3_StopReceiveDMA(); 
            char program_error[] = {"BT_RX TIMEOUT!!!"};
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