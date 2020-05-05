
#include<linux/kernel.h>
#include<linux/init.h>
#include<linux/module.h>
#include<linux/slab.h>
#include<linux/usb.h>
#include<linux/blkdev.h>
#include<linux/genhd.h>
#include<linux/spinlock.h>
#include <asm/highmem.h>
#include<linux/bio.h>
#include<linux/fs.h>
#include<linux/interrupt.h>
#include<linux/workqueue.h>
#include<linux/sched.h>

#define DEVICE_NAME "myUSBDriver"
#define __bio_kunmap_atomic(addr, kmtype) kunmap_atomic(addr)

#define PENDRIVE_VID        0x0781    //0x0781      // 0x1307  //   	  0x0781      // 	  0x1307  //   	 
#define PENDRIVE_PID       0x5567 //0x5567         // 0x0163  //0x5567         //    0x5567//


#define BOMS_RESET                    0xFF
#define BOMS_RESET_REQ_TYPE           0x21
#define BOMS_GET_MAX_LUN              0xFE
#define BOMS_GET_MAX_LUN_REQ_TYPE     0xA1
#define READ_CAPACITY_LENGTH	      0x08
#define REQUEST_DATA_LENGTH           0x12
#define	USB_ERROR_PIPE                -32
#define RETRY_MAX						5
#define REQUEST_SENSE_LENGTH          0x12
#define INQUIRY_LENGTH                0x24
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


static struct usb_device_id usbdev_table [] = {
	{USB_DEVICE(PENDRIVE_VID,PENDRIVE_PID)},
	{} /*terminating entry*/	
};

struct blkdev_private{
        int size;                       /* Device size in sectors */
        u8 *data;                       /* The data array */
        short users;                    /* How many users */
        short media_change;             /* Flag a media change? */
        spinlock_t lock;                /* For mutual exclusion */
        struct request_queue *queue;    /* The device request queue */
        struct gendisk *gd;             /* The gendisk structure */   
};	

struct usb_device *udev;
uint8_t endpoint_in , endpoint_out ;
unsigned long long total_sectors, sector_size;
struct gendisk *usb_disk = NULL;
struct request *req;
static struct blkdev_private *p_blkdev = NULL;

static void reset_recovery(void)  // for Stall conditions
{
	//---------------------------------Reset ----------------------------------------
	int r1;
	r1 = usb_control_msg(udev,usb_sndctrlpipe(udev,0),BOMS_RESET,BOMS_RESET_REQ_TYPE,0,0,NULL,0,1000);
	if(r1<0)
		printk("error code for failed reset recovery: %d",r1);
	else
		printk("successful Reset");
    //----------------------------------Halt------------------------------------------
	usb_clear_halt(udev,usb_rcvbulkpipe(udev,endpoint_in));
	usb_clear_halt(udev,usb_sndbulkpipe(udev,endpoint_out));
	
}

// for CSW status check 
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
		printk("get_mass_storage_status: mismatched tags (expected %08X, received %08X)\n",
			expected_tag, csw->dCSWTag);
		return -1;
	}

	if(csw->dCSWDataResidue!=0)
		printk("Data residue %d",csw->dCSWDataResidue);
	if (size != 13) {
		printk("   get_mass_storage_status: received %d bytes (expected 13)\n", size);
		return -1;
	}	
	printk(KERN_INFO "Mass Storage Status: %02X (%s)\n", csw->bCSWStatus, csw->bCSWStatus?"FAILED":"Success");
	if(csw->bCSWStatus)
	{
		if (csw->bCSWStatus == 1)
			return -2;

		if(csw->bCSWStatus == 2)
			reset_recovery();
	}
	return 0;
}  

// for CBW command transfer
static int send_command(struct usb_device *udev,uint8_t endpoint,
                         uint8_t *cdb, uint8_t direction, int data_length, uint32_t *ret_tag)
{
	
	static uint32_t tag = 1;
	int r;
	int i;
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
	cbw->bCBWLUN =0;
	cbw->bCBWCBLength = cdb_len;
	memcpy(cbw->CBWCB, cdb, cdb_len);
	
	i = 0;
	do {
		// The transfer length must always be exactly 31 bytes.
		r = usb_bulk_msg(udev,usb_sndbulkpipe(udev,endpoint),(void*)cbw,31, &size,1000);
		if (r == USB_ERROR_PIPE) {
			usb_clear_halt(udev,usb_rcvbulkpipe(udev,endpoint));
		}
		i++;
	} while ((r == USB_ERROR_PIPE) && (i<RETRY_MAX));

	if (r != 0) {
		printk("Failed command transfer,error code : %d",r);
		return -1;
	} 
	return 0;
} 

