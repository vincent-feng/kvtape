/**
 * @file   kvtape.c
 * @author vincent.feng <vincent.feng@yahoo.com>
 * @date   Thu Feb 28 12:17:39 2013
 * 
 * @brief  
 *
 * kernel space virtual tape emulation.
 * So far, the LLD driver works well for tar and mt command.
 * 
 */
#include <linux/module.h>
#include <linux/device.h> 
#include <linux/highmem.h> 
#include <scsi/scsi_host.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/scatterlist.h>
#include "kernel_fop.h"

/*If not define following macros, "Unknown symbol driver_register" similar errors appears. */
#ifdef MODULE
MODULE_AUTHOR("vincent");
MODULE_DESCRIPTION("kernel space virtual tape driver");
MODULE_SUPPORTED_DEVICE("st, sg");

#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif
#endif	/* MODULE */

#define MAX_COMMANDS_PER_LUN 16
#define MAX_LUNS  8
#define MAX_IOV_SLOTS 8
#define MAX_SECTORS_PER_CMD  128
#define MAX_TARGET_IDS	8
#define MAX_LUNS  8
#define MAX_CDB_LEN 12

//static struct device scsi_dev;
static struct Scsi_Host *shost;
static int vdisk_fd = -1;
static int cur_record_no = 0;

#define DEBUG_PRINT 1

enum _filemark {
    NOT_MARK,
    FILEMARK,
    SETMARK,
    DATAMARK
};

typedef struct {
    struct work_struct work;
    struct scsi_cmnd* cmnd;
    void (*done)(struct scsi_cmnd*);
} my_work_t;

//data returned by request sense command.
union sense_data {
    struct  {
        int8_t byte0;
        int8_t segment_num;
        int8_t sense_key:4;
        int8_t Reserved:1;
        int8_t ili:1;
        int8_t eom:1;
        int8_t filemark:1;
        int8_t info[4];
        int8_t additional_sense_len;
        int8_t cmd_info[4];
        int8_t asense_key;
        int8_t asense_key_q;
        int8_t field_rep_unit_code;
        int8_t sense_key_spec[3];    
    } bits;
    unsigned char data[18];
};

static void fill_inquriy_response(char* buf)
{
    union inquiry_data {
        unsigned char data[36];
        struct  {
            int8_t peripheral_type:5;
            int8_t peripheral_qualifier:3;
            int8_t dev_type_modifier:7;
            int8_t rmb:1;
            int8_t ansi_ver:3;
            int8_t ecma_ver:3;
            int8_t iso_ver:2;
            int8_t response_format:4;
            int8_t reserved0:2;
            int8_t trmiop:1;
            int8_t aenc:1;
            int8_t additional_len;
            int8_t reserved1;
            int8_t reserved2;
            int8_t sftre:1;
            int8_t cmdque:1;
            int8_t reserved3:1;
            int8_t linked:1;
            int8_t sync:1;
            int8_t wbus16:1;
            int8_t wbus32:1;
            int8_t reladr:1;
            int8_t vendor_identify[8];
            int8_t product_identify[16];
            int8_t product_revision_lev[4];
        } fields;
    };
	
    union inquiry_data inquiry_response;
	inquiry_response.fields.peripheral_type = 0x01;//tape
    inquiry_response.fields.peripheral_qualifier = 0x00; 
    inquiry_response.fields.dev_type_modifier = 0x00;
    inquiry_response.fields.rmb = 0x01;//removealbe
    inquiry_response.fields.ansi_ver = 0x02;//support scsi-2 standard
    inquiry_response.fields.ecma_ver = 0x00;    
    inquiry_response.fields.iso_ver = 0x00;
    //inquiry data format defined by scsi-2 
    inquiry_response.fields.response_format = 0x02;
    inquiry_response.fields.reserved0 = 0x00;
    //doesn't support TERMINATE IO PROCESS message.
    inquiry_response.fields.trmiop = 0x00;
    inquiry_response.fields.aenc = 0x00;
    //inquiry_response is 36 bytes long
    inquiry_response.fields.additional_len = 31; 
    inquiry_response.fields.reserved1 = 0x00;
    inquiry_response.fields.reserved2 = 0x00;
    //respond RESET condition with HARD RESET alternative(scsi-2 6.2.2.1)
    inquiry_response.fields.sftre = 0x00;
    //command queue is not supported.
    inquiry_response.fields.cmdque = 0x00;
    
    inquiry_response.fields.reserved3 = 0x00;
    //At present, linked command is not supported
    inquiry_response.fields.linked = 0x00;
    //synchrouns data transfer is supported.
    inquiry_response.fields.sync = 0x01; 
    //At present, one byte and two bytes data transfer are supported.
    inquiry_response.fields.wbus16 = 0x00;
    inquiry_response.fields.wbus32 = 0x00;
    inquiry_response.fields.reladr = 0x00;    
    memcpy(inquiry_response.fields.vendor_identify, "virtual ", 8);
    memcpy(inquiry_response.fields.product_identify, "Scsitape  (c)vincent", 16);
    memcpy(inquiry_response.fields.product_revision_lev, "0200", 4);
    memcpy(buf, inquiry_response.data, 0x24);
}

