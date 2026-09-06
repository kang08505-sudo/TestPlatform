#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xil_types.h"
#include "xil_cache.h"
#include "xparameters.h"
#include "xgpio.h"
#include "xaxivdma.h"
#include "xaxivdma_i.h"
#include "display_ctrl/display_ctrl.h"
#include "vdma_api/vdma_api.h"
#include "clk_wiz/clk_wiz.h"
#include "emio_sccb_cfg/emio_sccb_cfg.h"
#include "ov5640/ov5640_init.h"

//宏定义
#define BYTES_PIXEL        3                           //像素字节数，RGB888占3个字节
#define FRAME_BUFFER_NUM   3                           //帧缓存个数3
#define CLK_WIZ_ID         XPAR_CLK_WIZ_0_DEVICE_ID   //时钟IP核器件ID

#define VDMA_ID            XPAR_AXIVDMA_0_DEVICE_ID    //VDMA器件ID
#define DISP_VTC_ID        XPAR_VTC_0_DEVICE_ID        //VTC器件ID
//PL端  AXI GPIO 0(lcd_id)器件 ID
#define AXI_GPIO_0_ID      XPAR_AXI_GPIO_0_DEVICE_ID   
//使用AXI GPIO(lcd_id)通道1
#define AXI_GPIO_0_CHANEL  1                           

//全局变量
//frame buffer的起始地址
unsigned int const frame_buffer_addr = (XPAR_PS7_DDR_0_S_AXI_BASEADDR
										+ 0x1000000);
XAxiVdma     vdma;
DisplayCtrl  dispCtrl;
XGpio        axi_gpio_inst;   //PL端 AXI GPIO 驱动实例
VideoMode    vd_mode;
unsigned int lcd_id;

int main(void)
{
	u32 status;
	u16 cmos_h_pixel;   //ov5640 DVP 输出水平像素点数
	u16 cmos_v_pixel;   //ov5640 DVP 输出垂直像素点数
	u16 total_h_pixel;  //ov5640 水平总像素大小
	u16 total_v_pixel;  //ov5640 垂直总像素大小

	XGpio_Initialize(&axi_gpio_inst,AXI_GPIO_0_ID);                //GPIO初始化
	XGpio_SetDataDirection(&axi_gpio_inst,AXI_GPIO_0_CHANEL,0x07); //设置AXI GPIO为输入
	lcd_id = lcd_id_read(&axi_gpio_inst,AXI_GPIO_0_CHANEL);  //获取LCD的ID
	XGpio_SetDataDirection(&axi_gpio_inst,AXI_GPIO_0_CHANEL,0x00); //设置AXI GPIO为输出
	xil_printf("LCD ID: %x\r\n",lcd_id);

	//根据获取的LCD的ID号来进行ov5640显示分辨率参数的选择
	switch(lcd_id){
		case 0x4342 :  //4.3寸屏,480*272分辨率
			cmos_h_pixel = 480;
			cmos_v_pixel = 272;
			total_h_pixel = 1800;
			total_v_pixel = 1000;
			break;
		case 0x4384 :  //4.3寸屏,800*480分辨率
			cmos_h_pixel = 800;
			cmos_v_pixel = 480;
			total_h_pixel = 1800;
			total_v_pixel = 1000;
			break;
		case 0x7084 :  //7寸屏,800*480分辨率
			cmos_h_pixel = 800;
			cmos_v_pixel = 480;
			total_h_pixel = 1800;
			total_v_pixel = 1000;
			break;
		case 0x7016 :  //7寸屏,1024*600分辨率
			cmos_h_pixel = 1024;
			cmos_v_pixel = 600;
			total_h_pixel = 2200;
			total_v_pixel = 1000;
			break;
		case 0x1018 :  //10.1寸屏,1280*800分辨率
			cmos_h_pixel = 1280;
			cmos_v_pixel = 800;
			total_h_pixel = 2570;
			total_v_pixel = 980;
			break;
		default :
			cmos_h_pixel = 800;
			cmos_v_pixel = 480;
			total_h_pixel = 1800;
			total_v_pixel = 1000;
			break;
	}

	emio_init();                         //初始化EMIO
	status = ov5640_init( cmos_h_pixel,  //初始化ov5640
						  cmos_v_pixel,
						 total_h_pixel,
						 total_v_pixel);
	if(status == 0)
		xil_printf("OV5640 detected successful!\r\n");
	else
		xil_printf("OV5640 detected failed!\r\n");

	//根据获取的LCD的ID号来进行video参数的选择
	switch(lcd_id){
		case 0x4342 : vd_mode = VMODE_480x272;  break;  //4.3寸屏,480*272分辨率
		case 0x4384 : vd_mode = VMODE_800x480;  break;  //4.3寸屏,800*480分辨率
		case 0x7084 : vd_mode = VMODE_800x480;  break;  //7寸屏,800*480分辨率
		case 0x7016 : vd_mode = VMODE_1024x600; break;  //7寸屏,1024*600分辨率
		case 0x1018 : vd_mode = VMODE_1280x800; break;  //10.1寸屏,1280*800分辨率
		default : vd_mode = VMODE_800x480; break;
	}

	//配置VDMA
	run_vdma_frame_buffer(&vdma, VDMA_ID, vd_mode.width, vd_mode.height,
							frame_buffer_addr,0,0,BOTH);
	//设置时钟IP核输出的时钟频率
	clk_wiz_cfg(CLK_WIZ_ID,vd_mode.freq);
	//初始化Display controller
	DisplayInitialize(&dispCtrl, DISP_VTC_ID);
    //设置VideoMode
	DisplaySetMode(&dispCtrl, &vd_mode);
	DisplayStart(&dispCtrl);

    return 0;
}

