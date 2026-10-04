# 부모·자식 프로세스 상태를 어디에 보관할까?

이 문서는 `todo.md`의 자료구조·수명 설계 항목을 설명한다. 아래 구조체와 흐름은 **구현 제안**이며, 실제 C 소스에 적용된 코드는 아니다.

## 1. 지금 이해한 흐름 확인

맞다. 다만 `tid`는 자식 스레드 자체가 아니라 **자식 스레드의 정수 ID**다.

```c
/* process_execute()를 실행 중인 현재 스레드가 부모다. */
tid = thread_create (file_name, PRI_DEFAULT, start_process, fn_copy);
```

- 부모: 이 `thread_create()`를 호출하는 스레드.
- 자식: `thread_create()`가 새로 만드는 스레드.
- `tid`: 새 자식의 ID. 생성 실패 시 `TID_ERROR`(-1).
- `start_process`: 자식이 실행할 함수.
- `fn_copy`: 자식의 `start_process(void *...)`에 전달할 주소.

이 과제에서는 사용자 프로세스 하나가 스레드 하나를 기반으로 실행되므로, 자식의 TID를 PID로 사용하는 설계가 가능하다.

```text
부모                                     자식
process_execute()
  thread_create(..., start_process, aux)
                                         start_process(aux)
                                           load()
                                           intr_exit → 사용자 모드
  자식의 로드 완료를 기다림                 _start() → main()
  PID 또는 -1 반환
```

자식은 부모의 `thread_create()`가 반환하기 전에도 실행되거나 종료할 수 있다. 따라서 공유 정보는 **thread_create() 호출 전에 초기화**해야 한다.

근거: [process_execute / start_process](../src/userprog/process.c), [thread_create](../src/threads/thread.c).

## 2. 자식 하나당 별도의 상태 구조체 하나

자식의 `struct thread`는 종료 후 회수된다.(즉 메모리가 해제되어 자식의 정보는 사라진다.) 하지만 부모가 나중에 `wait(pid)`를 호출할 수 있으므로, 자식의 종료결과를 thread 외 다른 공간에 보관해야야한다. \
구조체 설명: [thread.h](../src/threads/thread.h)
<details>
<summary>Thread 구조체 설명-주석 및 코드</summary>

- thread structure(4kb) : (kernel stack(start at offset of 4kb) + thread meta data(at offset 0kb))
- 주의 사항
  - thread의 meta data를 많이 설정 x(<1kb)
  - thread의 object의 크기를 크게해서는 안됨 -> 발생 위험 : assertion failure magic 값이 사라져 assertion 발생

**elem :**

- run queue(thread.c)의 요소 : ready state인 thread만 run queue에 들어갈 수 있다. 자원이 있어 앞의 thread가 끝나면 실행될 가능성이 있는 thread
- semaphor wait list(synch.c)의 요소 blocked state인 thread만 semaphore wait list에 들어갈 수 있음. 즉 ready도 못하는 상태

</details>

\
`child_status`의 정의는 **`src/userprog/process.c`에 둔다.** 자식 상태의 생성·조회·종료·회수를 이 파일에서 담당하고, `syscall.c`는 공개 함수를 통해 종료 코드를 전달한다. 따라서 다른 C 파일에는 구조체의 내부 필드를 공개하지 않는다.

아래 정의는 `process.c`의 헤더 포함 부분 다음, 이 구조체를 사용하는 함수들보다 앞에 둔다. 기존 헤더는 유지하고 `bool`, 목록 요소, 세마포어·락에 필요한 헤더를 포함한다.

```c
#include <stdbool.h>
#include <list.h>
#include "threads/thread.h"
#include "threads/synch.h"

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
```

구조체 정의는 **자료의 모양을 정하는 것**이지, 자식별 저장 공간을 만드는 것은 아니다. 실제 상태 객체는 `process.c`의 `process_execute()`에서 자식마다 `malloc()`으로 할당한다. 이를 위해 `process.c`에는 `threads/malloc.h`를 포함한다.

