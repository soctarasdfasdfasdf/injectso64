/*
 * Copyright (C) 2007-2009 Stealth.
 * All rights reserved.
 *
 * This is NOT a common BSD license, so read on.
 *
 * Redistribution in source and use in binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. The provided software is FOR EDUCATIONAL PURPOSES ONLY! You must not
 *    use this software or parts of it to commit crime or any illegal
 *    activities. Local law may forbid usage or redistribution of this
 *    software in your country.
 * 2. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 3. Redistribution in binary form is not allowed.
 * 4. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by Stealth.
 * 5. The name Stealth may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <elf.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stddef.h>
#include <assert.h>
#include <sys/ptrace.h>
#include <asm/ptrace.h>
#include <sys/user.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#include <dlfcn.h>

void die(const char *s)
{
	perror(s);
	exit(errno);
}

// Find address of lib in the context of process with pid
char *find_libc_start(pid_t pid)
{
	char path[1024];
	char buf[1024], *start = NULL, *end = NULL, *p = NULL;
	char *addr1 = NULL, *addr2 = NULL;
	FILE *f = NULL;
	

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);

	if ((f = fopen(path, "r")) == NULL)
		die("fopen");

	for (;;) {
		if (!fgets(buf, sizeof(buf), f))
			break;
		if (!strstr(buf, "r-xp"))
			continue;
		if (!(p = strstr(buf, "/")))
			continue;
		if (!strstr(p, "/libc-"))
			continue;
		start = strtok(buf, "-");
		addr1 = (char *)strtoul(start, NULL, 16);
		end = strtok(NULL, " ");
		addr2 = (char *)strtoul(end, NULL, 16);
		break;
	}

	fclose(f);	
	return addr1;
}

// copy buf of length blen to addr in memory of process with pid
int poke_text(pid_t pid, size_t addr, char *buf, size_t blen)
{
	int i = 0;
	char *ptr = malloc(blen + blen % sizeof(size_t));	// word align
	memcpy(ptr, buf, blen);

	for (i = 0; i < blen; i += sizeof(size_t)) {
		if (ptrace(PTRACE_POKETEXT, pid, addr + i, *(size_t *)&ptr[i]) < 0)
			die("ptrace");
	}
	free(ptr);
	return 0;
}


// copy blen words from process with pid's memory, starting at addr,  into buf
int peek_text(pid_t pid, size_t addr, char *buf, size_t blen)
{
	int i = 0;
	size_t word = 0;
	for (i = 0; i < blen; i += sizeof(size_t)) {
		word = ptrace(PTRACE_PEEKTEXT,pid, addr + i, NULL);
		memcpy(&buf[i], &word, sizeof(word));
	}
	return 0;
}

// inject code into process with pid
// dlopen_addr - address of dlopen function in libc
// libc_addr - not used? set to zero in function context
// dso - 
int inject_code(pid_t pid, size_t libc_addr, size_t dlopen_addr, char *dso)
{
	char sbuf1[1024], sbuf2[1024];
	struct user_regs_struct regs, saved_regs;
	int status;

	if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) // attach to process w/ pid
		die("ptrace 1");
	waitpid(pid, &status, 0); // wait for child to recieve SIGSTOP
	if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) < 0) // put tracee registers in regs
		die("ptrace 2");

	peek_text(pid, regs.rsp + 1024, sbuf1, sizeof(sbuf1);) //copy [rsp,rsp+2048) into buffers
	peek_text(pid, regs.rsp, sbuf2, sizeof(sbuf2));

	/* fake saved return address */
	libc_addr = 0x0;
	poke_text(pid, regs.rsp, (char *)&libc_addr, sizeof(libc_addr)); // zero out rsp to rsp+1024
	poke_text(pid, regs.rsp + 1024, dso, strlen(dso) + 1); // copies dso buffer+null term into rsp+1024

	memcpy(&saved_regs, &regs, sizeof(regs)); // save registers

	/* pointer to &args */
	printf("rdi=%zx rsp=%zx rip=%zx\n", regs.rdi, regs.rsp, regs.rip);

	regs.rdi = regs.rsp + 1024; // first argument to dlopen
	regs.rsi = RTLD_NOW|RTLD_GLOBAL|RTLD_NODELETE; // second argument to dlopen
	regs.rip = dlopen_addr + 2;// kernel bug?! always need to add 2!

	if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) < 0) // sets process pid's registers
		die("ptrace 3");
	if (ptrace(PTRACE_CONT, pid, NULL, NULL) < 0) // restarts stopped tracee process
		die("ptrace 4");

	/* Should receive a SIGSEGV */
	waitpid(pid, &status, 0); // tracee segfaults, catch

	if (ptrace(PTRACE_SETREGS, pid, 0, &saved_regs) < 0) // restore saved registers
		die("ptrace 5");

	poke_text(pid, saved_regs.rsp + 1024, sbuf1, sizeof(sbuf1)); // restore saved stack 
	poke_text(pid, saved_regs.rsp, sbuf2, sizeof(sbuf2));

	if (ptrace(PTRACE_DETACH, pid, NULL, NULL) < 0) // detach from tracee
		die("ptrace 6");

	return 0;
}


// Prints usage
void usage(const char *path)
{
	printf("Usage: %s <pid> <dso-path>\n", path);
}


int main(int argc, char **argv)
{
	pid_t daemon_pid = -1;
	char *my_libc = NULL, *daemon_libc = NULL;
	char *dl_open_address = NULL;
	char *dlopen_mode = NULL;
	FILE *pfd = NULL;
	char buf[128], *space = NULL;

	/* nm /lib64/libc.so.6|grep __libc_dlopen_mode: 00000000000f2a40 t __libc_dlopen_mode */
	size_t dlopen_offset = 0;

	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	setbuffer(stdout, NULL, 0); // sets output stream to unbuffered - write when input recieved

	my_libc = find_libc_start(getpid()); //gets PID of this process
	
	printf("Trying to obtain __libc_dlopen_mode() address relative to libc start address.\n");
	printf("[1] Using my own __libc_dlopen_mode ...\n");
	dlopen_mode = dlsym(NULL, "__libc_dlopen_mode"); //gets address of dlopen
	if (dlopen_mode) // found dlopen address
		dlopen_offset = dlopen_mode - my_libc;
		
	if (dlopen_offset == 0 && // dlsym didn't find dlopen, use nm
	    (pfd = popen("nm /lib64/libc.so.6|grep __libc_dlopen_mode", "r")) != NULL) {
		printf("[2] Using nm method ... ");
		fgets(buf, sizeof(buf), pfd);
		if ((space = strchr(buf, ' ')) != NULL)
			*space = 0;
		dlopen_offset = strtoul(buf, NULL, 16);
		fclose(pfd);
	}
	if (dlopen_offset == 0) {
		printf("failed!\nNo more methods, bailing out.\n");
		return 1;
	}
	printf("success!\n");

	dl_open_address = find_libc_start(getpid()) + dlopen_offset; //address of dlopen function
	daemon_pid = (pid_t)atoi(argv[1]); // pid to inject to?
	daemon_libc = find_libc_start(daemon_pid); // libc in daemon process

	printf("me: {__libc_dlopen_mode:%p, dlopen_offset:%zx}\n=> daemon: {__libc_dlopen_mode:%p, libc:%p}\n",
	       dl_open_address, dlopen_offset, daemon_libc + dlopen_offset, daemon_libc);

	inject_code(daemon_pid, (size_t)daemon_libc,
	            (size_t)(daemon_libc + dlopen_offset), argv[2]);

	printf("done!\n");
	return 0;
}