//scatterlist must be chained.
struct scatterlist* sg_next(struct scatterlist* sg)
{
    return sg + 1;
}

static void do_inquiry(struct scsi_cmnd *cmnd)
{    
    char* va = NULL;
    if (scsi_sg_count(cmnd)) {
        struct scatterlist* sg = NULL;
	int i = 0;
	scsi_for_each_sg(cmnd, sg, scsi_sg_count(cmnd), i) {
	    va = kmap(sg_page(sg)) + sg->offset;
            printk("\nscatter list buf[%d], len:%d", i, sg->length);
            if (sg->length >= 0x24) {
                fill_inquriy_response(va);
            }
            kunmap(sg_page(sg));
            break;
	}       
    } else {        
		printk("\nkvtape error %s: sg_count is 0\n",__func__);
    }
}

static void gen_get_filemark_sense(struct scsi_cmnd *cmnd, char* sense_buf, int remain)
{
    int tranfer_len = 0;
    char* p = NULL;

    union sense_data sense_invalide_opcode;
    memset(sense_invalide_opcode.data, 0, sizeof(union sense_data));
    sense_invalide_opcode.bits.byte0 = 0xF0;//current error code.
    sense_invalide_opcode.bits.filemark = 0x01;
    sense_invalide_opcode.bits.sense_key = 0x00;//no sense
    sense_invalide_opcode.bits.asense_key = 0x00;
    sense_invalide_opcode.bits.asense_key_q = 0x00;

    switch(cmnd->cmnd[0]) {
    case 0x08://read
        tranfer_len = (unsigned int)cmnd->cmnd[2] << 16;
        tranfer_len += (unsigned int)cmnd->cmnd[3] << 8;
        tranfer_len += (unsigned int)cmnd->cmnd[4];
        if (cmnd->cmnd[1] & 0x01) {
            tranfer_len -= remain/0x8000;//lu[req->lun].sector_size;
        } else {
            tranfer_len -= remain;
        }
        break;

    case 0x11://space
        tranfer_len = (unsigned int)cmnd->cmnd[2] << 16;
        tranfer_len += (unsigned int)cmnd->cmnd[3] << 8;
        tranfer_len += (unsigned int)cmnd->cmnd[4];
        tranfer_len -= remain;
        break;

    default:
        break;
    }
    
    p = (char*)&tranfer_len;                
    sense_invalide_opcode.bits.info[0] = *(p + 3);
    sense_invalide_opcode.bits.info[1] = *(p + 2);
    sense_invalide_opcode.bits.info[2] = *(p + 1);
    sense_invalide_opcode.bits.info[3] = *(p + 0);
    memcpy(sense_buf, sense_invalide_opcode.data, sizeof(sense_invalide_opcode.data));
}

