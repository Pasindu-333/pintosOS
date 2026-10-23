#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/shutdown.h"
#include "devices/input.h"

/* Global filesystem lock — only one thread in the FS at a time. */
struct lock fs_lock;

static void syscall_handler (struct intr_frame *);

/* ---- Safe user memory access helpers ---- */

/* Verify that the user pointer UADDR is valid:
   - non-NULL
   - below PHYS_BASE
   - mapped in the current page directory
   Terminates the process if invalid. */
static void
verify_user_ptr (const void *uaddr)
{
  if (uaddr == NULL
      || !is_user_vaddr (uaddr)
      || pagedir_get_page (thread_current ()->pagedir, uaddr) == NULL)
    {
      thread_current ()->exit_status = -1;
      thread_exit ();
    }
}

/* Verify an entire user buffer [buf, buf+size) is valid. */
static void
verify_user_buf (const void *buf, unsigned size)
{
  if (size == 0)
    return;

  if (buf == NULL
      || !is_user_vaddr (buf)
      || !is_user_vaddr ((const uint8_t *) buf + size - 1)
      || (uintptr_t) buf + size < (uintptr_t) buf)
    {
      thread_current ()->exit_status = -1;
      thread_exit ();
    }

  const uint8_t *p = buf;
  const uint8_t *end = (const uint8_t *) buf + size;
  while (p < end)
    {
      verify_user_ptr (p);
      /* Advance to start of next page */
      uintptr_t next_page = (uintptr_t) pg_round_down (p) + PGSIZE;
      if (next_page >= (uintptr_t) end)
        {
          verify_user_ptr (end - 1);
          break;
        }
      p = (const uint8_t *) next_page;
    }
}

/* Verify a user-supplied string pointer — check each byte until '\0'. */
static void
verify_user_str (const char *str)
{
  verify_user_ptr (str);
  while (*str != '\0')
    {
      str++;
      verify_user_ptr (str);
    }
}

/* Read a 32-bit value from the user stack at address ADDR. */
static uint32_t
read_user_int (const uint32_t *addr)
{
  verify_user_ptr (addr);
  verify_user_ptr ((const char *) addr + 3);
  return *addr;
}

/* ---- File descriptor helpers ---- */

/* Allocate a new file descriptor and store FILE in it.
   Returns the fd, or -1 on failure. */
static int
fd_alloc (struct file *file)
{
  struct thread *t = thread_current ();
  int fd;
  for (fd = 2; fd < FD_MAX; fd++)
    {
      if (t->fd_table[fd] == NULL)
        {
          t->fd_table[fd] = file;
          return fd;
        }
    }
  return -1;
}

/* Look up file descriptor FD. Returns NULL if invalid. */
static struct file *
fd_lookup (int fd)
{
  struct thread *t = thread_current ();
  if (fd < 2 || fd >= FD_MAX)
    return NULL;
  return t->fd_table[fd];
}

/* Close file descriptor FD. */
static void
fd_close (int fd)
{
  struct thread *t = thread_current ();
  if (fd < 2 || fd >= FD_MAX)
    return;
  if (t->fd_table[fd] != NULL)
    {
      lock_acquire (&fs_lock);
      file_close (t->fd_table[fd]);
      lock_release (&fs_lock);
      t->fd_table[fd] = NULL;
    }
}

/* ---- System call implementations ---- */

static void
sys_halt (void)
{
  shutdown_power_off ();
}

static void
sys_exit (int status)
{
  thread_current ()->exit_status = status;
  thread_exit ();
}

static pid_t
sys_exec (const char *cmd_line)
{
  verify_user_str (cmd_line);
  /* Do NOT hold fs_lock here: process_execute() blocks waiting for
     the child to load, and load() itself needs the fs_lock. */
  pid_t pid = process_execute (cmd_line);
  return pid;
}

static int
sys_wait (pid_t pid)
{
  return process_wait (pid);
}

static bool
sys_create (const char *file, unsigned initial_size)
{
  verify_user_str (file);
  lock_acquire (&fs_lock);
  bool result = filesys_create (file, (off_t) initial_size);
  lock_release (&fs_lock);
  return result;
}

static bool
sys_remove (const char *file)
{
  verify_user_str (file);
  lock_acquire (&fs_lock);
  bool result = filesys_remove (file);
  lock_release (&fs_lock);
  return result;
}

static int
sys_open (const char *file)
{
  verify_user_str (file);
  lock_acquire (&fs_lock);
  struct file *f = filesys_open (file);
  lock_release (&fs_lock);
  if (f == NULL)
    return -1;
  int fd = fd_alloc (f);
  if (fd == -1)
    {
      lock_acquire (&fs_lock);
      file_close (f);
      lock_release (&fs_lock);
    }
  return fd;
}

