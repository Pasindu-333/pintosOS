#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"

/* Structure used to pass both command line and child_process record
   from process_execute() to start_process(). */
struct exec_info
  {
    char *cmd_line;
    struct child_process *cp;
  };

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/** Starts a new thread running a user program loaded from FILENAME.
   Returns the new process's thread id, or TID_ERROR / PID_ERROR on
   failure. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char *name_copy;
  char *prog_name;
  char *save_ptr;
  tid_t tid;
  struct thread *cur = thread_current ();

  /* Allocate child_process record for parent-child synchronization. */
  struct child_process *cp = malloc (sizeof *cp);
  if (cp == NULL)
    return PID_ERROR;

  cp->tid = TID_ERROR;
  cp->exit_status = -1;
  cp->exited = false;
  cp->waited_on = false;
  cp->load_success = false;
  cp->parent_alive = true;
  sema_init (&cp->wait_sema, 0);
  sema_init (&cp->load_sema, 0);

  /* Make a copy of FILE_NAME to pass to the child thread. */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    {
      free (cp);
      return TID_ERROR;
    }
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Extract program name for thread_create. */
  name_copy = palloc_get_page (0);
  if (name_copy == NULL)
    {
      palloc_free_page (fn_copy);
      free (cp);
      return TID_ERROR;
    }
  strlcpy (name_copy, file_name, PGSIZE);
  prog_name = strtok_r (name_copy, " ", &save_ptr);

  /* Allocate exec_info to pass to start_process via thread_create. */
  struct exec_info *info = malloc (sizeof *info);
  if (info == NULL)
    {
      palloc_free_page (fn_copy);
      palloc_free_page (name_copy);
      free (cp);
      return TID_ERROR;
    }
  info->cmd_line = fn_copy;
  info->cp = cp;

  /* Insert cp into parent's child list before child thread runs. */
  list_push_back (&cur->child_list, &cp->elem);

  /* Create child thread. */
  tid = thread_create (prog_name, PRI_DEFAULT, start_process, info);
  palloc_free_page (name_copy);

  if (tid == TID_ERROR)
    {
      list_remove (&cp->elem);
      palloc_free_page (fn_copy);
      free (info);
      free (cp);
      return TID_ERROR;
    }

  cp->tid = tid;

  /* Wait for the child to finish loading (success or failure). */
  sema_down (&cp->load_sema);

  if (!cp->load_success)
    return PID_ERROR;

  return tid;
}

/** Thread function: loads a user process and starts it. */
static void
start_process (void *aux)
{
  struct exec_info *info = aux;
  char *file_name = info->cmd_line;
  struct child_process *cp = info->cp;
  struct intr_frame if_;
  bool success;
  struct thread *cur = thread_current ();

  cur->cp = cp;
  free (info);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  /* Free the page holding the command line. */
  palloc_free_page (file_name);

  /* Signal parent that loading has completed. */
  if (cur->cp != NULL)
    {
      cur->cp->load_success = success;
      sema_up (&cur->cp->load_sema);
    }

  if (!success)
    thread_exit ();

  /* Jump to user space. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/** Waits for child process TID and returns its exit status.
   Returns -1 if TID is not a direct child, or already waited on. */
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct child_process *cp = NULL;
  struct list_elem *e;

  /* Search our child list for the child with child_tid. */
  for (e = list_begin (&cur->child_list);
       e != list_end (&cur->child_list);
       e = list_next (e))
    {
      struct child_process *c = list_entry (e, struct child_process, elem);
      if (c->tid == child_tid)
        {
          cp = c;
          break;
        }
    }

  if (cp == NULL || cp->waited_on)
    return -1;

  cp->waited_on = true;

  /* Block until child exits. */
  if (!cp->exited)
    sema_down (&cp->wait_sema);

  int status = cp->exit_status;

  /* Remove from list and free child_process record. */
  list_remove (&cp->elem);
  free (cp);

  return status;
}

/** Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  int i;

  /* Print termination message for user processes. */
  if (cur->pagedir != NULL)
    printf ("%s: exit(%d)\n", cur->name, cur->exit_status);

  /* Close all open file descriptors. */
  lock_acquire (&fs_lock);
  for (i = 2; i < FD_MAX; i++)
    {
      if (cur->fd_table[i] != NULL)
        {
          file_close (cur->fd_table[i]);
          cur->fd_table[i] = NULL;
        }
    }

  /* Allow writes to and close the executable. */
  if (cur->executable != NULL)
    {
      file_allow_write (cur->executable);
      file_close (cur->executable);
      cur->executable = NULL;
    }
  lock_release (&fs_lock);

  /* Notify parent via our child_process record. */
  if (cur->cp != NULL)
    {
      cur->cp->exit_status = cur->exit_status;
      cur->cp->exited = true;
      sema_up (&cur->cp->wait_sema);
      /* If parent already exited, child cleans up the record. */
      if (!cur->cp->parent_alive)
        free (cur->cp);
    }

  /* Clean up children list: free records for children that already exited;
     for those still alive, mark parent_alive = false so child frees it upon exit. */
  while (!list_empty (&cur->child_list))
    {
      struct child_process *child = list_entry (list_pop_front (&cur->child_list),
                                                struct child_process, elem);
      if (child->exited)
        free (child);
      else
        child->parent_alive = false;
    }

  /* Destroy page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/** Sets up the CPU for running user code in the current thread. */
