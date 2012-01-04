#define _GNU_SOURCE

#include <assert.h>
#include <string.h>

#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include "write_trace.h"
#include "rec_process_event.h"
#include "rec_sched.h"
#include "handle_signal.h"

#include "../share/hpc.h"
#include "../share/ipc.h"
#include "../share/sys.h"
#include "../share/util.h"

#define PTRACE_EVENT_NONE			0
#define GET_EVENT(status)	 		((0xFF0000 & status) >> 16)

/**
 * Single steps to the next event that must be recorded. This can either be a system call, or reading the time
 * stamp counter (for now)
 */
void goto_next_event_singlestep(struct context* context)
{
	pid_t tid = context->child_tid;

	while (1) {
		int inst_size;
		char* inst = get_inst(tid, 0, &inst_size);
		if ((strncmp(inst, "sysenter", 7) == 0) || (strncmp(inst, "int", 3) == 0)) {
			record_inst_done(context);
			free(inst);
			printf("breaking out\n");
			break;
		}
		record_inst(context, inst);
		free(inst);
		if (context->pending_sig != 0) {
			//printf("pending sig: %d\n", context->pending_sig);
		}

		sys_ptrace_singlestep(tid, context->pending_sig);
		sys_waitpid(tid, &(context->status));

		if (WSTOPSIG(context->status) == SIGSEGV) {
			break;
		}
	}

	assert(GET_EVENT(context->status)==0);
}

static void cont_nonblock(struct context* context)
{
	sys_ptrace_syscall_sig(context->child_tid, context->pending_sig);
	context->pending_sig = 0;
}


static void check_event(struct context *ctx) {
	if (ctx->event < 0) {
		fprintf(stderr,"event: %d\n",ctx->event);
		fprintf(stderr,"status: %x  signal %x\n",ctx->status,WSTOPSIG(ctx->status));
		print_register_file_tid(ctx->child_tid);
		perror("bitch");

		cont_block(ctx);

		print_register_file_tid(ctx->child_tid);
		//sys_exit();
	}
}

static int wait_nonblock(struct context *ctx)
{
	int ret = sys_waitpid_nonblock(ctx->child_tid, &(ctx->status));

	if (ret) {
		ctx->event = read_child_orig_eax(ctx->child_tid);
		handle_signal(ctx);
		check_event(ctx);
	}

	return ret;
}

void cont_block(struct context *ctx)
{
	goto_next_event(ctx);
	handle_signal(ctx);
}

static int allow_ctx_switch(struct context *ctx)
{
	int event = ctx->event;

	/* int futex(int *uaddr, int op, int val, const struct timespec *timeout, int *uaddr2, int val3); */
	if (event == SYS_futex) {
		int op = read_child_ecx(ctx->child_tid) & FUTEX_CMD_MASK;
		printf("op: %x\n",op);


		if (op == FUTEX_WAIT) {
			return 1;
		}

		if (op == FUTEX_WAKE || op == FUTEX_WAKE_OP || op == FUTEX_WAKE_PRIVATE) {
			//return 0;
		}
		return 0;
	}

	return 1;
}

