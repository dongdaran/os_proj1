# 2026 Fall Pintos Project 1 TODO

기준: [과제 PDF](../2026fall_pintos_proj1.pdf), 현재 저장소 소스, [로컬 User Programs 매뉴얼](../doc/userprog.texi).
아래 체크박스는 현재 구현·확인한 항목을 표시한다. 상세 구현·검증 기록은 각 `*_imple.md`를 참고한다.

## 1. 무엇을 만드는 과제인가?

**사용자 프로그램을 실행하고, 인자를 전달하고, 시스템 콜로 프로세스와 파일을 다룰 수 있게 만드는 User Program 과제**다.
일반 Pintos 매뉴얼의 Project 2(User Programs)가 이 수업에서는 Project 1이다. 소스 주석의 `project 2` 또는 `problem 2-2`도 이번 과제와 관련된다.
Alarm clock, priority donation, MLFQS 구현은 이번 PDF의 요구사항이 아니다. 빌드·채점 위치는 `src/userprog`다.

| 구분 | 해야 할 일 | PDF |
| --- | --- | --- |
| Prj1-1 | 종료 메시지, 인자 전달, 사용자 메모리 검증, `halt`, `exit`, `exec`, `wait`, 표준 입출력 `read`/`write` | pp.31–61 |
| 추가 시스템 콜 | `fibonacci`, `max_of_four_int`, 이를 호출하는 `additional` 프로그램 | pp.54–58 |
| Prj1-2 | 파일 관련 시스템 콜, 프로세스별 FD 관리, 실행 파일 쓰기 금지, 파일 시스템 동기화 | pp.62–83 |
| 보고서·제출 | e-class 양식의 보고서와 소스 압축 제출 | pp.84–95 |

최초 계획 작성 당시 상태(현재 구현 상태는 3절 체크리스트와 구현 기록 참고):

- `src/userprog/syscall.c`: 시스템 콜을 처리하지 않고 `system call!`을 출력한 뒤 종료한다.
- `src/userprog/process.c`: `process_wait()`는 즉시 `-1`을 반환한다. 명령행 전체를 실행 파일명으로 사용하고, 초기 스택에는 인자가 없다.
- `src/threads/thread.h`: 부모·자식 상태, 종료 코드, FD 관리 정보가 아직 없다.
- `src/lib/user/syscall.c`: 기존 시스템 콜의 사용자 측 래퍼는 있지만 추가 두 시스템 콜과 `syscall4`는 없다.

## 2. 어떤 파일을 건드려야 하나?

경로는 저장소 루트 기준이다. 소스 수정·추가는 아래 수정/추가 파일 표에 있는 파일로 제한한다. 참고 파일은 읽기만 하며, 테스트 소스와 테스트 등록 파일도 수정하지 않는다.

| 수정/추가 파일 | 작업 내용 |
| --- | --- |
| **`src/userprog/process.c`** | `process_execute()`의 프로그램명 분리와 자식 생성, `start_process()`의 로드 결과 통지, `load()`/`setup_stack()`의 인자 스택 구성, `process_wait()` 구현, `process_exit()`의 종료 처리·자원 회수. 실행 파일을 종료 시까지 열어 두고 쓰기 금지 |
| `src/userprog/process.h` | 프로세스 상태 구조체나 다른 파일에서 사용할 종료 처리 함수 등 필요한 선언 |
| **`src/userprog/syscall.c`** | 사용자 메모리 검증, 시스템 콜 번호·인자 추출, 호출 분기, 반환값 설정, 표준 입출력·파일 입출력, 추가 두 시스템 콜, 파일 시스템 락 |
| `src/userprog/syscall.h` | 추가 시스템 콜 선언 및 여러 파일에서 공유할 함수/락 접근 선언 |
| **`src/threads/thread.h`** | 프로세스별 종료 상태, 자식 관리, FD 관리, 실행 파일 포인터 등 추가. 사용자 프로세스용 필드는 `#ifdef USERPROG` 안에 두는 방식 권장 |
| `src/threads/thread.c` | 추가 필드 초기화와 필요 시 종료 경로 연결. 이미 `thread_exit()`에서 `process_exit()`를 호출하므로 중복 정리하지 않기 |
| `src/lib/syscall-nr.h` | 두 시스템 콜 번호 추가. 기존 번호는 유지 |
| `src/lib/user/syscall.h` | `int fibonacci(int n);`, `int max_of_four_int(int a, int b, int c, int d);` 선언 |
| `src/lib/user/syscall.c` | `syscall4` 추가, 두 사용자 측 호출 래퍼 구현 |
| **`src/examples/additional.c` (신규)** | 정수 인자 4개를 받아 첫 인자의 피보나치 값과 네 인자의 최댓값 출력 |
| `src/examples/Makefile` | `PROGS`에 `additional`, 소스 목록에 `additional_SRC = additional.c` 추가 |

