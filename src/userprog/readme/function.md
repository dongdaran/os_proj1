# userprog 함수 설명

현재 `userprog`의 6개 C 파일에 정의된 함수 38개를 설명한다. 헤더에 공개된 함수뿐 아니라 파일 내부에서 사용하는 `static` 함수도 포함한다. 아래 설명은 과제의 완성된 동작이 아니라 **현재 소스의 실제 구현**을 기준으로 한다.

## 먼저 알아둘 용어

| 용어 | 의미 |
| --- | --- |
| 사용자 모드 / 커널 모드 | 각각 일반 프로그램과 운영체제가 실행되는 CPU 권한 수준. 이 코드에서는 ring 3 / ring 0을 사용한다. |
| `PGSIZE` / `PHYS_BASE` | 페이지 크기(4 KiB) / 사용자 가상 주소 공간과 커널 가상 주소 공간의 경계. |
| 페이지 디렉터리 / 페이지 테이블 | 가상 주소를 물리 프레임에 연결하는 2단계 자료구조. 각 항목을 PDE / PTE라고 한다. |
| `upage` / `kpage` | 사용자 가상 페이지의 시작 주소 / 해당 물리 프레임을 커널에서 접근할 때 사용하는 가상 주소. |
| TLB | CPU가 최근 주소 변환 결과를 저장하는 캐시. 페이지 테이블 변경 시 갱신이 필요할 수 있다. |
| ELF | 실행 파일 형식. 헤더의 진입점과 적재할 세그먼트 정보를 사용한다. |
| GDT / TSS | 세그먼트와 권한 정보를 담는 표 / 사용자 모드에서 커널 모드로 진입할 때 사용할 커널 스택 정보를 담는 구조체. |
| `struct intr_frame` | 인터럽트 발생 시 저장하거나 사용자 모드 진입을 위해 준비하는 레지스터 상태. |

## 전체 실행 흐름

```text
process_execute(file_name)
  └─ thread_create(..., start_process, 파일 이름 복사본)
       └─ start_process()
            ├─ load()
            │    ├─ pagedir_create() → process_activate()
            │    ├─ ELF 헤더 검사
            │    ├─ validate_segment() → load_segment() → install_page()
            │    └─ setup_stack() → install_page()
            └─ intr_exit으로 사용자 모드 진입

문맥 전환: process_activate() → pagedir_activate() + tss_update()
시스템 콜: int 0x30 → syscall_handler()
예외: kill() 또는 page_fault() → kill()
스레드 종료: thread_exit() → process_exit() → pagedir_destroy()
```

`thread_create()`, `thread_exit()`, `intr_exit`은 다른 디렉터리에서 구현된 기능이다. 자식 스레드는 `process_execute()`가 반환하기 전에도 실행되거나 종료될 수 있다.

## process.c — 프로세스 실행과 ELF 적재 (10개)

### `tid_t process_execute(const char *file_name)`

- **역할:** 실행 파일을 적재할 새 스레드를 만든다.
- **입력·반환:** 실행 파일 이름을 받아 새 스레드의 ID를 반환한다. 이름 복사 공간 확보 또는 스레드 생성에 실패하면 `TID_ERROR`를 반환한다.
- **동작:** 한 페이지를 할당해 `file_name`을 복사하고, `start_process()`를 시작 함수로 지정해 기본 우선순위의 스레드를 생성한다. 복사본은 호출자가 원본 문자열을 변경해도 자식의 적재 작업이 영향을 받지 않게 한다. 스레드 생성 실패 시 복사본을 해제한다.
- **현재 한계:** 자식의 ELF 적재 결과를 기다리지 않는다. 반환된 ID만으로 실행 파일 적재 성공을 판단할 수 없다. 명령행 인자를 분리하지 않으며 이름은 최대 `PGSIZE - 1`바이트까지 복사된다.

### `static void start_process(void *file_name_)`

