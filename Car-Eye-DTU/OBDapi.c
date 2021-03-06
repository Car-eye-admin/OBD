/*
*
*
*与OBD模块的通讯接口
***************************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rt_misc.h>

#include "eat_modem.h"
#include "eat_interface.h"
#include "definedata.h"
#include "eat_uart.h"
#include "UartTask.h"
#include "AppUser1.h"
#include "app_update.h"
#include "SVRapi.h"
#include "OBDapi.h"
#include "gps.h"
#include "db.h"
#include "BigMem.h"


#define OBD_FRAME_FMT1 0xa5
#define OBD_FRAME_FMT2 0xa5
#define OBD_FRAME_MAX 512
static u8 OBD_SENDTO_MDM_BUF[OBD_FRAME_MAX];
static u8 MDM_SENDTO_OBD_BUF[OBD_FRAME_MAX];
static _OBDDB obddb;
static __OBDDB obddbex;
static u8 APP_UPDATE_FLAG = 0;//升级类型标志
                              /*10 = M2M_APP升级
                                01 = 
                              */
u8 APP_UPDATE_FLAGEX = 0;//obd要求升级的类型
static u8 APP_UPDATE_FORCE = 0;//是否强制升级 0x55 = 需要强制升级
static u16 Lupdate_index = 0;//升级时的序号
static u32 Lupdate_index_time = 0;//升级时更新时间 如果时间超过15秒升级自动断开
static u8  Lupdate_index_time_over = 0;//升级同一数据连续发送次数
/*记录实时车速、发动机转速以及更新时间
*/
static u32 Lengingspeed = 0;
static u32 Lenginespeedmax = 0;//最大发动机转速 600-6000
static u32 Lvehiclespeed = 0;
static u32 Lvehiclespeedmax = 0;//最大车速 10-150
static u32 Lupdatetimespeed = 0;
static u8 obd_msgnum_flag = 0;

static u8 obd_write_enable_flag = 0;
//行程记录
static _ROUTE Lcurroute;

/*通过车速判断急加速急减速
*急加速条件:
*         - 加度>18Km/h(5m/s 百公里加速为5.6s)
*         - 下一次速度必须>先前速度+加速度
*急减速条件
*         - 先前速度>30Km/h(10m/s)
*         - 减速度>18Km/h(5m/s 百公里加速为5.6s)
*         - 下一次速度必须<先前速度-减速度
*/
static _speedcheck speedcheck = {0};
static u8 hw_th_unable = 0;//硬件防盗解除标记 0x55 == 启动防盗 0x00 ==解除防盗
static u32 OBD_DATA_NUM = 0;//记录OBD数据数 只有在硬件防盗使能 且OBD_DATA_NUM <5 时才报警

u32 Lenginespeedmax_get(void){
	return Lenginespeedmax;
}
u32 Lvehiclespeedmax_get(void){
	return Lvehiclespeedmax;
}

/*拖车检测
*/
unsigned char obd_vehiclelost_check(u8 *datain){
	u32 lat0, lat1, lon0, lon1,u32t1,cellid0,cellid1,localid0,localid1;
	double dlat0,dlat1;
	u8 datasend[12];
	
	AT_CENG_CELL(&localid1, cellid1);
	db_gpscell_get(&localid0, &cellid0);
	if(0 == localid1 || 0 == cellid1 || 0 == localid0 || 0 == cellid0)return 0;
	if(localid1 == localid0 || cellid1 == cellid0)return 0;
	
	lat0 = *(datain + 0);
	lat0 = (lat0 << 8) + *(datain + 1);
	lat0 = (lat0 << 8) + *(datain + 2);
	lat0 = (lat0 << 8) + *(datain + 3);
	//lat0 += 20000;//测试拖车报警专门加上该参数用于测试  test  22291212
	lon0 = *(datain + 4);
	lon0 = (lon0 << 8) + *(datain + 5);
	lon0 = (lon0 << 8) + *(datain + 6);
	lon0 = (lon0 << 8) + *(datain + 7);
	datasend[0] = 0x8d;
	datasend[1] = 0x0c;
	datasend[2] = (lat0 >> 24) & 0x00ff;
	datasend[3] = (lat0 >> 16) & 0x00ff;
	datasend[4] = (lat0 >> 8) & 0x00ff;
	datasend[5] = (lat0 >> 0) & 0x00ff;
	datasend[6] = (lon0 >> 24) & 0x00ff;
	datasend[7] = (lon0 >> 16) & 0x00ff;
	datasend[8] = (lon0 >> 8) & 0x00ff;
	datasend[9] = (lon0 >> 0) & 0x00ff;
	if(0 == lat0 || 0 == lon0)return 0;
	db_gps_get(&lat1, &lon1);
	if(0 == lat1 || 0 == lon1)return 0;
	user_debug("i:+++++++[%d,%d,%d,%d]", lat0, lon0, lat1, lon1);
	u32t1 = lat0 / 1000000;
	dlat0 = u32t1 * 60.0 + (lat0 / 1000000.0 - u32t1) * 100.0;
	u32t1 = lat1 / 1000000;
	dlat1 = u32t1 * 60.0 + (lat1 / 1000000.0 - u32t1) * 100.0;
	if(dlat0 > dlat1)dlat0 = dlat0 - dlat1;
	else dlat0 = dlat1 - dlat0;
	if(dlat0 < 60.0 && dlat0 > 0.5){
		SVR_FrameSend(datasend, 10);
		return 1;
	}
	
	u32t1 = lon0 / 1000000;
	dlat0 = u32t1 * 60.0 + (lon0 / 1000000.0 - u32t1) * 100.0;
	u32t1 = lon1 / 1000000;
	dlat1 = u32t1 * 60.0 + (lon1 / 1000000.0 - u32t1) * 100.0;
	if(dlat0 > dlat1)dlat0 = dlat0 - dlat1;
	else dlat0 = dlat1 - dlat0;
	if(dlat0 < 60.0 && dlat0 > 0.5){
		SVR_FrameSend(datasend, 10);
		return 1;
	}
	return 0;
}



void hw_th_unable_set(u8 flag)
{
	if(0 == flag)hw_th_unable = 0x00;
	else if(1 == flag)hw_th_unable = 0x55;
	else if(0x7f == flag && 0x55 == hw_th_unable)hw_th_unable = 0x88;
}

void obd_rout(u8 cmdsub, u8 *data, u32 datalen)
{
	u8 back[32];
	static u8 datalog[4];
	u32 time;

	if(0x02 == cmdsub)
	{//返回时间	
		datalog[0] = 0;
		datalog[1] = 0;
		datalog[2] = 0;
		datalog[3] = 0;
		back[0] = 0x45;
	  	back[1] = 0x02;
	  	time = G_system_time_getEx();
	  	if(time != 0)
		{
	      		back[2] = (time >> 24) & 0x00ff;
	      		back[3] = (time >> 16) & 0x00ff;
	      		back[4] = (time >> 8) & 0x00ff;
	      		back[5] = (time >> 0) & 0x00ff;
	      		obd_write(back, 6);
	      		user_debug("i:obd-rout-time");
	  	}
	  	else user_debug("i:obd-rout-time not OK");
	}
	else if(0x01 == cmdsub)
	{//返回行程
		db_obd_gpsclr();
		if(NULL == data || datalen < 5 || datalen > 120)
		{
			back2OBD_7f(0x14, 0x01);
			user_debug("i:obd-rout error");
			return;
		}
	
		if((datalog[0] == *(data +9)) && (datalog[1] == *(data +10)) && (datalog[2] == *(data +11)) && (datalog[3] == *(data +12)))
		{
			back[0] = 0x45;
	    		back[1] = 0x01;
	    		obd_write(back, 2);
	    		user_debug("i:obd-rout-return was OK");
	    		return;
		}
		if(0 == SVR_rout_tosvr(&data[4], datalen - 5))
		{
			back[0] = 0x45;
	    		back[1] = 0x01;
	    		obd_write(back, 2);
	    		datalog[0] = *(data +9);
	    		datalog[1] = *(data +10);
	    		datalog[2] = *(data +11);
	    		datalog[3] = *(data +12);
	    		user_debug("i:obd-rout-return OK");
		}
		else
		{
			 back2OBD_7f(0x14, 0x01);
			 user_debug("i:obd-rout-return error");
		}
	}
}



/*行程结束 有M2M自动判断 这时数据不会发送到服务器 而是字节保存
*/
void obd_routendex(void)
{
	if(0 == Lcurroute.starttime || 0 == Lcurroute.distance)return;
	SVR_rout_save(Lcurroute.starttime, Lcurroute.distance, Lcurroute.fuel);
	Lcurroute.starttime = 0;
	Lcurroute.distance = 0;
	Lcurroute.fuel = 0;
}

void obd_speedset(u32 engine, u32 vehicle){
	if(engine < 300 && vehicle > 0)return;//异常数据
	if(vehicle > 300)return;//车速超过300认为异常
	if(engine > 10000)return;//发动机转速超过1W转认为异常
	if(engine > 600 && engine > Lenginespeedmax)
	{
		if(engine < 6000)Lenginespeedmax = engine;
	}
	if(vehicle > 10 && vehicle < 150 && vehicle > Lvehiclespeedmax)
	{
		Lvehiclespeedmax = vehicle;
	}
	Lengingspeed = engine;
	Lvehiclespeed = vehicle;
	Lupdatetimespeed = user_time();            
}
/*获取车速
*3S内的车速才有效 否则是无效的
*当车速无效时返回1
*      有效时返回0
*/
u8 obd_vehiclespeedget(u32 *speed){
	u32 curtime;
	
	curtime = user_time();										//lilei-?-2016-10-18-not use-infuction
	if(curtime > Lupdatetimespeed && curtime - Lupdatetimespeed > 3)return 1;
	else if(curtime < Lupdatetimespeed && Lupdatetimespeed - curtime > 3)return 1;
	*speed = Lvehiclespeed;
	return 0;
}
/*应答OBD接口
*
*******************************************************************/
void back2OBD_7f(u8 cmd, u8 status){
	u8 back[3];
	
	back[0] = 0x7f;
	back[1] = cmd;
	back[2] = status;
	obd_write(back, 3);
}
void back2OBD_2Bytes(u8 byte1, u8 byte2){
	u8 back[2];
	
	back[0] = byte1;
	back[1] = byte2;
	obd_write(back, 2);
}
void back2OBD_3Bytes(u8 byte1, u8 byte2, u8 byte3){
	u8 back[3];
	
	back[0] = byte1;
	back[1] = byte2;
	back[2] = byte3;
	obd_write(back, 3);
}
void back2SVR8407(u8 byte1, u8 type){
	u8 back[5];
	
	memset(back, 0, 5);
	back[0] = 0x84;
	back[1] = 0x07;
	back[2] = byte1;
	back[3] = type;
	SVR_FrameSend(back, 4);
}

