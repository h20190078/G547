#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include "chardev.h"

uint16_t num;
char alignVar;
int chanVar;
static dev_t first; // variable for device number

static struct cdev c_dev;
static struct class *cls;
char binaryNum[10]= "0000000000";
char result[17]="0000000000000000";

//STEP4: Driver call back functions
static int my_open(struct inode *i,struct file *f)
{
printk(KERN_INFO "mychar:open()\n");
return 0;

}
static int my_close(struct inode *i,struct file *f)
{
printk(KERN_INFO "mychar:close()\n");
return 0;

}

static ssize_t my_read(struct file *f,char __user *buf,size_t len,loff_t *off)
{
	int i;
	printk(KERN_INFO "mychar:read()");
        get_random_bytes(&num,sizeof(num));
        num = num % 1024;
	printk(KERN_INFO "Num =  %d",num);

	for(i=0;num>0;i++)
	{
	binaryNum[9-i]= (num % 2) + '0';
	num=num/2;
	} 
	//binaryNum[16] =
	
	printk(KERN_INFO "Binary no.=%s",binaryNum);

	if(alignVar=='r')
	{
		for(i=6;i<15;i++)
		{ 
			result[i] = binaryNum[i-6];
		} 
	}
	
	else if (alignVar=='l')
	{
		for(i=0;i<10;i++)
		{
			result[i]=binaryNum[i];
		} 
	}
	
	result[16] = '\0';
	printk(KERN_INFO "result=%s",result);
	printk(KERN_INFO "result obtained");

	if(copy_to_user(buf,result,sizeof(result))<0)
	{
		printk(KERN_INFO "some data missing\n");
	}

	return sizeof(result); 
	}
	


static long my_ioctl(struct file *f,unsigned int cmd, unsigned long arg)
{

switch(cmd){
	 case IOCTL_SET_CHANNEL:
	 chanVar=arg;
         break;

	 case IOCTL_SET_ALIGNMENT:
         alignVar=(char)arg;
	//printk(KERN_INFO "align %c",alignVar);
	 		 	break;
	}
   return 0;
}


static struct file_operations fops=
			{
				.owner=THIS_MODULE,
				.open=my_open,
				.release=my_close,
				.read=my_read,
				.unlocked_ioctl=my_ioctl

			};


static int __init mychar_init(void)
{
	printk(KERN_INFO"Namaste:mychar driver registered");

	// STEP1 : reserve <major,minor>

	if (alloc_chrdev_region(&first,0,1,"BITS-PILANI")<0)
	{
printk(KERN_INFO "step1 done");

		return -1;
	}

	//STEP2: creation of Device file
	if((cls=class_create(THIS_MODULE,"chardriver"))==NULL)
	{
  printk(KERN_INFO"class not created");
	unregister_chrdev_region(first,1);
	return -1;
	}

	if(device_create(cls,NULL,first,NULL,"adc8")==NULL)
	{
	 	printk(KERN_INFO"device not created");
		class_destroy(cls);
		unregister_chrdev_region(first,1);
		return -1;
	}

	//STEP3: link fops and cdev to the device node
	cdev_init(&c_dev,&fops);
	if(cdev_add(&c_dev,first,1)==1)
		{
		device_destroy(cls,first);
		class_destroy(cls);
		unregister_chrdev_region(first,1);
		return -1;
		}

		return 0;
}

static void __exit mychar_exit(void)
{
	cdev_del(&c_dev);
	device_destroy(cls,first);
	class_destroy(cls);
	unregister_chrdev_region(first,1);
	printk(KERN_INFO "Bye : mychar driver unregisterd\n\n");

}

module_init(mychar_init);
module_exit(mychar_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Karavi Bharani");
