/* x86 instruction injector */
/* domas // @xoreaxeaxeax */

/* some logic in the fault handler requires compiling without optimizations */

#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <execinfo.h>
#include <limits.h>
#include <ucontext.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <assert.h>
#include <sched.h>
#include <pthread.h>
#include <sys/wait.h>

/* configuration */

struct {
	/* main limit on search is # of prefixes to explore */
	bool allow_dup_prefix; 
	int max_prefix;
	int brute_depth;
	long seed;
	int range_bytes;
	bool show_tick;
	int jobs;
	bool force_core;
	int core;
	/* run as root to allow access to [0].  this will allow most memory accesses
	 * generated by the injector to succeed rather than fault, which permits
	 * improved fault analysis (e.g. sigsegv will preempt sigfpe; eliminating
	 * the initial sigsegv will allow reception of the more descriptive signals) */
	bool enable_null_access;
	bool nx_support;
} config={
	.allow_dup_prefix=false,
	.max_prefix=0,
	.brute_depth=4,
	.seed=0,
	.range_bytes=0,
	.show_tick=false,
	.jobs=1,
	.force_core=false,
	.core=0,
	.enable_null_access=false,
	.nx_support=true,
};

/* capstone */

#define USE_CAPSTONE true /* sifter offloads some capstone work to injector */

#if USE_CAPSTONE
	#include <capstone/capstone.h>
	#if __x86_64__
		#define CS_MODE CS_MODE_64
	#else
		#define CS_MODE CS_MODE_32
	#endif
#endif

#if USE_CAPSTONE
csh capstone_handle;
cs_insn *capstone_insn;
#endif

/* 32 vs 64 */

#if __x86_64__
	#define IP REG_RIP 
#else
	#define IP REG_EIP 
#endif

/* leave state as 0 */
/* : encourages instructions to access a consistent address (0) */
/* : avoids crashing the injector (e.g. incidental write to .data) */
/* only change when necessary for synthesizing specific instructions */
#if __x86_64__
typedef struct {
	uint64_t rax;
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rsi;
	uint64_t rdi;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t rbp;
	uint64_t rsp;
} state_t;
state_t inject_state={
	.rax=0,
	.rbx=0,
	.rcx=0,
	.rdx=0,
	.rsi=0,
	.rdi=0,
	.r8=0,
	.r9=0,
	.r10=0,
	.r11=0,
	.r12=0,
	.r13=0,
	.r14=0,
	.r15=0,
	.rbp=0,
	.rsp=0,
};
#else
typedef struct {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t esi;
	uint32_t edi;
	uint32_t ebp;
	uint32_t esp;
} state_t;
state_t inject_state={
	.eax=0,
	.ebx=0,
	.ecx=0,
	.edx=0,
	.esi=0,
	.edi=0,
	.ebp=0,
	.esp=0,
};
#endif

/* helpers */

#define STR(x) #x
#define XSTR(x) STR(x)

/* x86/64 */

#define UD2_SIZE  2
#define PAGE_SIZE 4096
#define TF	    0x100

/* injection */

#define USE_TF true /* leave true, except when synthesizing some specific instructions */

typedef enum { BRUTE, RAND, TUNNEL, DRIVEN } search_mode_t;
search_mode_t mode=TUNNEL;

void* packet_buffer;
char* packet;

#ifdef SIGSTKSZ
#undef SIGSTKSZ
#endif
#define SIGSTKSZ 65536
static char stack[SIGSTKSZ];
stack_t ss = { .ss_size = SIGSTKSZ, .ss_sp = stack, };

struct {
	uint64_t dummy_stack_hi[256];
	uint64_t dummy_stack_lo[256];
} dummy_stack __attribute__ ((aligned(PAGE_SIZE)));

#define MAX_INSN_LENGTH 15 /* actually 15 */

/* fault handler tries to use fault address to make an initial guess of
 * instruction length; but length of jump instructions can't be determined from
 * trap alone */
/* set to this if something seems wrong */
#define JMP_LENGTH 16 

typedef struct {
	uint8_t bytes[MAX_INSN_LENGTH];
	int len; /* the number of specified bytes in the instruction */
} insn_t;

typedef struct {
	insn_t i;
	int index;
	int last_len;
} inj_t;
inj_t inj;

static const insn_t null_insn={};

mcontext_t fault_context;

/* feedback */

typedef enum { TEXT, RAW } output_t;
output_t output=TEXT;

#define TICK_MASK 0xffff

#define RAW_REPORT_INSN_BYTES 16

#define RAW_REPORT_DISAS_MNE false /* sifter assumes false */
#define RAW_REPORT_DISAS_MNE_BYTES 16
#define RAW_REPORT_DISAS_OPS false /* sifter assumes false */
#define RAW_REPORT_DISAS_OPS_BYTES 32
#define RAW_REPORT_DISAS_LEN true  /* sifter assumes true */
#define RAW_REPORT_DISAS_VAL true  /* sifter assumes true */

typedef struct __attribute__ ((packed)) {
	uint32_t valid;
	uint32_t length;
	uint32_t signum;
	uint32_t si_code;
	uint32_t addr;
} result_t;
result_t result;

typedef struct __attribute__ ((packed)) {
#if RAW_REPORT_DISAS_MNE
	char mne[RAW_REPORT_DISAS_MNE_BYTES];
#endif
#if RAW_REPORT_DISAS_OPS
	char ops[RAW_REPORT_DISAS_OPS_BYTES];
#endif
#if RAW_REPORT_DISAS_LEN
	int len;
#endif
#if RAW_REPORT_DISAS_VAL
	int val;
#endif
} disas_t;
disas_t disas;

/* blacklists */

#define MAX_BLACKLIST 128