void back2SVR8403(u8 byte1, u8 filetype, u16 byte2, u16 version, u16 customerid){
	u8 back[10],sendindex;
	
	back[0] = 0x84;
	back[1] = 0x03;
	back[2] = filetype;
	back[3] = byte1;
	back[4] = (byte2 >> 8) & 0x00ff;
	back[5] = byte2 & 0x00ff;
	back[6] = (version >> 8) & 0x00ff;
	back[7] = (version) & 0x00ff;
	back[8] = (customerid >> 8) & 0x00ff;
	back[9] = (customerid) & 0x00ff;
	for(sendindex = 0; sendindex < 5; sendindex ++){
	   if(0 == SVR_FrameSend(back, 10))break;
	   eat_sleep(300);
  }
}
void back2SVR8404(u8 byte3){
	u8 back[3];
	
	back[0] = 0x84;
	back[1] = 0x04;
	back[2] = byte3;
	SVR_FrameSend(back, 3);
}

/***发送升级成功或者失败的状态***/    //add by  lilei-2016-08-29 
void back2SVR8405(u8 status,u8 type,u16 ver,u16 id)
{
	u8 back[12];
	
	back[0] = 0x84;
	back[1] = 0x05;
	back[2] = status;
	back[3] = type;
	
	back[4] = (ver >> 8) & 0x00ff;
	back[5] = (ver) & 0x00ff;
	back[6] = (id >> 8) & 0x00ff;
	back[7] = (id) & 0x00ff;
	SVR_FrameSend(back, 8);
}
/*****************************************************************************/

void obd_init(void)
{
	obddb.dataindex = 0;
	obddb.datalen = 0;
	obddb.msgin = 0;
	obddb.msgout = 0;
	obddb.msgnum = 0;
	
	Lenginespeedmax = 0;
	Lvehiclespeedmax = 0;
	Lupdate_index = 0;
	Lupdate_index_time = 0;
	Lupdate_index_time_over = 0;
	
	obd_msgnum_flag = 0;
	obd_write_enable_flag = 0;
	
	Lcurroute.starttime = 0;
	APP_UPDATE_FORCE = 0;
	
	speedcheck.time = 0;
	APP_UPDATE_FLAGEX = 0;
	APP_UPDATE_FLAG = 0;
}


void obd_Rx_bufclr(void){
	obddb.dataindex = 0;
	obddb.datalen = 0;
	obddb.msgin = 0;
	obddb.msgout = 0;
	obddb.msgnum = 0;
}

/**
*M2M向OBD模块发送数据
*
*data应包含：命令字 + 数据
**************************************************/
u8 obd_write(u8 *data, u16 datalen){
	u8 dataindex,dataindex1;
	u8 cs;
	int overtime;
	
	if(NULL == data || 0 == datalen || datalen > OBD_FRAME_MAX - 6)return 1;
	overtime = 0;
	while(1){
		if(0x00 == obd_write_enable_flag)break;
		eat_sleep(1);
		overtime ++;
		if(overtime >= 5)break;
	}
	obd_write_enable_flag = 0x55;
	dataindex = 0;
	MDM_SENDTO_OBD_BUF[dataindex ++] = OBD_FRAME_FMT1;
	MDM_SENDTO_OBD_BUF[dataindex ++] = OBD_FRAME_FMT2;
	MDM_SENDTO_OBD_BUF[dataindex ++] = 0;
	MDM_SENDTO_OBD_BUF[dataindex ++] = datalen & 0x00ff;
	for(dataindex1 = 0; dataindex1 < datalen; dataindex1 ++){
		MDM_SENDTO_OBD_BUF[dataindex ++] = *(data + dataindex1);
	}
	cs = 0;
	for(dataindex1 = 2; dataindex1 < dataindex; dataindex1 ++)cs += MDM_SENDTO_OBD_BUF[dataindex1];
	MDM_SENDTO_OBD_BUF[dataindex ++] = cs;
	//debug_hex("OBD-BACK:", MDM_SENDTO_OBD_BUF, dataindex);
	eat_uart_write(EAT_UART_1, MDM_SENDTO_OBD_BUF, dataindex);
	
	obd_write_enable_flag = 0x00;
	return dataindex;
}

u16 obd_write_tomessage(u8 *data, u16 datalen){
	u8 dataindex,dataindex1;
	u8 cs;
	
	if(NULL == data || 0 == datalen || datalen > OBD_FRAME_MAX - 6)return 0;
	dataindex = 0;
	MDM_SENDTO_OBD_BUF[dataindex ++] = OBD_FRAME_FMT1;
	MDM_SENDTO_OBD_BUF[dataindex ++] = OBD_FRAME_FMT2;
	MDM_SENDTO_OBD_BUF[dataindex ++] = 0;
	MDM_SENDTO_OBD_BUF[dataindex ++] = datalen & 0x00ff;
	for(dataindex1 = 0; dataindex1 < datalen; dataindex1 ++){
		MDM_SENDTO_OBD_BUF[dataindex ++] = *(data + dataindex1);
	}
	cs = 0;
	for(dataindex1 = 2; dataindex1 < dataindex; dataindex1 ++)cs += MDM_SENDTO_OBD_BUF[dataindex1];
	MDM_SENDTO_OBD_BUF[dataindex ++] = cs;
	
	memcpy((s8 *)data, (s8 *) MDM_SENDTO_OBD_BUF, dataindex);
	return dataindex;
}

/**透传模式
*M2M向OBD模块发送数据
*
*data应包含：头 + 长度 + 命令字 + 数据 +CS
**************************************************/
u8 obd_writeEx(u8 *data, u16 datalen){

	eat_uart_write(EAT_UART_1, data, datalen);
	
	return datalen;
}



/***
*从串口1读取OBD返回的数据
*1、判断数据是否正确
*2、提取相应数据
*该接口只提供给EAT_EVENT_UART_READY_RD事件接受者调用
**************************************************/
//static u8 Lobd_msg_dealing = 0;
u8 obdex_read_sub(u8 cmd1, u8 cmd2, u8 *data, u8 datalen){
	if(NULL == data || 0 == datalen || datalen > 64){
		user_debug("i:obdex_read_sub error[%d]",datalen);
		return 0;
	}
	if(obddbex.msgnumex >= OBD_RX_MSG_MAX){
		user_debug("i:obdex_read_sub msgnumex error[%d]",obddbex.msgnumex);
		return 0;
	}
	obddbex.msgex[obddbex.msginex].len = datalen;
	obddbex.msgex[obddbex.msginex].cmd = cmd1;
	obddbex.msgex[obddbex.msginex].cmd_sub = cmd2;
	obddbex.msgex[obddbex.msginex].time = G_system_time_getEx();//Lstime_get();//当前时间
	memcpy((s8 *)obddbex.msgex[obddbex.msginex].data, (s8 *)data, datalen);
	obddbex.msginex ++;
	if(obddbex.msginex >= OBD_RX_MSG_MAX)obddbex.msginex = 0;
	if(0x55 == obd_msgnum_flag)eat_sleep(5);
	obd_msgnum_flag = 0x55;
	obddbex.msgnumex ++;
	obd_msgnum_flag = 0x00;
	//user_debug("obdex_read_sub:[%d-%d-%d-%02x-%02x]", obddbex.msgnumex,obddbex.msginex, obddbex.msgoutex,  cmd1,cmd2);
	return datalen;
}

/*OBD模块返回数据检测
*0 = 通过
*非0 = 异常
*/
u8 obd_read_check(u8 cmd1, u8 *datatemp, u32 datainlen){
	u8 u8t1;
	u32 len,len1;
	u8 findex,cs;
	u16 datalen;
	
	if(NULL == datatemp || datainlen < 1 || datainlen > 250){
		user_debug("i:obd_read_check formate datalen error");
		return 1;
	}
	if(OBD_FRAME_FMT1 == *(datatemp + 0) && OBD_FRAME_FMT2 == *(datatemp + 1)){
		datalen = *(datatemp + 2);
	 	datalen = (datalen << 8) + *(datatemp + 3);
	 	if((datalen + 5) > datainlen || datalen > 250){//一帧数据最大长度不超过250个字节
	 		user_debug("i:obd_read_check len error[%d-%d]",datalen, datainlen);
	 		back2OBD_7f(datatemp[4], 0x04);
	 		return 2;
	 	}
	 	else len = datalen + 5;//如果接收到的数据大于一帧的数据（出现混帧情况）直接把一帧剩余的数据丢弃
	 	cs = 0;
	 	for(u8t1 = 2; u8t1 < datalen + 4; u8t1 ++)cs += *(datatemp + u8t1);
	 	if(cs != *(datatemp + len -1)){
	 		user_debug("i:obd_read_check cs error[%02x-%02x]",cs, *(datatemp + len -1));
	 		back2OBD_7f(cmd1, 0x02);
	 		return 3;
	 	}
	}
	else{
		user_debug("i:obd_read_check formate title error");
		 return 2;
	}
	
	return 0;
}