static int
sys_filesize (int fd)
{
  struct file *f = fd_lookup (fd);
  if (f == NULL)
    return -1;
  lock_acquire (&fs_lock);
  int size = (int) file_length (f);
  lock_release (&fs_lock);
  return size;
}

static int
sys_read (int fd, void *buffer, unsigned size)
{
  verify_user_buf (buffer, size);

  if (fd == STDIN_FILENO)
    {
      /* Read from keyboard. */
      uint8_t *buf = buffer;
      unsigned i;
      for (i = 0; i < size; i++)
        buf[i] = input_getc ();
      return (int) size;
    }

  struct file *f = fd_lookup (fd);
  if (f == NULL)
    return -1;
  lock_acquire (&fs_lock);
  int bytes = (int) file_read (f, buffer, (off_t) size);
  lock_release (&fs_lock);
  return bytes;
}

static int
sys_write (int fd, const void *buffer, unsigned size)
{
  verify_user_buf (buffer, size);

  if (fd == STDOUT_FILENO)
    {
      /* Write to console in chunks of at most 512 bytes. */
      const char *buf = buffer;
      unsigned remaining = size;
      while (remaining > 0)
        {
          unsigned chunk = remaining < 512 ? remaining : 512;
          putbuf (buf, chunk);
          buf += chunk;
          remaining -= chunk;
        }
      return (int) size;
    }

  struct file *f = fd_lookup (fd);
  if (f == NULL)
    return -1;
  lock_acquire (&fs_lock);
  int bytes = (int) file_write (f, buffer, (off_t) size);
  lock_release (&fs_lock);
  return bytes;
}

static void
sys_seek (int fd, unsigned position)
{
  struct file *f = fd_lookup (fd);
  if (f == NULL)
    return;
  lock_acquire (&fs_lock);
  file_seek (f, (off_t) position);
  lock_release (&fs_lock);
}

static unsigned
sys_tell (int fd)
{
  struct file *f = fd_lookup (fd);
  if (f == NULL)
    return 0;
  lock_acquire (&fs_lock);
  unsigned pos = (unsigned) file_tell (f);
  lock_release (&fs_lock);
  return pos;
}

static void
sys_close (int fd)
{
  fd_close (fd);
}

/* ---- Initialise and dispatch ---- */

void
syscall_init (void) 
{
  lock_init (&fs_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  /* The syscall number is at the top of the user stack. */
  uint32_t *sp = (uint32_t *) f->esp;

  /* Validate the stack pointer itself. */
  verify_user_ptr (sp);

  int syscall_num = (int) read_user_int (sp);

  switch (syscall_num)
    {
    case SYS_HALT:
      sys_halt ();
      break;

    case SYS_EXIT:
      sys_exit ((int) read_user_int (sp + 1));
      break;

    case SYS_EXEC:
      f->eax = (uint32_t) sys_exec ((const char *) read_user_int (sp + 1));
      break;

    case SYS_WAIT:
      f->eax = (uint32_t) sys_wait ((pid_t) read_user_int (sp + 1));
      break;

    case SYS_CREATE:
      f->eax = (uint32_t) sys_create ((const char *) read_user_int (sp + 1),
                                       (unsigned) read_user_int (sp + 2));
      break;

    case SYS_REMOVE:
      f->eax = (uint32_t) sys_remove ((const char *) read_user_int (sp + 1));
      break;

    case SYS_OPEN:
      f->eax = (uint32_t) sys_open ((const char *) read_user_int (sp + 1));
      break;

    case SYS_FILESIZE:
      f->eax = (uint32_t) sys_filesize ((int) read_user_int (sp + 1));
      break;

    case SYS_READ:
      f->eax = (uint32_t) sys_read ((int)      read_user_int (sp + 1),
                                    (void *)   read_user_int (sp + 2),
                                    (unsigned) read_user_int (sp + 3));
      break;

    case SYS_WRITE:
      f->eax = (uint32_t) sys_write ((int)            read_user_int (sp + 1),
                                     (const void *)   read_user_int (sp + 2),
                                     (unsigned)       read_user_int (sp + 3));
      break;

    case SYS_SEEK:
      sys_seek ((int)      read_user_int (sp + 1),
                (unsigned) read_user_int (sp + 2));
      break;

    case SYS_TELL:
      f->eax = (uint32_t) sys_tell ((int) read_user_int (sp + 1));
      break;

    case SYS_CLOSE:
      sys_close ((int) read_user_int (sp + 1));
      break;

    default:
      /* Unknown system call — kill the process. */
      thread_current ()->exit_status = -1;
      thread_exit ();
      break;
    }
}