typedef struct {
	char* opcode;
	char* reason;
} ignore_op_t;

ignore_op_t opcode_blacklist[MAX_BLACKLIST]={
	{ "\x0f\x34", "sysenter" },
	{ "\x0f\xa1", "pop fs" },
	{ "\x0f\xa9", "pop gs" },
	{ "\x8e", "mov seg" },
	{ "\xc8", "enter" },
#if !__x86_64__
	/* vex in 64 (though still can be vex in 32...) */
	{ "\xc5", "lds" },
	{ "\xc4", "les" },
#endif
	{ "\x0f\xb2", "lss" },
	{ "\x0f\xb4", "lfs" },
	{ "\x0f\xb5", "lgs" },
#if __x86_64__
	/* 64 bit only - intel "discourages" using this without a rex* prefix, and
	 * so capstone doesn't parse it */
	{ "\x63", "movsxd" }, 
#endif
	/* segfaulting with various "mov sp" (always sp) in random synthesizing, too
	 * tired to figure out why: 66 bc7453 */
	{ "\xbc", "mov sp" },
	/* segfaulting with "shr sp, 1" (always sp) in random synthesizing, too tired to
	 * figure out why: 66 d1ec */
	/* haven't observed but assuming "shl sp, 1" and "sar sp, 1" fault as well */
	{ "\xd1\xec", "shr sp, 1" },
	{ "\xd1\xe4", "shl sp, 1" },
	{ "\xd1\xfc", "sar sp, 1" },
	/* same with "rcr sp, 1", assuming same for rcl, ror, rol */
	{ "\xd1\xdc", "rcr sp, 1" },
	{ "\xd1\xd4", "rcl sp, 1" },
	{ "\xd1\xcc", "ror sp, 1" },
	{ "\xd1\xc4", "rol sp, 1" },
	/* same with lea sp */
	{ "\x8d\xa2", "lea sp" },
	/* i guess these are because if you shift esp, you wind up way outside your
	 * address space; if you shift sp, just a little, you stay in and crash */
	/* unable to resolve a constant length for xbegin, causes tunnel to stall */
	{ "\xc7\xf8", "xbegin" },
	/* int 80 will obviously cause issues */
	{ "\xcd\x80", "int 0x80" },
	/* as will syscall */
	{ "\x0f\x05", "syscall" },
	/* ud2 is an undefined opcode, and messes up a length differential search
	 * b/c of the fault it throws */
	{ "\x0f\xb9", "ud2" },
	{ "\xc2", "ret 0x0000" },
	{ NULL, NULL }
};

typedef struct {
	char* prefix;
	char* reason;
} ignore_pre_t;

ignore_pre_t prefix_blacklist[]={
#if !__x86_64__
	/* avoid overwriting tls or something in 32 bit code */
	{ "\x65", "gs" },
#endif
	{ NULL, NULL }
};

/* search ranges */