static void gen_get_setmark_sense(struct scsi_cmnd *cmnd, char* sense_buf, int remain)
{
    int tranfer_len = 0;
    char* p = NULL;
    union sense_data sense_invalide_opcode;
    memset(sense_invalide_opcode.data, 0, sizeof(union sense_data));
    memset(sense_invalide_opcode.data, 0, sizeof(union sense_data));
    sense_invalide_opcode.bits.byte0 = 0xF0;//current error code.
    sense_invalide_opcode.bits.filemark = 0x01;
    sense_invalide_opcode.bits.sense_key = 0x00;
    sense_invalide_opcode.bits.asense_key = 0x00; 
    sense_invalide_opcode.bits.asense_key_q = 0x03;//setmark found

    
    switch(cmnd->cmnd[0]) {
    case 0x08://read
        tranfer_len = (unsigned int)cmnd->cmnd[2] << 16;
        tranfer_len += (unsigned int)cmnd->cmnd[3] << 8;
        tranfer_len += (unsigned int)cmnd->cmnd[4];
        if (cmnd->cmnd[1] & 0x01) {
            tranfer_len -= remain/0x8000;//lu[req->lun].sector_size;
        } else {
            tranfer_len -= remain;
        }
        break;

    case 0x11://space
        tranfer_len = (unsigned int)cmnd->cmnd[2] << 16;
        tranfer_len += (unsigned int)cmnd->cmnd[3] << 8;
        tranfer_len += (unsigned int)cmnd->cmnd[4];
        tranfer_len -= remain;
        break;

    default:
        break;
    }
    p = (char*)&tranfer_len;                
    sense_invalide_opcode.bits.info[0] = *(p + 3);
    sense_invalide_opcode.bits.info[1] = *(p + 2);
    sense_invalide_opcode.bits.info[2] = *(p + 1);
    sense_invalide_opcode.bits.info[3] = *(p + 0);
    memcpy(sense_buf, sense_invalide_opcode.data, sizeof(sense_invalide_opcode.data));
}

static void do_test_unit_ready(struct scsi_cmnd *cmnd)
{
    //do nothing.
}

static void do_rewind(struct scsi_cmnd *cmnd)
{
    kernel_file_seek(vdisk_fd, 0, SEEK_SET);
}


static void do_erase(struct scsi_cmnd *cmnd)
{
    //do nothing.
}

static void do_mode_select6(struct scsi_cmnd *cmnd)
{   
    char* buf = NULL;
    printk("\ndo_mode_select6, use_sg:%d\n\n",scsi_sg_count(cmnd));

    if (scsi_sg_count(cmnd)) {
        struct scatterlist* sg = NULL;
	int i = 0;
	scsi_for_each_sg(cmnd, sg, scsi_sg_count(cmnd), i) {
	    buf = kmap(sg_page(sg)) + sg->offset;           
            if (buf[3] == 8) {
                uint32_t blk_size = buf[9];
                blk_size = (blk_size<<8) + buf[10];
                blk_size = (blk_size<<8) + buf[11];
                printk("\ntape mode select set block size:%d\n", blk_size);
            }
            kunmap(sg_page(sg));
            break;
	}       
    } else {
		printk("\nkvtape error %s: sg_count is 0\n",__func__);
    }    
}

//read next record length.
static int do_read_recordlen(void)
{
    int len = 0;
    int ret = kernel_file_read(vdisk_fd, &len, 4);
    if (4 == ret) {
        return len;
    } else {
        return -1;
    }
}


static void  do_space_blocks(struct scsi_cmnd* cmnd, uint32_t space_cnt)
{	
    int32_t record_len = 0;
	uint8_t tape_mark = 0;
    int request_data_len = space_cnt;
    while (request_data_len > 0) {
        record_len = do_read_recordlen();
		if (record_len < 0) {//check end of partition
			//gen_endpartition_sense(req);
			return;
		}else if (0 == record_len) {//check end of data
			//gen_enddata_sense(req);
			return;
		} else if (1 == record_len) {//check tape mark
            kernel_file_read(vdisk_fd, &record_len, 1);            
            if (FILEMARK == tape_mark) {
                gen_get_filemark_sense(cmnd, cmnd->sense_buffer, request_data_len);                
                return;
            } else if (SETMARK == tape_mark) {                
                gen_get_setmark_sense(cmnd, cmnd->sense_buffer, request_data_len);
                return;
            } else {// 1 byte record is strange.
                request_data_len--;
                continue;
            }
        }
		//normal record.
        kernel_file_seek(vdisk_fd, record_len, SEEK_CUR);            
        request_data_len--;
    } 
}