읽고 활용할 파일:

| 참고 파일 | 확인할 내용 |
| --- | --- |
| `src/threads/init.c` | `run_task()` → `process_wait(process_execute(task))` 실행 흐름 |
| `src/threads/synch.c`, `synch.h` | `sema_down/up`, `lock_acquire/release` 활용. 동기화 도구 자체의 재구현은 필요 없음 |
| `src/threads/vaddr.h`, `src/userprog/pagedir.c`, `pagedir.h` | `PHYS_BASE`, `is_user_vaddr()`, `pagedir_get_page()`로 주소 범위·매핑 확인 |
| `src/threads/pte.h` | 사용자 버퍼의 쓰기 권한 확인이 필요할 때 페이지 테이블 비트 참고 |
| `src/threads/interrupt.h` | `struct intr_frame`의 `esp`, `eax` |
| `src/lib/user/entry.c` | `_start(argc, argv)` → `main()` → `exit()` 흐름 |
| `src/devices/shutdown.h`, `input.h` | `shutdown_power_off()`, `input_getc()` |
| `src/lib/kernel/console.c` | 표준 출력용 `putbuf()` |
| `src/lib/string.c`, `src/lib/stdio.c` | `strtok_r()`, 스택 디버깅용 `hex_dump()` |
| `src/filesys/filesys.h`, `file.h` 및 대응 `.c` | 제공된 파일 시스템 API 사용법 |
| `src/tests/userprog/`, `src/tests/userprog/no-vm/`, `src/tests/filesys/base/` | 테스트 코드와 `.ck` 기대 결과 |

`pagedir.c/.h`는 수정하지 않고 기존 API와 `pte.h`의 도구를 활용한다.
PDF는 이번 과제에서 **파일 시스템 내부 코드를 수정할 필요가 없다고 명시**한다(p.63). `src/threads/qemu/`도 과제 구현 대상이 아니다.

## 3. 권장 구현 순서와 완료 조건

### ① 자료구조와 실행 흐름 파악

- [x] `init.c` → `process_execute()` → `start_process()` → `load()` → 사용자 `_start()` → 시스템 콜 흐름 읽기.
- [x] 자식별 PID, 로드 성공 여부, 종료 상태, 대기 여부, 동기화 객체를 어디에 보관할지 정하기.
- [x] 자식 스레드가 먼저 사라져도 부모가 종료 상태를 읽을 수 있도록 상태 정보의 수명 설계하기. 부모가 먼저 종료하거나 `wait()`하지 않는 경우도 회수 가능해야 한다.
- [x] `struct thread`와 커널 스택은 4KB 페이지를 공유하므로 큰 FD 배열·인자 배열을 무작정 넣지 않기.

### ② 인자 전달 (PDF pp.35–41)

주요 파일: `process.c`.

- [x] 명령행을 커널 소유 메모리에 복사하고 `strtok_r()` 등으로 분리하기. 연속 공백 처리하기.
- [x] `echo x`에서 실행 파일은 `echo`로 열고, 사용자에게는 `argc=2`, `argv={"echo", "x", NULL}` 전달하기.
- [x] 스택을 높은 주소에서 낮은 주소 방향으로 구성하기: **인자 문자열 → 4바이트 정렬 패딩 → NULL 포인터 → 각 인자 주소 → argv 주소 → argc → 가짜 반환 주소(0)**.
- [x] 문자열뿐 아니라 포인터·패딩을 포함한 전체 크기가 한 스택 페이지를 넘지 않게 검사하기. PDF는 명령행 길이 4KB 미만을 가정한다.
- [ ] `hex_dump()`로 확인하고 채점 전 디버그 출력 제거하기.

완료 확인: `args-none`, `args-single`, `args-multiple`, `args-many`, `args-dbl-space`.
출력을 끝까지 확인하려면 아래 `write`, `exit`, `wait`도 필요하다. 임시 무한 루프는 최종 구현에 남기지 않는다.

