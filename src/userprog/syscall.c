#include "userprog/syscall.h"
#include <syscall-nr.h>
#include <limits.h>
#include <stdio.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/pte.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

static void syscall_handler (struct intr_frame *);
static void invalid_user_access (void) NO_RETURN;
static uint8_t user_byte (uintptr_t);
struct lock filesys_lock;

static struct file_descriptor *
find_file (int fd)
{
  struct list *files = &thread_current ()->file_descriptors;
  struct list_elem *e;
  for (e = list_begin (files); e != list_end (files); e = list_next (e))
    {
      struct file_descriptor *entry =
        list_entry (e, struct file_descriptor, elem);
      if (entry->fd == fd)
        return entry;
    }
  return NULL;
}

/* Returns a kernel copy of a NUL-terminated user string, or NULL if its
   terminator does not occur within one page. Invalid memory exits the process. */
static char *
copy_user_string (uintptr_t address)
{
  size_t length, i;
  char *copy;
  for (length = 0; length < PGSIZE; length++)
    if (user_byte (address + length) == '\0')
      break;
  if (length == PGSIZE)
    return NULL;
  copy = palloc_get_page (0);
  if (copy == NULL)
    return NULL;
  for (i = 0; i <= length; i++)
    copy[i] = user_byte (address + i);
  return copy;
}

// 3-3 : 잘못된 사용자 주소와 시스템 콜 번호는 같은 -1 종료 경로를 사용한다.
static void
invalid_user_access (void)
{
  process_set_exit_status (-1);
  thread_exit ();
}

// 3-1 : 사용자 주소를 검증한 뒤 1바이트를 읽는다. user_word와 exec에서 사용한다.
static uint8_t
user_byte (uintptr_t address)
{
  uint8_t *mapped = NULL;
  // 3-1 : NULL과 커널 영역을 제외한 뒤, 현재 프로세스의 페이지 매핑을 찾는다.
  // 매핑이 있으면 같은 메모리를 가리키는 커널 주소를 받고, 없으면 NULL을 받는다.
  if (address != 0 && address < (uintptr_t) PHYS_BASE)
    mapped = pagedir_get_page (thread_current ()->pagedir, (void *) address);
  if (mapped == NULL)
    invalid_user_access ();
  return *mapped; // 3-1 : 검증된 커널 주소를 통해 실제 바이트를 읽는다.
}

// 3-1 : 사용자 스택의 시스템 콜 번호 또는 인자 하나(4바이트)를 읽는다.
static uint32_t
user_word (uintptr_t address)
{
  uint32_t value = 0;
  unsigned i;
  // 3-1 : 시작 주소뿐 아니라 4바이트 전체가 사용자 영역 안에 있어야 한다.
  if (address > (uintptr_t) PHYS_BASE - sizeof value)
    invalid_user_access ();
  // 3-1 : 페이지 경계에 걸친 값도 안전하게 읽도록 각 바이트를 user_byte로 검사한다.
  // 낮은 주소의 바이트부터 0, 8, 16, 24비트 이동해 리틀 엔디언 값으로 합친다.
  for (i = 0; i < sizeof value; i++)
    value |= (uint32_t) user_byte (address + i) << (8 * i);
  return value;
}

// 3-2 : I/O 전에 전체 버퍼의 매핑을 검사하고 read 대상은 쓰기 권한도 확인한다.
// user_word는 포인터 값 자체를 읽고, 이 함수는 그 포인터가 가리키는 범위를 검사한다.
static void
check_buffer (uintptr_t address, unsigned size, bool writable)
{
  uint32_t *pd = thread_current ()->pagedir;
  uintptr_t page, last;

  // 3-2 : 크기 0이면 버퍼에 접근하지 않으므로 NULL도 허용한다.
  if (size == 0)
    return;
  // 3-2 : 끝 주소를 더하는 대신 남은 범위와 비교해 정수 넘침을 피한다.
  if (address == 0 || address >= (uintptr_t) PHYS_BASE
      || size > (uintptr_t) PHYS_BASE - address)
    goto invalid;
  last = address + size - 1; // 3-2 : 범위 검증 후 마지막 바이트 주소를 계산한다.
  // 3-2 : 첫 페이지와 마지막 페이지 사이의 미매핑 페이지도 빠짐없이 검사한다.
  // 시작 주소를 페이지 시작으로 내린 뒤 PGSIZE씩 이동한다. 바이트마다 검사할 필요는 없다.
  for (page = (uintptr_t) pg_round_down ((void *) address);
       page <= last; page += PGSIZE)
    {
      if (pagedir_get_page (pd, (void *) page) == NULL)
        goto invalid;
      // 3-2 : read는 사용자 메모리에 쓰므로 읽기 전용 페이지를 거부한다.
      // pd_no로 페이지 테이블을 찾고, pt_no로 해당 엔트리를 골라 PTE_W를 확인한다.
      // write는 버퍼를 읽기만 하므로 writable이 false이며 이 검사를 생략한다.
      if (writable
          && !(pde_get_pt (pd[pd_no ((void *) page)])
               [pt_no ((void *) page)] & PTE_W))
        goto invalid;
    }
  // 3-2 : 모든 페이지가 유효하므로 호출자가 실제 입출력을 진행할 수 있다.
  return;

 invalid:
  // 3-2 : 입력 소비나 출력 전에 잘못된 버퍼를 발견하면 프로세스를 -1로 종료한다.
  invalid_user_access ();
}

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