- **역할:** 새 스레드에서 실행 파일을 적재하고 사용자 모드로 진입한다.
- **입력·반환:** `process_execute()`가 만든 파일 이름 복사본을 받으며 호출 지점으로 돌아오지 않는다.
- **동작:** `intr_frame`을 초기화하고 사용자 코드·데이터 세그먼트와 인터럽트 허용 플래그를 설정한다. `load()`로 진입점(`eip`)과 스택 포인터(`esp`)를 채운 뒤 파일 이름 복사본을 해제한다.
- **성공·실패:** 적재 실패 시 `thread_exit()`을 호출한다. 성공 시 `%esp`를 준비한 프레임으로 바꾸고 `intr_exit`으로 점프해 사용자 프로그램을 실행한다.

### `int process_wait(tid_t child_tid UNUSED)`

- **의도된 역할:** 지정한 자식 프로세스가 종료될 때까지 기다리고 종료 상태를 돌려준다. 유효하지 않은 자식, 중복 대기, 커널에 의한 종료 등을 처리해야 한다.
- **현재 동작:** 인자를 사용하지 않고 즉시 `-1`을 반환하는 미구현 함수다. 대기, 부모·자식 관계 검사, 종료 상태 수집은 수행하지 않는다.

### `void process_exit(void)`

- **역할:** 현재 프로세스의 페이지 디렉터리와 그에 연결된 사용자 메모리를 정리한다. 반환값은 없다.
- **동작:** `cur->pagedir`을 별도 변수에 보관하고, `cur->pagedir = NULL` → `pagedir_activate(NULL)` → `pagedir_destroy(pd)` 순서로 처리한다. 페이지 디렉터리가 없으면 아무 작업도 하지 않는다.
- **순서의 이유:** 인터럽트 후 문맥 전환에서 해제할 디렉터리가 다시 활성화되는 것을 막고, CPU가 사용 중인 페이지 디렉터리를 먼저 해제하는 문제를 피한다.
- **현재 범위:** 파일 디스크립터 정리나 부모에게 종료 상태 전달 등의 기능은 없다.

### `void process_activate(void)`

- **역할:** 현재 스레드에 맞춰 CPU의 주소 공간과 커널 진입 스택을 설정한다. 반환값은 없다.
- **동작:** `pagedir_activate(t->pagedir)`로 페이지 디렉터리를 선택하고 `tss_update()`로 커널 스택 위치를 갱신한다. 문맥 전환 시 호출되며 `load()`에서도 새 디렉터리를 만든 직후 호출한다.

### `static bool load(const char *file_name, void (**eip)(void), void **esp)`

- **역할:** ELF 실행 파일을 현재 스레드의 사용자 주소 공간에 적재한다. 앞선 `static` 선언 때문에 파일 내부 함수다.
- **입력·반환:** 파일 이름과 출력 포인터 두 개를 받는다. 성공 시 `*eip`에 진입점, `*esp`에 초기 스택 포인터를 저장하고 `true`를 반환한다. 실패 시 `false`를 반환한다.
- **동작:** 페이지 디렉터리 생성·활성화 → 파일 열기 → ELF 식별자·실행 파일 종류·아키텍처·프로그램 헤더 크기와 개수 검사 → 프로그램 헤더 순회 → 스택 생성 순서로 진행한다.
- **세그먼트 처리:** `PT_LOAD`는 검증 후 페이지 단위로 읽고 나머지를 0으로 채운다. `PF_W`로 쓰기 허용 여부를 결정한다. `PT_DYNAMIC`, `PT_INTERP`, `PT_SHLIB`는 적재 실패로 처리하며 나머지 종류는 무시한다.
- **정리·한계:** 열린 파일은 성공 여부와 관계없이 마지막에 닫는다. 이미 매핑한 페이지는 실패 시 여기서 직접 해제하지 않고 이후 프로세스 종료 경로에서 정리한다. 인자 전달과 실행 중인 파일의 쓰기 금지는 구현되어 있지 않다.

### `static bool validate_segment(const struct Elf32_Phdr *phdr, struct file *file)`