| 필드 | 초기값 | 용도와 접근 규칙 |
| --- | --- | --- |
| `pid` | `TID_ERROR` | 생성 성공 후 부모가 반환된 TID 저장. 부모가 자식 목록을 검색할 때 사용 |
| `load_ok` | `false` | 자식이 로드 결과를 기록. 부모는 `load_done`을 받은 뒤 읽음 |
| `exit_status` | `-1` | 정상 `exit(status)` 때 자식이 변경. 예외 종료는 기본값 유지. 부모는 `exit_done`을 받은 뒤 읽음 |
| `waited` | `false` | 부모가 `wait()`를 시작할 때 `true`로 변경하여 중복 대기 방지 |
| `load_done` | 세마포어 값 `0` | 자식의 로드 완료를 부모에게 통지 |
| `exit_done` | 세마포어 값 `0` | 자식의 종료 결과와 자원 정리 완료를 부모에게 통지 |
| `refs` | `2` | 부모 몫 1개 + 자식 몫 1개. 둘 다 놓으면 해제 |
| `refs_lock` | `lock_init()` | 부모·자식의 `refs` 감소가 겹쳐도 안전하도록 보호 |
| `elem` | 목록 삽입 시 연결 | 부모의 자식 목록에 이 구조체를 연결 |

`waited`는 “자식이 종료했는가”가 아니라 **“부모가 이미 이 자식에 대한 wait 권리를 사용했는가”**다. 종료 완료 여부는 `exit_done`으로 전달하므로 별도의 `exited` 필드는 필수가 아니다.

### `child_status` Detail

 `child_status`는 **자식 프로세스 하나의 실행 결과를 보관하는 구조체**다. 자식 프로세스는 로드 성공 여부와(`load(filename)`), 종료 여부(`exit`)를 기록하고, 부모 프로세스는 해당 완료 신호를 기다린 뒤에 읽어 wait 여부 등과 같은 의사결정을 진행한다. 해당 구조체는 부모 프로세스가 자식 프로세스를 `wait`를 해야하는 경우를 위해 사용된다.

#### pid: 자식 thread의 ID
`pid`는 자식의 정수 ID다. 예를 들어 `pid == 3`이면 3번 자식의 기록이다. 부모가 `wait(3)`을 호출하면 자신의 자식 목록에서 pid 값이 3인 자식 객체의 정보를 찾는다.

#### load_ok / load_done

- `load_ok`: 로드가 성공했는지 저장하는 `bool` 값. 
- `load_done`: 로드 결과가 준비되었음을 알리는 세마포어. 즉 자식 프로세스가 일단 `load(filename)`을 시도했음을 의미한다. 부모가 아무 때나 읽으면 **아직 로드 중이라 false인지, 로드가 실패해서 false인지 구분할 수 없다.** 그래서 해당 변수를 도입하여 로드가 완료가 되었는지 안되었는를 구분한다.

다음은 앞뒤 처리를 생략한 순서 설명용 코드다. `status`는 부모와 자식이 공유하는 상태 객체의 포인터다.

```c
/* 자식: 먼저 결과를 쓰고, 그다음 알린다. */
status->load_ok = success;
sema_up (&status->load_done);

/* 부모: 알림을 기다린 다음 결과를 읽는다. */
sema_down (&status->load_done);
bool success = status->load_ok;
```

위 두 부분을 자세히 설명하자면, 한 함수에서 연달아 실행하는 코드가 아니라, 각각 자식과 부모 쪽의 동작이다.

세마포어를 `sema_init(&status->load_done, 0)`으로 초기화하면 처음에는 완료 신호가 없다.

| 실행 순서 | 동작 |
| --- | --- |
| 부모가 먼저 `sema_down()` | 신호가 없어 부모가 잠든다. 자식이 `sema_up()`하면 부모가 진행할 수 있다 |
| 자식이 먼저 `sema_up()` | 신호가 세마포어 값으로 남는다. 부모는 나중에 `sema_down()`해도 잠들 필요 없이 진행한다 |

따라서 “`load_done`을 받은 뒤 읽는다”는 말은 **`sema_down(&status->load_done)`을 통과한 다음 `load_ok`를 읽는다**는 뜻이다. 로드가 실패해도 `load_ok = false`를 기록하고 완료 신호를 보내야 한다. 그렇지 않으면 부모가 계속 기다린다.

