#include <stdio.h>
#include "chardev.h"
#include <sys/ioctl.h>          /* ioctl */
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>



char alignVar;
int channel;
char result[16];

int ioctl_set_channel(int file_desc,int channel)
{
    int ret_val;

    ret_val = ioctl(file_desc,IOCTL_SET_CHANNEL,channel);

    if (ret_val < 0) {
        printf("ioctl_set_msg failed:%d\n", ret_val);
        return -1;
    }
    return 0;
}

int ioctl_set_alignment(int file_desc,char alignVar)
{
    int ret_val;

    ret_val = ioctl(file_desc,IOCTL_SET_ALIGNMENT,alignVar);

    if (ret_val < 0) {
        printf("ioctl_set_alignment failed:%u \n", ret_val);
        return -1;
    }
    return 0;
}

int main(){

int file_desc,ret_val;

printf("enter the channel(1-8):");
scanf("%d", &channel);
printf("enter the alignment(r or l):");
scanf(" %c", &alignVar);


file_desc = open(DEVICE_FILE_NAME, 0);
  if (file_desc < 0) {
        printf("Can't open device file: %s\n", DEVICE_FILE_NAME);
        return -1;
    }


ioctl_set_channel(file_desc,channel);
ioctl_set_alignment(file_desc,alignVar);

if(read(file_desc,&result,sizeof(result))<0)
{
printf("Can't read properly");
}
puts(result);
//printf("Data read from ADC is: %d",result);
close(file_desc);
return 0;
}