### ③ 사용자 메모리 검증 + 시스템 콜 분기 (PDF pp.53, 59–61)

주요 파일: `syscall.c`. 2절의 수정/추가 파일 목록 안에서 구현하며 `exception.c`와 테스트 파일은 수정하지 않는다.

- [x] `f->esp`에서 시스템 콜 번호와 인자를 안전하게 읽고, 반환값은 `f->eax`에 저장하기.
- [x] NULL, 커널 주소(`PHYS_BASE` 이상), 매핑되지 않은 주소를 거부하기.
- [x] 포인터 시작 주소만 보지 말고 **정수 인자 전체, 문자열의 NUL까지, 버퍼 전체 범위**를 검사하기. 페이지 경계와 주소 덧셈 오버플로 처리하기.
- [x] `read()`의 목적지 버퍼는 쓰기 가능해야 한다. 매핑 여부만으로 쓰기 권한까지 확인되는 것은 아니다.
- [x] 잘못된 사용자 접근은 해당 프로세스를 `exit(-1)`에 해당하는 경로로 종료하고, 보유 락·메모리가 남지 않게 하기.
- [x] PDF의 두 방식 중 하나를 일관되게 선택하기. 시작하기에는 접근 전 검증이 단순하다. fault 처리 방식을 택하면 커널에서 발생한 사용자 메모리 접근 fault의 복구까지 구현해야 한다.

완료 확인: `sc-bad-sp`, `sc-bad-arg`, `sc-boundary*`, `exec-bad-ptr`, 이후 파일 관련 `*-bad-ptr`, `*-boundary`.

### ④ 기본 시스템 콜 + 프로세스 수명 관리 (Prj1-1)

주요 파일: `syscall.c`, `process.c/.h`, `thread.c/.h`.

| 시스템 콜 | 필요한 동작 |
| --- | --- |
| `halt()` | `shutdown_power_off()` 호출 |
| `exit(status)` | 종료 상태 저장, 종료 메시지 출력, 자원 회수, 부모에게 종료 통지 |
| `exec(cmd_line)` | 새 자식 프로세스 생성. **자식 로드 성공 여부가 확정된 뒤** PID 또는 실패 시 `-1` 반환. UNIX의 현재 프로세스 교체와 다름 |
| `wait(pid)` | 직접 생성한 자식을 한 번만 기다리고 종료 상태 반환. 잘못된 PID·비자식·중복 대기는 즉시 `-1`, 예외 종료한 자식은 `-1` |
| `read(0, buffer, size)` | `input_getc()`로 표준 입력을 읽어 버퍼에 저장하고 읽은 바이트 수 반환 |
| `write(1, buffer, size)` | `putbuf()`로 표준 출력하고 쓴 바이트 수 반환 |

- [x] 사용자 프로세스 종료 시 정확히 `프로그램명: exit(상태)\n`을 한 번 출력하기. 예: `echo: exit(0)`.
- [x] 프로그램명에는 명령행 인자를 포함하지 않기. 커널 스레드 종료나 `halt()`에는 이 메시지를 출력하지 않기.
- [x] 정상 종료와 예외 종료의 정리 경로 통합하기. 로드 실패 때의 종료 메시지는 매뉴얼상 선택 사항이다.
- [x] 이미 종료한 자식에 대한 `wait()`도 처리하기. 자식이 먼저 종료했다고 상태를 잃으면 안 된다.
- [x] 로드 완료 대기와 종료 대기를 구분하기. PDF는 busy waiting도 허용하지만 기존 세마포어 활용을 권장한다.
- [x] 생성·로드 중 실패한 경우 임시 페이지와 상태 객체를 회수하기.

`halt()`와 프로그램명·종료 메시지 조건은 3-5에서 구현·검증했다. 전체 기본 기능의 완료 확인 목록 중 `exec-multiple`, `multi-recurse` 등 이번에 실행하지 않은 테스트는 별도 검증이 필요하다. `hex_dump()` 직접 확인은 수행 기록이 없어 미체크로 남겼다.

완료 확인: `exit`, `halt`, `exec-once`, `exec-multiple`, `exec-arg`, `exec-missing`, `wait-simple`, `wait-twice`, `wait-bad-pid`, `wait-killed`, `multi-recurse`.

### ⑤ 추가 시스템 콜 + additional 프로그램 (PDF pp.54–58)

주요 파일: `lib/syscall-nr.h`, `lib/user/syscall.c/.h`, `userprog/syscall.c/.h`, `examples/additional.c`, `examples/Makefile`.