u8 obd_produce_deal(u8 cmd1, u8 cmd2, u8 *datatemp, u32 len){
	u16 dataindex;
	u16 u16t1;
	u8 *filebuf;
	char *ver;
	u32 u32t1;
	u8 cmd,cmdsub,u8result,u8t1,flag,index;
	u8 databack[200], databacklen;
	u32 port;
	u8 *imei,*u8ptr;
	
	if(cmd1 != 0x6e)return 1;
data_deal_loop:	
	databacklen = 0;
	switch(cmd1)
	{
		case 0x6e://测试
			      if(0x02 == cmd2)
			     {//2015/10/23 10:03 fangcuisong
			      		memset(databack, 0, 200);
			      		databacklen = 0;
			      		databack[databacklen ++] = 0x6e;
			      		databack[databacklen ++] = 0x02;
			      		databack[databacklen ++] = 1;
			      		obd_write(databack, 1 + 3);
			      }
			      else if(0x67 == cmd2)
				{//设备类型 2G
			      		databacklen = 0;
			      		databack[databacklen ++] = 0x6e;
			      		databack[databacklen ++] = 0x67;
			      		databack[databacklen ++] = 2;
			      		obd_write(databack, databacklen);
			      }
			      else if(0x68 == cmd2)
			      {//版本号
			      		databacklen = 0;
			      		databack[databacklen ++] = 0x6e;
			      		databack[databacklen ++] = 0x68;
			      		databack[databacklen ++] = 1;
			      		obd_write(databack, databacklen);
			      }
			      else if(0x63 == cmd2)
			      {//IMEI
			      		memset(databack, 0, 128);
			      		databack[0] = 0x6e;
			      		databack[1] = 0x63;
			      		imei = (u8 *)m2m_imeiget();
			      		//user_debug("IMEI=%s", imei);
			      		u8result = strlen((char *)imei);
			      		strcat((char *)&databack[2], (char *)imei);
			      		obd_write(databack, u8result + 1 + 2);
			      }
			      else if(0x69 == cmd2)
			      {//获取M2M固件版本
			      		memset(databack, 0, 200);
			      		u8ptr = (u8 *)m2m_verget();
			      		u32t1 = strlen(u8ptr);
			      	
			      		strcpy((char *)&databack[2], (char *)u8ptr);
			      		user_debug("i:VER[%d]:%s",u32t1, &databack[2]);
			      		if(u32t1 > 200)u32t1 = 200;
			      		u8t1 = 0;
			      		if(u32t1 > 2)
				      {
			      	    		for(u16t1 = 2; u16t1 < u32t1+2; u16t1 ++)
						{
			      	    			if('s' == databack[u16t1] && 'i' == databack[u16t1+1] && 'o' == databack[u16t1+2] && 'n' == databack[u16t1+3] &&':' == databack[u16t1+4])
							{
			      	    				u16t1 += 5;
			      	    				u8t1 = 2;
			      	    				for(; u16t1 < u32t1+2; u16t1 ++)
								{
			      	    					if(0x0d == databack[u16t1] || 0x0a == databack[u16t1])break;
			      	    					databack[2+u8t1] = databack[u16t1];
			      	    					u8t1 ++;
			      	    				}
			      	    				break;
			      	    			}
			      	    		}
			        	}
			        	databack[2+u8t1] = 0;
			      		databack[0] = 0x6e;
			      		databack[1] = 0x69;
			      		obd_write(databack, u8t1 + 1 + 2);
			      }
			      else if(0x60 == cmd2)
				{//电压读取
			      		u32t1 = m2m_volget();
			      		databack[0] = 0x6e;
			      		databack[1] = 0x60;
			      		databack[2] = (u32t1 >> 8) & 0x00ff;
			      		databack[3] = (u32t1 >> 0) & 0x00ff;
			      		obd_write(databack, 4);
			      }
			      else if(0x61 == cmd2)
			      {//CSQ
			      		databack[0] = 0x6e;
			      		databack[1] = 0x61;
			      		databack[2] = 15;
			      		obd_write(databack, 3);
			      }
			      else if(0x64 == cmd2)
			      {//SIM卡信息
			      		memset(databack, 0, 128);
			      		databack[0] = 0x6e;
			      		databack[1] = 0x64;
			      		u8ptr = (u8 *)m2m_ccidget();
			      		u8result = strlen((char *)u8ptr);
			      		strcpy((char *)&databack[2], (char *)u8ptr);
			      		obd_write(databack, u8result + 1 + 2);
			      }
			      else if(0x6a == cmd2)
				{//拨号测试
			      		memset(databack, 0, 128);
			      		databack[0] = 0x6e;
			      		databack[1] = 0x6a;
			      		//u8result = AT_ATD(datatemp+6);
			      		databack[2] = 0;//u8result;
			      		obd_write(databack, 3);
			      }
			      else if(0x6b == cmd2)
				{//接听测试
			      		memset(databack, 0, 128);
			      		databack[0] = 0x6e;
			      		databack[1] = 0x6b;
			      		//u8result = AT_ATA();
			      		if(1 == u8result)databack[2] = 0;
			      		else databack[2] = 1;
			      		obd_write(databack, 3);
			      }
			      else if(0x6c == cmd2)
			      {//掉电测试
			      		memset(databack, 0, 128);
			      		databack[0] = 0x6e;
			      		databack[1] = 0x6c;
			      		if(0x55 == Lpowr_lost_flag_get())databack[2] = 0;//2015/10/19 20:40 fcsong000833
			      		else databack[2] = 1;
			      		obd_write(databack, 3);
			      }
			      else if(0x6d == cmd2)
				{//语音测试
			      		memset(databack, 0, 128);
			      		databack[0] = 0x6e;
			      		databack[1] = 0x6d;
			      		AT_CREC("C:\\User\\test.amr", 99);
			      		databack[2] = 0;
			      		obd_write(databack, 3);
			      }
			      else if(0x6f == cmd2)
				{//保存软件版本信息
			      		memset(databack, 0, 128);
			      		databack[0] = 0x6e;
			      		databack[1] = 0x6f;
			      		u8result = db_swver_save(datatemp + 6);
			      		databack[2] = u8result;
			      		obd_write(databack, 3);
			      }
			      else if(0x65 == cmd2)
				{//网络测试
			      		databack[0] = 0x6e;
			      		databack[1] = 0x65;
			      		if(m2m_status() >= 5)databack[2] = 0x00;
			      		else databack[2] = 0x01;
			      		obd_write(databack, 3);
			      }
			      else if(0x66 == cmd2)
				{//创建文件夹
			      		databack[0] = 0x6e;
			      		databack[1] = 0x66;
			      		databack[2] = 0;
			      	
			      		obd_write(databack, 3);
			      }
			      else if(0x70 == cmd2)
				{//出厂设置 2015/10/8 19:45 fangcuisong
			      		flag = 0;
		          		u8t1 = 0;
		          		memset(databack, 0, 128);
		          		for(index = 6; index < dataindex; index ++)
					{
					       if(0 == *(datatemp  +index))
						{
						         index ++;
						         port = 0;
						         if(0 == flag || 1 == flag)
							  {
						             	    port = *(datatemp + index ++);
							           port = (port << 8) + (*(datatemp + index ++));
							           port = (port << 8) + (*(datatemp + index ++));
							           port = (port << 8) + (*(datatemp + index));
							           if(u8t1 > 8)
								    {
									  if(0 == flag)
									 {
									       user_debug("i:IP0=%s,port=%d", databack, port);
									       db_svr_addrset(databack);
									       db_svr_portset(port);
									       if(db_svr_save() != 0)
										{
									           	databack[0] = 0x6e;
			                            				databack[1] = 0x70;
			                            				databack[2] = 0x01;
			                            				obd_write(databack, 3);
			                            				break;
									       }
									       db_svr_init();
									       if(port != db_svr_portget())
										{
									           	databack[0] = 0x6e;
			                            				databack[1] = 0x70;
			                            				databack[2] = 0x02;
			                            				obd_write(databack, 3);
			                            				user_debug("i:IP1=%d,port=%d", db_svr_portget(), port);
			                            				break;
									        }      
									        memset(databack, 0, 128);
									  }
									  else if(1 == flag)
									  {
									        db_svr_addr1set(databack);
									        db_svr_port1set(port);
									        if(db_svr_save() != 0)
										 {
									           	  databack[0] = 0x6e;
			                            				  databack[1] = 0x70;
			                            				  databack[2] = 0x03;
			                            				  obd_write(databack, 3);
			                            				  break;
									         }
									         db_svr_init();
									         if(port != db_svr_port1get())
										  {
									           	 databack[0] = 0x6e;
			                            				 databack[1] = 0x70;
			                            				 databack[2] = 0x04;
			                            				 obd_write(databack, 3);
			                            				 break;
									         }
									         user_debug("i:IP2=%s,port=%d", databack, port);
									         memset(databack, 0, 128);
									}
					                 }
					           }
					           flag ++;
					           if(flag >= 2)break;
						    u8t1 = 0;
				         }
				         else if(0 == flag || 1 == flag)
					  {
						   databack[u8t1 ++]= *(datatemp + index);
					  }
				 }
				 if(index >= dataindex || flag >= 2)
				{
				     databack[0] = 0x6e;
			            databack[1] = 0x70;
			            databack[2] = 0x00;
			            obd_write(databack, 3);
			        }
			   }
			   break;
	  default:
	  	   return 1;
	  	   break;
	}
	return 0;
}


u8 obd_read_sub_ex(u8 cmd1, u8 cmd2, u8 *datatemp, u32 len){
	u32 datalen;
	u8 databack[48], databacklen;
	u8 *imei;
	
	if(0x00 == obd_produce_deal(cmd1, cmd2, datatemp, len))return 0;
	//user_debug("OBD Data[%d,%02x-%02x]", len, cmd1, cmd2);
	//以下指令为生产指令 需要优先处理
	
	if(len >= 64 || CMD_OBDDATA_AUTO == cmd1 || (CMD_FILE_LOAD == cmd1 && cmd2 != 0x07));
	else{
		if(0 == obdex_read_sub(cmd1, cmd2, datatemp, len));
		else return len;
	}
	if(obddb.msgnum >= OBD_RX_MSG_MAX || obddb.datalen + len >= OBD_RX_BUF_MAX){//为了防止数据丢失 数据需要保存到fs中
		//user_debug("obd_read obddb.msgnum=%d",obddb.msgnum);
		return 0;//缓冲区满后数据直接丢弃 该接口不允许对缓冲区进行清除 否则可能导致系统异常 缓冲区的处理需要上层代码处理
	}
	obddb.msg[obddb.msgin].index = obddb.dataindex;
	obddb.msg[obddb.msgin].len = len;
	obddb.msg[obddb.msgin].cmd = cmd1;
	obddb.msg[obddb.msgin].cmd_sub = cmd2;
	obddb.msg[obddb.msgin].time = G_system_time_getEx();//当前时间
	for(datalen = 0; datalen < len; datalen ++){
		obddb.data[obddb.dataindex] = datatemp[datalen];
		obddb.dataindex ++;
		if(obddb.dataindex >= OBD_RX_BUF_MAX)obddb.dataindex = 0;
	}
	obddb.msgin ++;
	if(obddb.msgin >= OBD_RX_MSG_MAX)obddb.msgin = 0;
	if(0x55 == obd_msgnum_flag)eat_sleep(5);
	obd_msgnum_flag = 0x55;
	obddb.msgnum ++;
	obd_msgnum_flag = 0x00;
	return len;
}

void obd_imei2pc(void){
	u8 databack[48],u8result;
	u8 *imei;
	
	memset(databack, 0, 48);
  databack[0] = 0x6e;
	databack[1] = 0x63;
	imei = (u8 *)m2m_imeiget();
	
	u8result = strlen((char *)imei);
	if(u8result && u8result < 40)strcat((char *)&databack[2], (char *)imei);
	obd_write(databack, u8result + 1 + 2);
}

u8 obd_read_sub(u8 *datain, u32 datainlen){
	u32 u32t1, u32t2;
	u8 cmd1,cmd2;
	
	u32t2 = 0;
	cmd1 = 0;
	cmd2 = 0;
	if(0x88 == *(datain + 0) && 0x88 == *(datain + 1) && 0x88 == *(datain + 2) && 0x88 == *(datain + 3))user_debug_enable();
	if(0x11 == *(datain + 0) && 0x11 == *(datain + 1) && 0x11 == *(datain + 2) && 0x11 == *(datain + 3)){//返回IMEI
		obd_imei2pc();
	}
	if (datainlen > 6){
		for(u32t1 = 0; u32t1 < datainlen; u32t1 ++){
			if(0xa5 == *(datain + u32t1) && 0xa5 == *(datain + u32t1 + 1)){
				if(cmd1 != 0 && cmd2 != 0 && u32t1 > u32t2){
					if(0 == obd_read_check(cmd1, datain  + u32t2, u32t1 - u32t2)){
					    if(0x14 == cmd1 && 0x03 == cmd2){
					    	gps_data_read(datain  + u32t2, u32t1 - u32t2);
					    	eat_send_msg_to_user(EAT_USER_0, EAT_USER_4, EAT_FALSE, 4, "GPS", EAT_NULL);
					    }
					    else{
					    	//user_debug("obd_read_sub_ex00:%02x,%02x",cmd1,cmd2);
					    	obd_read_sub_ex(cmd1, cmd2, datain  + u32t2, u32t1 - u32t2);
					    }
					 }
					 //else user_debug("obd_read_sub_ex00 error:%02x,%02x",cmd1,cmd2);
				}
				cmd1 = *(datain + u32t1 + 4);
				cmd2 = *(datain + u32t1 + 5);
				u32t2 = u32t1;
			}
		}
		if(cmd1 != 0 && cmd2 != 0 && u32t1 > u32t2){
			if(0 == obd_read_check(cmd1 ,datain  + u32t2, u32t1 - u32t2)){
			    if(0x14 == cmd1 && 0x03 == cmd2){
			    	gps_data_read(datain  + u32t2, u32t1 - u32t2);
			    	eat_send_msg_to_user(EAT_USER_0, EAT_USER_4, EAT_FALSE, 4, "GPS", EAT_NULL);
			    	//发送消息到GPS 线程 
		      }
			    else{
			    	//user_debug("obd_read_sub_ex:%02x,%02x",cmd1,cmd2);
			    	obd_read_sub_ex(cmd1, cmd2, datain + u32t2, u32t1 - u32t2);
			    }
			 }
			 //else user_debug("obd_read_sub_ex error:%02x,%02x",cmd1,cmd2);
		}
	}
	
	return 0;
}
/***
*从串口1读取OBD返回的数据
*1、判断数据是否正确
*2、提取相应数据
*该接口只提供给EAT_EVENT_UART_READY_RD事件接受者调用
**************************************************/
u8 obd_read(void){
	u8 datatemp[OBD_FRAME_MAX],u8t1;
	u32 len,len1;
	EatUart_enum uart = EAT_UART_1;
	
	len1 = 0;
		while(1){
			len = eat_uart_read(uart, datatemp, OBD_FRAME_MAX);
			obd_read_sub(datatemp, len);
			len1 += len;
			if(len1 > 1024)break;
			if(len < OBD_FRAME_MAX)break;//如果len == OBD_FRAME_MAX说明缓冲区满 需要把所有数据读出 数据直接扔了
		}
		return len1;
}