// ------------------------------------------------REQUEST SENSE SCSI Command------------------------------------------------
static void get_sense(struct usb_device *udev, uint8_t endpoint_in, uint8_t endpoint_out)
{
	uint8_t cdb[16];	// SCSI Command Descriptor Block
	uint8_t *sense=(uint8_t *)kmalloc(18*sizeof(uint8_t),GFP_KERNEL);
	uint32_t expected_tag;
	int size;
	int rc;

	// Request Sense
	printk("Request Sense:\n");
	memset(sense, 0, sizeof(sense));
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x03;	// Request Sense
	cdb[4] = REQUEST_SENSE_LENGTH;

	send_command(udev,endpoint_out,cdb,USB_DIR_IN,REQUEST_SENSE_LENGTH,&expected_tag);

	rc=usb_bulk_msg(udev,usb_rcvbulkpipe(udev,endpoint_in),(void*)sense,18,&size, 1000);
	if (rc < 0)
	{
		printk("usb_bulk_transfer failed, error code: %d\n",rc);
		return;
	}
	printk("received %d bytes\n", size);

	if ((sense[0] != 0x70) && (sense[0] != 0x71)) {
		printk("ERROR No sense data\n");
	} else {
		printk("ERROR Sense: %02X %02X %02X\n", sense[2]&0x0F, sense[12], sense[13]);
	}
	get_mass_storage_status(udev, endpoint_in, expected_tag);
}


//-----------------------------------------------READ CAPACITY SCSI Command-----------------------------------------------
static int usb_read_capacity(struct usb_device *udev,uint8_t lun,uint8_t endpoint_in, uint8_t endpoint_out)
{
	int result;
	unsigned int size;
	uint8_t cdb[16];	// SCSI Command Descriptor Block
	uint8_t *buffer=(uint8_t *)kmalloc(64*sizeof(uint8_t),GFP_KERNEL);
	uint32_t expected_tag;
	long device_size;
	
	printk("Reading Capacity -->\n");
	memset(buffer, 0, sizeof(buffer));
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x25;	// Read Capacity

 	send_command(udev,endpoint_out,cdb,USB_DIR_IN,READ_CAPACITY_LENGTH,&expected_tag);

	result=usb_bulk_msg(udev,usb_rcvbulkpipe(udev,endpoint_in),(void*)buffer,64,&size, 1000);
	if(result<0)
		printk("error in status %d",result);
	total_sectors = (be_to_int32(&buffer[0])+1);
	sector_size =be_to_int32(&buffer[4]);
	device_size =(long)((((total_sectors)/1024) * sector_size)/1024);
	printk(KERN_INFO "total_sectors: %lld, sector_size Size: %lld \n", total_sectors, sector_size); 
	printk("Device size(in MB) : %ld MB \n",device_size);
	printk("Device size(in GB) : %ld GB \n",(device_size/1024));
	if(get_mass_storage_status(udev, endpoint_in, expected_tag)==-2)
		get_sense(udev,endpoint_in,endpoint_out);
	return 0;
}