static void  do_space_filemark(struct scsi_cmnd* cmnd, uint32_t space_cnt)
{
    int32_t record_len = 0;
    uint8_t tape_mark = 0;

	int request_data_len = space_cnt;
    while (request_data_len > 0) {
        record_len = do_read_recordlen();
		if (record_len < 0) {
			//gen_endpartition_sense(req);
            printk("\ntape recordlen < 0, should gen_endpartition_sense\n");
			return;
		}else if (0 == record_len) {
			//gen_enddata_sense(req);
            printk("\ntape recordlen == 0, should gen_enddata_sense\n");
			return;
		}
		
        if (record_len > 1) {
            kernel_file_seek(vdisk_fd, record_len, SEEK_CUR);            
        } else {//check setmark
            kernel_file_read(vdisk_fd, &tape_mark, 1);
            if (FILEMARK == tape_mark) {
                request_data_len--;
            } else if (SETMARK == tape_mark) {
                printk("\nSCSI_TAPE get setmark when space filemark\n");
                //gen_get_setmark_sense(req);
				return;
            } else {//1 byte regular record.
                kernel_file_seek(vdisk_fd, record_len, SEEK_CUR);            
            }
        }
    }
}

static void do_space(struct scsi_cmnd* cmnd)
{
    uint8_t space_type = cmnd->cmnd[1] & 0x07;
    uint32_t space_cnt = (uint32_t)cmnd->cmnd[2] << 16;
    space_cnt += (uint32_t)cmnd->cmnd[3] << 8;
    space_cnt += (uint32_t)cmnd->cmnd[4];

    switch (space_type) {
    case 0://space blocks
        do_space_blocks(cmnd, space_cnt);
        break;

    case 1://space filemark
        do_space_filemark(cmnd, space_cnt);
        break;

    default:
        printk("\nSCSI_TAPE not supported space type:%d\n", space_type);
        break;
    }
}

static void do_read_position(struct scsi_cmnd* cmnd)
{
    char* p = (char*)&cur_record_no;
    char* buf = NULL;
   
    uint8_t databuf[20] = {
        0x04,
        0x00,
        0x00,
        0x00,
        *(p + 3),
        *(p + 2),
        *(p + 1),
        *(p + 0),
        *(p + 3),
        *(p + 2),
        *(p + 1),
        *(p + 0),
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00
    };
    
   buf = NULL;
   if (scsi_sg_count(cmnd)) {
		struct scatterlist* sg = NULL;
		int i = 0;
		scsi_for_each_sg(cmnd, sg, scsi_sg_count(cmnd), i) {
			buf = kmap(sg_page(sg)) + sg->offset;           
            memcpy(buf, p, sizeof(databuf));
            kunmap(sg_page(sg));
            break;
		}       
    }  else { 
		printk("\nkvtape error in %s: sg_count is 0\n",__func__);
    }
}


//return -1 chk condition, 0 -ok
static int fill_one_record(struct scsi_cmnd *cmnd, char* buf, int len)
{
    unsigned char tape_mark = 0;
    int record_len = 0;    
    uint32_t read_cnt = 0;

    while (len > 0) {
        record_len = do_read_recordlen();
        printk("\n%s record_len:%d\n",__func__,record_len);
        if (record_len < 0) {
            return 0;
        }

        if (1 == record_len) { //check tape mark
            kernel_file_read(vdisk_fd, &tape_mark, 1);
            if (FILEMARK == tape_mark) {//get filemar
                //fix me, len is not correct.
                gen_get_filemark_sense(cmnd, cmnd->sense_buffer, len);
                cmnd->result = DRIVER_SENSE;
                goto err;
            } else if (SETMARK == tape_mark) {//get setmark
                //fix me, len is not correct.
                gen_get_setmark_sense(cmnd, cmnd->sense_buffer, len);
                cmnd->result = DRIVER_SENSE;
                goto err;
            } else {//Not sure if there is 1 byte record.
                printk("\ntape_error: encounter 1 byte record!\n");
                /* req->data_buf[read_cnt] = tape_mark; */
                read_cnt++;
                len--;
                continue;
            }
        }

        if (len < record_len) {
            int ret = kernel_file_read(vdisk_fd, buf + read_cnt, len);
            printk("\n%s len:%d, record_len%d,kernel_file_read return %d\n",__func__, len, record_len, ret);
            read_cnt += len;
            len = 0;
        } else {
            int ret = kernel_file_read(vdisk_fd, buf + read_cnt, record_len);
            printk("\n%s len:%d, record_len%d,kernel_file_read return %d\n",__func__, len, record_len, ret);
            read_cnt += record_len;
            len -= record_len;
        }
    }
    return 0;

 err:
    return -1;
}