static unsigned short Lcustomer_id = 0;
static unsigned short Lupdate_version = 0;
/*升级保持
*该接口在以下情况下使用：
*服务器通过84 03发送升级数据，但数据中发送过程中丢失，设备没有接收到
*服务器也不可能接收到发送下一帧的请求，这时需要有该接口来发送请求指令
*该接口由appUser1线程在每隔2S发送1次 20秒后退出
**************************************/
u8 obd_update_keep(void){
	u32 time;
	
	if(Lupdate_index && APP_UPDATE_FLAG)
	{
		
		time = user_time();
		//user_debug("time=%lu,Lupdate_index_time=%lu\r\n",time,Lupdate_index_time);
		if(time > Lupdate_index_time + 5)               //add by  lilei-2016-0827  由之前的2000�S改成3S 
		{
			if(Lupdate_index_time_over >= 6)
			{//升级自动关闭
				Lupdate_index = 0;
				Lupdate_index_time_over = 0;
				APP_UPDATE_FLAGEX = 0;
				APP_UPDATE_FLAG = 0;
				user_debug("update time delay over \r\n");
				back2OBD_2Bytes(0xc4, 0x00);//结束升级
				bigmem_free();//需要吧内存释放掉
				return 0;
			}
			Lupdate_index_time_over ++;
			user_debug("lilei-84 03 send again,Lupdate_index=%d\r\n",Lupdate_index);
			back2SVR8403(0x00,APP_UPDATE_FLAG, Lupdate_index, Lupdate_version, Lcustomer_id);         
			//user_debug("obd_update_keep[%d,%d]", APP_UPDATE_FLAG, Lupdate_index);
			Lupdate_index_time = time;
		}
	}
	return 0;
}
/*
*升级处理
*M2M接收到的所有升级数据通过该接口来进行处理
*data=a5 a5 L1 L2 CMD1 CMD2 D1 D2 ... CS完整一帧数据
**************************************************/
u8 obd_cmd84(u8 cmd_sub, u8 *data, u16 datalen)
{
	u8 u8t1;
	u32 len,cs32;
	u8 cs;
	u8 updateflag;
	u16 ver;
	u32 filelen;
	u8 filename[32];
	
	user_debug("i:obd_cmd84:[%d]", datalen);
	if(datalen < 7)return 1;
	app_sleep_flag_set(0);//升级时防止M2M进入睡眠
	if(0x07 == cmd_sub)
	{
		Lupdate_index = 0;
		APP_UPDATE_FLAG = *(data + 6);
		if(0 == APP_UPDATE_FLAGEX)APP_UPDATE_FLAGEX = APP_UPDATE_FLAG;//服务器强制终端升级
		if(APP_UPDATE_FLAGEX != APP_UPDATE_FLAG)
		{
			  back2SVR8407(0x7f, APP_UPDATE_FLAG);
			  APP_UPDATE_FLAG = 0;
			  APP_UPDATE_FLAGEX = 0;
			  user_debug("i:obd_cmd84:file type not match\r\n");
			  back2OBD_2Bytes(0xc4, 0x00);//结束升级
			  return 0;
		}
		updateflag = *(data + 7);
		ver = *(data + 8);
		ver = (ver << 8) + *(data + 9);
		Lupdate_version  = ver;
		filelen = *(data + 10);
		filelen = (filelen << 8) + *(data + 11);
		filelen = (filelen << 8) + *(data + 12);
		filelen = (filelen << 8) + *(data + 13);
		if(0 == filelen)
		{
			user_debug("i:obd_cmd84:len is 0\r\n");
			back2SVR8407(0x7f, APP_UPDATE_FLAG);
			APP_UPDATE_FLAG = 0;
			APP_UPDATE_FLAGEX = 0;
	    		back2OBD_2Bytes(0xc4, 0x00);//结束升级
			return 0;
		}
		//增加客户ID 
		if(datalen >= 16)
		{
		    Lcustomer_id = *(data + 14);
		    Lcustomer_id = (Lcustomer_id << 8) + *(data + 15);
	      }
		if(0 == updateflag && APP_UPDATE_FORCE != 0x55)
		{//非要求强制升级
			if(1 == db_update_vercheck(ver, APP_UPDATE_FLAG))
			{
				back2SVR8407(0x7f, APP_UPDATE_FLAG);
				APP_UPDATE_FLAG = 0;
				APP_UPDATE_FLAGEX = 0;
				user_debug("i:obd_cmd84:ver not new[%d]", ver);
				back2OBD_2Bytes(0xc4, 0x00);//结束升级
				return 0;
			}
		}
		else
		{
			 user_debug("i:update must be");
		   	 APP_UPDATE_FORCE = 0x00;
		}
		
		if(UPDATE_M2M_APP == APP_UPDATE_FLAG || UPDATE_M2M_LICENSE == APP_UPDATE_FLAG)
		{//升级APP端数据 无条件进入升级
			update_init();
			sprintf(filename, "%04x", ver);
			if(1 == update_start(filelen, APP_UPDATE_FLAG, filename))
			{
				user_debug("i:obd_cmd84,file[%d-%d],saveerror", APP_UPDATE_FLAG,filelen);
				back2SVR8407(0x7f, APP_UPDATE_FLAG);
				back2OBD_2Bytes(0xc4, 0x00);//结束升级
				APP_UPDATE_FLAG = 0;
				APP_UPDATE_FLAGEX = 0;
			}
			else
			{
				 user_debug("i:update-start-0[%d-%d]", APP_UPDATE_FLAG, Lupdate_index);
				 back2SVR8403(0x00, APP_UPDATE_FLAG, Lupdate_index, Lupdate_version, Lcustomer_id);
			}
			back2OBD_2Bytes(0xc4, 0x07);//2014/11/17 18:40 Fangcuisong 原来的0xc8-->0xc4
			//需要开始升级

			return 0;
		}
		else
		{	//升级OBD端数据 根据当前条件决定是否升级
			//先不判断条件
		   	if(1)
		   	{
		   	    	update_init();
			    	sprintf(filename, "%04x", ver);
			    	if(1 == update_start(filelen, APP_UPDATE_FLAG, filename))
			   	{
			    	    	user_debug("i:obd_cmd84-,file[%d-%d],saveerror", APP_UPDATE_FLAG,filelen);
				    	back2OBD_2Bytes(0xc4, 0x00);//结束升级
				    	APP_UPDATE_FLAG = 0;
				    	APP_UPDATE_FLAGEX = 0;
			    	}
			    	else back2SVR8403(0x00, APP_UPDATE_FLAG, Lupdate_index, Lupdate_version, Lcustomer_id);//back2OBD8407(0x00, APP_UPDATE_FLAG);
			    	back2OBD_2Bytes(0xc4, 0x07);//2014/11/17 18:40 Fangcuisong 原来的0xc8-->0xc4
			    	return 0;
		   	}
	  	}
	}
	else if(0x03 == cmd_sub)
	{
		    if(0 == APP_UPDATE_FLAG)return 1;
		    if(APP_UPDATE_FLAGEX != APP_UPDATE_FLAG)
		    {
				APP_UPDATE_FLAG = 0;
			      APP_UPDATE_FLAGEX = 0;
			      user_debug("i:obd_cmd84:ver not new[%d]", ver);
			      back2OBD_2Bytes(0xc4, 0x00);//结束升级
			      return 1;
		    }
		    cs = 0;
		    back2OBD_2Bytes(0xc4, 0x03);
		    Lupdate_index = *(data + 6);
		    Lupdate_index = (Lupdate_index << 8) + *(data + 7);
		    Lupdate_index_time = user_time();
		    Lupdate_index_time_over = 0;
		    for(len = 2; len < datalen - 1; len ++)cs += *(data + len);
		    if(cs != *(data + datalen - 1))
		    {//校验错误
			  	  user_debug("i:update cs error ???[%d-%02x-%02x]",datalen, cs, *(data + datalen - 1));
			  	  back2SVR8403(0x00,APP_UPDATE_FLAG,  Lupdate_index, Lupdate_version, Lcustomer_id);
		    		 return 1;//校验错误 重新发送
		     }
		    u8t1 = update_datainEx(data + 10, datalen - 11,Lupdate_index);
		    if(0x02 == u8t1)
		    {
		    	  user_debug("i:update datain error ???[%d-%d-%d]",u8t1, datalen, Lupdate_index);
		    	  back2SVR8403(0x00, APP_UPDATE_FLAG, Lupdate_index, Lupdate_version, Lcustomer_id);
		    	  return u8t1;//接收错误
		    }
		    else if(0x01 == u8t1 || 0x03 == u8t1)
		    {//升级失败
		    	  APP_UPDATE_FLAG = 0;
		    	  APP_UPDATE_FLAGEX = 0;
		    	  user_debug("i:update datain error1 ???[%d-%d-%d]",u8t1, datalen, Lupdate_index);
		    	  back2SVR8403(0x7f,APP_UPDATE_FLAG, Lupdate_index, Lupdate_version, Lcustomer_id);
		    	  back2OBD_2Bytes(0xc4, 0x00);//结束升级
		    		return u8t1;//接收错误
		    }
		    //user_debug("8403[%02x,%d-%d]",APP_UPDATE_FLAG,Lupdate_index,datalen);
		    Lupdate_index ++;
		
		   
		    user_debug("send Lupdate_index=%d\r\n",Lupdate_index);    //add by lilei-2016-0827
		    back2SVR8403(0x00, APP_UPDATE_FLAG, Lupdate_index, Lupdate_version, Lcustomer_id);
		   
	}
	else if(0x04 == cmd_sub)
	{
		if(0 == APP_UPDATE_FLAG)return 1;
		if(APP_UPDATE_FLAGEX != APP_UPDATE_FLAG)
		{
				 APP_UPDATE_FLAG = 0;
				 APP_UPDATE_FLAGEX = 0;
				 user_debug("i:obd_cmd84:ver not new[%d]", ver);
				 back2OBD_2Bytes(0xc4, 0x00);//结束升级
				 return 1;
		}
		Lupdate_index = 0;
		user_infor("e:APP_UPDATE-CS:%02x", *(data + 6));
		cs32 = *(data + 6);
		cs32 = (cs32 << 8) + *(data + 7);
		cs32 = (cs32 << 8) + *(data + 8);
		cs32 = (cs32 << 8) + *(data + 9);
		if(0 == update_cs(cs32))
		{
			back2SVR8404(0x00);
			back2OBD_2Bytes(0xc4, 0x04);
			if(UPDATE_M2M_APP == APP_UPDATE_FLAG || UPDATE_M2M_LICENSE == APP_UPDATE_FLAG)
			{
			      back2SVR8405(0,APP_UPDATE_FLAG,Lupdate_version,Lcustomer_id);       //add by lilei-2016-0829新增升级成功与失败的状态
				if(update_do() != 0)
				{
					user_debug("i:M2M update error");
					back2OBD_2Bytes(0xc4, 0x00);
					back2SVR8405(1,APP_UPDATE_FLAG,Lupdate_version,Lcustomer_id);      //add by lilei-2016-0829新增升级成功与失败的状态
				}
			
			}
			else
			{//数据下载到OBD
			    if(UPDATE_OBD_LICENSE == APP_UPDATE_FLAG)
			    {//OBD License
			    		APP_UPDATE_FLAG = 0;
			    		APP_UPDATE_FLAGEX = 0;
			    		if(0 == update_obdlicense())
					{
			    			user_infor("e:OBD_License update ok");
			    			return 0;
			    		}
			    		else
					{
			    	  		user_debug("i:OBD_License update error");
			    	  		return 1;
			      		}
          			}
          			if(UPDATE_OBD_APP == APP_UPDATE_FLAG)
				{
          				
          				if(0 == update_obdapp())
					{
          					user_infor("e:OBD_APP update ok");
						user_debug("APP_UPDATE_FLAG=%02X,Lupdate_version=%04X,Lcustomer_id=%04X",APP_UPDATE_FLAG,Lupdate_version,Lcustomer_id);
						back2SVR8405(0,APP_UPDATE_FLAG,Lupdate_version,Lcustomer_id);     //add by lilei-2016-0829新增升级成功与失败的状态
						APP_UPDATE_FLAG = 0;
          					APP_UPDATE_FLAGEX = 0;
			    			return 0;
          				}
          				else
					{
					       user_debug("APP_UPDATE_FLAG=%02X,Lupdate_version=%04X,Lcustomer_id=%04X",APP_UPDATE_FLAG,Lupdate_version,Lcustomer_id);
					       back2SVR8405(1,APP_UPDATE_FLAG,Lupdate_version,Lcustomer_id);    //add by lilei-2016-0829新增升级成功与失败的状态
			    	  		user_debug("i:OBD_APP update error");
					      APP_UPDATE_FLAG = 0;
          					APP_UPDATE_FLAGEX = 0;
			    	  		return 1;
			      		}
			    }
			    if(UPDATE_MUSIC == APP_UPDATE_FLAG)
			    {
			    		APP_UPDATE_FLAG = 0;
			    		APP_UPDATE_FLAGEX = 0;
			    		if(0 == update_obdmusic())
					{
			    			user_infor("e:OBD_MUSIC update ok");
			    			return 0;
			    		}
			    		else
					{
			    	  		user_debug("i:OBD_MUSIC update error");
			    			return 1;
			      		}	
			    }
			    
		  }
		  APP_UPDATE_FLAG = 0;
		  APP_UPDATE_FLAGEX = 0;
	   }
	   else    //效验证失败     						
	   {

			back2SVR8404(0x02);     					 			
			back2OBD_2Bytes(0xc4, 0x00);				  
			APP_UPDATE_FLAG = 0;					
		  	APP_UPDATE_FLAGEX = 0;					

	   }
		
    }
	return 0;
}