typedef struct { insn_t start; insn_t end; bool started; } range_t;
insn_t* range_marker=NULL;
range_t search_range={};
range_t total_range={
	.start={.bytes={0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, .len=0},
	.end={.bytes={0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}, .len=0},
	.started=false
};

/* processes */

pthread_mutex_t* pool_mutex=NULL;
pthread_mutex_t* output_mutex=NULL;
pthread_mutexattr_t mutex_attr;

/* syncronized output */

#define LINE_BUFFER_SIZE 256
#define BUFFER_LINES 16
#define SYNC_LINES_STDOUT BUFFER_LINES /* must be <= BUFFER_LINES */
#define SYNC_LINES_STDERR BUFFER_LINES /* must be <= BUFFER_LINES */
char stdout_buffer[LINE_BUFFER_SIZE*BUFFER_LINES];
char* stdout_buffer_pos=stdout_buffer;
int stdout_sync_counter=0;
char stderr_buffer[LINE_BUFFER_SIZE*BUFFER_LINES];
char* stderr_buffer_pos=stderr_buffer;
int stderr_sync_counter=0;

/* functions */

#if USE_CAPSTONE
int print_asm(FILE* f);
#endif
void print_mc(FILE*, int);
bool is_prefix(uint8_t);
int prefix_count(void);
bool has_dup_prefix(void);
bool has_opcode(uint8_t*);
bool has_prefix(uint8_t*);
void preamble(void);
void inject(int);
void state_handler(int, siginfo_t*, void*);
void fault_handler(int, siginfo_t*, void*);
void configure_sig_handler(void (*)(int, siginfo_t*, void*));
void give_result(FILE*);
void usage(void);
bool move_next_instruction(void);
bool move_next_range(void);

extern char debug, resume, preamble_start, preamble_end;
static int expected_length;

void sync_fprintf(FILE* f, const char *format, ...)
{
	va_list args;
	va_start(args, format);

	if (f==stdout) {
		stdout_buffer_pos+=vsprintf(stdout_buffer_pos, format, args);
	}
	else if (f==stderr) {
		stderr_buffer_pos+=vsprintf(stderr_buffer_pos, format, args);
	}
	else {
		assert(0);
	}

	va_end(args);
}

void sync_fwrite(const void* ptr, size_t size, size_t count, FILE* f)
{
	if (f==stdout) {
		memcpy(stdout_buffer_pos, ptr, size*count);
		stdout_buffer_pos+=size*count;
	}
	else if (f==stderr) {
		memcpy(stderr_buffer_pos, ptr, size*count);
		stderr_buffer_pos+=size*count;
	}
	else {
		assert(0);
	}
}

void sync_fflush(FILE* f, bool force)
{
	if (f==stdout) {
		stdout_sync_counter++;
		if (stdout_sync_counter==SYNC_LINES_STDOUT || force) {
			stdout_sync_counter=0;
			pthread_mutex_lock(output_mutex);
			fwrite(stdout_buffer, stdout_buffer_pos-stdout_buffer, 1, f);
			fflush(f);
			pthread_mutex_unlock(output_mutex);
			stdout_buffer_pos=stdout_buffer;
		}
	}
	else if (f==stderr) {
		stderr_sync_counter++;
		if (stderr_sync_counter==SYNC_LINES_STDERR || force) {
			stderr_sync_counter=0;
			pthread_mutex_lock(output_mutex);
			fwrite(stderr_buffer, stderr_buffer_pos-stderr_buffer, 1, f);
			fflush(f);
			pthread_mutex_unlock(output_mutex);
			stderr_buffer_pos=stderr_buffer;
		}
	}
	else {
		assert(0);
	}
}

void zero_insn_end(insn_t* insn, int marker)
{
	int i;
	for (i=marker; i<MAX_INSN_LENGTH; i++) {
		insn->bytes[i]=0;
	}
}

bool increment_range(insn_t* insn, int marker)
{
	int i=marker-1;
	zero_insn_end(insn, marker);

	if (i>=0) {
		insn->bytes[i]++;
		while (insn->bytes[i]==0) {
			i--;
			if (i<0) {
				break;
			}
			insn->bytes[i]++;
		}
	}

	insn->len=marker;

	return i>=0;
}

void print_insn(FILE* f, insn_t* insn)
{
	int i;
	for (i=0; i<sizeof(insn->bytes); i++) {
		sync_fprintf(f, "%02x", insn->bytes[i]);
	}
}

void print_range(FILE* f, range_t* range)
{
	print_insn(f, &range->start);
	sync_fprintf(f, ";");
	print_insn(f, &range->end);
}

/* must call before forking */
void initialize_ranges(void)
{
	if (range_marker==NULL) {
		range_marker=mmap(NULL, sizeof *range_marker, 
				PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
		*range_marker=total_range.start;
	}
}

void free_ranges(void)
{
	if (range_marker!=NULL) {
		munmap(range_marker, sizeof *range_marker);
	}
}

bool move_next_range(void)
{
	bool result=true;

	switch (mode) {
		case RAND:
		case DRIVEN:
			if (search_range.started) {
				result=false;
			}
			else {
				search_range=total_range;
			}
			break;
		case BRUTE:
		case TUNNEL:
			pthread_mutex_lock(pool_mutex);
			search_range.started=false;
			if (memcmp(range_marker->bytes, total_range.end.bytes,
						sizeof(range_marker->bytes))==0) {
				/* reached end of range */
				result=false;
			}
			else {
				search_range.start=*range_marker;
				search_range.end=*range_marker;
//TODO: there are search bugs here
//#error make sure you don't skip over the first instruction (e.g. 000000...)
//#error there's another error here somewhere...
//somehow take start range from end range..
//len can mostly be taken from range_bytes WHEN YOU MOVE TO A NEW RANGE but
//needs to be from total_range.start/end.len when you are deriving from that
//right now len is set in total_range, and in increment_range for range.end
				if (!increment_range(&search_range.end, config.range_bytes)) {
					/* if increment rolled over, set to end */
					search_range.end=total_range.end;
				}
				else if (memcmp(search_range.end.bytes,
							total_range.end.bytes, sizeof(search_range.end.bytes))>0) {
					/* if increment moved past end, set to end */
					search_range.end=total_range.end;
				}

				*range_marker=search_range.end;
			}
			pthread_mutex_unlock(pool_mutex);
			break;
		default:
			assert(0);
	}

	return result;
}

#if USE_CAPSTONE
int print_asm(FILE* f)
{
	if (output==TEXT) {
		uint8_t* code=inj.i.bytes;
		size_t code_size=MAX_INSN_LENGTH;
		uint64_t address=(uintptr_t)packet_buffer;
	
		if (cs_disasm_iter(
				capstone_handle,
				(const uint8_t**)&code,
				&code_size,
				&address,
				capstone_insn)
			) {
			sync_fprintf(
				f,
				"%10s %-45s (%2d)",
				capstone_insn[0].mnemonic,
				capstone_insn[0].op_str,
				(int)(address-(uintptr_t)packet_buffer)
				);
		}
		else {
			sync_fprintf(
				f,
				"%10s %-45s (%2d)",
				"(unk)",
				" ",
				(int)(address-(uintptr_t)packet_buffer)
				);
		}
		expected_length=(int)(address-(uintptr_t)packet_buffer);
	}

	return 0;
}
#endif

void print_mc(FILE* f, int length)
{
	int i;
	bool p=false;
	if (!is_prefix(inj.i.bytes[0])) {
		sync_fprintf(f, " ");
		p=true;
	}
	for (i=0; i<length && i<MAX_INSN_LENGTH; i++) {
		sync_fprintf(f, "%02x", inj.i.bytes[i]);
		if (
			!p && 
			i<MAX_INSN_LENGTH-1 && 
			is_prefix(inj.i.bytes[i]) && 
			!is_prefix(inj.i.bytes[i+1])
			) {
			sync_fprintf(f, " ");
			p=true;
		}
	}
}

/* this becomes hairy with "mandatory prefix" instructions */
bool is_prefix(uint8_t x)
{
	return 
		x==0xf0 || /* lock */
		x==0xf2 || /* repne / bound */
		x==0xf3 || /* rep */
		x==0x2e || /* cs / branch taken */
		x==0x36 || /* ss / branch not taken */
		x==0x3e || /* ds */
		x==0x26 || /* es */
		x==0x64 || /* fs */
		x==0x65 || /* gs */
		x==0x66 || /* data */
		x==0x67	/* addr */
#if __x86_64__
		|| (x>=0x40 && x<=0x4f) /* rex */
#endif
		;
}

int prefix_count(void)
{
	int i;
	for (i=0; i<MAX_INSN_LENGTH; i++) {
		if (!is_prefix(inj.i.bytes[i])) {
			return i;
		}
	}
	return i;
}

bool has_dup_prefix(void)
{
	int i;
	int byte_count[256];
	memset(byte_count, 0, 256*sizeof(int));

	for (i=0; i<MAX_INSN_LENGTH; i++) {
		if (is_prefix(inj.i.bytes[i])) {
			byte_count[inj.i.bytes[i]]++;
		}
		else {
			break;
		}
	}

	for (i=0; i<256; i++) {
		if (byte_count[i]>1) {
			return true;
		}
	}

	return false;
}

//TODO: can't blacklist 00
bool has_opcode(uint8_t* op)
{
	int i, j;
	for (i=0; i<MAX_INSN_LENGTH; i++) {
		if (!is_prefix(inj.i.bytes[i])) {
			j=0;
			do {
				if (i+j>=MAX_INSN_LENGTH || op[j]!=inj.i.bytes[i+j]) {
					return false;
				}
				j++;
			} while (op[j]);

			return true;
		}
	}
	return false;
}


//TODO: can't blacklist 00
bool has_prefix(uint8_t* pre)
{
	int i, j;
	for (i=0; i<MAX_INSN_LENGTH; i++) {
		if (is_prefix(inj.i.bytes[i])) {
			j=0;
			do {
				if (inj.i.bytes[i]==pre[j]) {
					return true;
				}
				j++;
			} while (pre[j]);
		}
		else {
			return false;
		}
	}
	return false;
}

/* gcc doesn't allow naked inline, i hate it */
void preamble(void)
{
#if __x86_64__
	__asm__ __volatile__ ("\
			.global preamble_start	                \n\
			preamble_start:	                       \n\
			pushfq	                                \n\
			orq %0, (%%rsp)	                       \n\
			popfq	                                 \n\
			.global preamble_end	                  \n\
			preamble_end:	                         \n\
			"
			:
			:"i"(TF)
			);
#else
	__asm__ __volatile__ ("\
			.global preamble_start	                \n\
			preamble_start:	                       \n\
			pushfl	                                \n\
			orl %0, (%%esp)	                       \n\
			popfl	                                 \n\
			.global preamble_end	                  \n\
			preamble_end:	                         \n\
			"
			:
			:"i"(TF)
			);
#endif
}

void inject(int insn_size)
{
	/* could probably fork here to avoid risk of destroying the controlling process */
	/* only really comes up in random injection, just roll the dice for now */

	int i;
	int preamble_length=(&preamble_end-&preamble_start);
	static bool have_state=false;

	if (!USE_TF) { preamble_length=0; }

	packet=packet_buffer+PAGE_SIZE-insn_size-preamble_length;

	/* optimization - don't bother to write protect page */
	//	assert(!mprotect(packet_buffer,PAGE_SIZE,PROT_READ|PROT_WRITE|PROT_EXEC));
	for (i=0; i<preamble_length; i++) {
		((char*)packet)[i]=((char*)&preamble_start)[i];
	}
	for (i=0; i<MAX_INSN_LENGTH && i<insn_size; i++) {
		((char*)packet)[i+preamble_length]=inj.i.bytes[i];
	}
	//	assert(!mprotect(packet_buffer,PAGE_SIZE,PROT_READ|PROT_EXEC));

	if (config.enable_null_access) {
		/* without this we need to blacklist any instruction that modifies esp */
		void* p=NULL; /* suppress warning */
		memset(p, 0, PAGE_SIZE);
	}

	dummy_stack.dummy_stack_lo[0]=0;

	if (!have_state) {
		/* optimization: only get state first time */
		have_state=true;
		configure_sig_handler(state_handler);
		__asm__ __volatile__ ("ud2\n");
	}

	configure_sig_handler(fault_handler);

#if __x86_64__
	__asm__ __volatile__ ("\
			mov %[rax], %%rax \n\
			mov %[rbx], %%rbx \n\
			mov %[rcx], %%rcx \n\
			mov %[rdx], %%rdx \n\
			mov %[rsi], %%rsi \n\
			mov %[rdi], %%rdi \n\
			mov %[r8],  %%r8  \n\
			mov %[r9],  %%r9  \n\
			mov %[r10], %%r10 \n\
			mov %[r11], %%r11 \n\
			mov %[r12], %%r12 \n\
			mov %[r13], %%r13 \n\
			mov %[r14], %%r14 \n\
			mov %[r15], %%r15 \n\
			mov %[rbp], %%rbp \n\
			mov %[rsp], %%rsp \n\
			jmp *%[packet]	\n\
			"
			: /* no output */
			: [rax]"m"(inject_state.rax),
			  [rbx]"m"(inject_state.rbx),
			  [rcx]"m"(inject_state.rcx),
			  [rdx]"m"(inject_state.rdx),
			  [rsi]"m"(inject_state.rsi),
			  [rdi]"m"(inject_state.rdi),
			  [r8]"m"(inject_state.r8),
			  [r9]"m"(inject_state.r9),
			  [r10]"m"(inject_state.r10),
			  [r11]"m"(inject_state.r11),
			  [r12]"m"(inject_state.r12),
			  [r13]"m"(inject_state.r13),
			  [r14]"m"(inject_state.r14),
			  [r15]"m"(inject_state.r15),
			  [rbp]"m"(inject_state.rbp),
			  [rsp]"i"(&dummy_stack.dummy_stack_lo),
			  [packet]"m"(packet)
			);
#else
	__asm__ __volatile__ ("\
			mov %[eax], %%eax \n\
			mov %[ebx], %%ebx \n\
			mov %[ecx], %%ecx \n\
			mov %[edx], %%edx \n\
			mov %[esi], %%esi \n\
			mov %[edi], %%edi \n\
			mov %[ebp], %%ebp \n\
			mov %[esp], %%esp \n\
			jmp *%[packet]	\n\
			"
			:
			:
			[eax]"m"(inject_state.eax),
			[ebx]"m"(inject_state.ebx),
			[ecx]"m"(inject_state.ecx),
			[edx]"m"(inject_state.edx),
			[esi]"m"(inject_state.esi),
			[edi]"m"(inject_state.edi),
			[ebp]"m"(inject_state.ebp),
			[esp]"i"(&dummy_stack.dummy_stack_lo),
			[packet]"m"(packet)
			);
#endif

	__asm__ __volatile__ ("\
			.global resume   \n\
			resume:	      \n\
			"
			);
	;
}

void state_handler(int signum, siginfo_t* si, void* p)
{
	fault_context=((ucontext_t*)p)->uc_mcontext;
	((ucontext_t*)p)->uc_mcontext.gregs[IP]+=UD2_SIZE;
}

void fault_handler(int signum, siginfo_t* si, void* p)
{
	int insn_length;
	ucontext_t* uc=(ucontext_t*)p;
	int preamble_length=(&preamble_end-&preamble_start);

	if (!USE_TF) { preamble_length=0; }

	/* make an initial estimate on the instruction length from the fault address */
	insn_length=
		(uintptr_t)uc->uc_mcontext.gregs[IP]-(uintptr_t)packet-preamble_length;

	if (insn_length<0) {
		insn_length=JMP_LENGTH;
	}
	else if (insn_length>MAX_INSN_LENGTH) {
		insn_length=JMP_LENGTH;
	}

	result=(result_t){
		1,
		insn_length,
		signum,
		si->si_code,
		(signum==SIGSEGV||signum==SIGBUS)?(uint32_t)(uintptr_t)si->si_addr:(uint32_t)-1
	};

	memcpy(uc->uc_mcontext.gregs, fault_context.gregs, sizeof(fault_context.gregs));
	uc->uc_mcontext.gregs[IP]=(uintptr_t)&resume;
	uc->uc_mcontext.gregs[REG_EFL]&=~TF;
}

void configure_sig_handler(void (*handler)(int, siginfo_t*, void*))
{
	struct sigaction s;

	s.sa_sigaction=handler;
	s.sa_flags=SA_SIGINFO|SA_ONSTACK;

	sigfillset(&s.sa_mask);

	sigaction(SIGILL,  &s, NULL);
	sigaction(SIGSEGV, &s, NULL);
	sigaction(SIGFPE,  &s, NULL);
	sigaction(SIGBUS,  &s, NULL);
	sigaction(SIGTRAP, &s, NULL);
}

/* note: this does not provide an even distribution */
void get_rand_insn_in_range(range_t* r)
{
	static uint8_t inclusive_end[MAX_INSN_LENGTH];
	int i;
	bool all_max=true;
	bool all_min=true;

	memcpy(inclusive_end, &r->end.bytes, MAX_INSN_LENGTH);
	i=MAX_INSN_LENGTH-1;
	while (i>=0) {
		inclusive_end[i]--;
		if (inclusive_end[i]!=0xff) {
			break;
		}
		i--;
	}

	for (i=0; i<MAX_INSN_LENGTH; i++) {
		if (all_max && all_min) {
			inj.i.bytes[i]=
				rand()%(inclusive_end[i]-r->start.bytes[i]+1)+r->start.bytes[i];
		}
		else if (all_max) {
			inj.i.bytes[i]=
				rand()%(inclusive_end[i]+1);
		}
		else if (all_min) {
			inj.i.bytes[i]=
				rand()%(256-r->start.bytes[i])+r->start.bytes[i];
		}
		else {
			inj.i.bytes[i]=
				rand()%256;
		}
		all_max=all_max&&(inj.i.bytes[i]==inclusive_end[i]);
		all_min=all_min&&(inj.i.bytes[i]==r->start.bytes[i]);
	}
}

void init_inj(const insn_t* new_insn)
{
	inj.i=*new_insn;
	inj.index=-1;
	inj.last_len=-1;
}

bool move_next_instruction(void)
{
	int i;

	switch (mode) {
		case RAND:
			if (!search_range.started) {
				init_inj(&null_insn);
				get_rand_insn_in_range(&search_range);
			}
			else {
				get_rand_insn_in_range(&search_range);
			}
			break;
		case BRUTE:
			if (!search_range.started) {
				init_inj(&search_range.start);
				inj.index=config.brute_depth-1;
			}
			else {
				for (inj.index=config.brute_depth-1; inj.index>=0; inj.index--) {
					inj.i.bytes[inj.index]++;
					if (inj.i.bytes[inj.index]) {
						break;
					}
				}
			}
			break;
		case TUNNEL:
			if (!search_range.started) {
				init_inj(&search_range.start);
				inj.index=search_range.start.len;
			}
			else {
				/* not a perfect algorithm; should really look at length
				 * patterns of oher bytes at current index, not "last" length;
				 * also situations in which this may not dig deep enough, should
				 * really be looking at no length changes for n bytes, not just
				 * last byte.  but it's good enough for now. */

				/* if the last iteration changed the instruction length, go deeper */
				/* but not if we're already as deep as the instruction goes */
				//TODO: should also count a change in the signal as a reason to
				//go deeper
				if (result.length!=inj.last_len && inj.index<result.length-1) {
					inj.index++;
				}
				inj.last_len=result.length;

				inj.i.bytes[inj.index]++;

				while (inj.index>=0 && inj.i.bytes[inj.index]==0) {
					inj.index--;
					if (inj.index>=0) {
						inj.i.bytes[inj.index]++;
					}
					/* new tunnel level, reset length */
					inj.last_len=-1;
				}
			}
			break;
		case DRIVEN:
			i=MAX_INSN_LENGTH;
			do {
				i-=fread(inj.i.bytes, 1, i, stdin);
			} while (i>0);
			break;
		default:
			assert(0);
	}
	search_range.started=true;

	i=0;
	while (opcode_blacklist[i].opcode) {
		if (has_opcode((uint8_t*)opcode_blacklist[i].opcode)) {
			switch (output) {
				case TEXT:
					sync_fprintf(stdout, "x: "); print_mc(stdout, 16);
					sync_fprintf(stdout, "... (%s)\n", opcode_blacklist[i].reason);
					sync_fflush(stdout, false);
					break;
				case RAW:
					result=(result_t){0,0,0,0,0};
					give_result(stdout);
					break;
				default:
					assert(0);
			}
			return move_next_instruction();
		}
		i++;
	}

	i=0;
	while (prefix_blacklist[i].prefix) {
		if (has_prefix((uint8_t*)prefix_blacklist[i].prefix)) {
			switch (output) {
				case TEXT:
					sync_fprintf(stdout, "x: "); print_mc(stdout, 16);
					sync_fprintf(stdout, "... (%s)\n", prefix_blacklist[i].reason);
					sync_fflush(stdout, false);
					break;
				case RAW:
					result=(result_t){0,0,0,0,0};
					give_result(stdout);
					break;
				default:
					assert(0);
			}
			return move_next_instruction();
		}
		i++;
	}

	if (prefix_count()>config.max_prefix || 
			(!config.allow_dup_prefix && has_dup_prefix())) {
		switch (output) {
			case TEXT:
				sync_fprintf(stdout, "x: "); print_mc(stdout, 16);
				sync_fprintf(stdout, "... (%s)\n", "prefix violation");
				sync_fflush(stdout, false);
				break;
			case RAW:
				result=(result_t){0,0,0,0,0};
				give_result(stdout);
				break;
			default:
				assert(0);
		}
		return move_next_instruction();
	}

	/* early exit */
	/* check if we are at, or past, the end instruction */
	if (memcmp(inj.i.bytes, search_range.end.bytes, sizeof(inj.i.bytes))>=0) {
		return false;
	}

	/* search based exit */
	switch (mode) {
		case RAND:
			return true;
		case BRUTE:
			return inj.index>=0;
		case TUNNEL:
			return inj.index>=0;
		case DRIVEN:
			return true;
		default:
			assert(0);
	}
}

void give_result(FILE* f)
{
	uint8_t* code;
	size_t code_size;
	uint64_t address;
	switch (output) {
		case TEXT:
			switch (mode) {
				case BRUTE:
				case TUNNEL:
				case RAND:
				case DRIVEN:
					sync_fprintf(f, " %s", expected_length==result.length?" ":".");
					sync_fprintf(f, "r: (%2d) ", result.length);
					if (result.signum==SIGILL)  { sync_fprintf(f, "sigill "); }
					if (result.signum==SIGSEGV) { sync_fprintf(f, "sigsegv"); }
					if (result.signum==SIGFPE)  { sync_fprintf(f, "sigfpe "); }
					if (result.signum==SIGBUS)  { sync_fprintf(f, "sigbus "); }
					if (result.signum==SIGTRAP) { sync_fprintf(f, "sigtrap"); }
					sync_fprintf(f, " %3d ", result.si_code);
					sync_fprintf(f, " %08x ", result.addr);
					print_mc(f, result.length);
					sync_fprintf(f, "\n");
					break;
				default:
					assert(0);
			}
			break;
		case RAW:
#if USE_CAPSTONE
			code=inj.i.bytes;
			code_size=MAX_INSN_LENGTH;
			address=(uintptr_t)packet_buffer;
		
			if (cs_disasm_iter(
					capstone_handle,
					(const uint8_t**)&code,
					&code_size,
					&address,
					capstone_insn)
				) {
#if RAW_REPORT_DISAS_MNE 
				strncpy(disas.mne, capstone_insn[0].mnemonic, RAW_DISAS_MNEMONIC_BYTES);
#endif
#if RAW_REPORT_DISAS_OPS
				strncpy(disas.ops, capstone_insn[0].op_str, RAW_DISAS_OP_BYTES);
#endif
#if RAW_REPORT_DISAS_LEN
				disas.len=(int)(address-(uintptr_t)packet_buffer);
#endif
#if RAW_REPORT_DISAS_VAL
				disas.val=true;
#endif
			}
			else {
#if RAW_REPORT_DISAS_MNE 
				strncpy(disas.mne, "(unk)", RAW_DISAS_MNEMONIC_BYTES);
#endif
#if RAW_REPORT_DISAS_OPS
				strncpy(disas.ops, " ", RAW_DISAS_OP_BYTES);
#endif
#if RAW_REPORT_DISAS_LEN
				disas.len=(int)(address-(uintptr_t)packet_buffer);
#endif
#if RAW_REPORT_DISAS_VAL
				disas.val=false;
#endif
			}
#if RAW_REPORT_DISAS_MNE || RAW_REPORT_DISAS_OPS || RAW_REPORT_DISAS_LEN
			sync_fwrite(&disas, sizeof(disas), 1, stdout);
#endif
#endif
			sync_fwrite(inj.i.bytes, RAW_REPORT_INSN_BYTES, 1, stdout);
			sync_fwrite(&result, sizeof(result), 1, stdout);
			/* fflush(stdout); */
			break;
		default:
			assert(0);
	}
	sync_fflush(stdout, false);
}

void usage(void)
{
	printf("injector [-b|-r|-t|-d] [-R|-T] [-x] [-0] [-D] [-N]\n");
	printf("\t[-s seed] [-B brute_depth] [-P max_prefix]\n");
	printf("\t[-i instruction] [-e instruction]\n");
	printf("\t[-c core] [-X blacklist]\n");
	printf("\t[-j jobs] [-l range_bytes]\n");
}

void help(void)
{
	printf("injector [OPTIONS...]\n");
	printf("\t[-b|-r|-t|-d] ....... mode: brute, random, tunnel, directed (default: tunnel)\n");
	printf("\t[-R|-T] ............. output: raw, text (default: text)\n");
	printf("\t[-x] ................ show tick (default: %d)\n", config.show_tick);
	printf("\t[-0] ................ allow null dereference (requires sudo) (default: %d)\n", config.enable_null_access);
	printf("\t[-D] ................ allow duplicate prefixes (default: %d)\n", config.allow_dup_prefix);
	printf("\t[-N] ................ no nx bit support (default: %d)\n", config.nx_support);
	printf("\t[-s seed] ........... in random search, seed (default: time(0))\n");
	printf("\t[-B brute_depth] .... in brute search, maximum search depth (default: %d)\n", config.brute_depth);
	printf("\t[-P max_prefix] ..... maximum number of prefixes to search (default: %d)\n", config.max_prefix);
	printf("\t[-i instruction] .... instruction at which to start search, inclusive (default: 0)\n");
	printf("\t[-e instruction] .... instruction at which to end search, exclusive (default: ff..ff)\n");
	printf("\t[-c core] ........... core on which to perform search (default: any)\n");
	printf("\t[-X blacklist] ...... blacklist the specified instruction\n");
	printf("\t[-j jobs] ........... number of simultaneous jobs to run (default: %d)\n", config.jobs);
	printf("\t[-l range_bytes] .... number of base instruction bytes in each sub range (default: %d)\n", config.range_bytes);
}

void init_config(int argc, char** argv)
{
	int c, i, j;
	opterr=0;
	bool seed_given=false;
	while ((c=getopt(argc,argv,"?brtdRTx0Ns:DB:P:S:i:e:c:X:j:l:"))!=-1) {
		switch (c) {
			case '?':
				help();
				exit(-1);
				break;
			case 'b':
				mode=BRUTE;
				break;
			case 'r':
				mode=RAND;
				break;
			case 't':
				mode=TUNNEL;
				break;
			case 'd':
				mode=DRIVEN;
				break;
			case 'R':
				output=RAW;
				break;
			case 'T':
				output=TEXT;
				break;
			case 'x':
				config.show_tick=true;
				break;
			case '0':
				config.enable_null_access=true;
				break;
			case 'N':
				config.nx_support=false;
				break;
			case 's':
				sscanf(optarg, "%ld", &config.seed);
				seed_given=true;
				break;
			case 'P':
				sscanf(optarg, "%d", &config.max_prefix);
				break;
			case 'B':
				sscanf(optarg, "%d", &config.brute_depth);
				break;
			case 'D':
				config.allow_dup_prefix=true;
				break;
			case 'i':
				i=0;
				while (optarg[i*2] && optarg[i*2+1] && i<MAX_INSN_LENGTH) {
					unsigned int k;
					sscanf(optarg+i*2, "%02x", &k);
					total_range.start.bytes[i]=k;
					i++;
				}
				total_range.start.len=i;
				while (i<MAX_INSN_LENGTH) {
					total_range.start.bytes[i]=0;
					i++;
				}
				break;
			case 'e':
				i=0;
				while (optarg[i*2] && optarg[i*2+1] && i<MAX_INSN_LENGTH) {
					unsigned int k;
					sscanf(optarg+i*2, "%02x", &k);
					total_range.end.bytes[i]=k;
					i++;
				}
				total_range.end.len=i;
				while (i<MAX_INSN_LENGTH) {
					total_range.end.bytes[i]=0;
					i++;
				}
				break;
			case 'c':
				config.force_core=true;
				sscanf(optarg, "%d", &config.core);
				break;
			case 'X':
				j=0;
				while (opcode_blacklist[j].opcode) {
					j++;
				}
				opcode_blacklist[j].opcode=malloc(strlen(optarg)/2+1);
				assert (opcode_blacklist[j].opcode);
				i=0;
				while (optarg[i*2] && optarg[i*2+1]) {
					unsigned int k;
					sscanf(optarg+i*2, "%02x", &k);
					opcode_blacklist[j].opcode[i]=k;
					i++;
				}
				opcode_blacklist[j].opcode[i]='\0';
				opcode_blacklist[j].reason="user_blacklist";
				opcode_blacklist[++j]=(ignore_op_t){NULL,NULL};
				break;
			case 'j':
				sscanf(optarg, "%d", &config.jobs);
				break;
			case 'l':
				sscanf(optarg, "%d", &config.range_bytes);
				break;
			default:
				usage();
				exit(-1);
		}
	}

	if (optind!=argc) {
		usage();
		exit(1);
	}

	if (!seed_given) {
		config.seed=time(0);
	}
}

void pin_core(void)
{
	if (config.force_core) {
		cpu_set_t mask;
		CPU_ZERO(&mask);
		CPU_SET(config.core,&mask);
		if (sched_setaffinity(0, sizeof(mask), &mask)) {
			printf("error: failed to set cpu\n");
			exit(1);
		}
	}
}

void tick(void)
{
	static uint64_t t=0;
	if (config.show_tick) {
		t++;
		if ((t&TICK_MASK)==0) {
			sync_fprintf(stderr, "t: ");
			print_mc(stderr, 8);
			sync_fprintf(stderr, "... ");
			#if USE_CAPSTONE
			print_asm(stderr);
			sync_fprintf(stderr, "\t");
			#endif
			give_result(stderr);
			sync_fflush(stderr, false);
		}
	}
}

void pretext(void)
{
	/* assistive output for analyzing hangs in text mode */
	if (output==TEXT) {
		sync_fprintf(stdout, "r: ");
		print_mc(stdout, 8);
		sync_fprintf(stdout, "... ");
		#if USE_CAPSTONE
		print_asm(stdout);
		sync_fprintf(stdout, " ");
		#endif
		sync_fflush(stdout, false);
	}
}

int main(int argc, char** argv)
{
	int pid;
	int job=0;
	int i;
	void* packet_buffer_unaligned;
	void* null_p;

	pthread_mutexattr_init(&mutex_attr);
	pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
	pool_mutex=mmap(NULL, sizeof *pool_mutex, 
			PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	output_mutex=mmap(NULL, sizeof *output_mutex,
			PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	pthread_mutex_init(pool_mutex, &mutex_attr);
	pthread_mutex_init(output_mutex, &mutex_attr);

	init_config(argc, argv);
	pin_core();

	srand(config.seed);

	packet_buffer_unaligned=malloc(PAGE_SIZE*3);
	packet_buffer=(void*)
		(((uintptr_t)packet_buffer_unaligned+(PAGE_SIZE-1))&~(PAGE_SIZE-1));
	assert(!mprotect(packet_buffer,PAGE_SIZE,PROT_READ|PROT_WRITE|PROT_EXEC));
	if (config.nx_support) {
		/* enabling reads and writes on the following page lets us properly
		 * resolve the lengths of some rip-relative instructions that come up
		 * during tunneling: e.g. inc (%rip) - if the next page has no
		 * permissions at all, the fault from this instruction executing is
		 * indistinguishable from the fault of the instruction fetch failing,
		 * preventing correct length determination.  allowing read/write ensures
		 * 'inc (%rip)' can succeed, so that we can find its length. */
		assert(!mprotect(packet_buffer+PAGE_SIZE,PAGE_SIZE,PROT_READ|PROT_WRITE));
	}
	else {
		/* on systems that don't support the no-execute bit, providing
		 * read/write access (like above) is the same as providing execute
		 * access, so this will not work.  on these systems, provide no access
		 * at all - systems without NX will also not support rip-relative
		 * addressing, and (with the proper register initializations) should not
		 * be able to reach the following page during tunneling style fuzzing */
		assert(!mprotect(packet_buffer+PAGE_SIZE,PAGE_SIZE,PROT_NONE));
	}

#if USE_CAPSTONE
	if (cs_open(CS_ARCH_X86, CS_MODE, &capstone_handle) != CS_ERR_OK) {
		exit(1);
	}
	capstone_insn = cs_malloc(capstone_handle);
#endif

	if (config.enable_null_access) {
		null_p=mmap(0, PAGE_SIZE, PROT_READ|PROT_WRITE,
			MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
		if (null_p==MAP_FAILED) {
			printf("null access requires running as root\n");
			exit(1);
		}
	}

	/*
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	*/

	sigaltstack(&ss, 0);

	initialize_ranges();

	for (i=0; i<config.jobs-1; i++) {
		pid=fork();
		assert(pid>=0);
		if (pid==0) {
			break;
		}
		job++;
	}

	while (move_next_range()) {
		/* sync_fprintf(stderr, "job: %d // range: ", job); print_range(stderr, &search_range); sync_fprintf(stderr, "\n");  sync_fflush(stderr,true); */
		while (move_next_instruction()) {
			/* sync_fprintf(stderr, "job: %d // mc: ", job); print_mc(stderr, 16); sync_fprintf(stderr, "\n"); sync_fflush(stderr,true); */
			pretext();
			for (i=1; i<=MAX_INSN_LENGTH; i++) {
				inject(i);
				/* approach 1: examine exception type */
				/* suffers from failure to resolve length when instruction
				 * accesses mapped but protected memory (e.g. writes to .text section) */
				/* si_code = SEGV_ACCERR, SEGV_MAPERR, or undocumented SI_KERNEL */
				/* SI_KERNEL appears with in, out, hlt, various retf, movabs, mov cr, etc */
				/* SI_ACCERR is expected when the instruction fetch fails */
				//if (result.signum!=SIGSEGV || result.si_code!=SEGV_ACCERR) {
				/* approach 2: examine exception address */
				/* correctly resolves instruction length in most foreseeable
				 * situations */
				if (result.addr!=(uint32_t)(uintptr_t)(packet_buffer+PAGE_SIZE)) {
					break;
				}
			}
			result.length=i;
			give_result(stdout);
			tick();
		}
	}

	sync_fflush(stdout, true);
	sync_fflush(stderr, true);

	/* sync_fprintf(stderr, "lazarus!\n"); */

#if USE_CAPSTONE
	cs_free(capstone_insn, 1);
	cs_close(&capstone_handle);
#endif

	if (config.enable_null_access) {
		munmap(null_p, PAGE_SIZE);
	}

	free(packet_buffer_unaligned);

	if (pid!=0) {
		for (i=0; i<config.jobs-1; i++) {
			wait(NULL);
		}
		free_ranges();
		pthread_mutex_destroy(pool_mutex);
		pthread_mutex_destroy(output_mutex);
	}

	return 0;
}