#### exit_status와 exit_done: 종료 코드와 종료 완료 알림

- `exit_status`: 자식 프로세스의 종료 코드를 저장하는 `int` 값. 종료 여부를 나타내는 플래그가 아니다.
- `exit_done`: 종료 코드 기록과 프로세스 자원 정리가 끝났음을 알리는 세마포어. `bool` 플래그가 아니다.

`exit_status`의 초기값은 `-1`이다. 부모가 아무 때나 읽으면 **아직 실행 중이라 -1인지, 실제로 -1을 남기고 종료했는지 구분할 수 없다.** 그래서 완료 알림을 기다린 뒤 읽는다.

다음은 자식이 `exit(42)`를 호출한 경우의 순서 설명용 코드다. `status`는 부모와 자식이 공유하는 상태 객체의 포인터다. `main()`이 42를 반환해도 `_start()`가 `exit(42)`를 호출하므로 같다.

```c
/* 자식: 먼저 종료 코드를 쓰고, 자원 정리 후 알린다. */
status->exit_status = 42;
/* 파일과 페이지 디렉터리 등 자원 정리 */
sema_up (&status->exit_done);

/* 부모: 알림을 기다린 다음 종료 코드를 읽는다. */
sema_down (&status->exit_done);
int exit_status = status->exit_status;
```

위 두 부분은 한 함수에서 연달아 실행하는 코드가 아니라, 각각 자식과 부모 쪽의 동작이다. 상태 객체의 목록 제거와 메모리 해제 처리는 생략했다.

세마포어를 `sema_init(&status->exit_done, 0)`으로 초기화하면 처음에는 완료 신호가 없다.

| 실행 순서 | 동작 |
| --- | --- |
| 부모가 먼저 `sema_down()` | 신호가 없어 부모가 잠든다. 자식이 종료 정리 후 `sema_up()`하면 부모가 진행할 수 있다 |
| 자식이 먼저 `sema_up()` | 신호와 종료 코드가 남는다. 부모는 나중에 `sema_down()`해도 잠들 필요 없이 진행한다 |

따라서 “`exit_done`을 받은 뒤 읽는다”는 말은 **`sema_down(&status->exit_done)`을 통과한 다음 `exit_status`를 읽는다**는 뜻이다. 읽은 값은 `wait(pid)`의 반환값이 된다. 위 예시에서는 42다. 예외로 종료할 때도 -1을 기록하고 완료 신호를 보내야 부모가 계속 기다리지 않는다.

| 구분 | 저장된 결과 | 결과를 읽기 전에 기다리는 신호 | 부모의 대기 위치 |
| --- | --- | --- | --- |
| 실행 준비 | `load_ok`: 로드 성공 여부 | `load_done` | `process_execute()` |
| 실행 종료 | `exit_status`: 종료 코드 | `exit_done` | `process_wait()` |

#### waited: 기다릴 권리를 이미 사용했는가?

부모가 유효한 자식에 대해 처음 `wait(pid)`를 시작할 때 `true`로 바꿔 두번의 wait를 요청하는 일을 막는다.

#### refs와 refs_lock: 기록을 언제 지워도 되는가?

`refs`는 **이 상태 객체를 아직 사용할 권리를 가진 쪽의 수**다. 부모 몫 1개, 자식 몫 1개로 시작하므로 초기값은 2다.

```text
자식이 먼저 종료하는 경우:
  생성 준비              refs = 2  (부모 + 자식)
  자식이 사용을 끝냄      refs = 1  (부모가 나중에 읽을 수 있으므로 보존)
  부모가 wait로 읽음      refs = 0  → 상태 객체 해제

부모가 먼저 종료하는 경우:
  생성 준비              refs = 2  (부모 + 자식)
  부모가 사용을 끝냄      refs = 1  (자식이 아직 사용하므로 보존)
  자식이 종료함           refs = 0  → 상태 객체 해제
```