/*数据下载 直接通过PC软件将数据下载到M2M中
*a5 a5 L1 L2 85 07 filesize0 filesize1 filesize2 filesize3 filename
*/
u8 obd_cmd85(u8 cmd_sub, u8 *data, u16 datalen)
{ 
	u8 filetype,*filebuf;
	u32 filesize,bufsize;
	u8 filename[128],u8t1;
	u8 back[16];
	u16 frameindex,framenum,u16t1;
	u32 overtime, errnum ,cs1,cs2;
	u8 cmd1, cmd2, *obddata,u8result;
	u16 obddatalen,obddatalenex;
	u32 app_space_value,APP_DATA_STORAGE_BASE,APP_DATA_RUN_BASE;
	
	user_debug("i:obd_cmd85 ...");
	back[0] = 0xc5;
	back[1] = 0x07;
	if(NULL == data || datalen < 12)return 1;
	if(cmd_sub != 0x07)return 2;
	filesize = *(data + 6);
	filesize = (filesize << 8) + *(data + 7);
	filesize = (filesize << 8) + *(data + 8);
	filesize = (filesize << 8) + *(data + 9);
	if(filesize >= BIG_MEM_MAX)
	{//数据不允许超过150K
		back[2] = 0x01;
		obd_write(back, 3);
		user_debug("i:obd_cmd85 filesize[%d] too big", filesize);
		return 2;
	}
	memset((s8 *)filename, 0, 128);
	for(u8t1 = 0;  u8t1 < 128 && (10 + u8t1 < datalen); u8t1 ++)
	{
		if(0 == *(data + 10 + u8t1))break;
		filename[u8t1] = *(data + 10 + u8t1);
	}
	if(u8t1 >= 128)
	{
		back[2] = 0x01;
		obd_write(back, 3);
		user_debug("i:obd_cmd85 filename[%] too long", u8t1);
		return 2;
	}
	filebuf = NULL;
	bufsize = 0;
	filebuf = bigmem_get(2);//获取缓冲区
	if(NULL == filebuf)
	{
		back[2] = 0x01;
		obd_write(back, 3);
		user_debug("i:obd_cmd85 bitmem error");
		return 2;
	}
	//获取数据
	frameindex = 0;
	framenum = filesize / 50;//默认采用每帧50字节方式传输
	if(0 == (filesize % 50));
	else framenum ++;
	overtime = 0;
	errnum = 0;
	for(frameindex = 0; frameindex < framenum; ){
_LOOP:
		back[1] = 0x03;
		back[2] = (frameindex >> 8) & 0x00ff;
		back[3] = (frameindex >> 0) & 0x00ff;
		obd_write(back, 4);
		overtime = 0;
		while(1){
			cmd1 = 0;
  	  if(0x00 == obd_data_read_ex(&cmd1, &cmd2, &obddata, &obddatalen)){
  	  	if(CMD_FILE_LOAD == cmd1 && 0x03 == cmd2){
  	  		obddatalenex = *(obddata + 2);
  	  		obddatalenex = (obddatalenex << 8) + *(obddata + 3);
  	  		if(obddatalenex <= 4 || obddatalenex > 54)continue;
  	  		u16t1 = *(obddata + 6);
  	  		u16t1 = (u16t1 << 8) + *(obddata + 7);
  	  		if(u16t1 == frameindex){
  	  			memcpy((s8 *)(filebuf + bufsize),  (s8 *)(obddata + 8),  obddatalenex - 4);
  	  			bufsize += (obddatalenex - 4);
  	  			errnum = 0;
  	  			frameindex ++;
  	  			break;
  	  		}
  	  		else user_debug("i:frameindex error[%d,%d]", u16t1, frameindex);
  	  	}
  	  }
  	  eat_sleep(5);
  	  overtime ++;
  	  if(overtime >= 100){
  	  	errnum ++;
  	  	if(errnum >= 3){
  	  		back[0] = 0x7f;
  	  		back[1] = CMD_FILE_LOAD;
  	  		back[2] = 2;
  	  		obd_write(back, 3);
  	  		bigmem_free();
  	  		user_debug("i:obd_cmd85 overtime[%d]", frameindex);
  	  		return 0x7f;
  	  	}
  	  	else goto _LOOP;
  	  }
		}
	}
	if(bufsize != filesize){
		back[0] = 0x7f;
  	back[1] = CMD_FILE_LOAD;
  	back[2] = 3;
  	obd_write(back, 3);
		bigmem_free();
		user_debug("i:obd_cmd85 filesize error[%d,%d]", bufsize, filesize);
		return 3;
	}
	
	//校验
	for(errnum = 0; errnum < 3; errnum ++){
		back[0] = 0xc5;
		back[1] = 0x04;
		obd_write(back, 2);
		overtime = 0;
		while(1){
			cmd1 = 0;
  	  if(0x00 == obd_data_read_ex(&cmd1, &cmd2, &obddata, &obddatalen)){
  	  	if(CMD_FILE_LOAD == cmd1 && 0x04 == cmd2){
  	  		cs1 = *(obddata + 6);
  	  		cs1 = (cs1 << 8) + *(obddata + 7);
  	  		cs1 = (cs1 << 8) + *(obddata + 8);
  	  		cs1 = (cs1 << 8) + *(obddata + 9);
  	  		break;
  	  	}
  	  }
  	  eat_sleep(5);
  	  overtime ++;
  	  if(overtime >= 100)break;
		}
		if(cmd1 != 0)break;
	}
	if(errnum >= 3){
   		back[0] = 0x7f;
  	  back[1] = CMD_FILE_LOAD;
  	  back[2] = 4;
  	  obd_write(back, 3);
   		bigmem_free();
   		user_debug("i:obd_cmd85 check sum overtime");
   		return 0x7f;
 }
	cs2 = 0;
	for(filesize = 0; filesize < bufsize; filesize ++){
		cs2 += filebuf[filesize];
	}
	
	if(cs1 != cs2){
		back[0] = 0x7f;
  	back[1] = CMD_FILE_LOAD;
  	back[2] = 5;
  	obd_write(back, 3);
  	bigmem_free();
  	user_debug("i:obd_cmd85 check sum error [%d,%d]",cs1,cs2);
  	return 5;
	}
	//数据下载完成
	back[0] = 0xc5;
	back[1] = 0x05;
	back[2] = 0x00;
	obd_write(back, 3);
	
	//2015/12/1 11:28 fangcuisong
	if('S' == filename[0] && 'I' == filename[1] && 'M' == filename[2] && 'a' == filename[8] && 'p' == filename[9]);
	else{
		 u8result = db_save(filebuf, bufsize, filename);//APP数据不需要保存直接在内存中就可以
		 bigmem_free();
		 return 0;
	}
	
	if(bufsize < 1024)return 6;//文件下载异常
  APP_DATA_RUN_BASE = eat_get_app_base_addr();//获取app地址
	app_space_value = eat_get_app_space();//获取app空间大小
	APP_DATA_STORAGE_BASE = APP_DATA_RUN_BASE + (app_space_value >> 1);
	//删除升级保存地址所在的flash空间
	eat_flash_erase((void *)(APP_DATA_STORAGE_BASE), bufsize);
	//将升级程序写入到flash空间区域，起始地址为APP_DATA_STORAGE_BASE
	eat_flash_write((void *)(APP_DATA_STORAGE_BASE), filebuf, bufsize);
	//升级程序
	eat_update_app((void *)(APP_DATA_RUN_BASE),(void *)(APP_DATA_STORAGE_BASE),bufsize, EAT_PIN_NUM,EAT_PIN_NUM,EAT_FALSE);
	  
	bigmem_free();
  //L_FILE_SIZE = bufsize;	
	user_debug("i:obd_cmd85 OK...");
	return u8result;
}
/*
*提取实时的发动机转速以及车速
************************************/
u8 obd_cmd6c(u8 cmd_sub, u8 *data, u16 datalen, u32 time){//直接透传到服务器
	u32 engine, speed, fuel;
	u32 u32t1, u32t2, u32t3,datat;
	u8 id,note,ids[18];
	
	if(0x06 == cmd_sub)
	{//提取车速、发动机转速
		engine = *(data + 6);
		engine = (engine << 8) + *(data + 7);
		engine = (engine << 8) + *(data + 8);
		engine = (engine << 8) + *(data + 9);
		speed = *(data + 10);
		speed = (speed << 8) + *(data + 11);
		speed = (speed << 8) + *(data + 12);
		speed = (speed << 8) + *(data + 13);
		if(*(data + 17) <= 4)
		{
			if(*(data + 17) != 0)user_debug("i:<<<<<<<<<<SPEED=[%d-%d]",speed, *(data + 17));
			gps_3g_check_set(*(data + 17));
		}
		//电压
		u32t1 = *(data + 18);
		u32t1 = (u32t1 << 8) + *(data + 19);
		u32t1 = (u32t1 << 8) + *(data + 20);
		u32t1 = (u32t1 << 8) + *(data + 21);
		//anydata_bettery_set(u32t1 & 0x00ffff);       //add by lilei-2016-0823
		//剩余油量
		u32t1 = *(data + 22);
		u32t1 = (u32t1 << 8) + *(data + 23);
		u32t1 = (u32t1 << 8) + *(data + 24);
		u32t1 = (u32t1 << 8) + *(data + 25);
		//anydata_fuelleve_set(u32t1 / 100);		//add by lilei-2016-0823
		obd_speedset(engine, speed);
		gps_obdinsert(6, engine * 100);//2015/9/9 16:34 fangcuisong
		gps_obdinsert(7, speed * 100);
		//anydata_engine_set(engine);			//add by lilei-2016-0823
		//anydata_speed_set(speed);			//add by lilei-2016-0823
		//user_debug("=====[%d,%d]", engine, speed);
		return 0;
	}

	u32t1 = 0;
	u32t2 = 0;
	id = 0;
	note = 0;
	for(u32t3 = 8; u32t3 < datalen - 1 ; )
	{
		if(0x8c == *(data + u32t3))
		{//本次油耗
			id = *(data + u32t3);
			u32t3 ++;
			u32t2 = *(data + u32t3++);
			u32t2 = (u32t2 << 8) + *(data + u32t3++);
			u32t2 = (u32t2 << 8) + *(data + u32t3++);
			u32t2 = (u32t2 << 8) + *(data + u32t3++);
			datat = u32t2;
			Lcurroute.fuel = u32t2;
			gps_obdinsert(id, datat);
 			//anydata_fuel_set(datat *10);//ML      //add by lilei-2016-0823
 		}
		else if(0x95 == *(data + u32t3))
		{//本次里程
			id = *(data + u32t3);
			u32t3 ++;
			u32t1 = *(data + u32t3++);
			u32t1 = (u32t1 << 8) + *(data + u32t3++);
			u32t1 = (u32t1 << 8) + *(data + u32t3++);
			u32t1 = (u32t1 << 8) + *(data + u32t3++);
			datat = u32t1;
			Lcurroute.distance = u32t1;
			gps_obdinsert(id, datat);
			//anydata_dist_set(datat * 10);//M		//add by lilei-2016-0823
		}
		else if(6 == *(data + u32t3))
		{//发动机转速
			id = *(data + u32t3);
			u32t3 ++;
			engine = *(data + u32t3++);
			engine = (engine << 8) + *(data + u32t3++);
			engine = (engine << 8) + *(data + u32t3++);
			engine = (engine << 8) + *(data + u32t3++);
			datat = engine;
			gps_obdinsert(id, datat);
			//anydata_engine_set(engine / 100);      //add by lilei-2016-0823
		}
		else if(7 == *(data + u32t3))
		{//车速
			id = *(data + u32t3);
			u32t3 ++;
			speed = *(data + u32t3++);
			speed = (speed << 8) + *(data + u32t3++);
			speed = (speed << 8) + *(data + u32t3++);
			speed = (speed << 8) + *(data + u32t3++);
			datat = speed;
			gps_obdinsert(id, datat);
		}
		else if(150 == *(data + u32t3) || 151 == *(data + u32t3) )
		{//瞬时油耗
			if(150 == *(data + u32t3))
			{
				gps_vehicle_status_set(2);
				gps_obdinsert(151, 0);//同一时间只能一个数据有效
			}
			else
			{
				 gps_vehicle_status_set(1);
				 gps_obdinsert(150, 0);
			}
			id = *(data + u32t3);
			u32t3 ++;
			fuel = *(data + u32t3++);
			fuel = (fuel << 8) + *(data + u32t3++);
			fuel = (fuel << 8) + *(data + u32t3++);
			fuel = (fuel << 8) + *(data + u32t3++);
			datat = fuel;
			gps_obdinsert(id, datat);
		}
		else
		{
		  	id = *(data + u32t3);
			u32t3 ++; 
			datat = *(data + u32t3++);
			datat = (datat << 8) + *(data + u32t3++);
			datat = (datat << 8) + *(data + u32t3++);
			datat = (datat << 8) + *(data + u32t3++);
			if(u32t3 >= datalen)break;
	  }
	  Lbdata2_insert(id, datat);
	  db_obd_insert(id, datat);
	  ids[note] = id;
	  note ++;
	}
	Lbdata2_insert_done();
	//SVR_FrameSendEx(data, datalen, time);
	return 0;
}

