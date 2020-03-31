#include<linux/kernel.h>
#include<linux/module.h>
#include<linux/usb.h>
#include<linux/slab.h>

#define PENDRIVE_VID    0x1307         //  0x1307  // 0x0781
#define PENDRIVE_PID    0x0163      // 0x0163  //0x5567 


#define BOMS_RESET                    0xFF
#define BOMS_RESET_REQ_TYPE           0x21
#define BOMS_GET_MAX_LUN              0xFE
#define BOMS_GET_MAX_LUN_REQ_TYPE     0xA1
#define READ_CAPACITY_LENGTH	      0x08
#define REQUEST_DATA_LENGTH           0x12
#define be_to_int32(buf) (((buf)[0]<<24)|((buf)[1]<<16)|((buf)[2]<<8)|(buf)[3])

struct command_block_wrapper {
	uint8_t dCBWSignature[4];
	uint32_t dCBWTag;
	uint32_t dCBWDataTransferLength;
	uint8_t bmCBWFlags;
	uint8_t bCBWLUN;
	uint8_t bCBWCBLength;
	uint8_t CBWCB[16];
};

struct command_status_wrapper {
	uint8_t dCSWSignature[4];
	uint32_t dCSWTag;
	uint32_t dCSWDataResidue;
	uint8_t bCSWStatus;
};


 static uint8_t cdb_length[256] = {
//	 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
	06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,  //  0
	06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,06,  //  1
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  2
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  3
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  4
	10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,  //  5
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  6
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  7
	16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,  //  8
	16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,  //  9
	12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,  //  A
	12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,  //  B
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  C
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  D
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  E
	00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,  //  F
}; 

static void usbdev_disconnect(struct usb_interface *interface)
{
	printk(KERN_INFO "USBDEV Device Removed\n");
	return;
}

static struct usb_device_id usbdev_table [] = {
	{USB_DEVICE(PENDRIVE_VID,PENDRIVE_PID)},
	{} /*terminating entry*/	
};
static int get_mass_storage_status(struct usb_device *udev, uint8_t endpoint, uint32_t expected_tag)
{	
	int r;
	int size;
	struct command_status_wrapper *csw;
	csw=(struct command_status_wrapper *)kmalloc(sizeof(struct command_status_wrapper),GFP_KERNEL);
	r=usb_bulk_msg(udev,usb_rcvbulkpipe(udev,endpoint),(void*)csw,13, &size, 1000);
	if(r<0)
		printk("error in status");
	
	if (csw->dCSWTag != expected_tag) {
		printk("   get_mass_storage_status: mismatched tags (expected %08X, received %08X)\n",
			expected_tag, csw->dCSWTag);
		return -1;
	}
	if (size != 13) {
		printk("   get_mass_storage_status: received %d bytes (expected 13)\n", size);
		return -1;
	}	
	printk(KERN_INFO "Mass Storage Status: %02X (%s)\n", csw->bCSWStatus, csw->bCSWStatus?"FAILED":"Success");
	return 0;
}
static int send_command(struct usb_device *udev,uint8_t endpoint, uint8_t lun,
                         uint8_t *cdb, uint8_t direction, int data_length, uint32_t *ret_tag)
{
	
	uint32_t tag = 1;
	int r;
	int size;
	uint8_t cdb_len;
	struct command_block_wrapper *cbw;
	cbw=(struct command_block_wrapper *)kmalloc(sizeof(struct command_block_wrapper),GFP_KERNEL);
	
	if (cdb == NULL) {
		return -1;
	}
	if (endpoint & USB_DIR_IN) {
		printk("send_mass_storage_command: cannot send command on IN endpoint\n");
		return -1;
	}	
	cdb_len = cdb_length[cdb[0]];
	if ((cdb_len == 0) || (cdb_len > sizeof(cbw->CBWCB))) {
		printk("Invalid command\n");
		return -1;
	}	

	memset(cbw, 0, sizeof(*cbw));
	cbw->dCBWSignature[0] = 'U';
	cbw->dCBWSignature[1] = 'S';
	cbw->dCBWSignature[2] = 'B';
	cbw->dCBWSignature[3] = 'C';
	*ret_tag = tag;
	cbw->dCBWTag = tag++;
	cbw->dCBWDataTransferLength = data_length;
	cbw->bmCBWFlags = direction;
	cbw->bCBWLUN = lun;
	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);
	
	r = usb_bulk_msg(udev,usb_sndbulkpipe(udev,endpoint),(void*)cbw,31, &size,1000);
	if(r!=0)
		printk("Failed command transfer %d",r);
	else 	
		printk("read capacity command sent successfully");
	
	printk("sent %d CDB bytes\n", cdb_len);
	return 0;
} 

static int test_mass_storage(struct usb_device *udev,uint8_t endpoint_in, uint8_t endpoint_out)