- **역할:** ELF 프로그램 헤더가 적재 가능한 메모리 영역을 지정하는지 검사한다.
- **입력·반환:** 프로그램 헤더와 실행 파일을 받아 모든 검사를 통과하면 `true`, 아니면 `false`를 반환한다.
- **검사:** 파일 오프셋과 가상 주소의 페이지 내부 오프셋 일치, 파일 오프셋의 파일 길이 이내 여부, `p_memsz >= p_filesz`, 0이 아닌 메모리 크기, 시작 주소와 `p_vaddr + p_memsz`의 사용자 영역 여부, 주소 덧셈의 오버플로, 첫 번째 페이지(주소 0 포함) 매핑 금지를 확인한다.
- **주의:** 세그먼트의 파일 데이터 끝까지 존재하는지는 여기서 별도로 검사하지 않는다. 실제 읽기 부족은 `load_segment()`에서 실패로 처리한다.

### `static bool load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes, uint32_t zero_bytes, bool writable)`

- **역할:** 실행 파일의 세그먼트를 사용자 페이지들로 적재한다.
- **입력:** 파일, 읽기 시작 위치 `ofs`, 매핑 시작 주소 `upage`, 파일에서 읽을 바이트 수, 0으로 채울 바이트 수, 쓰기 허용 여부를 받는다. `ofs`와 `upage`는 페이지 정렬되어야 하고 두 바이트 수의 합은 페이지 크기의 배수여야 한다.
- **동작:** 사용자 풀에서 페이지를 하나씩 할당하고, 필요한 데이터를 읽은 뒤 나머지를 0으로 채워 `install_page()`로 연결한다. 다음 사용자 페이지로 이동하며 반복한다.
- **반환·실패:** 모두 적재하면 `true`다. 할당·읽기·매핑 실패 시 현재 미매핑 페이지를 필요에 따라 해제하고 `false`를 반환한다. 이전 반복에서 이미 매핑한 페이지는 프로세스 종료 시 정리된다.

### `static bool setup_stack(void **esp)`

- **역할:** 사용자 주소 공간 맨 위에 초기 스택 한 페이지를 만든다.
- **동작·반환:** `PAL_USER | PAL_ZERO`로 페이지를 확보하고 `PHYS_BASE - PGSIZE`에 쓰기 가능한 페이지로 매핑한다. 성공 시 `*esp = PHYS_BASE`로 설정하고 `true`를 반환한다. 할당 또는 매핑 실패 시 `false`이며, 매핑 실패 시 확보한 페이지를 해제한다.
- **현재 한계:** 스택 공간만 만든다. `argc`, `argv`, 문자열 인자, 가짜 반환 주소는 넣지 않는다.

### `static bool install_page(void *upage, void *kpage, bool writable)`

- **역할:** 현재 스레드의 페이지 디렉터리에 사용자 페이지와 물리 프레임의 연결을 추가한다.
- **입력·반환:** 사용자 페이지 시작 주소, 프레임의 커널 가상 주소, 쓰기 허용 여부를 받는다. 기존 매핑이 없고 `pagedir_set_page()`가 성공하면 `true`, 이미 매핑되어 있거나 페이지 테이블 할당이 실패하면 `false`다.
- **특징:** 프레임 자체를 할당하거나 실패 시 해제하지 않는다. 호출자가 그 책임을 가진다.

## pagedir.c — 페이지 디렉터리 관리 (13개)

### `uint32_t *pagedir_create(void)`

새 페이지 디렉터리용 페이지를 할당하고 `init_page_dir`을 복사한다. 커널 주소 매핑을 공유하면서 사용자 주소 매핑은 없는 디렉터리를 만든다. 성공 시 포인터, 할당 실패 시 `NULL`을 반환한다.

### `void pagedir_destroy(uint32_t *pd)`

사용자 영역의 PDE와 PTE를 순회해 present 비트가 켜진 사용자 프레임, 그 페이지 테이블, 마지막으로 디렉터리 자체를 해제한다. 공유하는 커널 영역의 페이지 테이블·프레임은 해제하지 않는다. `pd == NULL`이면 아무 일도 하지 않으며 `init_page_dir`은 전달할 수 없다. 활성 디렉터리 교체는 호출자가 먼저 해야 한다.

### `static uint32_t *lookup_page(uint32_t *pd, const void *vaddr, bool create)`

`vaddr`에 해당하는 **PTE의 주소**를 찾는다. 해당 페이지 테이블이 없으면 `create == true`일 때 0으로 초기화된 테이블을 만들어 PDE에 연결한다. 생성하지 않거나 할당에 실패하면 `NULL`을 반환한다. 새 테이블 생성은 사용자 주소에만 허용된다. PTE 포인터를 반환했다고 해서 그 PTE에 유효한 프레임이 연결되었다는 뜻은 아니다.