/*M2M周期性发送心跳数据到OBD，在一定时间内如果OBD没有接收到该数据将会强制M2M重新启动
*该数据发送周期为2S
*内容：
*0x6c 0x07 M2M_Status GPS_Status GPS_Speed System_time
*/
u8 obd_heard(void){
	u8 m2mstatus,gpsstatus;
	u32 gpsspeed, systemtime,lac,lon;
	u8 back[32];
	u8 HeardCnt=0;
	
	m2mstatus = m2m_status();
	gpsstatus = gps_status_get();
	if(gpsstatus != 2)gpsspeed = 0;//无效
	else{
		if(0 == Lupdatetimespeed)gpsspeed = 0;//如果OBD最后返回的速度为0 GPS速度强制为0 为了防止静漂导致数据异常
		else gpsspeed = gps_speed_get();
	}
	systemtime = G_system_time_getEx();

	memset(back, 0, 13);
	back[0] = 0x6c;
	back[1] = 0x07;
	back[2] = m2mstatus;
	back[3] = gpsstatus;
	back[4] = (gpsspeed >> 24)& 0x00ff;
	back[5] = (gpsspeed >> 16)& 0x00ff;
	back[6] = (gpsspeed >> 8)& 0x00ff;
	back[7] = (gpsspeed >> 0)& 0x00ff;
	back[8] = (systemtime >> 24)& 0x00ff;
	back[9] = (systemtime >> 16)& 0x00ff;
	back[10] = (systemtime >> 8)& 0x00ff;
	back[11] = (systemtime >> 0)& 0x00ff;
	db_gps_get(&lac, &lon);
	back[12] = (lac >> 24)& 0x00ff;
	back[13] = (lac >> 16)& 0x00ff;
	back[14] = (lac >> 8)& 0x00ff;
	back[15] = (lac >> 0)& 0x00ff;
	back[16] = (lon >> 24)& 0x00ff;
	back[17] = (lon >> 16)& 0x00ff;
	back[18] = (lon >> 8)& 0x00ff;
	back[19] = (lon >> 0)& 0x00ff;

	/*add by lilei log heart-2016-0523*/

	obd_write(back, 20);
	
	return 0;
}

static unsigned char OBD_VOL_OFF = 0x80;//默认报警使能
void obd_vol_offenable(void){
	if(OBD_VOL_OFF < 0x7f)OBD_VOL_OFF ++;
}
u8 obd_cmd8e(u8 cmd_sub, u8 *data, u16 datalen, u32 time){//直接透传到服务器
	u32 engine, speed;
	u32 u32t1, u32t2, u32t3;
	u8 back[3];
	
	if(0x10 == cmd_sub)
	{
		if(0x01 == *(data + 6))
		{
		  	user_debug("i:GSM---Restart ");
		}
	  	else if(0x02 == *(data + 6))
		{
		  	user_debug("i:GPS---Restart ");
	  	}
	  	return 0;
	}
	else if(0x01 == cmd_sub)
	{
	    if(0x01 == *(data + 6))
	    {//OBD开始工作
	    }
	    else if(0x03 == *(data + 6))
	    {//OBD与车辆建立了连接
	    	
	    }
	    else if(0x02 == *(data + 6))
	    {//0BD进入睡眠
	    		back[0] = 0xce;
	      		back[1] = 0x01;
	      		obd_write(back, 2);
	      		bigmem_save();//设备将马上被OBD重新上电 需要无条件保存1级缓冲中的数据
	      		db_gps_save();
	    }
  	}
  	else if(0x02 == cmd_sub)
	{
		
  		if(0x01 == *(data + 6) && OBD_VOL_OFF < 3)return 0;//如果OBD_VOL_OFF=0x88 表示报警已经上传
  		Lobd_start_mode_set(*(data + 6));
  		if(0x01 == *(data + 6))
		{
  			OBD_VOL_OFF = 0;
  			//anydata_EVT_MODEM_OFF();//上电报警     //add by lilei-2016--823
  		}
  	}
	user_debug("Send serve 0x8e\r\n");
	SVR_FrameSendEx(data, datalen, time);
	return 0;
}
u8 obd_data_read(u8 *cmd1, u8 *cmd2, u8 **data, u16 *datalen){
	u16 dataindex;
	u16 u16t1;
	
	if(0 == obddbex.msgnumex)return 0x7f;
	//优先处理缓冲区的数据;
	if(0x55 == obd_msgnum_flag)eat_sleep(1);
	obd_msgnum_flag = 0x55;
	obddbex.msgnumex --;
	obd_msgnum_flag = 0x00;
	u16t1 = 0;
	if(obddbex.msgex[obddbex.msgoutex].len >= OBD_FRAME_MAX){
		user_debug("i:obd_data_read len err[%d]",obddbex.msgex[obddbex.msgoutex].len);
		obddbex.msgoutex ++;
	  if(obddbex.msgoutex >= OBD_RX_MSG_MAX)obddbex.msgoutex = 0;
	  return 2;
	}
	for(dataindex = 0; dataindex < obddbex.msgex[obddbex.msgoutex].len; dataindex ++){
		OBD_SENDTO_MDM_BUF[dataindex] = obddbex.msgex[obddbex.msgoutex].data[dataindex];
	}
	*cmd1 = obddbex.msgex[obddbex.msgoutex].cmd;
	*cmd2 = obddbex.msgex[obddbex.msgoutex].cmd_sub;
	*data = OBD_SENDTO_MDM_BUF;
	*datalen = obddbex.msgex[obddbex.msgoutex].len;
	obddbex.msgoutex ++;
	if(obddbex.msgoutex >= OBD_RX_MSG_MAX)obddbex.msgoutex = 0;
	return 0;
}