- [x] `fibonacci(int n)` 구현. 예시에서 `fibonacci(10) == 55`.
- [x] `max_of_four_int(int a, int b, int c, int d)` 구현. 음수 입력에서도 최댓값을 올바르게 반환하기.
- [x] 시스템 콜 번호 → 사용자 래퍼 → 커널 분기 → `eax` 반환까지 연결하기. 네 인자 전달용 `syscall4` 추가하기.
- [x] `additional`이 정수 인자 4개를 받아 두 시스템 콜을 실제 호출하게 만들기.
- [x] Pintos 안에서 `additional 10 20 62 40`을 실행하여 결과 줄 **`55 62`** 확인하기.

함수명과 실행 파일명은 PDF의 이름을 그대로 사용한다. 피보나치의 음수·정수 오버플로 범위는 PDF에 명시되어 있지 않으므로 임의로 과제 명세라고 단정하지 말고 e-class 보충 공지가 있으면 확인한다.

### ⑥ 파일 시스템 콜 확장 (Prj1-2, PDF pp.62–83)

주요 파일: `syscall.c`, `process.c`, `thread.h` 및 초기화 코드.

| 시스템 콜 | 활용할 기존 API / 동작 |
| --- | --- |
| `create` | `filesys_create()`, 성공 여부 반환. 파일을 열거나 FD를 반환하지 않음 |
| `remove` | `filesys_remove()`, 성공 여부 반환 |
| `open` | `filesys_open()`, 프로세스 FD에 등록. 실패는 `-1` |
| `close` | `file_close()`, FD 등록 해제 |
| `filesize` | `file_length()` |
| `seek` / `tell` | `file_seek()` / `file_tell()` |
| `read` / `write` | 표준 입출력 처리를 유지하고 일반 FD에는 `file_read()` / `file_write()` 적용 |

- [x] FD는 프로세스별로 관리하기. 0·1은 표준 입출력용이고 일반 파일 FD는 2부터 사용 가능하다.
- [x] 자식은 부모의 FD를 상속하지 않게 하기. 같은 파일을 여러 번 열어도 각각 독립된 파일 위치를 유지하기.
- [x] 잘못된 FD, 중복 close, 잘못된 입출력 방향, 크기 0, EOF, 존재하지 않는 파일을 처리하기.
- [x] 종료 시 열려 있는 파일과 FD 관리 메모리를 모두 회수하기.
- [x] 내부 동기화가 없는 파일 시스템을 공통 락으로 보호하기. 시스템 콜뿐 아니라 **실행 파일 로드와 종료 정리**도 같은 보호 규칙 적용하기.
- [x] 파일 시스템 락을 잡은 채 자식의 로드·종료를 기다리지 않기. 사용자 주소 오류 시에도 락이 남지 않게 하기.
- [x] 실행 중인 파일에 `file_deny_write()`를 적용하고 프로세스 종료까지 해당 파일을 열어 두기. 종료 시 닫아 쓰기 금지를 해제하기.
- [x] `multi-oom`에서 반복 실행해도 자원이 누수되지 않게 실패·종료 경로 점검하기.

완료 확인: `create-*`, `open-*`, `close-*`, `read-*`, `write-*`, `multi-child-fd`, `rox-*`, `bad-*`, `no-vm/multi-oom`, `filesys/base` 전체(특히 `syn-read`, `syn-write`, `syn-remove`).

## 4. 빌드와 테스트 방법

아래는 **저장소 루트에서 시작**하는 명령이다. 실행 환경의 GCC 32비트 빌드 도구와 `qemu-system-i386`가 준비되어 있어야 한다. 구현별 실행 결과는 각 `*_imple.md`에 기록한다.

```bash
export PATH="$PWD/src/utils:$PATH"
make -C src/utils
make -C src/examples
make -C src/userprog

# 공식 채점 대상 실행
make -C src/userprog check
make -C src/userprog grade

# 특정 테스트만 확인할 때 (빌드 후)
make -C src/userprog/build tests/userprog/args-single.result
cat src/userprog/build/tests/userprog/args-single.output
cat src/userprog/build/tests/userprog/args-single.result
```

코드를 변경하지 않은 채 동일 테스트를 강제로 다시 실행하려면 해당 테스트의 `.output`, `.result`를 정리한 뒤 실행한다.
전체 결과는 `src/userprog/build/results`, 점수는 `src/userprog/build/grade`에서 확인한다. `grade`는 `make grade`로 생성된다.
루트의 `make check`는 여러 프로젝트용 테스트를 순회하므로 이번 과제 확인에는 위의 `src/userprog` 명령을 사용한다.

