#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t svc_thread_main_init(void);
int svc_thread(int number, uint64_t* registers);
int ot2_signal_event(uint32_t handle);
// 1 = 该游戏目录下存在同名标记文件 | 1 = a marker file with this name exists in the game directory
int ot2_game_flag(const char* name);
// 该游戏的工作目录（boot.log、shaders、各诊断标记都在这里）；未启动时为空串
// The game's work directory (boot.log, shaders and the diagnostic markers live
// here); an empty string before boot.
const char* ot2_game_directory(void);
void ot2_host_log(const char* format, ...);
__attribute__((noreturn)) void ot2_host_die(const char* format, ...);
void guest_thread_init(void);
__attribute__((noreturn)) void guest_thread_exit(void);
__attribute__((noreturn)) void svc_thread_exit_on_host(void);
void crash_thread_init(void);
void dm_thread_enter(uint64_t entry,uint64_t argument,uint64_t stack_top);
uint64_t ot2_call_guest(const char* symbol,const uint64_t* arguments);
uint64_t ot2_call_guest_address(uint64_t address,const uint64_t* arguments);
uint64_t ot2_guest_symbol_address(const char* symbol);
// Callable entry that runs a gated function's original body (the gate replaced
// its first instruction). An ungated address comes back unchanged; 0 means the
// gate cannot be stepped around and the caller must not call the export.
uint64_t ot2_original_entry(uint64_t entry);
// Android library calls may exceed small SDK thread stacks. Preserve the
// suspended guest stack for callbacks while running this service on its host stack.
uint64_t ot2_call_on_host_stack(uint64_t (*function)(uint64_t*),uint64_t* arguments);
void* ot2_native_gate(const char* name,void* function);
void svc_thread_dump_waiters(void);
void svc_thread_dump_addresses(void);
// 最后 192 条同步事件的时序（谁最后睡下、之前最后一条唤醒是什么） | Ordering of the last 192 synchronization events (who slept last, what the last wakeup was)
void svc_thread_dump_timeline(void);
void svc_thread_note_address(uint64_t address,int is_signal);
#ifdef __cplusplus
}
#endif