`refs_lock`은 이 참조 수를 바꾸는 작업을 한쪽씩 수행하게 하는 락이다. 부모와 자식이 동시에 사용을 끝내더라도 감소와 최종 해제 판단이 충돌하지 않도록 한다. `child_status`의 자세한 해제 순서는 6절에 정리되어 있다.

#### elem: 리스트 자체가 아니라 리스트에 연결하는 고리

부모의 `struct thread`에는 자식 프로세스들을 저장하는 `struct list child_statuses`이라는 목록이 있고, 이들은 **elem**을 가져 각 child_status 사이를 탐색할 수 있다.

```text
부모의 child_statuses: head와 tail 사이에 자식 상태들을 연결

           A의 child_status     B의 child_status     C의 child_status
           ┌────────────────┐  ┌────────────────┐  ┌────────────────┐
           │ pid = 3        │  │ pid = 4        │  │ pid = 5        │
           │ exit_status    │  │ exit_status    │  │ exit_status    │
head ◀───▶ │ elem           │◀▶│ elem           │◀▶│ elem           │ ◀───▶ tail
           └────────────────┘  └────────────────┘  └────────────────┘

각 상자 = 별도로 할당한 child_status 하나
화살표  = elem의 prev / next 포인터로 연결
```



아래는 Pintos 리스트 사용법만 보여주는 예시다. `child`의 나머지 필드는 사용하지 않으며 실제 프로세스 생성 코드는 아니다.

```c
struct list child_statuses;
list_init (&child_statuses);

struct child_status child;
child.pid = 3;
child.exit_status = 42;

/* 구조체 전체를 복사하는 것이 아니라, 그 안의 고리를 연결한다. */
list_push_back (&child_statuses, &child.elem);

struct list_elem *e = list_front (&child_statuses);
struct child_status *found = list_entry (e, struct child_status, elem);

ASSERT (found == &child);
ASSERT (found->pid == 3 && found->exit_status == 42);

list_remove (&found->elem);
ASSERT (list_empty (&child_statuses));
```

`list_entry(e, struct child_status, elem)`은 **“e가 가리키는 고리는 child_status 안의 elem이다. 그 고리가 들어 있는 원래 구조체 주소를 구해라”**라는 뜻이다. 따라서 `found->pid`처럼 원래 필드에 접근할 수 있다.

`list_remove()`는 연결만 끊는다. 메모리를 해제하지 않는다. 위 예시는 지역 변수를 사용하고 같은 범위 안에서 연결을 제거하지만, 실제 자식 상태는 생성 함수가 반환한 뒤에도 살아 있어야 하므로 동적 할당하고 `refs`가 0일 때 해제한다.

## 3. 부모와 자식은 상태 정보를 어떻게 찾나?

부모는 여러 자식을 만들 수 있다. 그래서 부모에게는 **자식 상태 목록**이 필요하다. 자식에게는 **자기 상태의 주소**가 필요하다.

`thread.h`에서 `struct thread`보다 앞에 다음 선언을 둔다.

```c
struct child_status;
```

“이 이름의 구조체가 있다”는 뜻이다. 주소만 저장할 때는 내부 필드를 몰라도 된다. 전체 구조체 정의는 앞서 정한 대로 `process.c`에 둔다.

`struct thread`의 `#ifdef USERPROG` 안에 두 필드를 추가한다.

```c
struct list child_statuses;           /* 부모 thread가 만든 자식들의 상태 목록 */
struct child_status *own_status;  /* 내 부모와 공유하는 나의 상태 주소 */
```

예를 들어 A가 B를 만들었다면:

```text
A의 child_statuses 목록 ──→ B의 child_status ←── B의 own_status
                      PID, 로드 결과,
                      종료 코드 등
```

두 군데에 결과를 복사하는 것이 아니라 **같은 상태 객체를 함께 보는 것**이다. B도 자식을 만들면 B의 `child_statuses`에 그 자식의 상태를 넣는다.

`thread.c`의 `init_thread()`에서 위 필드도 `#ifdef USERPROG` 안에서 초기화한다.

```c
list_init (&t->child_statuses);  /* 처음에는 자식이 없음 */
t->own_status = NULL;        /* 자기 상태는 start_process()에서 연결 */
```

