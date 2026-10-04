# 1_2 구현 기록

`1_2.md`의 자식 상태 관리와 사용자 `exec`·`wait`·`exit` 연결을 구현했다. 표 전체의 파일 시스템 콜 구현은 이번 변경에 포함하지 않았다.

## 변경한 코드

링크는 이 문서 기준의 상대 경로와 줄 번호 앵커(#L)를 사용한다.

| 파일(+URL) | line | 구현한 내용 |
| --- | --- | --- |
| [process.c](../src/userprog/process.c#L26) | 26, 39 | `child_status`와 `start_info` 정의. PID·로드 결과·종료 코드·대기 여부·세마포어·참조 수를 자식별로 별도 할당 |
| [process.c](../src/userprog/process.c#L47) | 47 | `child_status_release()`: 락으로 refs 감소를 보호하고, 부모와 자식 모두 사용을 끝냈을 때 해제 |
| [process.c](../src/userprog/process.c#L64) | 64 | `process_execute()`: 공유 상태 초기화, 자식 생성, 부모 목록 등록, 로드 완료 대기. 할당·생성·로드 실패 시 메모리 정리. 한 페이지 이상 길이의 문자열 거부 |
| [process.c](../src/userprog/process.c#L127) | 127 | `start_process()`: 전달받은 상태를 `own_status`에 연결하고 load 결과를 기록한 뒤 부모에게 신호 전달. 실패 때도 통지 |
| [process.c](../src/userprog/process.c#L171) | 171 | `process_wait()`: 자기 자식 검색, 중복 대기 거부, 종료 신호 대기, 종료 코드 반환, 목록 제거와 부모 참조 해제 |
| [process.c](../src/userprog/process.c#L196) | 196 | `process_set_exit_status()`: syscall.c에서 구조체 내부를 알 필요 없이 종료 코드를 저장 |
| [process.c](../src/userprog/process.c#L205) | 205 | `process_exit()`: 로드된 사용자 프로세스의 종료 메시지 출력, 자기 자식들에 대한 부모 참조 해제, 기존 페이지 디렉터리 정리 후 자기 종료 통지와 자식 참조 해제 |
| [process.h](../src/userprog/process.h#L9) | 9 | `process_set_exit_status()` 선언. 구조체 정의는 process.c에 유지 |
| [thread.h](../src/threads/thread.h#L82) | 82, 101–102 | 전방 선언과 `child_statuses`, `own_status` 추가. 실제 필드는 `#ifdef USERPROG` 안에 둠 |
| [thread.c](../src/threads/thread.c#L467) | 467–470 | `init_thread()`에서 자식 목록과 자기 상태 포인터 초기화 |
| [syscall.c](../src/userprog/syscall.c#L14) | 14, 28 | 사용자 주소를 바이트별로 검증하여 시스템 콜 번호·인자 읽기. NULL·커널 주소·미매핑 주소를 거부하고 -1 종료. 페이지 경계를 넘는 정수도 검사 |
| [syscall.c](../src/userprog/syscall.c#L49) | 49 | `SYS_EXIT`, `SYS_WAIT`, `SYS_EXEC` 분기와 반환값 전달. exec 문자열은 최대 한 페이지까지 NUL을 확인한 뒤 커널 메모리로 복사. 지원하지 않는 시스템 콜은 -1 종료 |

기존 `thread_exit()` → `process_exit()` 연결을 사용했다. `exception.c`는 수정하지 않았다. 사용자 예외 종료는 기존 `thread_exit()` 경로를 통해 초기 종료 코드 -1을 전달한다.

## 아직 구현하지 않은 부분

- 프로그램명·명령행 인자 분리와 `argc/argv` 사용자 스택 구성. 현재 exec는 인자 없는 실행 파일명으로 검증했다. 일반 C 사용자 프로그램은 아직 정상 실행을 보장하지 않는다.
- 표준 입출력, 파일 시스템 콜, FD 관리, 실행 파일 쓰기 금지와 파일 시스템 공통 락.
- `halt`, 추가 두 시스템 콜과 `additional` 프로그램.

현재 할당하는 자원은 상태 객체·전달 객체·명령행 페이지·기존 사용자 메모리다. 아직 없는 FD와 실행 파일 보관 필드는 추가하지 않았으므로 그 자원의 종료 정리도 다음 구현에서 연결해야 한다.

## 검증 결과

- `make -C src/userprog -j4`: 통과.
- `make -C src/threads -j4`: 통과. USERPROG 없는 빌드도 확인.
- 기존 어셈블리의 `.note.GNU-stack` 링커 경고가 남아 있다. 이번 C 변경의 컴파일 경고는 없었다.
- QEMU에서 아래 독립 테스트 실행: `status-check: exit(0)` 확인.
- 확인한 동작: 자식 선종료, 부모의 종료 대기, 종료 코드 42 반환, 중복 wait 거부, 잘못된 PID 거부, 잘못된 사용자 스택의 -1 종료, 없는 실행 파일 로드 100회 실패, wait 없이 부모가 먼저 종료한 뒤 자식 종료.
- 인자 스택과 write가 필요한 기존 `wait-simple` 등 전체 채점 테스트의 통과를 주장하지 않는다. 모든 할당 실패 지점의 강제 주입이나 정확한 메모리 누수 계측도 수행하지 않았다.

## 검증 재실행

저장소 루트에서 아래 Python 코드를 실행한다. 테스트 소스와 실행 파일은 `/tmp`에만 만든다. 실제 커널 소스는 수정하지 않는다. 아래 프로그램은 C의 `_start(argc, argv)` 대신 어셈블리 진입점에서 직접 시스템 콜을 호출하므로, 아직 구현하지 않은 인자 스택·표준 출력에 의존하지 않는다.

```python
from pathlib import Path
import subprocess
import tempfile

root = Path.cwd()
assert (root / "src/userprog/process.c").is_file(), "저장소 루트에서 실행하세요"
subprocess.run(["make", "-C", "src/userprog", "-j4"], check=True)
tmp = Path(tempfile.mkdtemp(prefix="pintos-status-check-"))
sources = {}
sources["status-child"] = """
.section .text
.global _start
_start:
pushl $42
pushl $1
int $0x30
ud2
"""

sources["status-slow"] = """
.section .text
.global _start
_start:
movl $20000000, %ecx
1: loop 1b
pushl $42
pushl $1
int $0x30
ud2
"""

sources["status-bad"] = """
.section .text
.global _start
_start:
xorl %esp, %esp
int $0x30
ud2
"""

sources["status-orphan"] = """
.section .text
.global _start
_start:
pushl $name
pushl $2
int $0x30
addl $8, %esp
cmpl $-1, %eax
je fail
pushl $7
jmp done
fail: pushl $99
done: pushl $1
int $0x30
ud2
.section .rodata
name: .asciz "status-slow"
"""

sources["status-check"] = """
.section .text
.global _start
_start:

# Child exits first; preserve the returned PID in ebx.
pushl $child
pushl $2
int $0x30
addl $8, %esp
cmpl $-1, %eax
je fail
movl %eax, %ebx
movl $20000000, %ecx
1: loop 1b
pushl %ebx
pushl $3
int $0x30
addl $8, %esp
cmpl $42, %eax
jne fail
# A second wait must fail.
pushl %ebx
pushl $3
int $0x30
addl $8, %esp
cmpl $-1, %eax
jne fail
# No such child.
pushl $123456
pushl $3
int $0x30
addl $8, %esp
cmpl $-1, %eax
jne fail
# Parent waits before the child exits.
pushl $slow
pushl $2
int $0x30
addl $8, %esp
pushl %eax
pushl $3
int $0x30
addl $8, %esp
cmpl $42, %eax
jne fail
# Invalid user stack must terminate the child with -1.
pushl $bad
pushl $2
int $0x30
addl $8, %esp
cmpl $-1, %eax
je fail
pushl %eax
pushl $3
int $0x30
addl $8, %esp
cmpl $-1, %eax
jne fail
# Repeat load failures to exercise cleanup.
movl $100, %esi
2:
pushl $missing
pushl $2
int $0x30
addl $8, %esp
cmpl $-1, %eax
jne fail
decl %esi
jnz 2b
# Child exits without waiting for its own child.
pushl $orphan
pushl $2
int $0x30
addl $8, %esp
cmpl $-1, %eax
je fail
pushl %eax
pushl $3
int $0x30
addl $8, %esp
cmpl $7, %eax
jne fail
# Give the orphan time to exit before the kernel shuts down.
movl $100000000, %ecx
3: loop 3b
pushl $0
jmp done
fail: pushl $99
done: pushl $1
int $0x30
ud2
.section .rodata
child: .asciz "status-child"
slow: .asciz "status-slow"
bad: .asciz "status-bad"
missing: .asciz "missing"
orphan: .asciz "status-orphan"
"""

for name, source in sources.items():
    asm = tmp / (name + ".S")
    obj = tmp / (name + ".o")
    asm.write_text(source)
    subprocess.run(["as", "--32", str(asm), "-o", str(obj)], check=True)
    subprocess.run(["ld", "-melf_i386", "-z", "noseparate-code", "-T",
                    str(root / "src/lib/user/user.lds"), str(obj),
                    "-o", str(tmp / name)], check=True)

command = [str(root / "src/utils/pintos"), "--qemu", "-v", "-k", "-T", "60",
           "--filesys-size=2"]
for name in sources:
    command += ["-p", str(tmp / name), "-a", name]
command += ["--", "-q", "-f", "run", "status-check"]
run = subprocess.run(command, cwd=root / "src/userprog/build",
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                     text=True, timeout=90)
output = run.stdout
(tmp / "output").write_text(output)
assert run.returncode == 0, output
assert "status-check: exit(0)" in output, output
assert "status-child: exit(42)" in output, output
assert "status-bad: exit(-1)" in output, output
assert output.count("load: missing: open failed") == 100, output
assert "status-orphan: exit(7)" in output, output
assert output.index("status-orphan: exit(7)") < output.rindex("status-slow: exit(42)"), output
assert "PANIC" not in output and "TIMEOUT" not in output, output
print("PASS:", tmp / "output")
```