/** 
 * Read fix block length data or variable block length data. For variable block length, after read, if file position
 * locates within block, skip to end of the block. That is, one block is not allowed divided to two read operations.
 *
 * @param req 
 */
static void do_read(struct scsi_cmnd *cmnd)
{
    char* buf = NULL;//cmnd->request_buffer;

    int request_data_len = (uint32_t)cmnd->cmnd[2] << 16;
    request_data_len += (uint32_t)cmnd->cmnd[3] << 8;
    request_data_len += (uint32_t)cmnd->cmnd[4];
    if (0 == (cmnd->cmnd[1] & 0x01)) {
        printk("\ntape read variable blocksize, transferlen:%d, use_sg:%d ", request_data_len, scsi_sg_count(cmnd));
    } else {
        printk("\ntape read fixed blocksize %d blocks, blocksize:0x8000, use_sg:%d ", request_data_len, scsi_sg_count(cmnd));
        request_data_len *= 0x8000;
    }


    if (scsi_sg_count(cmnd)) {
        struct scatterlist* sg = NULL;
		int i = 0;
		int ret = 0;
        int buf_len = 0;
		scsi_for_each_sg(cmnd, sg, scsi_sg_count(cmnd), i) {
			buf = kmap(sg_page(sg)) + sg->offset;           
			buf_len = request_data_len < sg->length? request_data_len : sg->length;
            ret = fill_one_record(cmnd, buf, buf_len);
            kunmap(sg_page(sg));
 
            if (ret < 0) {
                break;
            }
            request_data_len -= buf_len;
            if (request_data_len <= 0) {
                break;
            }
		}       
    } else {
        //buf = cmnd->request_buffer;
        //fill_one_record(cmnd, buf, request_data_len);
        printk("\nkvtape error %s: sg_count is 0\n",__func__);
    }
}


static void do_write(struct scsi_cmnd *cmnd)
{
    int ret = 0;
    char* buf = NULL;//cmnd->request_buffer;
    int transfer_len = (uint32_t)cmnd->cmnd[2] << 16;
    transfer_len += (uint32_t)cmnd->cmnd[3] << 8;
    transfer_len += (uint32_t)cmnd->cmnd[4];
    if (0 == (cmnd->cmnd[1] & 0x01)) {
        printk("\ntape write variable blocksize, transferlen:%d", transfer_len);
    } else {
        printk("\ntape write fixed blocksize %d blocks, blocksize:0x8000", transfer_len);
        transfer_len *= 0x8000;
    }
    
    //write record len
    ret = kernel_file_write(vdisk_fd, &transfer_len, 4);

    if (scsi_sg_count(cmnd)) {
        struct scatterlist* sg = NULL;
		int i = 0;
		int ret = 0;  
		scsi_for_each_sg(cmnd, sg, scsi_sg_count(cmnd), i) {
			int real_transfer = 0;
			buf = kmap(sg_page(sg)) + sg->offset;           
			if (transfer_len >= sg[i].length) {
                transfer_len -= sg[i].length;
                real_transfer = sg[i].length;
            } else {
                real_transfer = transfer_len;
                transfer_len = 0;
            }
			ret = kernel_file_write(vdisk_fd, buf, real_transfer);
            kunmap(sg_page(sg));
            if (0 == transfer_len) {
                break;
            }
		}       
    } else {
        //ret = kernel_file_write(vdisk_fd, buf, transfer_len);
        //printk("\nwrite 6, write len:%d", ret);
		printk("\nkvtape error %s: sg_count is 0\n",__func__);
    }    
}

static void do_write_filemark(struct scsi_cmnd *cmnd)
{
    uint8_t mark = FILEMARK;
    int mark_len = 1;
    uint32_t mark_count = cmnd->cmnd[2];
    mark_count = (mark_count << 8) + cmnd->cmnd[3];
    mark_count = (mark_count << 8) + cmnd->cmnd[4];
    
    if (cmnd->cmnd[1] & 0x020) {//setmark
        mark = SETMARK;        
    } else {//filemark
        mark = FILEMARK;
    }

    while (mark_count > 0) {
        int ret = kernel_file_write(vdisk_fd, &mark_len, 1);
        ret = kernel_file_write(vdisk_fd, &mark, 1);
        mark_count--;
        cur_record_no++;
    }
}

