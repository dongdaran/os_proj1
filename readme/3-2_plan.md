# 3-② 인자 전달 구현 계획

## 1. 목표

현재는 `echo hello` 같은 명령행 전체를 파일명으로 사용한다.

이번 작업에서는 명령행을 파싱해 **실행 파일 이름을 분리하고, 인자를 사용자 스택에 넣어 `main()`에 전달한다.**

예를 들어 `exec("echo hello")`를 실행하면:

- 실행 파일 `echo`를 연다.
- 사용자 스택에 인자 문자열, 각 문자열의 주소, `argc`, `argv` 등을 넣는다.
- 사용자 프로그램의 `main()`이 `argc = 2`, `argv = {"echo", "hello", NULL}`을 받게 한다.
- 이후 사용자 스택에 넣어 main에 전달함.

## 2. 수정할 파일

| 파일 | 할 일 |
| --- | --- |
| [process.c](../src/userprog/process.c) | 프로그램 이름 분리, 인자 분리, 사용자 스택 구성, 실패 시 정리 |
| [syscall.c](../src/userprog/syscall.c) | 사용자 입출력 시스템 콜 `read`, `write` 구현 |

`child_status`, `child_statuses`, `own_status`, 기존 refs 처리와 공개 함수 선언은 그대로 활용한다. 인자 배열을 `struct thread`에 추가하지 않는다. 기존에 허용한 파일 범위 안에서 수정한다.

## 3. 함수별 구현 순서

### ① process_execute(): 스레드 이름만 먼저 분리

명령행을 페이지에 복사하는 기존 코드를 유지한다. 4KB 이상 문자열을 거부하는 검사도 유지한다.

1. 복사한 명령행의 앞 공백을 건너뛴다.
2. 첫 단어가 없으면 빈 명령이므로 메모리를 정리하고 -1을 반환한다.
3. 첫 단어를 작은 이름 버퍼에 복사해 `thread_create()`의 이름으로 쓴다(eg. echo). 현재 스레드 이름은 16바이트이므로 표시 이름은 최대 15글자다.
4. 자식에게는 인자를 포함한 원래 명령행 페이지를 그대로 전달한다.


### ② start_process(): 명령행을 argc와 argv로 분리

예를 들어 다음 문자열을 받는다.

```text
"  echo   hello world  "
```

1. 공백으로 구분된 단어 수를 세어 `argc`를 구한다. 연속 공백은 단어 하나로 세지 않는다.
2. `argc`개의 `char *`를 담을 임시 배열을 `malloc()`으로 할당한다. 큰 배열을 커널 스택에 두지 않는다.
3. `strtok_r(cmdline, " ", &save_ptr)`로 분리하고 각 단어의 주소를 배열에 저장한다.

```text
임시 argv[0] → "echo"
임시 argv[1] → "hello"
임시 argv[2] → "world"
```

이 주소들은 아직 **커널의 명령행 페이지 안 주소**다. 사용자에게 그대로 넘기면 안 된다. 사용자 스택에 문자열을 복사한 뒤 그곳의 주소를 전달해야 한다.d

### ③ load(): argv[0]으로 파일 열기

내부 함수의 인자를 다음처럼 바꾸는 방향으로 구현한다.

```c
load (int argc, char **argv, void (**eip) (void), void **esp);
setup_stack (void **esp, int argc, char **argv);
```

- `filesys_open(argv[0])`으로 실행 파일을 연다.
- 기존 ELF 검사와 프로그램 적재는 유지한다.
- `setup_stack()`에 인자 배열을 전달한다.
- 파일 적재와 인자 스택 구성이 모두 성공한 경우에만 `load()`가 true를 반환한다.

함수 선언과 정의, 호출부를 함께 변경한다. 두 함수는 process.c 내부 함수이므로 process.h에 공개하지 않는다.

### ④ setup_stack(): 사용자 메모리에 인자 넣기

현재처럼 사용자 공간 맨 위에 4KB 페이지를 하나 만든다. 쓰기 전에 필요한 총 크기가 한 페이지에 들어가는지 검사한다.

```text
문자열 크기 = 모든 인자의 strlen + 각 문자열의 끝 '\0'
정렬 패딩   = 문자열 복사 후 주소를 4의 배수로 맞추는 0~3바이트
포인터 공간 = (argc + 1) × 4바이트     ← argv와 마지막 NULL
나머지      = 12바이트                 ← argv 주소, argc, 가짜 반환 주소

총 크기 = 문자열 크기 + 정렬 패딩 + 포인터 공간 + 12
```

이 저장소는 32비트 Pintos이므로 포인터와 정수는 4바이트다. 총 크기가 `PGSIZE`를 넘으면 실패한다. 명령행 자체가 4KB 미만이어도 포인터까지 더하면 넘칠 수 있다.

높은 주소에서 낮은 주소로 다음 순서로 넣는다.