### `bool pagedir_set_page(uint32_t *pd, void *upage, void *kpage, bool writable)`

사용자 페이지 `upage`를 `kpage`가 가리키는 물리 프레임에 매핑한다. `lookup_page(..., true)`로 PTE를 확보한 뒤 사용자 접근 권한과 쓰기 권한을 설정한다. 성공하면 `true`, 페이지 테이블 할당 실패 시 `false`를 반환한다. 두 주소는 페이지 정렬되어야 하며, 이미 present인 페이지를 다시 매핑하면 `false`가 아니라 `ASSERT` 실패다.

### `void *pagedir_get_page(uint32_t *pd, const void *uaddr)`

사용자 가상 주소 `uaddr`에 연결된 메모리를 **커널 가상 주소**로 반환한다. 페이지 내부 오프셋까지 더하므로 페이지 중간 주소도 조회할 수 있다. 페이지 테이블이 없거나 present 비트가 꺼져 있으면 `NULL`을 반환한다. 사용자 쓰기 권한 검사를 대신해 주지는 않는다.

### `void pagedir_clear_page(uint32_t *pd, void *upage)`

페이지 정렬된 사용자 주소의 PTE에서 present 비트만 지워 이후 접근이 페이지 폴트를 일으키게 한다. 다른 비트는 유지하며 필요한 경우 TLB를 무효화한다. 매핑이 없으면 아무 일도 하지 않는다. **프레임을 해제하지 않는다.** 이후 `pagedir_destroy()`도 present가 꺼진 항목의 프레임은 해제하지 않으므로 프레임 수명은 호출자가 관리해야 한다.

### `bool pagedir_is_dirty(uint32_t *pd, const void *vpage)`

해당 PTE의 dirty 비트(`PTE_D`)를 확인한다. 비트가 켜져 있으면 `true`, PTE가 없거나 비트가 꺼져 있으면 `false`다. CPU는 해당 매핑을 통한 쓰기를 기록하는 데 이 비트를 사용한다. 함수 자체는 present 비트를 별도로 검사하지 않는다.

### `void pagedir_set_dirty(uint32_t *pd, const void *vpage, bool dirty)`

해당 PTE의 dirty 비트를 설정하거나 해제한다. PTE가 없으면 아무 일도 하지 않는다. `false`로 지울 때는 이후 쓰기가 다시 기록될 수 있도록 활성 디렉터리의 TLB를 무효화한다. 메모리 내용 자체를 수정하거나 디스크에 저장하는 함수는 아니다.

### `bool pagedir_is_accessed(uint32_t *pd, const void *vpage)`

해당 PTE의 accessed 비트(`PTE_A`)를 확인한다. 비트가 켜져 있으면 `true`, PTE가 없거나 비트가 꺼져 있으면 `false`다. 해당 매핑을 통한 읽기·쓰기 등의 접근 여부를 확인하는 용도이며 present 비트는 별도로 검사하지 않는다. 접근 기록은 비트를 초기화한 시점을 기준으로 해석해야 한다.

### `void pagedir_set_accessed(uint32_t *pd, const void *vpage, bool accessed)`

해당 PTE의 accessed 비트를 설정하거나 해제한다. PTE가 없으면 아무 일도 하지 않는다. `false`로 지울 때는 활성 디렉터리의 TLB를 무효화해 이후 접근이 다시 기록되도록 한다. 페이지 교체 정책에서 접근 이력을 초기화할 때 사용할 수 있다.

### `void pagedir_activate(uint32_t *pd)`

페이지 디렉터리의 물리 주소를 CPU의 `CR3` 레지스터에 써서 주소 공간을 전환한다. `pd == NULL`이면 커널 기본 디렉터리인 `init_page_dir`을 사용한다. 이 코드에서는 같은 디렉터리를 다시 활성화하는 동작을 TLB 무효화에도 사용한다.

### `static uint32_t *active_pd(void)`

`CR3`에서 현재 페이지 디렉터리의 물리 주소를 읽고 `ptov()`로 커널 가상 주소로 바꿔 반환한다. 새 디렉터리를 생성하거나 활성 디렉터리를 변경하지 않는다.

