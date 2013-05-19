/**
 * @file   kernel_fop.h
 * @author vincent.feng <vincent.feng@yahoo.com>
 * @date   Thu Mar 14 12:01:53 2013
 * 
 * @brief  Kernel space file operation interface declaration.
 * 
 */

#ifndef KERNEL_FOP_H__
#define KERNEL_FOP_H__

int kernel_file_open(const char* path, int flags);
int kernel_file_read(int fd, void* buf, size_t count);
int kernel_file_write(int fd, void* buf, size_t count);
off_t kernel_file_seek(int fd, off_t offset, int whence);
void kernel_file_close(int fd);

#endif