첫 사용자 프로그램을 만드는 초기 커널 스레드도 `child_statuses` 목록이 필요하므로 모든 스레드에 대해 초기화한다. `own_status`가 NULL이면 부모에게 전달할 자기 상태가 없는 경우다.

`child_status`는 별도 할당한다. 자식의 `struct thread`가 없어져도 부모는 상태 객체를 계속 읽을 수 있다. 부모 스레드의 주소를 자식에 따로 저장할 필요도 없다.

**크기에도 주의한다.** `struct thread`와 커널 스택은 4KB를 나눠 쓴다. 큰 FD 배열이나 인자 배열은 별도 할당한다. 함수 안의 큰 지역 배열도 같은 커널 스택을 쓰므로 피한다.

## 4. 새 자식에게 상태 주소를 어떻게 전달하나?

현재 코드는 파일 이름의 주소 하나만 넘긴다.

```c
tid = thread_create (file_name, PRI_DEFAULT, start_process, fn_copy);
```

마지막 인자가 자식의 `start_process()`에 전달된다. 이제는 명령행과 상태 주소가 모두 필요하므로 둘을 묶는다. 이 구조체도 `process.c`의 함수들 앞에 둔다.

```c
struct start_info
  {
    char *cmdline;                 /* 복사한 명령행 주소 */
    struct child_status *status;  /* 부모와 공유할 상태 주소 */
  };
```

부모는 `start_info`를 `malloc()`으로 할당하고 다음처럼 넘긴다. 아래 예시는 할당 실패 처리 등을 생략했다.

```c
info->cmdline = fn_copy;
info->status = status;
tid = thread_create (program_name, PRI_DEFAULT, start_process, info);
```

`program_name`은 실행 파일 이름이다. 예를 들어 명령행이 `echo hello`이면 이름은 `echo`다. 명령행 분리와 사용자 인자 스택 구성은 별도로 구현해야 한다.

자식은 전달받은 주소에서 두 값을 꺼낸다.

```c
/* start_process(void *aux) 안 */
struct start_info *info = aux;
char *cmdline = info->cmdline;
struct child_status *status = info->status;

thread_current ()->own_status = status;
free (info);
```

`free(info)`는 두 주소를 담던 작은 구조체만 해제한다. `cmdline`과 `status`가 가리키는 메모리는 별도 할당했으므로 그대로 남는다.

메모리를 해제할 담당은 다음처럼 정한다.

| 메모리 | 자식 생성 성공 시 | 자식 생성 실패 시 |
| --- | --- | --- |
| `start_info` | 자식이 주소를 꺼낸 뒤 `free()` | 부모가 `free()` |
| 명령행 페이지 | 자식이 로드·인자 구성 후 `palloc_free_page()` | 부모가 `palloc_free_page()` |
| `child_status` | 부모와 자식 모두 사용을 끝내면 `free()` | 자식이 없으므로 부모가 `free()` |

생성 성공 후 부모는 `info`와 명령행을 다시 사용하지 않는다. 자식이 이미 해제했을 수 있다. 부모는 따로 보관한 `status` 주소로 결과를 확인한다.

## 5. 각 함수에서 할 일

아래는 앞으로 구현할 순서다. 현재 코드에 모두 들어 있다는 뜻은 아니다.

### process_execute(): 자식을 만들고 로드 결과 기다리기

이 함수는 부모가 실행한다.

1. 명령행 페이지, `child_status`, `start_info`를 할당한다. 할당 실패 시 앞에서 할당한 메모리를 해제하고 -1을 반환한다.
2. `child_status`를 2절 표대로 초기화한다. 두 세마포어는 0, `refs`는 2로 설정하고 락도 초기화한다.
3. `thread_create()`로 자식을 만든다. 실패하면 세 메모리를 모두 해제하고 -1을 반환한다.
4. 성공하면 반환된 TID를 `status->pid`에 저장하고 `status->elem`을 부모의 `child_statuses` 목록에 넣는다.
5. `sema_down(&status->load_done)`으로 자식의 로드 완료를 기다린다.
6. `load_ok`가 true면 PID를 반환한다. false면 목록에서 빼고 부모가 이 상태의 사용을 끝냈다고 처리한 뒤 -1을 반환한다. 사용 종료 처리는 6절에서 설명한다.

