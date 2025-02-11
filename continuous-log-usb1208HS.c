/*
 *  Copyright (c) 2014 Warren J. Jasper <wjasper@tx.ncsu.edu>
 *  Copyright (c) 2016 assignee tbd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <time.h>

#include "pmd.h"
#include "usb-1208HS.h"

#define FALSE 0
#define TRUE 1

static int wMaxPacketSize;  // will be the same for all devices of this type so
                            // no need to be reentrant. 

#define BLOCKSIZE (1024)

int main (int argc, char **argv)
{
  double frequency;

  float table_AIN[NMODE][NGAINS_1208HS][2];
  float table_AO[NCHAN_AO_1208HS][2];

  int usb1208HS_2AO = FALSE;
  int usb1208HS_4AO = FALSE;
  libusb_device_handle *udev = NULL;
  int i, j;
  //int transferred;
  uint8_t gain, mode, channel;
  int ch;
  int ret;

  int fd;
  int fd_ts;
  
  uint16_t sdataIn[4*BLOCKSIZE];    // holds 13 bit unsigned analog input data
  uint8_t range[NCHAN_1208HS];

  struct timespec ts;
  struct timespec ts_old;

  udev = NULL;

  libusb_context * ctx = NULL;
  ret = libusb_init(&ctx);
  if (ret < 0) {
    perror("usb_device_find_USB_MCC: Failed to initialize libusb");
    exit(1);
  }

  if ((udev = usb_device_find_USB_MCC(USB1208HS_PID, NULL))) {
    printf("Success, found a USB 1208HS!\n");
  } else if ((udev = usb_device_find_USB_MCC(USB1208HS_2AO_PID, NULL))) {
    printf("Success, found a USB 1208HS-2AO!\n");
    usb1208HS_2AO = TRUE;
  } else if ((udev = usb_device_find_USB_MCC(USB1208HS_4AO_PID, NULL))) {
    printf("Success, found a USB 1208HS-4AO!\n");
    usb1208HS_4AO = TRUE;
  } else {
    printf("Failure, did not find a USB 1208HS, 1208HS-2AO or 1208HS-4AO!\n");
    return 0;
  }
  (void) usb1208HS_2AO;
  (void) usb1208HS_4AO;

  libusb_set_debug(ctx,LIBUSB_LOG_LEVEL_WARNING);

  usbInit_1208HS(udev);
  usbBuildGainTable_USB1208HS(udev, table_AIN);

  if (usb1208HS_4AO) {
    usbBuildGainTable_USB1208HS_4AO(udev, table_AO);
    printf("\n");
    for (i = 0; i < NCHAN_AO_1208HS; i++) {
      printf("VDAC%d:    Slope = %f    Offset = %f\n", i, table_AO[i][0], table_AO[i][1]);
    }
  }

  wMaxPacketSize = usb_get_max_packet_size(udev, 0);

  fd = open("data.raw", O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    perror("Error opening data file.");
    return -1;
  }

  fd_ts = open("timestamps.csv", O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd_ts < 0) {
    perror("Error opening ts file.");
    return -1;
  }


	printf("Testing USB-1208HS Analog Input Scan.\n");
	usbAInScanStop_USB1208HS(udev);
  channel = 6;
	printf("Input channel %d\n",channel);
  ch = '3';
  char * gain_range = "";
	switch(ch) {
	  case '1': gain = BP_10V; gain_range = "+/-10V"; break;
	  case '2': gain = BP_5V; gain_range = "+/-5V"; break;
	  case '3': gain = BP_2_5V; gain_range = "+/-2.5V"; break;
	  case '4': gain = UP_10V; gain_range = "0-10V SE"; break;
	  default:  gain = BP_10V; gain_range = "+/-10V"; break;
	}
	mode = SINGLE_ENDED;
	for (i = 0; i < NCHAN_1208HS; i++) {
	  range[i] = gain;
	}
	usbAInConfig_USB1208HS(udev, mode, range);
  frequency = 44100;
  printf("Sampling frequency: %1.02fHz\n",frequency);

  clock_gettime(CLOCK_REALTIME,&ts_old);

  uint8_t option = BURST_MODE;
	usbAInScanStart_USB1208HS(udev, 0, 0, frequency, (0xF<<channel), 0xff, option);
  usleep(1000);
  long long samples = 0;
  char buf[1000];
  int skipcnt = 0;
  long long backlog;
  double elapsed_s;
  long long expected_samples;
  int totalsamples = 0;
  while(samples < 44100) {
    ret = usbAInScanRead_USB1208HS(udev, BLOCKSIZE, 4, sdataIn);
    printf("ret %d ",ret);
    if (ret < 0) {
      printf("Warning: usbAInScanRead returned %d\n",ret);
      continue;
    }
    if (ret == 1) {
      // overrun ---> reset
      printf("Restarting sampling...\n");
      usbAInScanStop_USB1208HS(udev);
      usbAInConfig_USB1208HS(udev, mode, range);
      usbAInScanStart_USB1208HS(udev, 0, 0, frequency, (0xF<<channel), 0xff, option);
      continue;
    }
    /* write timestamp to file */
    clock_gettime(CLOCK_REALTIME,&ts);
    sprintf(buf,"%lld,%lld.%09ld\n",samples,(long long)ts.tv_sec,ts.tv_nsec);
    write(fd_ts,buf,strlen(buf));
    /* write data to file */
    int wr = write(fd,sdataIn,sizeof(sdataIn));
    if (wr != sizeof(sdataIn)) {
      printf("Only wrote %d bytes\n",wr);
    }
    /*sdataIn[i] = rint(sdataIn[i]*table_AIN[mode][gain][0] + table_AIN[mode][gain][1]);
    volts = volts_USB1208HS(mode, gain, sdataIn[i]);*/
    samples += BLOCKSIZE;
    /*ts_old.tv_sec = ts.tv_sec; ts_old.tv_nsec = ts.tv_nsec; */
  }
	usbAInScanStop_USB1208HS(udev);
  close(fd);
  close(fd_ts);

  return 0;
}