u8 obd_data_read_ex(u8 *cmd1, u8 *cmd2, u8 **data, u16 *datalen){
	u16 dataindex;
	u16 u16t1,datastart;
	
	if(0 == obddb.msgnum)return 0x7f;
	//优先处理缓冲区的数据;
	if(0x55 == obd_msgnum_flag)eat_sleep(1);
	obd_msgnum_flag = 0x55;
	obddb.msgnum --;
	obd_msgnum_flag = 0x00;
	u16t1 = 0;
	if(obddb.msg[obddb.msgout].len >= OBD_FRAME_MAX){
		user_debug("i:obd_data_read len err[%d]",obddb.msg[obddb.msgout].len);
		obddb.msgout ++;
	  if(obddb.msgout >= OBD_RX_MSG_MAX)obddb.msgout = 0;
	  return 2;
	}
	
	datastart = obddb.msg[obddb.msgout].index;
	u16t1 = 0;
	for(dataindex = 0; dataindex < obddb.msg[obddb.msgout].len; dataindex ++){
		OBD_SENDTO_MDM_BUF[dataindex] = obddb.data[datastart + u16t1];
		u16t1 ++;
		if(datastart + u16t1 >= OBD_RX_BUF_MAX){
			datastart = 0;
			u16t1 = 0;
		}
	}
	*cmd1 = obddb.msg[obddb.msgout].cmd;
	*cmd2 = obddb.msg[obddb.msgout].cmd_sub;
	*data = OBD_SENDTO_MDM_BUF;
	*datalen = obddb.msg[obddb.msgout].len;
	obddb.msgout ++;
	if(obddb.msgout >= OBD_RX_MSG_MAX)obddb.msgout = 0;
	return 0;
}

/*清除队列中的某条指令，防止OBD重复发送多条指令导致该操作多次执行  如：LICENSE升级
*/
u8 obd_cmdclear(u8 cmd, u8 cmdsub){
	u8 cmdindex, cmdoutindex;
	
	cmdoutindex = obddb.msgout;
	for(cmdindex = 0; cmdindex < obddb.msgnum; cmdindex ++){
		if(obddb.msg[cmdoutindex].cmd == cmd && obddb.msg[cmdoutindex].cmd_sub == cmdsub){
			obddb.msg[cmdoutindex].cmd = 0;
			obddb.msg[cmdoutindex].cmd_sub = 0;
		}
		cmdoutindex ++;
		if(cmdoutindex >= OBD_RX_MSG_MAX)cmdoutindex = 0;
	}
	return 0;
}

void obd_audio_play(u8 audioindex, u8 leve){
	
	return;//2015/9/25 10:23  fangcuisong  不播放语�
	if(0 == db_svr_mmcget()){
	    user_debug("i:audio unable =[%d-%d]", audioindex, leve);
	    return;
  }
	switch(audioindex){
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
			break;
		case 0x10:
			AT_CREC("C:\\User\\w_acce.amr", 80);
			break;
		case 0x11:
		case 0x12:
		case 0x13:
		case 0x14:
			break;
	  case 0x80://急加速
	  	AT_CREC("C:\\User\\w_acce.amr", 99);
	  	break;
	  case 0x81://急减速
	  	//AT_TTS("急减速,请注意驾驶");
	  	AT_CREC("C:\\User\\w_dece.amr", 99);
	  	break;
	  case 0x82://急拐弯
	  	//AT_TTS("急拐弯,请注意驾驶");
	  	AT_CREC("C:\\User\\w_round.amr", 99);
	  	break;
		case 0xff:
			break;
		default:
			break;
	}
}


void back2SVR8d04(u8 status){
	u8 back[5];
	
	memset(back, 0, 5);
	back[0] = 0x8d;
	back[1] = 0x04;
	back[2] = status;
	SVR_FrameSend(back, 3);
}
/*报警 由M2M产生报警源
*/
u8 obd_cmd8d(u8 cmd_sub){
	u8 *tell;
	if(1 == cmd_sub){
		back2SVR8d04(0x01);
		/*tell = db_svr_ttellget();//2015/10/17 16:37 不需要
		if(tell != NULL && strlen((char *)tell) > 3){
			AT_SMSENDex(tell,"vehicle-voltage too low, please start vehicle!");
	    eat_sleep(5000);//需要等待5S 否则短信可能无法发出
		}*/
		user_debug("i:send warn to svr[1]");
	}
	return 0;
}

/***
*该接口用于将OBD数据发送到服务器
*调用该接口之前必须确认M2M已经与服务器建立连接
****************************************************/
//extern void db_test(void);
u8 obdex_datadeal(void)
{
	u16 dataindex,datastart,dataindexex,datastartex;
	u16 u16t1;
	u8 cmd,cmdsub;
	u32 time;
	u8 cmddealnum;//记录本次处理的指令数  一次可连续处理5条指令
	
	cmddealnum = 0;
	//优先处理缓冲区的数据
	if(0 == obddbex.msgnumex)return 0;
_LOOP_:	
	time = obddbex.msgex[obddbex.msgoutex].time;
	memcpy((s8 *)OBD_SENDTO_MDM_BUF, obddbex.msgex[obddbex.msgoutex].data, obddbex.msgex[obddbex.msgoutex].len);
	dataindex = obddbex.msgex[obddbex.msgoutex].len;
	
	cmd = obddbex.msgex[obddbex.msgoutex].cmd;
	cmdsub = obddbex.msgex[obddbex.msgoutex].cmd_sub;
	//user_debug("obdex_datadeal[%02x,%02x,%02x,%02x]",obddbex.msgoutex,obddbex.msgnumex,cmd,cmdsub);
	//数据发送到服务器
//	db_test();

	switch(obddbex.msgex[obddbex.msgoutex].cmd)
	{
		case 0://无效指令
			   break;
		case CMD_GPS_ASSIST:
	  	   	obddbex.msgoutex ++;
	       	if(obddbex.msgoutex >= OBD_RX_MSG_MAX)obddbex.msgoutex = 0;
         		obddbex.msgnumex --;
	  	   	if(0x20 == cmdsub)
			{
  		      		//if(1 == gps_assist_toOBD())
				//{//返回星历数据
			    	   //星历数据无效
			   	      back2OBD_7f(CMD_GPS_ASSIST, 0);
			      //}
       	 	}
       	 	return 1;
	  	   	break;
	   	case CMD_ROUT://行程指令 
	  	   	obd_rout(obddbex.msgex[obddbex.msgoutex].cmd_sub, OBD_SENDTO_MDM_BUF, dataindex);
	  	   	break;
		case CMD_AUDIO_PLAY://播放音乐
	  	    	obd_audio_play(obddbex.msgex[obddbex.msgoutex].cmd_sub, OBD_SENDTO_MDM_BUF[6]);
	  	   	break;
	  	case CMD_FILE_LOAD:
	  	    	if(0x07 == obddbex.msgex[obddbex.msgoutex].cmd_sub)
			{
	  	    		obd_cmd85(obddbex.msgex[obddbex.msgoutex].cmd_sub, OBD_SENDTO_MDM_BUF, dataindex);
	  	    	}
	  	   	break;
	  	case CMD_VEHICLESET:
	       	if(0x03 == obddbex.msgex[obddbex.msgoutex].cmd_sub)
			{//重新设置总里程
		 	     db_obd_reset();
		       }
		     //2015/6/19 17:12
		       SVR_FrameSendEx(OBD_SENDTO_MDM_BUF, dataindex, time);//直接透传到服务器
		       break;
		case CMD_UPDATE_FORCE://强制升级
		case CMD_UPDATE://升级请求
			   user_debug("i:update:[%02x,%02x]",OBD_SENDTO_MDM_BUF[5],OBD_SENDTO_MDM_BUF[6]);
			   if(CMD_UPDATE_FORCE == obddbex.msgex[obddbex.msgoutex].cmd)
			   {
			   	APP_UPDATE_FORCE = 0x55;
			   }
			   else APP_UPDATE_FORCE = 0;
			   if(0x10 == obddbex.msgex[obddbex.msgoutex].cmd_sub)
			   {//License升级
			   	 if(0 == SVR_LicenseGet(OBD_SENDTO_MDM_BUF, dataindex, time))obd_cmdclear(CMD_UPDATE, 0x10);
			   	 break;
			   }
			   else if(0x07 == obddbex.msgex[obddbex.msgoutex].cmd_sub)
			   {//有升级请求 直接透传到服务器
			   	 //back2OBD_2Bytes(CMD_UPDATE + 0x40, obddbex.msgex[obddbex.msgoutex].cmd_sub);
			   	 if(0 == APP_UPDATE_FLAG)
				 {//0 == SVR_SvrChange(db_svr_addr1get(), db_svr_port1get())){
			   	 	   back2OBD_2Bytes(CMD_UPDATE + 0x40, obddbex.msgex[obddbex.msgoutex].cmd_sub);
			   	     	   //SVR_Cmd84(OBD_SENDTO_MDM_BUF[6]);//请求数据下载
			   	     	   APP_UPDATE_FLAGEX = OBD_SENDTO_MDM_BUF[6];
			   	 }
			   	 else
				 {
			   	 	 if(APP_UPDATE_FLAG == OBD_SENDTO_MDM_BUF[6])back2OBD_2Bytes(CMD_UPDATE + 0x40, obddbex.msgex[obddbex.msgoutex].cmd_sub);
			   	 	 else back2OBD_2Bytes(CMD_UPDATE + 0x40, 0);
			   	 }
			   }
		     //obd_cmd84(obddb.msg[obddb.msgout].cmd_sub, OBD_SENDTO_MDM_BUF, dataindex);
		     	  break;
		case CMD_M2M_CONTROL://OBD对M2M进行控制操作
			   if(0x02 == obddbex.msgex[obddbex.msgoutex].cmd_sub)
			   {
			   	  if(0x02 == OBD_SENDTO_MDM_BUF[6])
				  {//M2M需要断电重启动 保存行程
			   	  }
			   }
			   break;
		case CMD_OBDDATA_AUTO://广播数据
		     	obd_cmd6c(obddbex.msgex[obddbex.msgoutex].cmd_sub, OBD_SENDTO_MDM_BUF, dataindex, time);
		     	break;
		case CMD_OBD_STATUS://OBD状态返回
		     	obd_cmd8e(obddbex.msgex[obddbex.msgoutex].cmd_sub, OBD_SENDTO_MDM_BUF, dataindex, time);
		     	break;
		case CMD_VEHICLE_STATUS://车辆状态 透传到服务器
			   cmdsub = obddbex.msgex[obddbex.msgoutex].cmd_sub;
			   user_debug("\r\n--lilei-Recieve -8d-%02X\r\n",cmdsub);
			   cmd = CMD_VEHICLE_STATUS;
			   //user_debug("888888d[%02x,%02x,%d]", cmdsub, cmd,time);
			  // SVR_FrameSendEx(OBD_SENDTO_MDM_BUF, dataindex, time);//直接透传到服务器
			   if(0x04 == cmdsub)
			   {//警告类信息除发送到服务器 还需要以短信方式提供给车主
			   	   if(0x01 == OBD_SENDTO_MDM_BUF[6])
				   {//车辆电压过低
			   	   	AT_SMSENDex(NULL,"vehicle-voltage too low, please start vehicle!");
			   	   }
			   	   else if(0x02 == OBD_SENDTO_MDM_BUF[6])
				   {//车辆电压过低
			   	   	AT_SMSENDex(NULL,"shcock happen on your vehicle!");
			   	   }
			   	   else if(0x03 == OBD_SENDTO_MDM_BUF[6])
				   {//车辆电压过低
			   	   	AT_SMSENDex(NULL,"Danger maybe happen on your vehicle!");
			   	   }
			   	   eat_sleep(5000);//需要等待5S 否则短信可能无法发出
			   	   back2OBD_2Bytes(cmd + 0x40, cmdsub);
			   }
			   else if(0x05 == cmdsub)
			   {//急加速 急减速 急拐弯
			   		user_debug("i:8d 05=====[%02x-%02x-%02x-%02x-%02x-%02x-%02x]", OBD_SENDTO_MDM_BUF[2],OBD_SENDTO_MDM_BUF[3],OBD_SENDTO_MDM_BUF[4],OBD_SENDTO_MDM_BUF[5],OBD_SENDTO_MDM_BUF[6],OBD_SENDTO_MDM_BUF[7],OBD_SENDTO_MDM_BUF[8]);
			   		//anydata_accSet((unsigned char *)&OBD_SENDTO_MDM_BUF[6]);      //add by lilei-2016-0823
			   		//anydata_EVT_HARD_XX();			 //add by lilei-2016-0823
			   }
			   else if(0x0b == cmdsub)
			   {//震动报警
			   	obd_vehiclelost_check(&OBD_SENDTO_MDM_BUF[7]);
			   }
			   else if(cmdsub==0x10)                  									 //add  by lilei--OBD发送报警类型例超速怠速疲劳
			   {
			   	  user_debug("Receive -Obd-Over-Speed \r\n");
			   	  AT_CREC("C:\\User\\Over-Speed.amr", 99); 							//add by lilei-2016-0912 超速报警                                   
			   }
			   else if(cmdsub==0x11)												//add by lilei-2016-0912 疲劳驾驶报警
			   {
			   	   user_debug("Receive -Obd-Tired-Drive \r\n");
				   AT_CREC("C:\\User\\Tired-Drive.amr", 99); 
			   }
			   else if(cmdsub==0x12)
			   {
			   	  user_debug("Receive -Obd-Idle-Speed \r\n");
				  AT_CREC("C:\\User\\Idle-Speed.amr", 99); 						//add by lilei-2016-0912 怠速时间过长报警
			   }
			   break;
		case 0x7f:						//0x7f数据直接丢弃  2015/10/29 15:50 fangcuisong
			   break;
		default:
		     	//SVR_FrameSendEx(OBD_SENDTO_MDM_BUF, dataindex, time);//直接透传到服务器
		     	break;
	}
	if(obddbex.msgnumex)
	{
	    obddbex.msgoutex ++;
	    if(obddbex.msgoutex >= OBD_RX_MSG_MAX)obddbex.msgoutex = 0;
	    if(0x55 == obd_msgnum_flag)eat_sleep(1);
	    obd_msgnum_flag = 0x55;
	    obddbex.msgnumex --;
	    obd_msgnum_flag = 0x00;
  	}
	cmddealnum ++;
	if((cmddealnum < 5) && (obddbex.msgnumex > 0))goto _LOOP_;
	return 0;
}