//------------------------------------------------------ READ(10) SCSI Command--------------------------------------------
static int usb_read(sector_t initial_sector,sector_t nr_sect,char *page_address)
{
int result;
unsigned int size;
uint8_t cdb[16];	// SCSI Command Descriptor block
uint32_t expected_tag;
size=0;
memset(cdb,0,sizeof(cdb));
cdb[0] = 0x28;	// Read(10)
cdb[2]=(initial_sector>>24) & 0xFF;
cdb[3]=(initial_sector>>16) & 0xFF;
cdb[4]=(initial_sector>>8) & 0xFF;
cdb[5]=(initial_sector>>0) & 0xFF;
cdb[7]=(nr_sect>>8) & 0xFF;
cdb[8]=(nr_sect>>0) & 0xFF;	// 1 block
printk("READ : initial_sector: %llu ,  total sectors to write: %llu  , Page Address : %x",initial_sector,nr_sect,page_address);

send_command(udev,endpoint_out,cdb,USB_DIR_IN,(nr_sect*512),&expected_tag);
result=usb_bulk_msg(udev,usb_rcvbulkpipe(udev,endpoint_in),(void*)(page_address),(nr_sect*512),&size, 5000);
if(result<0)
	printk("error in status %d",result);
if(get_mass_storage_status(udev, endpoint_in, expected_tag)==-2)
	get_sense(udev,endpoint_in,endpoint_out);
return 0;
}

//---------------------------------------------------------WRITE(10) SCSI Command-------------------------------------------
 static int usb_write(sector_t initial_sector,sector_t nr_sect,char *page_address)
{  
	int result;
	unsigned int size;
	uint8_t cdb[16];	// SCSI Command Descriptor Block
	uint32_t expected_tag;
	memset(cdb,0,sizeof(cdb));
	cdb[0]=0x2A;
	cdb[2]=(initial_sector>>24)&0xFF;
	cdb[3]=(initial_sector>>16)&0xFF;
	cdb[4]=(initial_sector>>8)&0xFF;
	cdb[5]=(initial_sector>>0)&0xFF;
	cdb[7]=(nr_sect>>8)&0xFF;
	cdb[8]=(nr_sect>>0)&0xFF;	
	cdb[8]=0x01;
	printk("WRITE : initial_sector: %llu ,  total sectors to write: %llu  , Page Address : %x",initial_sector,nr_sect,page_address);
	send_command(udev,endpoint_out,cdb,USB_DIR_OUT,nr_sect*512,&expected_tag);
	result=usb_bulk_msg(udev,usb_sndbulkpipe(udev,endpoint_out),(void*)page_address,nr_sect*512,&size, 1000);
	if(result<0)
		printk("error in status %d",result);
	if(get_mass_storage_status(udev, endpoint_in, expected_tag)==-2)
		get_sense(udev,endpoint_in,endpoint_out);
	return 0;
}  

//----------------------------------------------------------- INQUIRY SCSI Command-------------------------------------------
static int usb_inquiry(struct usb_device *udev,uint8_t lun,uint8_t endpoint_in, uint8_t endpoint_out)
{   
	int result;
	int i;
	unsigned int size;
	uint8_t cdb[16];	// SCSI Command Descriptor Block
	uint8_t *buffer=(uint8_t *)kmalloc(64*sizeof(uint8_t),GFP_KERNEL);
	uint32_t expected_tag;
	char vid[9], pid[9], rev[5];

	// Send Inquiry
	printk("Sending Inquiry---->\n");
	memset(buffer, 0, sizeof(buffer));
	memset(cdb, 0, sizeof(cdb));
	cdb[0] = 0x12;	// Inquiry
	cdb[4] = INQUIRY_LENGTH;

	send_command(udev,endpoint_out,cdb,USB_DIR_IN,INQUIRY_LENGTH,&expected_tag);
	result=usb_bulk_msg(udev,usb_rcvbulkpipe(udev,endpoint_in),(void*)buffer,64,&size, 1000);
	if(result<0)
		printk("error in status %d",result);
	
	for (i=0; i<8; i++) {
		vid[i] = buffer[8+i];
		pid[i] = buffer[16+i];
		rev[i/2] = buffer[32+i/2];	
	}
	vid[8] = 0;
	pid[8] = 0;
	rev[4] = 0;
	printk("   VID:PID:REV \"%8s\":\"%8s\":\"%4s\"\n", vid, pid, rev);
	if(get_mass_storage_status(udev, endpoint_in, expected_tag)==-2)
		get_sense(udev,endpoint_in,endpoint_out);
	return 0;
}  

static void usb_transfer(sector_t sector,sector_t nsect, char *buffer, int write)
{
    unsigned long offset = sector*512;
    unsigned long nbytes = nsect*512;

    if ((offset + nbytes) > (total_sectors*512)) {
        printk (KERN_NOTICE "Beyond-end write (%ld %ld)\n", offset, nbytes);
        return;
    }
     if (write)
        usb_write(sector,nsect,buffer);
    else
        usb_read(sector,nsect,buffer);
    return; 
}  