자식은 `thread_create()`가 반환하기 전에도 실행될 수 있다. 그래서 **초기화는 생성 전에** 한다. 생성 후 부모가 기록하는 `pid`와 자식 목록은 자식이 건드리지 않는다.

### start_process(): 실행 준비 후 부모에게 결과 알리기

이 함수는 새 자식이 실행한다.

1. 전달받은 `start_info`에서 명령행과 상태 주소를 꺼내고 `own_status`를 연결한다. `start_info`는 해제한다.
2. 기존 코드처럼 사용자 실행에 필요한 CPU 상태를 준비하고, `load()`와 인자 스택 구성을 수행한다.
3. 사용이 끝난 명령행 페이지를 해제한다.
4. 성공 여부를 `load_ok`에 쓴 뒤 `sema_up(&status->load_done)`을 호출한다.
5. 실패했으면 `thread_exit()`로 종료한다. 성공했으면 기존 `intr_exit` 코드로 사용자 모드에 들어간다.

**실패해도 완료 신호는 보내야 한다.** 자식이 시작된 뒤 어느 준비 단계에서 실패하든, 부모에게 로드 실패를 알리고 종료해야 부모가 계속 기다리는 일이 없다.

### process_wait(pid): 자식의 종료 코드 받기

이 함수는 부모가 실행한다.

1. 자신의 `child_statuses` 목록에서 PID가 같은 상태를 찾는다. 없거나 `waited`가 true면 -1을 반환한다.
2. `waited`를 true로 바꾼다.
3. `sema_down(&status->exit_done)`으로 자식 종료를 기다린다. 이미 종료했다면 바로 진행한다.
4. `exit_status`를 지역 변수에 복사하고 상태를 자식 목록에서 뺀다.
5. 부모가 이 상태의 사용을 끝냈다고 처리한다. 이때 메모리가 해제될 수 있으므로 이후 `status`를 읽지 않는다.
6. 복사한 종료 코드를 반환한다.

자식 목록과 `waited`는 부모만 수정한다. 이 Pintos에서는 프로세스당 실행 스레드가 하나이므로 이 둘을 위한 별도 락은 필요 없다.

### exit 시스템 콜: 종료 코드 저장하기

구조체 정의는 `process.c`에 있으므로 `syscall.c`에서 필드를 직접 수정하지 않는다. 대신 다음 함수를 추가한다.

```c
/* process.h: 함수 선언 */
void process_set_exit_status (int status);
```

함수 내용은 `process.c`에 둔다. 현재 스레드의 `own_status`가 NULL이 아니면 그 안의 `exit_status`에 값을 저장하도록 만든다.

`syscall.c`는 `process.h`를 포함하고, 사용자 인자를 검증해서 종료 코드를 읽은 뒤 다음을 실행한다.

```c
process_set_exit_status (status);
thread_exit ();
```

사용자 예외나 잘못된 사용자 주소 때문에 종료할 때는 -1을 저장한다. 이 함수는 값만 저장한다. 종료 메시지 출력은 공통 종료 처리에서 한 번만 하도록 구현한다.

### process_exit(): 정리하고 부모에게 종료 알리기

`thread_exit()`는 이미 `process_exit()`를 호출한다. 따라서 위 코드 앞에서 `process_exit()`를 따로 호출하지 않는다.

종료하는 프로세스는 자기 자식도 있을 수 있고, 누군가의 자식이기도 하다. 두 관계를 모두 정리한다.

1. 자신의 `child_statuses` 목록에서 상태를 하나씩 빼고 부모로서 사용을 끝낸다. 자식들이 종료할 때까지 기다리지는 않는다.
2. 열린 파일, 실행 파일, 페이지 디렉터리 등 자신의 자원을 정리한다. 기존 페이지 디렉터리 해제 순서는 유지한다.
3. `own_status`가 NULL이 아니면 그 주소를 지역 변수에 저장하고 `own_status`를 NULL로 바꾼다. 저장한 주소로 `sema_up(&status->exit_done)`을 호출한 뒤 자식으로서 상태 사용을 끝낸다.