{	
	int r1=0,r=0,result;
	unsigned int size;
	uint8_t *lun=(uint8_t *)kmalloc(sizeof(uint8_t),GFP_KERNEL);
	uint8_t cdb[16];	// SCSI Command Descriptor Block
	uint8_t *buffer=(uint8_t *)kmalloc(64*sizeof(uint8_t),GFP_KERNEL);
	uint32_t expected_tag;
	unsigned long long max_lba, block_size;
	long device_size;
	printk("Reset mass storage device -->");
	r1 = usb_control_msg(udev,usb_sndctrlpipe(udev,0),BOMS_RESET,BOMS_RESET_REQ_TYPE,0,0,NULL,0,1000);
	if(r1<0)
		printk("error code: %d",r1);
	else
		printk("successful Reset");
	
	printk(KERN_INFO "Reading Max LUN -->\n");
	r = usb_control_msg(udev,usb_sndctrlpipe(udev,0),BOMS_GET_MAX_LUN,BOMS_GET_MAX_LUN_REQ_TYPE,0,0,(void*)lun,1,1000);
	if (r == 0) {
		*lun = 0;
	} 
	printk(KERN_INFO "Max LUN =%d \n",*lun); 

		printk("Reading Capacity -->\n");
		memset(buffer, 0, sizeof(buffer));
		memset(cdb, 0, sizeof(cdb));
		cdb[0] = 0x25;	// Read Capacity

 	send_command(udev,endpoint_out,*lun,cdb,USB_DIR_IN,READ_CAPACITY_LENGTH,&expected_tag);
	
	result=usb_bulk_msg(udev,usb_rcvbulkpipe(udev,endpoint_in),(void*)buffer,64,&size, 1000);
	if(result<0)
		printk("error in status %d",result);
	max_lba =   be_to_int32(&buffer[0]);
	block_size =be_to_int32(&buffer[4]);
	device_size = (long)((((max_lba + 1) / 1024) * block_size)/1024)	;
	printk(KERN_INFO "Max LBA: %lld, Block Size: %lld \n", max_lba, block_size); 
	printk("Device size(in MB) : %ld MB \n",device_size);
	printk("Device size(in GB) : %ld GB \n",(device_size/1024));
	get_mass_storage_status(udev, endpoint_in, expected_tag);
	return 0;
}

static int usbdev_probe(struct usb_interface *interface, const struct usb_device_id *id)
{	int i, type;
	unsigned char epAddr;
	struct usb_device *udev;
	struct usb_endpoint_descriptor *ep_desc;
	uint8_t endpoint_in = 0, endpoint_out = 0;
	if(id->idProduct == PENDRIVE_PID && id->idVendor==PENDRIVE_VID)
	{
		printk(KERN_INFO "“----------Known USB drive detected---------”\n");
	}
	udev=interface_to_usbdev(interface);

	printk("VID  %#06x\n",udev->descriptor.idVendor);
	printk("PID  %#06x\n",udev->descriptor.idProduct);
	printk(KERN_INFO "USB DEVICE CLASS: %x", interface->cur_altsetting->desc.bInterfaceClass);
	printk(KERN_INFO "USB INTERFACE SUB CLASS : %x", interface->cur_altsetting->desc.bInterfaceSubClass);
	printk(KERN_INFO "USB INTERFACE Protocol : %x", interface->cur_altsetting->desc.bInterfaceProtocol);
	printk(KERN_INFO "No. of Endpoints = %d \n", interface->cur_altsetting->desc.bNumEndpoints);

	for(i=0;i<interface->cur_altsetting->desc.bNumEndpoints;i++)
	{
		ep_desc = &interface->cur_altsetting->endpoint[i].desc;
		epAddr = ep_desc->bEndpointAddress;
		type=usb_endpoint_type(ep_desc);
		if(type==2){
		if(epAddr & 0x80)
		{		
			printk(KERN_INFO "EP %d is Bulk IN\n", i);
			endpoint_in=ep_desc->bEndpointAddress;
			
		}
		else
		{	
			endpoint_out=ep_desc->bEndpointAddress;
			printk(KERN_INFO "EP %d is Bulk OUT\n", i); 
		}
		}
		if(type==3)
		{
		if(epAddr && 0x80)
			printk(KERN_INFO "EP %d is Interrupt IN\n", i);
		else
			printk(KERN_INFO "EP %d is Interrupt OUT\n", i);
		}
		}
		if ((interface->cur_altsetting->desc.bInterfaceClass == 8)
			  && (interface->cur_altsetting->desc.bInterfaceSubClass == 6) 
			  && (interface->cur_altsetting->desc.bInterfaceProtocol == 80) ) 
			{
				// Mass storage devices that can use basic SCSI commands
				//test_mode = USE_SCSI;
				printk(KERN_INFO "Detected device is a valid SCSI mass storage device.\n \n");
				printk(KERN_INFO "*********Initiating SCSI Commands******\n");
			}

		else{
			printk(KERN_INFO "Detected device is not a valid SCSI mass storage device.\n");
		}

			test_mass_storage(udev,endpoint_in,endpoint_out);
return 0;
}

static struct usb_driver usbdev_driver = {
	name: "my_usb_device",  //name of the device
	probe: usbdev_probe, // Whenever Device is plugged in
	disconnect: usbdev_disconnect, // When we remove a device
	id_table: usbdev_table, //  List of devices served by this driver
};


int device_init(void)
{
	usb_register(&usbdev_driver);
	printk(KERN_NOTICE "UAS READ Capacity Driver Inserted\n");
	return 0;
}

void device_exit(void)
{
	usb_deregister(&usbdev_driver);
	printk("Device driver unregister");
}
module_init(device_init);
module_exit(device_exit);
MODULE_LICENSE("GPL");