void start_recording()
{
	struct context *ctx = NULL;

	/* record the initial status of the register file */
	ctx = get_active_thread(ctx);
	ctx->event = -1000;
	record_event(ctx, 0);

	while (rec_sched_get_num_threads() > 0) {
		/* get a thread that is ready to be executed */
		ctx = get_active_thread(ctx);

		/* the child process will either be interrupted by: (1) a signal, or (2) at
		 * the entry of the system call */
		//debug_print("%d: state %d\n", ctx->child_tid, ctx->exec_state);
		/* simple state machine to guarantee process in the application */
		switch (ctx->exec_state) {

		case EXEC_STATE_START:
		{
			//goto_next_event_singlestep(context);

			/* we need to issue a blocking continue here to serialize program execution */
			cont_block(ctx);
			//printf("%d: state 1: %ld\n",ctx->child_tid,read_child_orig_eax(ctx->child_tid));
			ctx->allow_ctx_switch = allow_ctx_switch(ctx);

			/* state might be overwritten if a signal occurs */
			if (ctx->event == SIG_SEGV_RDTSC || ctx->event == USR_SCHED) {
				ctx->allow_ctx_switch = 1;
			} else if (ctx->pending_sig) {
				ctx->allow_ctx_switch = 0;
				assert(ctx->event != SYS_rt_sigreturn);
			/* these system calls never return; we remain in the same execution state */
			} else if (ctx->event == SYS_sigreturn || ctx->event == SYS_rt_sigreturn) {
				/* we are at the entry of a system call */
			} else if (ctx->event > 0) {
				ctx->exec_state = EXEC_STATE_ENTRY_SYSCALL;

				/* this is a wired state -- no idea why it works */
			} else if (ctx->event == SYS_restart_syscall) {
				ctx->exec_state = EXEC_STATE_ENTRY_SYSCALL;
				ctx->allow_ctx_switch = 1;
				assert(1==0);
			}

			record_event(ctx, 0);
			break;
		}

		case EXEC_STATE_ENTRY_SYSCALL:
		{

			if (read_child_eax(ctx->child_tid) != -38) {
				assert(1==0);
//				ctx->exec_state = EXEC_STATE_START;
				//			break;
			}

			if (ctx->pending_sig != 0) {
				printf("pending signal in syscall entry: %d\n",ctx->pending_sig);
				assert(1==0);
			}
			/* continue and execute the system call */
			cont_nonblock(ctx);
			ctx->exec_state = EXEC_STATE_IN_SYSCALL;
			break;
		}

		case EXEC_STATE_IN_SYSCALL:
		{
			int ret = wait_nonblock(ctx);
			if (ret) {
				assert(ctx->pending_sig == 0);

				/* we received a signal while in the system call and send it right away*/
				/* we have already sent the signal and process sigreturn */
				if (ctx->event == SYS_sigreturn) {
					assert(1==0);
				}

				if (ctx->pending_sig) {
					printf("received signal in system call: %d  event: %d\n", ctx->pending_sig, ctx->event);
					assert(1==0);
					ctx->exec_state = EXEC_STATE_ENTRY_SYSCALL;
				}



				/* handle events */
				int event = GET_EVENT(ctx->status);
				switch (event) {

				case PTRACE_EVENT_NONE:
				{
					break;
				}

				case PTRACE_EVENT_CLONE:
				case PTRACE_EVENT_FORK:
				{
					/* get new tid, register at the scheduler and setup HPC */
					int new_tid = sys_ptrace_getmsg(ctx->child_tid);

					/* ensure that clone was successful */
					if (read_child_eax(ctx->child_tid) == -1) {
						fprintf(stderr, "error in clone system call -- bailing out\n");
						sys_exit();
					}

					/* wait until the new thread is ready */
					sys_waitpid(new_tid, &ctx->status);
					rec_sched_register_thread(ctx->child_tid, new_tid);

					/* execute an additional ptrace_sysc((0xFF0000 & status) >> 16), since we setup trace like that. */
					cont_block(ctx);
					assert(signal_pending(ctx->status) == 0);
					break;
				}

				case PTRACE_EVENT_EXEC:
				{
					cont_block(ctx);
					assert(signal_pending(ctx->status) == 0);
					break;
				}

				case PTRACE_EVENT_VFORK_DONE:
				case PTRACE_EVENT_EXIT:
				{
					ctx->event = USR_EXIT;
					record_event(ctx, 1);
					rec_sched_deregister_thread(&ctx);
					break;
				}

				default:
				{
					fprintf(stderr, "Unknown ptrace event: %x -- baling out\n", event);
					sys_exit();
				}

				} /* end switch */

				/* done event handling */


				if (ctx != NULL) {
					rec_process_syscall(ctx);
					record_event(ctx, 1);
					ctx->exec_state = EXEC_STATE_START;
					ctx->allow_ctx_switch = 1;
				}

				}
			break;
		}
		default:
		errx(1, "Unknown execution state: %x -- bailing out\n", ctx->exec_state);
			break;
		}
	} /* while loop */
}