// 3-5 : int로 표현 가능한 F(0)..F(46)만 계산한다. 범위 밖은 -1 반환.
static int
fibonacci (int n)
{
  int previous = 0, current = 1, i;
  if (n < 0 || n > 46)
    return -1;
  if (n == 0)
    return 0;
  for (i = 2; i <= n; i++)
    {
      int next = previous + current;
      previous = current;
      current = next;
    }
  return current;
}

static int
max_of_four_int (int a, int b, int c, int d)
{
  // 3-5 : 0이 아니라 첫 인자로 시작하여 모두 음수인 경우도 처리한다.
  if (b > a) a = b;
  if (c > a) a = c;
  if (d > a) a = d;
  return a;
}

static void
syscall_handler (struct intr_frame *f)
{
  uintptr_t sp = (uintptr_t) f->esp;
  uint32_t number = user_word (sp);

  switch (number)
    {
    // 3-5 : halt는 프로세스 종료 메시지를 출력하지 않고 시스템을 종료한다.
    case SYS_HALT:
      shutdown_power_off ();
    case SYS_FIBONACCI:
      f->eax = fibonacci ((int) user_word (sp + 4));
      return;
    case SYS_MAX_OF_FOUR_INT:
      // 3-5 : 네 번째 인자까지 기존 검증을 거치고 signed int로 비교한다.
      f->eax = max_of_four_int ((int) user_word (sp + 4),
                                (int) user_word (sp + 8),
                                (int) user_word (sp + 12),
                                (int) user_word (sp + 16));
      return;
    case SYS_CREATE:
    case SYS_REMOVE:
      {
        uintptr_t address = user_word (sp + 4);
        unsigned initial_size = number == SYS_CREATE ? user_word (sp + 8) : 0;
        char *name = copy_user_string (address);
        bool result = false;
        if (name != NULL)
          {
            if (number != SYS_CREATE || initial_size <= INT_MAX)
              {
                lock_acquire (&filesys_lock);
                if (number == SYS_CREATE)
                  result = filesys_create (name, (off_t) initial_size);
                else
                  result = filesys_remove (name);
                lock_release (&filesys_lock);
              }
            palloc_free_page (name);
          }
        f->eax = result;
        return;
      }
    case SYS_OPEN:
      {
        char *name = copy_user_string (user_word (sp + 4));
        struct file_descriptor *entry = malloc (sizeof *entry);
        struct file *file = NULL;
        if (name != NULL && entry != NULL)
          {
            lock_acquire (&filesys_lock);
            file = filesys_open (name);
            lock_release (&filesys_lock);
          }
        if (name != NULL)
          palloc_free_page (name);
        if (file == NULL || entry == NULL)
          {
            free (entry);
            f->eax = -1;
          }
        else
          {
            entry->fd = thread_current ()->next_fd++;
            entry->file = file;
            list_push_back (&thread_current ()->file_descriptors, &entry->elem);
            f->eax = entry->fd;
          }
        return;
      }
    case SYS_FILESIZE:
      {
        struct file_descriptor *entry = find_file ((int) user_word (sp + 4));
        f->eax = -1;
        if (entry != NULL)
          {
            lock_acquire (&filesys_lock);
            f->eax = file_length (entry->file);
            lock_release (&filesys_lock);
          }
        return;
      }
    case SYS_SEEK:
    case SYS_TELL:
      {
        struct file_descriptor *entry = find_file ((int) user_word (sp + 4));
        unsigned position = number == SYS_SEEK ? user_word (sp + 8) : 0;
        if (entry != NULL)
          {
            lock_acquire (&filesys_lock);
            if (number == SYS_SEEK)
              {
                if (position <= INT_MAX)
                  file_seek (entry->file, position);
              }
            else
              f->eax = file_tell (entry->file);
            lock_release (&filesys_lock);
          }
        if (number == SYS_SEEK)
          f->eax = 0;
        else if (entry == NULL)
          f->eax = -1;
        return;
      }
    case SYS_CLOSE:
      {
        struct file_descriptor *entry = find_file ((int) user_word (sp + 4));
        if (entry != NULL)
          {
            list_remove (&entry->elem);
            lock_acquire (&filesys_lock);
            file_close (entry->file);
            lock_release (&filesys_lock);
            free (entry);
          }
        return;
      }
    case SYS_EXIT:
      process_set_exit_status ((int) user_word (sp + 4));
      thread_exit ();
    case SYS_WAIT:
      f->eax = process_wait ((tid_t) user_word (sp + 4));
      return;
    // 3-2 : 기존 user_word 검증으로 fd, buffer, size를 안전하게 읽는다.
    case SYS_READ:
    case SYS_WRITE:
      {
        int fd = (int) user_word (sp + 4);
        uintptr_t buffer = user_word (sp + 8);
        unsigned size = user_word (sp + 12), i;
        bool reading = number == SYS_READ;

        // 3-2 : 표준 입력(fd 0)과 표준 출력(fd 1)만 지원한다.
        if ((reading && fd == 0) || (!reading && fd == 1))
          {
            check_buffer (buffer, size, reading);
            if (reading)
              for (i = 0; i < size; i++)
                ((uint8_t *) buffer)[i] = input_getc ();
            else if (size != 0)
              putbuf ((const char *) buffer, size);
            f->eax = size;
            return;
          }
        if (fd < 2 || find_file (fd) == NULL)
          {
            f->eax = -1;
            return;
          }
        check_buffer (buffer, size, reading);
        {
          struct file_descriptor *entry = find_file (fd);
          lock_acquire (&filesys_lock);
          f->eax = reading ? file_read (entry->file, (void *) buffer, size)
                           : file_write (entry->file, (const void *) buffer, size);
          lock_release (&filesys_lock);
        }
        return;
      }
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
      invalid_user_access ();
    }
}