static int usb_xfer_each(struct request *req)
{
    struct bio_vec bvec;
    struct req_iterator iter;

    rq_for_each_segment(bvec,req,iter){
    	sector_t sector = req->bio->bi_iter.bi_sector;	
    	char *buffer = (kmap_atomic(bio_iter_iovec((req->bio), (iter.iter)).bv_page) +bio_iter_iovec((req->bio), (iter.iter)).bv_offset);	
    	usb_transfer(sector,((bvec.bv_len)/512),buffer, bio_data_dir(req->bio)==WRITE);
    	__bio_kunmap_atomic(req->bio, KM_USER0);
    }
    return 0; 
}  

static struct workqueue_struct *myqueue=NULL;
struct dev_work{   
	struct work_struct work; 
	struct request *req;
 };

// for Bottom Half
static void delay_function(struct work_struct *work)
{
	unsigned long flags;
	struct dev_work *usb_work=container_of(work,struct dev_work,work);
	usb_xfer_each(usb_work->req);
	spin_lock_irqsave(&p_blkdev->lock,flags);
	__blk_end_request_cur(usb_work->req,0);
	spin_unlock_irqrestore(&p_blkdev->lock,flags);
	kfree(usb_work);
	return;
}

// request function
void usb_request(struct request_queue *q)  
{
	struct request *req;  
	struct dev_work *usb_work=NULL;

	while((req=blk_fetch_request(q)) != NULL)
	{
		if(blk_rq_is_passthrough(req)) 
		{
			printk(KERN_INFO "non FS request");
			__blk_end_request_all(req, -EIO);
			continue;
		}
		usb_work=(struct dev_work *)kmalloc(sizeof(struct dev_work),GFP_ATOMIC);
		if(usb_work==NULL){

			printk("Memory Allocation to deferred work failed");
			__blk_end_request_all(req, 0);
			continue;
		}

		usb_work->req=req;
		INIT_WORK(&usb_work->work,delay_function);
		queue_work(myqueue,&usb_work->work);
	}	
} 

// RESET, MAX LUN 
static int test_mass_storage(struct usb_device *udev,uint8_t endpoint_in, uint8_t endpoint_out)

{	
	int r1=0,r=0;
	uint8_t *lun=(uint8_t *)kmalloc(sizeof(uint8_t),GFP_KERNEL);
	printk("Reset mass storage device ---->");
	r1 = usb_control_msg(udev,usb_sndctrlpipe(udev,0),BOMS_RESET,BOMS_RESET_REQ_TYPE,0,0,NULL,0,1000);
	if(r1<0)
		printk("error code: %d",r1);
	else
		printk("successful Reset");
	
	printk("Reading Max LUN ---->\n");
	r = usb_control_msg(udev,usb_sndctrlpipe(udev,0),BOMS_GET_MAX_LUN,BOMS_GET_MAX_LUN_REQ_TYPE,0,0,(void*)lun,1,1000);
	*lun = 0; 
	printk("Max LUN =%d \n",*lun); 
    usb_inquiry(udev,0,endpoint_in,endpoint_out);
    usb_read_capacity(udev,0,endpoint_in,endpoint_out);
	return 0; 
}

static int blkdev_open(struct block_device *bdev, fmode_t mode)       
{
    struct blkdev_private *dev = bdev->bd_disk->private_data;
    spin_lock(&dev->lock);
    if (! dev->users) 
        check_disk_change(bdev);	
    dev->users++;
    spin_unlock(&dev->lock);
    return 0;
}

static void blkdev_release(struct gendisk *gdisk, fmode_t mode)
{
    struct blkdev_private *dev = gdisk->private_data;
    spin_lock(&dev->lock);
    dev->users--;
    spin_unlock(&dev->lock);

    return ;
}

int blkdev_media_changed(struct gendisk *gdisk)
{
    struct blkdev_private *dev = gdisk->private_data;
    printk(KERN_INFO "INSIDE block_media_changed");
    return dev->media_change;
}


