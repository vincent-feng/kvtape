/**
 * @file   kernel_fop.c
 * @author vincent.feng <vincent.feng@yahoo.com>
 * @date   Thu Mar 14 12:01:31 2013
 * 
 * @brief  File operations in kernel space interface implementation.
 * 
 * 
 */

#include <linux/fs.h>
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include "kernel_fop.h"

#define MAXFILEOP 10
static struct file* file_struct[MAXFILEOP] = {NULL};

static struct file* file_open(const char* path, int flags, int rights) 
{
    struct file* filp = NULL;
    mm_segment_t oldfs;
    int err = 0;

    oldfs = get_fs();
    set_fs(get_ds());
    filp = filp_open(path, flags, rights);
    set_fs(oldfs);
    if(IS_ERR(filp)) {
        err = PTR_ERR(filp);
        return NULL;
    }
    return filp;
}

static int file_read(struct file* file, unsigned long long offset, unsigned char* data, unsigned int size) 
{
    mm_segment_t oldfs;
    int ret;

    oldfs = get_fs();
    set_fs(get_ds());

    ret = vfs_read(file, data, size, &offset);

    set_fs(oldfs);
    return ret;
}   

static int file_write(struct file* file, unsigned long long offset, unsigned char* data, unsigned int size) 
{
    mm_segment_t oldfs;
    int ret;

    oldfs = get_fs();
    set_fs(get_ds());

    ret = vfs_write(file, data, size, &offset);
    printk("file_write write %d/%d", ret, size);
    set_fs(oldfs);
    return ret;
}

static void file_close(struct file* file) 
{
    filp_close(file, NULL);
}

int kernel_file_open(const char* path, int flags)
{
    struct file* fp_ptr = file_open(path, flags, 0777);
    if (NULL != file_struct[0]) {
        printk("file_struct[0] is not NULL");
        return -1;
    }
    file_struct[0] = fp_ptr;
    return 0;
}

int kernel_file_read(int fd, void* buf, size_t count)
{
    int ret = 0;
    if (NULL == file_struct[fd]) {
        return -1;
    }
    ret = file_read(file_struct[fd], file_struct[fd]->f_pos, (unsigned char*)buf, count);
    file_struct[fd]->f_pos += ret;
    return ret;
}

int kernel_file_write(int fd, void* buf, size_t count)
{
    int ret = 0;
    if (NULL == file_struct[fd]) {
        return -1;
    }
    ret = file_write(file_struct[fd], file_struct[fd]->f_pos, (unsigned char*)buf, count);
    file_struct[fd]->f_pos += ret;
    return ret;
}

off_t kernel_file_seek(int fd, off_t offset, int whence)
{
    if (NULL == file_struct[fd]) {
        return -1;
    }

    switch (whence) {
    case SEEK_SET:
        file_struct[fd]->f_pos = offset;
        break;
    case  SEEK_CUR:
        file_struct[fd]->f_pos += offset;
        break;
    default:
        printk("\nerror in kernel_file_seek, whence %d is not supported\n", whence);
        break;
    }
    return file_struct[fd]->f_pos;
}

void kernel_file_close(int fd)
{
   if (NULL == file_struct[fd]) {
       return;
   }
   file_close(file_struct[fd]);
}
