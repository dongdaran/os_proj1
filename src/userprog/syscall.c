#include "userprog/syscall.h"
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

static void syscall_handler (struct intr_frame *);

/* Read through the kernel mapping only after validating the user address. */
static uint8_t
user_byte (uintptr_t address)
{
  uint8_t *mapped = NULL;
  if (address != 0 && address < (uintptr_t) PHYS_BASE)
    mapped = pagedir_get_page (thread_current ()->pagedir, (void *) address);
  if (mapped == NULL)
    {
      process_set_exit_status (-1);
      thread_exit ();
    }
  return *mapped;
}

static uint32_t
user_word (uintptr_t address)
{
  uint32_t value = 0;
  unsigned i;
  if (address > (uintptr_t) PHYS_BASE - sizeof value)
    {
      process_set_exit_status (-1);
      thread_exit ();
    }
  for (i = 0; i < sizeof value; i++)
    value |= (uint32_t) user_byte (address + i) << (8 * i);
  return value;
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
  uintptr_t sp = (uintptr_t) f->esp;
  uint32_t number = user_word (sp);

  switch (number)
    {
    case SYS_EXIT:
      process_set_exit_status ((int) user_word (sp + 4));
      thread_exit ();
    case SYS_WAIT:
      f->eax = process_wait ((tid_t) user_word (sp + 4));
      return;
    case SYS_EXEC:
      {
        uintptr_t address = user_word (sp + 4);
        size_t length, i;
        char *cmdline;

        /* Validate before allocating, so invalid pointers cannot leak a page. */
        for (length = 0; length < PGSIZE; length++)
          if (user_byte (address + length) == '\0')
            break;
        if (length == PGSIZE)
          {
            f->eax = TID_ERROR;
            return;
          }
        cmdline = palloc_get_page (0);
        if (cmdline == NULL)
          {
            f->eax = TID_ERROR;
            return;
          }
        for (i = 0; i <= length; i++)
          cmdline[i] = user_byte (address + i);
        f->eax = process_execute (cmdline);
        palloc_free_page (cmdline);
        return;
      }
    default:
      process_set_exit_status (-1);
      break;
    }
  thread_exit ();
}