### `static void invalidate_pagedir(uint32_t *pd)`

`active_pd()`로 확인한 현재 디렉터리가 `pd`일 때만 `pagedir_activate(pd)`를 호출해 TLB를 무효화한다. 활성 상태가 아니면 지금 갱신할 필요가 없으므로 아무 일도 하지 않는다.

## exception.c — 예외 처리 (4개)

### `void exception_init(void)`

CPU 예외 처리기를 등록한다. 대부분은 `kill()`로 보내고, 페이지 폴트(벡터 14)는 `page_fault()`로 보낸다. breakpoint·overflow·BOUND 예외는 DPL 3으로 등록해 사용자 코드가 명시적으로 발생시킬 수 있게 한다. 다른 등록 예외는 DPL 0이지만 잘못된 명령이나 메모리 접근 때문에 사용자 코드에서 간접적으로 발생할 수 있다. 페이지 폴트는 `CR2`의 오류 주소를 안전하게 읽기 위해 인터럽트를 끈 상태로 처리기에 진입한다.

### `void exception_print_stats(void)`

`page_fault_cnt`에 누적한 페이지 폴트 횟수를 출력한다. 카운터를 초기화하지 않으며 반환값은 없다.

### `static void kill(struct intr_frame *f)`

예외 당시 코드 세그먼트(`f->cs`)로 처리 방법을 결정한다. 사용자 세그먼트이면 예외 정보와 레지스터 프레임을 출력하고 현재 스레드를 종료한다. 커널 세그먼트이면 프레임을 출력하고 `PANIC`으로 커널을 중단한다. 알 수 없는 세그먼트이면 메시지를 출력하고 스레드를 종료한다. 현재 구현에는 부모에게 종료 상태 `-1`을 저장·전달하는 처리가 없다.

### `static void page_fault(struct intr_frame *f)`

먼저 `CR2`에서 접근에 실패한 가상 주소를 읽고 인터럽트를 다시 허용한다. 페이지 폴트 횟수를 증가시킨 뒤 `f->error_code`의 `PF_P`, `PF_W`, `PF_U`를 해석해 페이지 부재/권한 위반, 쓰기/읽기, 사용자/커널 접근을 구분한다. 현재는 원인을 출력하고 `kill(f)`를 호출한다. 페이지 적재, 스택 확장, 잘못된 사용자 포인터 접근에서의 복구는 구현되어 있지 않다. 오류 주소는 실행 중이던 명령 주소인 `f->eip`와 다를 수 있다.

## syscall.c — 시스템 콜 진입점 (2개)

### `void syscall_init(void)`

인터럽트 벡터 `0x30`에 `syscall_handler()`를 등록한다. DPL 3이므로 사용자 코드가 `int 0x30`으로 호출할 수 있으며, 처리기는 인터럽트가 허용된 상태(`INTR_ON`)에서 실행된다. 개별 시스템 콜 기능을 구현하는 함수는 아니다.

### `static void syscall_handler(struct intr_frame *f UNUSED)`

현재는 `system call!`을 출력하고 `thread_exit()`으로 현재 스레드를 종료하는 기본 틀이다. 인터럽트 프레임을 사용하지 않으며 시스템 콜 번호·인자 추출, 사용자 포인터 검증, 호출별 분기, 반환값 설정은 아직 없다.

## gdt.c — 세그먼트와 권한 설정 (6개)

### `void gdt_init(void)`

null, 커널 코드·데이터, 사용자 코드·데이터, TSS의 총 6개 GDT 항목을 설정한다. 커널 세그먼트는 DPL 0, 사용자 세그먼트는 DPL 3이다. `tss_get()`으로 TSS 위치를 얻고, `lgdt`로 GDT를 CPU에 등록한 뒤 `ltr`로 TSS 선택자를 로드한다. 따라서 `tss_init()`이 먼저 실행되어 있어야 한다.

### `static uint64_t make_seg_desc(uint32_t base, uint32_t limit, enum seg_class class, int type, int dpl, enum seg_granularity granularity)`