static void do_mode_sense6(struct scsi_cmnd *cmnd)
{
    char* buf = NULL;
    char header[4] = {0};
    char blk_descriptor[8];

    header[0] = 0x00;//actual mode parameter list length - 1.
    header[1] = 0x00;//default media type, current mounted.
    header[2] = 0x00;//device is write enable.
    /*
      Total block descriptors length. Only contains one block descriptor.
    */
    header[3] = 0x08;

    /*
      (2)prepare block descriptor. We just need one block descriptor and
      every block descriptor is 8 bytes.
    */    

    blk_descriptor[0] = 0x01;
    blk_descriptor[1] = 0x00;
    blk_descriptor[2] = 0x00;
    blk_descriptor[3] = 0x00;
    blk_descriptor[4] = 0x00;
    blk_descriptor[5] = 0x00;
    blk_descriptor[6] = 0x00;
    blk_descriptor[7] = 0x00;

    header[0] = 8 + 4 -1;

    buf = NULL;//cmnd->request_buffer;
    if (scsi_sg_count(cmnd)) {  
        struct scatterlist* sg = NULL;
	int i = 0;
	scsi_for_each_sg(cmnd, sg, scsi_sg_count(cmnd), i) {	
	    buf = kmap(sg_page(sg)) + sg->offset;           
	    memcpy(buf, header, 4);
            memcpy(buf + 4, blk_descriptor, 8);
            kunmap(sg_page(sg));
            break;            
	}       
    } else {  
		printk("\nkvtape error in %s: sg_count is 0\n",__func__);
    }    
}

static void do_read_blocklimit(struct scsi_cmnd *cmnd)
{
   char* buf = NULL;//cmnd->request_buffer;
    /*variable block length range is not specified.*/
    char data_buf[6] = {0};
    data_buf[0] = 0;
    data_buf[1] = 0x00;
    data_buf[2] = 0x40;
    data_buf[3] = 0x00;
    data_buf[4] = 0x00;
    data_buf[5] = 0x01;

    if (scsi_sg_count(cmnd)) {  
        struct scatterlist* sg = NULL;
		int i = 0;
		scsi_for_each_sg(cmnd, sg, scsi_sg_count(cmnd), i) {	
			buf = kmap(sg_page(sg)) + sg->offset;           
			memcpy(buf, data_buf, 6);
            kunmap(sg_page(sg));
            break;            
		}       
    } else {    
      printk("\nkvtape error %s: sg_count is 0\n",__func__);
    }        
}

/*
  Every SCSI command passed from mid level through queuecommand will be queued, 
  and processed by this function.
*/
static void scsi_cmd_handler(struct work_struct *work)
{
    my_work_t* my_work = container_of(work, my_work_t, work);
    unsigned short i = 0;

    printk("\n do my_wq_function, cmnd->use_sg:%d\n", 
        scsi_sg_count(my_work->cmnd));
#ifdef DEBUG_PRINT
    while (i < my_work->cmnd->cmd_len) {
        printk("\ncdb[%d]:0x%x\n", i, my_work->cmnd->cmnd[i]);
    i++;
    }
#endif
    
    switch (my_work->cmnd->cmnd[0]) {
    case 0x12://inqiury
        do_inquiry(my_work->cmnd);
        break;
    case 0x00: //test unit ready
        do_test_unit_ready(my_work->cmnd);
        break;
    case 0x01://rewind
        do_rewind(my_work->cmnd);
        break;
    case 0x19://erase
        do_erase(my_work->cmnd);
        break;
    case 0x1A://mode sense6
        do_mode_sense6(my_work->cmnd);
        break;
    case 0x15://mode select6
        do_mode_select6(my_work->cmnd);
        break;
    case 0x05://read block limit
        do_read_blocklimit(my_work->cmnd);
        break;
    case 0x08: //read
        do_read(my_work->cmnd);
        break;
    case 0x0A://write
        do_write(my_work->cmnd);
        break;
    case 0x10://write file mark
        do_write_filemark(my_work->cmnd);
        break;
    case 0x11://space
        do_space(my_work->cmnd);
        break;
    case 0x34:
        do_read_position(my_work->cmnd);
        break;
    default:
        printk("\ncdb[0]:0x%x is not supported\n", my_work->cmnd->cmnd[0]);
        break;
    }
    scsi_set_resid(my_work->cmnd, 0); 
    my_work->done(my_work->cmnd);
    kfree((void *)my_work);
    return;
}