/***
*该接口用于将OBD数据发送到服务器
*调用该接口之前必须确认M2M已经与服务器建立连接
****************************************************/
//extern void db_test(void);
u8 obd_datadeal(void)
{
	u16 dataindex,datastart,dataindexex,datastartex;
	u16 u16t1;
	u8 cmd,cmdsub;
	u32 time;
	
	//优先处理缓冲区的数据
	
	if(0x88 == hw_th_unable)
	{
		time = G_system_time_getEx();
		OBD_SENDTO_MDM_BUF[0] = 0x8d;
		OBD_SENDTO_MDM_BUF[1] = 0x04;
		OBD_SENDTO_MDM_BUF[2] = 0x02;
		dataindex = obd_write_tomessage(OBD_SENDTO_MDM_BUF, 3);
		if(0x00 == SVR_cmdxx_Ex(OBD_SENDTO_MDM_BUF, dataindex, time, 0x8d))
		{
			user_debug("i:hw_th_unable...");
			AT_SMSENDex(NULL,"shcock happen on your vehicle!");
		  	hw_th_unable_set(0);
		}
		else
		{
			user_debug("i:hw_th_unable error[%d]", dataindex);
		}
	}
	
	obdex_datadeal();
	if(0 == obddb.msgnum)return 0;
	
	datastart = obddb.msg[obddb.msgout].index;
	time = obddb.msg[obddb.msgout].time;
	u16t1 = 0;
	for(dataindex = 0; dataindex < obddb.msg[obddb.msgout].len; dataindex ++)
	{
		OBD_SENDTO_MDM_BUF[dataindex] = obddb.data[datastart + u16t1];
		u16t1 ++;
		if(datastart + u16t1 >= OBD_RX_BUF_MAX)
		{
			datastart = 0;
			u16t1 = 0;
		}
	}
	
	//数据发送到服务器
//	db_test();

	switch(obddb.msg[obddb.msgout].cmd){
		case 0://无效指令
			   break;
	  case CMD_ROUT://行程指令
	  	   obd_rout(obddb.msg[obddb.msgout].cmd_sub, OBD_SENDTO_MDM_BUF, dataindex);
	  	   break;
		case CMD_UPDATE_FORCE://强制升级
		case CMD_UPDATE://升级请求
			   user_debug("i:update1:[%02x,%02x]",OBD_SENDTO_MDM_BUF[5],OBD_SENDTO_MDM_BUF[6]);
			   if(CMD_UPDATE_FORCE == obddb.msg[obddb.msgout].cmd){
			   	APP_UPDATE_FORCE = 0x55;
			   }
			   else APP_UPDATE_FORCE = 0;
			   if(0x10 == obddb.msg[obddb.msgout].cmd_sub){//License升级
			   	 if(0 == SVR_LicenseGet(OBD_SENDTO_MDM_BUF, dataindex, time))obd_cmdclear(CMD_UPDATE, 0x10);
			   	 break;
			   }
			   else if(0x07 == obddb.msg[obddb.msgout].cmd_sub){//有升级请求 直接透传到服务器
			   	 //back2OBD_2Bytes(CMD_UPDATE + 0x40, obddb.msg[obddb.msgout].cmd_sub);
			   	 if(0 == APP_UPDATE_FLAG){//0 == SVR_SvrChange(db_svr_addr1get(), db_svr_port1get())){
			   	 	   back2OBD_2Bytes(CMD_UPDATE + 0x40, obddb.msg[obddb.msgout].cmd_sub);
			   	     //SVR_Cmd84(OBD_SENDTO_MDM_BUF[6]);//请求数据下载
			   	     APP_UPDATE_FLAGEX = OBD_SENDTO_MDM_BUF[6];
			   	 }
			   	 else{
			   	 	 if(APP_UPDATE_FLAG == OBD_SENDTO_MDM_BUF[6])back2OBD_2Bytes(CMD_UPDATE + 0x40, obddb.msg[obddb.msgout].cmd_sub);
			   	 	 else back2OBD_2Bytes(CMD_UPDATE + 0x40, 0);
			   	 }
			   }
		     //obd_cmd84(obddb.msg[obddb.msgout].cmd_sub, OBD_SENDTO_MDM_BUF, dataindex);
		     break;
		case CMD_M2M_CONTROL://OBD对M2M进行控制操作
			   if(0x02 == obddb.msg[obddb.msgout].cmd_sub){
			   	  if(0x02 == OBD_SENDTO_MDM_BUF[6]){//M2M需要断电重启动 保存行程
			   	  }
			   }
			   break;
		case CMD_OBDDATA_AUTO://广播数据
		     obd_cmd6c(obddb.msg[obddb.msgout].cmd_sub, OBD_SENDTO_MDM_BUF, dataindex, time);
		     break;
		case CMD_OBD_STATUS://OBD状态返回
		     obd_cmd8e(obddb.msg[obddb.msgout].cmd_sub, OBD_SENDTO_MDM_BUF, dataindex, time);
		     break;
		case CMD_VEHICLE_STATUS://车辆状态 透传到服务器
			   cmdsub = obddb.msg[obddb.msgout].cmd_sub;
			   cmd = CMD_VEHICLE_STATUS;
			   //SVR_FrameSendEx(OBD_SENDTO_MDM_BUF, dataindex, time);//直接透传到服务器
			   if(0x04 == cmdsub){//警告类信息除发送到服务器 还需要以短信方式提供给车主
			   	   if(0x01 == OBD_SENDTO_MDM_BUF[6]){//车辆电压过低
			   	   	AT_SMSENDex(NULL,"vehicle-voltage too low, please start vehicle!");
			   	   }
			   	   else if(0x02 == OBD_SENDTO_MDM_BUF[6]){//车辆电压过低
			   	   	AT_SMSENDex(NULL,"shcock happen on your vehicle!");
			   	   }
			   	   else if(0x03 == OBD_SENDTO_MDM_BUF[6]){//车辆电压过低
			   	   	AT_SMSENDex(NULL,"Danger maybe happen on your vehicle!");
			   	   }
			   	   eat_sleep(5000);//需要等待5S 否则短信可能无法发出
			   	   back2OBD_2Bytes(cmd + 0x40, cmdsub);
			   }
			   else if(0x0b == cmdsub){//震动报警
			   	obd_vehiclelost_check(&OBD_SENDTO_MDM_BUF[7]);
			   }
			   else if(cmdsub==0x10)                  										 
			   {
			   		user_debug("Receive -Obd-Over-Speed \r\n");
			   	  	AT_CREC("C:\\User\\Over-Speed.amr", 99); 							                                   
			   }
			   else if(cmdsub==0x11)													
			   {
			   		 user_debug("Receive -Obd-Tired-Drive \r\n");
					 AT_CREC("C:\\User\\Tired-Drive.amr", 99); 
			   }
			   else if(cmdsub==0x12)
			   {
			   		 user_debug("Receive -Obd-Idle-Speed \r\n");
					 AT_CREC("C:\\User\\Idle-Speed.amr", 99); 							
			   }
			   break;
		case 0x7f:                                                    									
			  break;
		default:
		     //SVR_FrameSendEx(OBD_SENDTO_MDM_BUF, dataindex, time);						//直接透传到服务器
		     //debug_hex("OBD:",OBD_SENDTO_MDM_BUF, dataindex);
		     //应答OBD
	       //back2OBD_3Bytes(obddb.msg[obddb.msgout].cmd,obddb.msg[obddb.msgout].cmd_sub,0x01);
		     break;
	}
	obddb.msgout ++;
	if(obddb.msgout >= OBD_RX_MSG_MAX)obddb.msgout = 0;
	if(0x55 == obd_msgnum_flag)eat_sleep(1);
	obd_msgnum_flag = 0x55;
	obddb.msgnum --;
	obd_msgnum_flag = 0x00;
	return 0;
}




