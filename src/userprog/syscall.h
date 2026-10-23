#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

void syscall_init (void);

/* Global file system lock — acquire before any filesys call. */
extern struct lock fs_lock;

#endif /**< userprog/syscall.h */