1. 마지막 인자부터 문자열을 복사한다. 복사할 때마다 사용자 주소를 임시 argv 배열에 덮어쓴다. 이 시점 이후 해당 항목의 원래 커널 주소는 사용하지 않는다.
2. 스택 주소를 4바이트 경계에 맞추고 빈 곳을 0으로 채운다.
3. `argv[argc]`에 해당하는 NULL 포인터를 넣는다.
4. 마지막 인자부터 주소를 넣어, 메모리에 `argv[0], argv[1], ...` 순서로 배치한다.
5. 방금 만든 `argv[0]` 항목의 주소를 넣는다. 이 값이 사용자에게 전달할 `argv`다.
6. `argc`를 넣는다.
7. 가짜 반환 주소 0을 넣고, 그 주소를 최종 `esp`로 저장한다.

```text
높은 주소: PHYS_BASE (페이지 바로 위, 여기에 쓰면 안 됨)
┌───────────────────────────┐
│ "world\0"                 │
│ "hello\0"                 │
│ "echo\0"                  │
│ 정렬 패딩 (필요한 경우)     │
│ argv[3] = NULL            │
│ argv[2] → "world"         │
│ argv[1] → "hello"         │
│ argv[0] → "echo"          │ ← argv가 가리킬 위치
│ argv 주소                 │
│ argc = 3                 │
│ 가짜 반환 주소 = 0         │ ← 최종 esp
└───────────────────────────┘
낮은 주소 방향으로 스택이 자람
```

`_start(argc, argv)`는 이 배치에서 인자를 읽고 `main(argc, argv)`를 호출한다. `entry.c`는 수정할 필요가 없다.

### ⑤ start_process(): 정리 후 로드 완료 통지

1. `load()`가 반환하면 임시 argv 배열을 `free()`한다.
2. 명령행 페이지를 `palloc_free_page()`로 해제한다. 문자열은 사용자 스택에 복사했으므로 성공한 경우에도 원본은 더 필요 없다.
3. 최종 성공 여부를 `load_ok`에 기록하고 `load_done` 신호를 보낸다.
4. 실패면 `thread_exit()`, 성공이면 기존 `intr_exit`로 사용자 모드에 들어간다.

인자 배열 할당이나 스택 크기 검사에서 실패해도 부모에게 실패 신호를 보내야 한다. 일부 사용자 페이지가 이미 연결되어 있다면 기존 `process_exit()`가 페이지 디렉터리를 정리하도록 한다. 아직 연결하지 않은 페이지는 실패한 함수에서 직접 해제한다.

## 4. 테스트 출력을 위한 최소 write

현재 syscall.c는 `exec`, `wait`, `exit`만 처리한다. 제공된 `args-*` 테스트는 `printf()`를 쓰므로 인자 전달만 구현하면 출력 단계에서 종료된다.

테스트에 필요한 범위만 추가한다.

- `SYS_WRITE`의 fd, buffer, size를 안전하게 읽는다.
- fd가 1일 때 사용자 버퍼의 전체 범위가 사용자 공간이고 매핑되어 있는지 확인한다. 주소 덧셈 넘침과 페이지 경계도 검사한다.
- 검사 후 `putbuf()`로 출력하고 쓴 바이트 수를 `eax`로 반환한다.
- 크기 0은 메모리를 읽지 않고 0을 반환한다. 지원하지 않는 fd는 -1로 처리한다.
- 잘못된 사용자 메모리는 기존 -1 종료 경로로 보낸다.

일반 파일 쓰기와 FD 관리는 이번 계획에 포함하지 않는다.

## 5. 검증 순서와 완료 기준

1. userprog를 빌드한다. 임시 `hex_dump()` 또는 디버거로 `esp`, `argc`, `argv`, 각 문자열 주소와 마지막 NULL을 확인한다. 디버그 출력은 테스트 전에 제거한다.
2. 제공 테스트 `args-none`, `args-single`, `args-multiple`, `args-many`, `args-dbl-space`를 실행한다. `args-none`도 프로그램 이름이 있으므로 `argc`는 1이다.
3. `exec-arg`, `exec-missing`, `wait-simple`, `wait-twice`, `wait-killed`, `wait-bad-pid`로 기존 상태 관리가 유지되는지 확인한다.
4. 빈 문자열, 공백만 있는 문자열, 앞뒤 공백, 한 스택 페이지를 넘는 인자 구성을 검사한다. 실패 시 부모가 계속 기다리거나 커널이 중단되면 안 된다.
5. threads 빌드로 USERPROG 없는 구성도 확인한다.

빌드 후 개별 테스트 예시:

```sh
make -C src/userprog -j4
PATH="$PWD/src/utils:$PATH" make -C src/userprog/build \
  tests/userprog/args-none.result \
  tests/userprog/args-single.result \
  tests/userprog/args-multiple.result \
  tests/userprog/args-many.result \
  tests/userprog/args-dbl-space.result
```

**완료 기준:** 프로그램 이름으로 파일을 열고, 사용자 main이 정확한 argc/argv를 받으며, 크기 초과·할당 실패에서도 메모리를 정리하고 부모에게 실패를 알린다. 테스트 결과와 실제 변경 줄 번호는 구현 후 별도 기록한다.