static int kvtape_initiator_queuecommand(struct scsi_cmnd *cmnd,  void (*done)(struct scsi_cmnd*))
{
    my_work_t* work_ptr = (my_work_t*)kmalloc(sizeof(my_work_t), GFP_KERNEL);
 
    if (work_ptr) {     
        work_ptr->cmnd = cmnd;
        work_ptr->done = done;
        INIT_WORK(&work_ptr->work, scsi_cmd_handler);        
        schedule_work(&work_ptr->work);
    }
    return 0;
}

static int kvtape_initiator_abort(struct scsi_cmnd *Cmnd)
{
   printk("\n do kvtape_initiator_abort\n");
   return 0;
}

static int kvtape_initiator_reset(struct scsi_cmnd *Cmnd)
{
   printk("\n do kvtape_initiator_reset\n");
   return 0;
}

int kvtape_initiator_proc_info(struct Scsi_Host *sh, char *buffer, char **start,
                          off_t offset, int length, int inout)
{
   printk("\n do kvtape_initiator_proc_info\n");
   return 0;
}

static struct scsi_host_template driver_template = {
      proc_info:kvtape_initiator_proc_info,
      module:THIS_MODULE,
      name:"kvtape",
      info:NULL,
      ioctl:NULL,
      detect:NULL,
      release:NULL,
      queuecommand:kvtape_initiator_queuecommand,
      //eh_strategy_handler:NULL,
      eh_abort_handler:kvtape_initiator_abort,
      eh_device_reset_handler:kvtape_initiator_reset,
      eh_bus_reset_handler:NULL,
      eh_host_reset_handler:NULL,
      bios_param:NULL,
      /* max no. of simultaneously active SCSI commands driver can accept */
      can_queue:MAX_COMMANDS_PER_LUN * MAX_LUNS,
      this_id:-1,		/* our host has no id on the SCSI bus */
      /* max no. of simultaneously active SCSI commands driver can accept */
      sg_tablesize:MAX_IOV_SLOTS - 4,
      /* max no. of sectors driver can accept in 1 SCSI READ/WRITE command */
      /* SET BY A MODULE PARAMETER, see max_sectors above */
      max_sectors: 0,
      /* max no. of simultaneously outstanding commands per LUN */
      cmd_per_lun:MAX_COMMANDS_PER_LUN,
      /* no. of cards of this type found on this machine */
      present:0,
      unchecked_isa_dma:0,
      /* midlevel should merge requests for adjacent blocks of memory */
      use_clustering:ENABLE_CLUSTERING,
      /* The version that introduces this is greater than 2.6.18, but may be
       * less than 2.6.20 */
      /** This is an initiator device. */
      //supported_mode:MODE_INITIATOR,
      /** There is not settling. */
      skip_settle_delay:1,
      emulated:0,
/* proc_name xxx becomes the name for the directory /proc/scsi/xxx */
      proc_name:"kvtape"
};


static int kvtape_bus_match(struct device *dev, struct device_driver *dev_driver)
{
	printk("%s does nothing, driver->name:%s\n", __func__, dev_driver->name);
	return 1;
}	

static struct scsi_device* add_scsi_target(int target_id, int lun)
{
    struct scsi_device* sd = NULL;    
    printk("%s call into add_scsi_target\n", __func__);
    sd = (struct scsi_device*)scsi_add_device(shost, 0, target_id, lun);
    printk("\nadd_scsi_target, sd:%p \n",sd);
    return sd;
}

static void remove_scsi_target(struct scsi_device *sdev)
{
    printk("\ndo remove_scsi_target\n");
}

struct scsi_device* sdev1 = NULL;