부모는 3번의 신호를 받고 종료 코드를 읽는다. 자식 스레드의 4KB 페이지 자체는 이후 스케줄러가 해제한다.

## 6. child_status는 언제 해제하나?

**부모와 자식 둘 다 더 이상 사용하지 않을 때 해제한다.** 그때를 알기 위해 `refs`를 둔다.

```text
refs = 2  부모와 자식 모두 사용 중
refs = 1  한쪽만 사용 중
refs = 0  아무도 사용하지 않음 → free()
```

여기서 “사용을 끝낸다”는 말은 **자신의 몫으로 refs를 한 번 줄이고, 이후 이 상태에 접근하지 않는다**는 뜻이다.

| 누가 | 언제 refs를 1 줄이나? |
| --- | --- |
| 부모 | `wait()`로 결과를 읽은 뒤, 또는 로드 실패를 확인하고 상태를 버릴 때, 또는 wait하지 않고 부모가 종료할 때. 이 중 한 번만 |
| 자식 | 종료 정리를 마치고 `exit_done` 신호를 보낸 뒤 한 번 |

부모 쪽에서는 상태를 자식 목록에서 빼고 사용을 끝낸다. 그래야 나중에 부모가 종료할 때 같은 상태를 다시 처리하지 않는다.

숫자를 줄이고 해제 여부를 판단하는 동작은 `process.c`의 공통 함수 하나로 모은다. 함수는 다음 순서로 동작하게 만든다.

1. `refs_lock`을 잡는다.
2. `refs`를 1 줄인다.
3. 0이 되었는지를 지역 변수에 저장한다.
4. 락을 놓는다.
5. 0이었다면 상태를 `free()`한다.

락은 부모와 자식이 같은 숫자를 동시에 변경하지 못하게 한다. **락을 놓은 뒤 해제**해야 이미 해제된 메모리 안의 락을 건드리지 않는다. 이 함수 호출 후에는 상태가 없어졌을 수 있으므로 다시 접근하지 않는다.

| 실행 순서 | 상태 메모리는 어떻게 되나? |
| --- | --- |
| 자식이 먼저 종료 | 2 → 1. 부모가 나중에 wait로 읽거나 종료하면 1 → 0, 해제 |
| 부모가 먼저 종료 | 2 → 1. 자식은 계속 실행하고, 종료할 때 1 → 0, 해제 |
| 부모가 wait하지 않고 계속 실행 | 부모가 나중에 읽을 수 있으므로 상태를 보관. 부모 종료 시 부모 몫을 줄임 |
| 로드 실패 | 부모는 실패 확인 후, 자식은 종료 시 각각 1 감소. 마지막 쪽이 해제 |
| 스레드 생성 실패 | 자식이 전혀 없으므로 부모가 준비한 상태를 직접 해제 |

자식은 **먼저 `sema_up()`으로 알리고, 그다음 자신의 refs를 줄인다.** 신호를 받은 부모가 먼저 사용을 끝내더라도 자식 몫이 남아 있으므로, 자식이 신호를 보내는 도중 상태가 해제되지 않는다.

## 7. 구현 뒤 확인할 것

현재 문서는 설계 설명만 추가한 것이므로 아래 테스트를 실행하거나 통과했다고 주장하는 것은 아니다.

- `exec-missing`: 로드 실패가 -1로 돌아오며 부모가 멈추지 않는가?
- `wait-simple`, `wait-killed`: 정상·예외 종료 상태를 제대로 받는가?
- `wait-twice`, `wait-bad-pid`: 중복 대기와 비자식 PID를 거부하는가?
- 자식 선종료·부모 선종료·wait 없이 부모 종료 각각에서 중복 해제나 남은 참조가 없는가?

빌드와 제공 테스트는 `src/userprog`에서 진행한다. 예를 들어 구현 후 `make`를 하고 `build` 디렉터리에서 `make tests/userprog/wait-simple.result`로 개별 검사를 요청할 수 있다. 인자 전달과 시스템 콜 구현도 갖춰져 있어야 실행 흐름 전체를 검증할 수 있다.