직접 프로그램 실행 예시:

```bash
cd src/userprog/build
pintos --qemu --filesys-size=2 -p ../../examples/echo -a echo -- -f -q run 'echo x'
pintos --qemu --filesys-size=2 -p ../../examples/additional -a additional -- -f -q run 'additional 10 20 62 40'
```

첫 명령에서 프로그램 출력 `x`, 두 번째에서 `55 62`를 확인한다. `additional` 명령은 해당 프로그램 구현·빌드 후 사용한다.

## 5. 채점과 제출 TODO

PDF 기준: **테스트 80점 + 보고서 20점**, 추가 시스템 콜은 각각 개발 점수 2.5점(총점 환산 합계 4점) 가산이다.
기본 개발 점수 내 비중은 functionality 35%, robustness 25%, no-vm 10%, filesys/base 30%다.
PDF는 Prj1-1 21개, Prj1-2 55개로 총 76개 채점 테스트를 안내한다.
현재 저장소의 실행 목록에는 PDF 목록 외 `sc-boundary-3`, `exec-bound`, `exec-bound-2`, `exec-bound-3`도 있으므로 `make check`의 실행 수는 80개다. 이 추가 경계 테스트도 검증에 활용한다.

- [ ] e-class의 **지정 보고서 양식**을 받아 `[학번].docx`로 작성하기. 이 `todo.md`는 제출 보고서를 대체하지 않는다.
- [ ] 양식 질문에 맞춰 자료구조, 인자 스택, 사용자 주소 검증, exec/wait 동기화, FD·락 설계와 테스트 결과 정리하기.
- [ ] 디버그 출력 제거 후 `make check`, `make grade` 결과 확인하기.
- [ ] 추가 시스템 콜 실행 결과도 별도로 확인하기.
- [ ] PDF 명시 마감 **2026-10-04 23:59**까지 제출하기. 지각은 **10/07 23:59까지 최대 3일**, 하루당 10% 감점이다. 보충 공지는 별도 확인하기.
- [ ] 루트에 `[학번].docx`를 놓고, 같은 이름의 `[학번]` 디렉터리가 없는지 확인한 뒤 소스 백업하기.
- [ ] 루트에서 `bash submit.sh` 실행, 프로젝트 번호 `1`과 본인 학번 입력하기.
- [ ] `os_prj1_[학번].tar.gz` 내부에 학번 디렉터리 아래 **`src/`와 `[학번].docx`**가 들어 있는지 확인하기(`tar -tzf 파일명`). 별도 임시 디렉터리에 풀어 실제 내용도 확인하기.
- [ ] 현재 `src/threads/qemu/`에 QEMU 소스가 있으므로 제출 전 점검하기. `submit.sh`는 `src` 전체를 복사하므로 이것도 포함된다. 필요한 QEMU 설치는 보존하고, 과제와 무관한 소스·빌드 산출물은 제출용 사본에서 정리하기.
- [ ] 완성된 압축 파일을 e-class에 업로드하기. PDF는 제출 형식·방법 오류에 5% 감점을 명시한다.

개인 과제이며 PDF의 표절 관련 규정을 준수한다. 제출 스크립트는 내용의 정확성을 보장하지 않으므로 압축 결과를 직접 확인해야 한다.

## 6. PDF를 읽을 때 혼동하기 쉬운 부분

- p.82의 `src/lib/syscall.h`, `src/lib/syscall.c` 경로는 현재 저장소에서는 **`src/lib/user/syscall.h`, `src/lib/user/syscall.c`**다(p.58과도 일치).
- p.69의 FD 설명과 달리 실제 API에서 **`create()`는 bool**, **`open()`은 FD**를 반환한다. `lib/user/syscall.h`와 로컬 매뉴얼을 기준으로 구현한다.
- p.72 본문은 삭제를 언급하지만 제목과 제시 API의 핵심은 **실행 파일에 대한 쓰기 금지**다. `file_deny_write()`는 삭제 금지 API가 아니다. 기존 파일 시스템의 열린 파일 삭제 의미를 임의로 바꾸지 않는다.
- 보고서 양식은 PDF에 포함되어 있지 않으므로 e-class 파일을 별도로 받아야 한다.