static int kvtape_probe(struct device *dev)
{
	int retval = 0;

	printk("\ndo kvtape_probe\n");
	driver_template.max_sectors = MAX_SECTORS_PER_CMD;

	printk("%s call into scsi_host_alloc\n", __func__);
	shost = scsi_host_alloc(&driver_template,  0);
	printk("%s back from scsi_host_alloc, shost %p\n",	__func__, shost);

	if (unlikely(shost == NULL)) {
		printk("%s scsi_host_alloc failed\n", __func__);
		retval = -ENODEV;
		goto out;
	}

	shost->max_id = MAX_TARGET_IDS;
	shost->max_lun = MAX_LUNS;
	shost->max_cmd_len = MAX_CDB_LEN;
	//shost->hostdata[0] = (unsigned long)hostdata;
	retval = scsi_add_host(shost, dev);

	if (unlikely(retval)) {
		printk("%s scsi_add_host failed\n", __func__);
		retval = -ENODEV;
		scsi_host_put(shost);
	} else {
		/* Initialize the adapter's private data structure */
		printk("%s After scsi_add_host ok, call init_initiator and add_scsi_target\n", __func__);
	    if (NULL == sdev1) {
            sdev1 = add_scsi_target(1, 0);
            printk("\nkvtape_probe, sdev1:%p \n",sdev1);
        }
        retval = 0;
	}

out:
	return retval;
}	/* kvtape_probe */


int kvtape_remove(struct device *dev)
{
	int ret = 0;
	printk("\nenter %s, sdev1:%p \n",__func__, sdev1);
    if (NULL != sdev1) {
        remove_scsi_target(sdev1);
    }
 
    scsi_remove_host(shost);
    printk("%s back from scsi_remove_host\n",__func__);
    scsi_host_put(shost);
    printk("%s back from scsi_host_put\n",	__func__);
    return ret;
}

static void kvtape_device_release(struct device *dev)
{
	printk("\ncall into %s, do nothing\n", __func__);
	//scsi_unregister(shost);
    return;
}	/* kvtape_device_release */

static struct bus_type kvtape_bus = {
	.name = "kvtape_bus",
	.match = kvtape_bus_match,
};

struct device_driver kvtape_driver = {
	.name = "kvtape",
	.bus = &kvtape_bus,
	.probe = kvtape_probe,
	.remove = kvtape_remove
};


static struct device kvtape_pseudo = {
 	.init_name = "kvtape",
	.release = kvtape_device_release,
    .bus = &kvtape_bus
};


int init_module(void)
{
	int err = 0;

    printk("\nhello, vincent\n");
	printk("%s call into bus_register(&kvtape_bus %p)\n",	__func__, &kvtape_bus);
	err = bus_register(&kvtape_bus);
	printk("%s back from bus_register(&kvtape_bus %p), err %d\n",	__func__, &kvtape_bus, err);

	if (err) {
		goto out;
	}

	printk("%s call into driver_register(&kvtape_driver %p)\n",	__func__, &kvtape_driver);
	err = driver_register(&kvtape_driver);
	printk("%s back from driver_register(&kvtape_driver %p), err %d\n",	__func__, &kvtape_driver, err);

	printk("%s call into device_register(&kvtape_pseudo %p)\n",	__func__, &kvtape_pseudo);
	err = device_register(&kvtape_pseudo);
	printk("%s back from device_register(&kvtape_pseudo %p), err %d\n",__func__, &kvtape_pseudo, err);

	if (err) {
		printk("%s device_register(&kvtape_pseudo %p) failed %d\n",	__func__, &kvtape_pseudo, err);
		put_device(&kvtape_pseudo);	/* yes, even on an error! */
		goto out;
	}

    vdisk_fd = kernel_file_open("/home/vdisk.dat", O_RDWR|O_CREAT);
    printk("\nkernel_file_open vdisk.dat, fd:%d\n", vdisk_fd);

 out:
	return err;
}

void cleanup_module ( void )
{
    printk("\ngoodbye, vincent\n");

	printk("%s call into device_unregister\n",	__func__);
	device_unregister(&kvtape_pseudo);
	printk("%s back from device_unregister\n",__func__);

	printk(	"%s into from driver_unregister\n",	__func__);
	driver_unregister(&kvtape_driver);
	printk(	"%s back from driver_unregister\n",	__func__);

	printk("%s call into bus_unregister\n",	__func__);
	bus_unregister(&kvtape_bus);
	printk("%s back from bus_unregister\n",	__func__);

    if (-1 != vdisk_fd) {
        kernel_file_close(vdisk_fd);
        vdisk_fd = -1;
    }
}