int blkdev_revalidate(struct gendisk *gdisk)
{
    struct blkdev_private *dev = gdisk->private_data;
	printk(KERN_INFO "INSIDE block_revalidate");
    if (dev->media_change) {
        dev->media_change = 0;
        memset (dev->data, 0, dev->size);
    }
    return 0;
}

static void usbdev_disconnect(struct usb_interface *interface)
{
	struct gendisk *usb_disk;
	printk(KERN_INFO "USBDEV Device Removed\n");
	usb_disk = p_blkdev->gd;
	del_gendisk(usb_disk);
	blk_cleanup_queue(p_blkdev->queue);
	kfree(p_blkdev);
	return;
}

static struct block_device_operations blkdev_ops =
{
	.owner= THIS_MODULE,
	.open=blkdev_open,
	.release=blkdev_release,
	.media_changed		= blkdev_media_changed,
	.revalidate_disk	= blkdev_revalidate
};

static int usbdev_probe(struct usb_interface *interface, const struct usb_device_id *id)
{	int i, type;
	int usb_major;
	unsigned char epAddr;
	struct usb_endpoint_descriptor *ep_desc;
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
			printk("endpoint_in : %x",endpoint_in);
			
		}
		else
		{	
			endpoint_out=ep_desc->bEndpointAddress;
			printk(KERN_INFO "EP %d is Bulk OUT\n", i); 
			printk("endpoint_out : %x",endpoint_out);
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

				printk(KERN_INFO "Detected device is a valid SCSI mass storage device.\n \n");
				printk(KERN_INFO "*********Initiating SCSI Commands******\n");
			}

		else{
			printk(KERN_INFO "Detected device is not a valid SCSI mass storage device.\n");
		}

	test_mass_storage(udev,endpoint_in,endpoint_out);

	
	usb_major=0;
	usb_major = register_blkdev(0, "USB DISK");  // to reg the device, to have a device name, and assign a major no
	if (usb_major < 0) 
		printk(KERN_WARNING "USB_Disk: unable to get major number\n");
	
	p_blkdev = kmalloc(sizeof(struct blkdev_private),GFP_KERNEL); // private structre m/m allocation 
	
	if(!p_blkdev)
	{
		printk("ENOMEM  at %d\n",__LINE__);
		return 0;
	}
	memset(p_blkdev, 0, sizeof(struct blkdev_private)); 

	spin_lock_init(&p_blkdev->lock);  
	p_blkdev->queue = blk_init_queue(usb_request, &p_blkdev->lock); 
	usb_disk = p_blkdev->gd = alloc_disk(2); 
	if(!usb_disk)
	{
		kfree(p_blkdev);
		printk(KERN_INFO "alloc_disk failed\n");
		return 0;
	}
	// below all are part of gendisk structure
	usb_disk->major =usb_major;
	usb_disk->first_minor = 0;
	usb_disk->fops = &blkdev_ops;
	usb_disk->queue = p_blkdev->queue;
	usb_disk->private_data = p_blkdev;
	strcpy(usb_disk->disk_name, DEVICE_NAME);
	set_capacity(usb_disk,total_sectors); // defined 
	add_disk(usb_disk);  // do add disk now

return 0;
}

static struct usb_driver usbdev_driver = {
	name: "my_usb_device",  //name of the device
	probe: usbdev_probe, // Whenever Device is plugged in
	disconnect: usbdev_disconnect, // When we remove a device
	id_table: usbdev_table, //  List of devices served by this driver
};

int block_init(void)
{
	usb_register(&usbdev_driver);
	printk(KERN_NOTICE "UAS READ Capacity Driver Inserted\n");
	printk(KERN_INFO "Registered disk\n"); 
	myqueue=create_workqueue("myqueue");  // my worker thread name
	return 0;	
}

void block_exit(void)
{ 
	usb_deregister(&usbdev_driver);
	printk("Device driver unregister");
	flush_workqueue(myqueue);  // to exit the work done
	destroy_workqueue(myqueue);
	return;
}


module_init(block_init);
module_exit(block_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("KARAVI BHARANI & SANJANA LAHANE");
MODULE_DESCRIPTION("USB DRIVE");