void
process_activate (void)
{
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

/* ============================================================
   ELF binary loading
   ============================================================ */

typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

#define PE32Wx PRIx32
#define PE32Ax PRIx32
#define PE32Ox PRIx32
#define PE32Hx PRIx16

struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_STACK   0x6474e551

#define PF_X 1
#define PF_W 2
#define PF_R 4

static bool setup_stack (void **esp, const char *file_name);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);
static bool install_page (void *upage, void *kpage, bool writable);

/** Loads an ELF executable from FILE_NAME into the current thread.
   file_name may include arguments (e.g. "echo foo bar"); the first
   token is the executable name. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Extract executable name (strip arguments). */
  char prog_name[128];
  char *sp;
  strlcpy (prog_name, file_name, sizeof prog_name);
  sp = strchr (prog_name, ' ');
  if (sp != NULL)
    *sp = '\0';

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file (under filesystem lock). */
  lock_acquire (&fs_lock);
  file = filesys_open (prog_name);
  if (file == NULL) 
    {
      lock_release (&fs_lock);
      printf ("load: %s: open failed\n", prog_name);
      goto done; 
    }

  /* Deny writes to running executable. */
  file_deny_write (file);
  t->executable = file;

  /* Read and verify ELF header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      lock_release (&fs_lock);
      printf ("load: %s: error loading executable\n", prog_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        {
          lock_release (&fs_lock);
          goto done;
        }
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        {
          lock_release (&fs_lock);
          goto done;
        }
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          lock_release (&fs_lock);
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                {
                  lock_release (&fs_lock);
                  goto done;
                }
            }
          else
            {
              lock_release (&fs_lock);
              goto done;
            }
          break;
        }
    }
  lock_release (&fs_lock);

  if (!setup_stack (esp, file_name))
    goto done;

  *eip = (void (*) (void)) ehdr.e_entry;
  success = true;

 done:
  /* Keep file open on success (t->executable); close on failure. */
  if (!success && file != NULL)
    {
      lock_acquire (&fs_lock);
      file_allow_write (file);
      file_close (file);
      lock_release (&fs_lock);
      t->executable = NULL;
    }
  return success;
}

static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 
  if (phdr->p_memsz == 0)
    return false;
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;
  if (phdr->p_vaddr < PGSIZE)
    return false;
  return true;
}

static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/** Set up the user stack and push arguments following the
   SysV x86 ABI.  FILE_NAME is the full command line. */
static bool
setup_stack (void **esp, const char *file_name)
{
  uint8_t *kpage;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage == NULL)
    return false;

  if (!install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true))
    {
      palloc_free_page (kpage);
      return false;
    }

  /* Copy command line so strtok_r can modify it. */
  char *args = palloc_get_page (0);
  if (args == NULL)
    return false;
  strlcpy (args, file_name, PGSIZE);

  /* Tokenise. */
  char *argv[128];
  int argc = 0;
  char *token, *save_ptr;
  for (token = strtok_r (args, " ", &save_ptr);
       token != NULL && argc < 128;
       token = strtok_r (NULL, " ", &save_ptr))
    argv[argc++] = token;

  *esp = PHYS_BASE;

  /* Push argument strings (right-to-left). */
  char *arg_ptrs[128];
  int i;
  for (i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;
      *esp = (char *) *esp - len;
      memcpy (*esp, argv[i], len);
      arg_ptrs[i] = (char *) *esp;
    }

  /* Word-align esp. */
  uintptr_t addr = (uintptr_t) *esp & ~(uintptr_t) 3;
  *esp = (void *) addr;

  /* argv[argc] = NULL sentinel. */
  *esp = (char **) *esp - 1;
  *(char **) *esp = NULL;

  /* argv[i] pointers (right-to-left). */
  for (i = argc - 1; i >= 0; i--)
    {
      *esp = (char **) *esp - 1;
      *(char **) *esp = arg_ptrs[i];
    }

  /* argv (pointer to argv[0]). */
  char **argv0 = (char **) *esp;
  *esp = (char ***) *esp - 1;
  *(char ***) *esp = argv0;

  /* argc. */
  *esp = (int *) *esp - 1;
  *(int *) *esp = argc;

  /* Fake return address. */
  *esp = (void **) *esp - 1;
  *(void **) *esp = NULL;

  palloc_free_page (args);
  return true;
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