세그먼트 시작 주소, 20비트 한계값, 시스템/코드·데이터 구분, 종류, 권한 수준, 한계값 단위를 받아 64비트 디스크립터를 만든다. 인자 범위를 `ASSERT`로 확인한 뒤 각 필드를 정해진 비트 위치에 배치한다. present 비트와 32비트 세그먼트 설정 비트도 넣는다. GDT에 직접 저장하지 않고 완성된 값을 반환한다.

### `static uint64_t make_code_desc(int dpl)`

지정한 권한 수준의 읽기 가능한 코드 세그먼트 디스크립터를 반환한다. `make_seg_desc()`에 시작 주소 0, 한계값 `0xfffff`, 페이지 단위, 코드 종류 10을 전달해 4 GiB 주소 범위를 구성한다.

### `static uint64_t make_data_desc(int dpl)`

지정한 권한 수준의 쓰기 가능한 데이터 세그먼트 디스크립터를 반환한다. 코드 세그먼트와 같은 주소 범위를 사용하되 종류 값은 2이다. 사용자와 커널의 실제 메모리 접근 구분에는 페이지 테이블의 권한도 사용된다.

### `static uint64_t make_tss_desc(void *laddr)`

`laddr`에 있는 TSS의 디스크립터를 반환한다. 시스템 세그먼트 종류 9(available 32-bit TSS), DPL 0, 바이트 단위 한계값 `0x67`을 사용한다. 한계값이 마지막 바이트의 오프셋이므로 표현하는 TSS 범위는 104바이트다.

### `static uint64_t make_gdtr_operand(uint16_t limit, void *base)`

`lgdt`에 전달할 GDT 한계값과 시작 주소를 하나의 값으로 조립해 반환한다. 하위 16비트에 `limit`, 그 위 32비트에 `base`를 넣는다. 반환형은 64비트지만 이 32비트 실행 환경에서 `lgdt`가 읽는 정보는 6바이트다. `gdt_init()`은 한계값으로 GDT 크기에서 1을 뺀 값을 전달한다.

## tss.c — 사용자 모드에서 진입할 커널 스택 설정 (3개)

### `void tss_init(void)`

`PAL_ASSERT | PAL_ZERO`로 TSS용 페이지를 할당·초기화한다. 할당에 실패하면 정상 실패 반환 대신 패닉으로 이어진다. ring 0 스택 세그먼트 `ss0`를 `SEL_KDSEG`로 설정하고, I/O 비트맵 오프셋을 TSS 범위 밖인 `0xdfff`로 둔다. 마지막으로 `tss_update()`를 호출해 현재 스레드의 커널 스택 위치를 설정한다.

### `struct tss *tss_get(void)`

전역 TSS 포인터를 반환한다. 초기화되지 않았다면 `ASSERT`에 실패한다. `gdt_init()`이 TSS 디스크립터를 만들 때 사용한다.

### `void tss_update(void)`

TSS가 초기화되었는지 확인한 뒤 `esp0`를 `(uint8_t *) thread_current() + PGSIZE`로 설정한다. 스레드 구조체와 커널 스택이 한 페이지를 공유하고 스택이 아래로 자라므로 페이지 끝이 커널 스택의 초기 상단이다. 사용자 모드에서 인터럽트·예외·시스템 콜로 커널에 진입할 때 CPU가 이 위치를 사용한다. 스케줄러가 사용할 일반 실행 문맥 전체를 저장하는 함수는 아니다.

## 현재 미구현 부분을 읽을 때 주의할 점

| 위치 | 현재 상태 |
| --- | --- |
| `process_wait()` | 기다리지 않고 항상 `-1`을 반환한다. |
| `process_execute()` / `load()` | 명령행 인자 분리와 부모에게 적재 결과 전달이 없다. |
| `setup_stack()` | 빈 스택 한 페이지만 만들며 `argc`·`argv`를 배치하지 않는다. |
| `syscall_handler()` | 모든 시스템 콜에서 메시지를 출력하고 스레드를 종료한다. |
| `page_fault()` | 오류 원인 출력 후 종료/패닉으로 이어지며 가상 메모리 복구를 하지 않는다. |
| `process_exit()` | 페이지 디렉터리 관련 자원만 정리하며 종료 상태 전달은 없다. |
