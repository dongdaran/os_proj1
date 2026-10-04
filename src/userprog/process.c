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
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static bool load (int argc, char **argv, void (**eip) (void), void **esp);

struct child_status
  {
    tid_t pid;
    bool load_ok;
    int exit_status;
    bool waited;
    struct semaphore load_done;
    struct semaphore exit_done;
    int refs;
    struct lock refs_lock;
    struct list_elem elem;
  };

struct start_info
  {
    char *cmdline;
    struct child_status *status;
  };

/* child status를 삭제할 때 참조 카운트를 감소시키고, 마지막 참조라면 메모리를 해제하는 함수 */
static void
child_status_release (struct child_status *status)
{
  bool last;

  lock_acquire (&status->refs_lock);
  ASSERT (status->refs > 0);
  last = --status->refs == 0;
  lock_release (&status->refs_lock);
  if (last)
    free (status);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  char name[sizeof thread_current ()->name];
  char *first;
  size_t length;
  struct child_status *status;
  struct start_info *info;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  if (strlcpy (fn_copy, file_name, PGSIZE) >= PGSIZE)
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  // 3-2 : 앞 공백을 건너뛰고 실행 파일명이 없는 명령은 거부한다.
  first = fn_copy;
  while (*first == ' ')
    first++;
  if (*first == '\0')
    {
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  // 3-2 : 표시 이름만 15글자로 제한하고 자식 스레드에 넘길 cmd line 전체는 fn_copy에 보존한다.
  for (length = 0; first[length] != '\0' && first[length] != ' ' && length < sizeof name - 1; length++)
    name[length] = first[length];
  name[length] = '\0';

  status = malloc (sizeof *status);
  info = malloc (sizeof *info);
  if (status == NULL || info == NULL)
    {
      free (status);
      free (info);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }
  status->pid = TID_ERROR;
  status->load_ok = false;
  status->exit_status = -1;
  status->waited = false;
  sema_init (&status->load_done, 0);
  sema_init (&status->exit_done, 0);
  status->refs = 2;
  lock_init (&status->refs_lock);
  info->cmdline = fn_copy;
  info->status = status;
  
  // 3-2 : 새로운 자식 스레드 생성 및 실행(name : cmd 이름 / info : cmdline, status:정보)
  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (name, PRI_DEFAULT, start_process, info);
  if (tid == TID_ERROR)
    {
      free (info);
      free (status);
      palloc_free_page (fn_copy);
      return TID_ERROR;
    }

  status->pid = tid;
  list_push_back (&thread_current ()->child_statuses, &status->elem);
  sema_down (&status->load_done);
  if (!status->load_ok)
    {
      list_remove (&status->elem);
      child_status_release (status);
      return TID_ERROR;
    }
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *aux)
{
  struct start_info *info = aux;
  char *file_name = info->cmdline;
  struct child_status *status = info->status;
  struct intr_frame if_;
  bool success = false;
  int argc = 0, i;
  char *p, *save_ptr;
  char **argv;

  thread_current ()->own_status = status;
  free (info);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  // 3-2 : 단어의 시작만 세어 연속 공백을 제외한 argc를 구한다.
  // 3-2 : 현재 공백이 아니고 이전 문자가 공백이거나 문자열의 시작이면 단어의 시작으로 간주하여 argc를 증가시킨다.
  for (p = file_name; *p != '\0'; p++)
    if (*p != ' ' && (p == file_name || p[-1] == ' ')) argc++;

  // 3-2 : argv 배열 동적 할당.
  argv = malloc (argc * sizeof *argv);
  if (argv != NULL && argc > 0)
    {
      // 3-2 : strtok_r로 문자열을 분리하여 argv 배열에 커널 주소를 저장하고 load를 통해 해당 문자열을 사용자 스택으로 복사.
      p = strtok_r (file_name, " ", &save_ptr);
      for (i = 0; i < argc; i++)
        {
          argv[i] = p;
          p = strtok_r (NULL, " ", &save_ptr);
        }
      success = load (argc, argv, &if_.eip, &if_.esp);
    }

  /* If load failed, quit. */
  // 3-2 : 할당 또는 로드 실패도 임시 자원을 정리한 뒤 부모에게 결과를 알린다.
  free (argv);
  palloc_free_page (file_name);
  status->load_ok = success;
  sema_up (&status->load_done);
  if (!success) 
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   The status remains alive even if the child thread has already exited. */
int
process_wait (tid_t child_tid)
{
  struct list *children = &thread_current ()->child_statuses;
  struct list_elem *e;

  for (e = list_begin (children); e != list_end (children); e = list_next (e))
    {
      struct child_status *status = list_entry (e, struct child_status, elem);
      int exit_status;

      if (status->pid != child_tid)
        continue;
      if (status->waited)
        return -1;
      status->waited = true;
      sema_down (&status->exit_done);
      exit_status = status->exit_status;
      list_remove (&status->elem);
      child_status_release (status);
      return exit_status;
    }
  return -1;
}

void
process_set_exit_status (int exit_status)
{
  struct child_status *status = thread_current ()->own_status;
  if (status != NULL)
    status->exit_status = exit_status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  if (cur->own_status != NULL && cur->own_status->load_ok)
    printf ("%s: exit(%d)\n", cur->name, cur->own_status->exit_status);

  while (!list_empty (&cur->child_statuses))
    {
      struct child_status *status =
        list_entry (list_pop_front (&cur->child_statuses),
                    struct child_status, elem);
      child_status_release (status);
    }

  lock_acquire (&filesys_lock);
  while (!list_empty (&cur->file_descriptors))
    {
      struct file_descriptor *entry =
        list_entry (list_pop_front (&cur->file_descriptors),
                    struct file_descriptor, elem);
      file_close (entry->file);
      free (entry);
    }
  if (cur->executable != NULL)
    {
      file_close (cur->executable);
      cur->executable = NULL;
    }
  lock_release (&filesys_lock);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }

  if (cur->own_status != NULL)
    {
      struct child_status *status = cur->own_status;
      cur->own_status = NULL;
      sema_up (&status->exit_done);
      child_status_release (status);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
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

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
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

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, int argc, char **argv);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads the ELF executable named by ARGV[0] into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
static bool
load (int argc, char **argv, void (**eip) (void), void **esp)
{
  // 3-2 : 스레드 표시 이름이 아닌 잘리지 않은 첫 번째 인자로 실행 파일을 연다.
  const char *file_name = argv[0];
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  lock_acquire (&filesys_lock);
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      lock_release (&filesys_lock);
      goto done; 
    }
  file_deny_write (file);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
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
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  // 3-2 : ELF 적재뿐 아니라 인자 스택 구성까지 성공해야 로드 성공이다.
  if (!setup_stack (esp, argc, argv))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* Keep a successfully loaded executable open and deny writes until exit. */
  if (file != NULL)
    {
      if (success)
        t->executable = file;
      else
        file_close (file);
      lock_release (&filesys_lock);
    }
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
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
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Build the 32-bit C entry stack in one zeroed user page. */
static bool
setup_stack (void **esp, int argc, char **argv)
{
  uint8_t *kpage;
  char *sp = PHYS_BASE;
  char **user_argv;
  size_t strings = 0, padding;
  int i;

  // 3-2 : 문자열, 정렬, argv와 NULL, 진입 프레임 12바이트가 한 페이지에 들어야 한다.
  for (i = 0; i < argc; i++)
    strings += strlen (argv[i]) + 1;
  padding = ROUND_UP (strings, 4) - strings;
  if (strings + padding + (argc + 1) * sizeof (char *) + 12 > PGSIZE)
    return false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage == NULL)
    return false;
  // 3-2 : 매핑하지 못한 페이지는 여기서 해제하고, 매핑된 페이지는 process_exit이 정리한다.
  if (!install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true))
    {
      palloc_free_page (kpage);
      return false;
    }

  // 3-2 : 문자열을 높은 주소부터 역순 복사하고 argv를 실제 사용자 주소로 갱신한다.
  for (i = argc - 1; i >= 0; i--)
    {
      size_t length = strlen (argv[i]) + 1;
      sp -= length;
      memcpy (sp, argv[i], length);
      argv[i] = sp;
    }
  // 3-2 : PAL_ZERO의 0 패딩으로 4바이트 정렬한 뒤 argv[argc]의 NULL을 넣는다.
  sp -= padding;
  sp -= sizeof (char *);
  *(char **) sp = NULL;
  // 3-2 : 주소를 역순으로 넣어 낮은 주소부터 argv[0], argv[1], ... 순서가 되게 한다.
  for (i = argc - 1; i >= 0; i--)
    {
      sp -= sizeof (char *);
      *(char **) sp = argv[i];
    }
  
  // 3-2 : argv 주소, argc, 가짜 반환 주소 순으로 넣어 _start의 호출 규약을 맞춘다.(pg. 39)
  user_argv = (char **) sp; // 배열의 첫 원소 위치
  sp -= sizeof user_argv; // user stack에 argv 배열의 시작 주소를 저장하기 위해 공간 확보
  *(char ***) sp = user_argv; // user stack의 argv 배열의 시작 주소 저장
  sp -= sizeof argc; // argc를 스택에 저장하기 위해 공간 확보
  *(int *) sp = argc; // argc를 스택에 저장
  sp -= sizeof (void *); // 가짜 반환 주소를 스택에 저장하기 위해 공간 확보
  *(void **) sp = NULL; // 가짜 반환 주소 저장
  *esp = sp; // 최종 사용자 스택 포인터 갱신
  return true;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
