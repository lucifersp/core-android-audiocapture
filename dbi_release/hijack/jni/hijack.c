/* 
 * hijack.c - force a process to load a library
 *
 *  ARM / Android version by:
 *  Collin Mulliner <collin[at]mulliner.org>
 *  http://www.mulliner.org/android/
 *	(c) 2012
 *
 *
 *  original x86 version by:
 *  Copyright (C) 2002 Victor Zandy <zandy[at]cs.wisc.edu>
 *
 *  License: LGPL 2.1
 *
 */
 
#define _XOPEN_SOURCE 500  /* include pread,pwrite */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <elf.h>
#include <unistd.h>
#include <errno.h>       
#include <sys/mman.h>




int debug = 0;
unsigned int stack_start;
unsigned int stack_end;

/* memory map for libraries */
#define MAX_NAME_LEN 256
#define MEMORY_ONLY  "[memory]"
struct mm {
	char name[MAX_NAME_LEN];
	unsigned long start, end;
};

typedef struct symtab *symtab_t;
struct symlist {
	Elf32_Sym *sym;       /* symbols */
	char *str;            /* symbol strings */
	unsigned num;         /* number of symbols */
};
struct symtab {
	struct symlist *st;    /* "static" symbols */
	struct symlist *dyn;   /* dynamic symbols */
};

static void * 
xmalloc(size_t size)
{
	void *p;
	p = malloc(size);
	if (!p) {
#ifdef DEBUG
	  printf("hijack,xmalloc: Out of memory\n");
#endif
	  exit(1);
	}
	return p;
}

static struct symlist *
get_syms(int fd, Elf32_Shdr *symh, Elf32_Shdr *strh)
{
	struct symlist *sl, *ret;
	int rv;

	ret = NULL;
	sl = (struct symlist *) xmalloc(sizeof(struct symlist));
	sl->str = NULL;
	sl->sym = NULL;

	/* sanity */
	if (symh->sh_size % sizeof(Elf32_Sym)) { 
#ifdef DEBUG
		printf("elf_error\n");
#endif
		goto out;
	}

	/* symbol table */
	sl->num = symh->sh_size / sizeof(Elf32_Sym);
	sl->sym = (Elf32_Sym *) xmalloc(symh->sh_size);
	rv = pread(fd, sl->sym, symh->sh_size, symh->sh_offset);
	if (0 > rv) {
		//perror("read");
		goto out;
	}
	if (rv != symh->sh_size) {
#ifdef DEBUG
		printf("elf error\n");
#endif
		goto out;
	}

	/* string table */
	sl->str = (char *) xmalloc(strh->sh_size);
	rv = pread(fd, sl->str, strh->sh_size, strh->sh_offset);
	if (0 > rv) {
		//perror("read");
		goto out;
	}
	if (rv != strh->sh_size) {
#ifdef DEBUG
		printf("elf error");
#endif
		goto out;
	}

	ret = sl;
out:
	return ret;
}

static int
do_load(int fd, symtab_t symtab)
{
	int rv;
	size_t size;
	Elf32_Ehdr ehdr;
	Elf32_Shdr *shdr = NULL, *p;
	Elf32_Shdr *dynsymh, *dynstrh;
	Elf32_Shdr *symh, *strh;
	char *shstrtab = NULL;
	int i;
	int ret = -1;
	
	/* elf header */
	rv = read(fd, &ehdr, sizeof(ehdr));
	if (0 > rv) {
		//perror("read");
		goto out;
	}
	if (rv != sizeof(ehdr)) {
#ifdef DEBUG
		printf("elf error\n");
#endif
		goto out;
	}
	if (strncmp(ELFMAG, ehdr.e_ident, SELFMAG)) { /* sanity */
#ifdef DEBUG
		printf("not an elf\n");
#endif
		goto out;
	}
	if (sizeof(Elf32_Shdr) != ehdr.e_shentsize) { /* sanity */
#ifdef DEBUG
		printf("elf error\n");
#endif
		goto out;
	}

	/* section header table */
	size = ehdr.e_shentsize * ehdr.e_shnum;
	shdr = (Elf32_Shdr *) xmalloc(size);
	rv = pread(fd, shdr, size, ehdr.e_shoff);
	if (0 > rv) {
		//perror("read");
		goto out;
	}
	if (rv != size) {
#ifdef DEBUG
		printf("elf error");
#endif
		goto out;
	}
	
	/* section header string table */
	size = shdr[ehdr.e_shstrndx].sh_size;
	shstrtab = (char *) xmalloc(size);
	rv = pread(fd, shstrtab, size, shdr[ehdr.e_shstrndx].sh_offset);
	if (0 > rv) {
		//perror("read");
		goto out;
	}
	if (rv != size) {
#ifdef DEBUG
		printf("elf error\n");
#endif
		goto out;
	}

	/* symbol table headers */
	symh = dynsymh = NULL;
	strh = dynstrh = NULL;
	for (i = 0, p = shdr; i < ehdr.e_shnum; i++, p++)
		if (SHT_SYMTAB == p->sh_type) {
			if (symh) {
#ifdef DEBUG
				printf("too many symbol tables\n");
#endif
				goto out;
			}
			symh = p;
		} else if (SHT_DYNSYM == p->sh_type) {
			if (dynsymh) {
#ifdef DEBUG
				printf("too many symbol tables\n");
#endif
				goto out;

			}
			dynsymh = p;
		} else if (SHT_STRTAB == p->sh_type
			   && !strncmp(shstrtab+p->sh_name, ".strtab", 7)) {
			if (strh) {
#ifdef DEBUG 
				printf("too many string tables\n");
#endif
				goto out;
			}
			strh = p;
		} else if (SHT_STRTAB == p->sh_type
			   && !strncmp(shstrtab+p->sh_name, ".dynstr", 7)) {
			if (dynstrh) {
#ifdef DEBUG 
				printf("too many string tables\n");
#endif
				goto out;
			}
			dynstrh = p;
		}
	/* sanity checks */
	if ((!dynsymh && dynstrh) || (dynsymh && !dynstrh)) {
#ifdef DEBUG 
		printf("bad dynamic symbol table");
#endif
		goto out;
	}
	if ((!symh && strh) || (symh && !strh)) {
#ifdef DEBUG 
		printf("bad symbol table");
#endif
		goto out;
	}
	if (!dynsymh && !symh) {
#ifdef DEBUG 
		printf("no symbol table");
#endif
		goto out;
	}

	/* symbol tables */
	if (dynsymh)
		symtab->dyn = get_syms(fd, dynsymh, dynstrh);
	if (symh)
		symtab->st = get_syms(fd, symh, strh);
	ret = 0;
out:
	free(shstrtab);
	free(shdr);
	return ret;
}

static symtab_t
load_symtab(char *filename)
{
	int fd;
	symtab_t symtab;

	symtab = (symtab_t) xmalloc(sizeof(*symtab));
	memset(symtab, 0, sizeof(*symtab));

	fd = open(filename, O_RDONLY);
	if (0 > fd) {
		//perror("open");
		return NULL;
	}
	if (0 > do_load(fd, symtab)) {
#ifdef DEBUG 
		printf("Error ELF parsing %s\n", filename);
#endif
		free(symtab);
		symtab = NULL;
	}
	close(fd);
	return symtab;
}


static int
load_memmap(pid_t pid, struct mm *mm, int *nmmp)
{
	char raw[80000]; // this depends on the number of libraries an executable uses
	char name[MAX_NAME_LEN];
	char *p;
	unsigned long start, end;
	struct mm *m;
	int nmm = 0;
	int fd, rv;
	int i;

	sprintf(raw, "/proc/%d/maps", pid);
	fd = open(raw, O_RDONLY);
	if (0 > fd) {
#ifdef DEBUG 
		printf("Can't open %s for reading\n", raw);
#endif
		return -1;
	}

	/* Zero to ensure data is null terminated */
	memset(raw, 0, sizeof(raw));

	p = raw;
	while (1) {
		rv = read(fd, p, sizeof(raw)-(p-raw));
		if (0 > rv) {
			//perror("read");
			return -1;
		}
		if (0 == rv)
			break;
		p += rv;
		if (p-raw >= sizeof(raw)) {
#ifdef DEBUG 
			printf("Too many memory mapping\n");
#endif
			return -1;
		}
	}
	close(fd);

	p = strtok(raw, "\n");
	m = mm;
	while (p) {
		/* parse current map line */
		rv = sscanf(p, "%08lx-%08lx %*s %*s %*s %*s %s\n",
			    &start, &end, name);

		p = strtok(NULL, "\n");

		if (rv == 2) {
			m = &mm[nmm++];
			m->start = start;
			m->end = end;
			strcpy(m->name, MEMORY_ONLY);
			continue;
		}

		if (strstr(name, "stack") != 0) {
			stack_start = start;
			stack_end = end;
		}

		/* search backward for other mapping with same name */
		for (i = nmm-1; i >= 0; i--) {
			m = &mm[i];
			if (!strcmp(m->name, name))
				break;
		}

		if (i >= 0) {
			if (start < m->start)
				m->start = start;
			if (end > m->end)
				m->end = end;
		} else {
			/* new entry */
			m = &mm[nmm++];
			m->start = start;
			m->end = end;
			strcpy(m->name, name);
		}
	}

	*nmmp = nmm;
	return 0;
}

/* Find libc in MM, storing no more than LEN-1 chars of
   its name in NAME and set START to its starting
   address.  If libc cannot be found return -1 and
   leave NAME and START untouched.  Otherwise return 0
   and null-terminated NAME. */
static int
find_libc(char *name, int len, unsigned long *start,
	  struct mm *mm, int nmm)
{
	int i;
	struct mm *m;
	char *p;
	for (i = 0, m = mm; i < nmm; i++, m++) {
		if (!strcmp(m->name, MEMORY_ONLY))
			continue;
		p = strrchr(m->name, '/');
		if (!p)
			continue;
		p++;
		if (strncmp("libc", p, 4))
			continue;
		p += 4;

		/* here comes our crude test -> 'libc.so' or 'libc-[0-9]' */
		if (!strncmp(".so", p, 3) || (p[0] == '-' && isdigit(p[1])))
			break;
	}
	if (i >= nmm)
		/* not found */
		return -1;

	*start = m->start;
	strncpy(name, m->name, len);
	if (strlen(m->name) >= len)
		name[len-1] = '\0';
	return 0;
}

static int
find_linker_mem(char *name, int len, unsigned long *start,
	  struct mm *mm, int nmm)
{
	int i;
	struct mm *m;
	char *p;
	for (i = 0, m = mm; i < nmm; i++, m++) {
		//printf("name = %s\n", m->name);
		//printf("start = %x\n", m->start);
		if (!strcmp(m->name, MEMORY_ONLY))
			continue;
		p = strrchr(m->name, '/');
		if (!p)
			continue;
		p++;
		if (strncmp("linker", p, 6))
			continue;
		break; // <--- hack
		p += 4;

		/* here comes our crude test -> 'libc.so' or 'libc-[0-9]' */
		if (!strncmp(".so", p, 3) || (p[0] == '-' && isdigit(p[1])))
			break;
	}
	if (i >= nmm)
		/* not found */
		return -1;

	*start = m->start;
	strncpy(name, m->name, len);
	if (strlen(m->name) >= len)
		name[len-1] = '\0';
	return 0;
}

static int
lookup2(struct symlist *sl, unsigned char type,
	char *name, unsigned long *val)
{
	Elf32_Sym *p;
	int len;
	int i;

	len = strlen(name);
	for (i = 0, p = sl->sym; i < sl->num; i++, p++) {
		//printf("name: %s %x\n", sl->str+p->st_name, p->st_value);
		if (!strncmp(sl->str+p->st_name, name, len)
		    && ELF32_ST_TYPE(p->st_info) == type) {
			//if (p->st_value != 0) {
			*val = p->st_value;
			return 0;
			//}
		}
	}
	return -1;
}

static int
lookup_sym(symtab_t s, unsigned char type,
	   char *name, unsigned long *val)
{
	if (s->dyn && !lookup2(s->dyn, type, name, val))
		return 0;
	if (s->st && !lookup2(s->st, type, name, val))
		return 0;
	return -1;
}

static int
lookup_func_sym(symtab_t s, char *name, unsigned long *val)
{
	return lookup_sym(s, STT_FUNC, name, val);
}

static int
find_name(pid_t pid, char *name, unsigned long *addr)
{
	struct mm mm[1000];
	unsigned long libcaddr;
	int nmm;
	char libc[256];
	symtab_t s;

	if (0 > load_memmap(pid, mm, &nmm)) {
#ifdef DEBUG 
		printf("cannot read memory map\n");
#endif
		return -1;
	}
	if (0 > find_libc(libc, sizeof(libc), &libcaddr, mm, nmm)) {
#ifdef DEBUG 
		printf("cannot find libc\n");
#endif
		return -1;
	}
	s = load_symtab(libc);
	if (!s) {
#ifdef DEBUG 
		printf("cannot read symbol table\n");
#endif
		return -1;
	}
	if (0 > lookup_func_sym(s, name, addr)) {
#ifdef DEBUG 
		printf("cannot find %s\n", name);
#endif
		return -1;
	}
	*addr += libcaddr;
	return 0;
}

static int find_linker(pid_t pid, unsigned long *addr)
{
	struct mm mm[1000];
	unsigned long libcaddr;
	int nmm;
	char libc[256];
	symtab_t s;

	if (0 > load_memmap(pid, mm, &nmm)) {
#ifdef DEBUG 
		printf("cannot read memory map\n");
#endif
		return -1;
	}
	if (0 > find_linker_mem(libc, sizeof(libc), &libcaddr, mm, nmm)) {
#ifdef DEBUG 
		printf("cannot find libc\n");
#endif
		return -1;
	}
	
	*addr = libcaddr;
	
	return 1;
}

/* Write NLONG 4 byte words from BUF into PID starting
   at address POS.  Calling process must be attached to PID. */
static int
write_mem(pid_t pid, unsigned long *buf, int nlong, unsigned long pos)
{
	unsigned long *p;
	int i;

	for (p = buf, i = 0; i < nlong; p++, i++)
		if (0 > ptrace(PTRACE_POKETEXT, pid, pos+(i*4), *p))
			return -1;
	return 0;
}

static int
read_mem(pid_t pid, unsigned long *buf, int nlong, unsigned long pos)
{
	unsigned long *p;
	int i;

	for (p = buf, i = 0; i < nlong; p++, i++)
		if ((*p = ptrace(PTRACE_PEEKTEXT, pid, pos+(i*4), *p)) < 0)
			return -1;
	return 0;
}

unsigned int sc[] = {
0xe59f0040, //        ldr     r0, [pc, #64]   ; 48 <.text+0x48>
0xe3a01000, //        mov     r1, #0  ; 0x0
0xe1a0e00f, //        mov     lr, pc
0xe59ff038, //        ldr     pc, [pc, #56]   ; 4c <.text+0x4c>
0xe59fd02c, //        ldr     sp, [pc, #44]   ; 44 <.text+0x44>
0xe59f0010, //        ldr     r0, [pc, #20]   ; 30 <.text+0x30>
0xe59f1010, //        ldr     r1, [pc, #20]   ; 34 <.text+0x34>
0xe59f2010, //        ldr     r2, [pc, #20]   ; 38 <.text+0x38>
0xe59f3010, //        ldr     r3, [pc, #20]   ; 3c <.text+0x3c>
0xe59fe010, //        ldr     lr, [pc, #20]   ; 40 <.text+0x40>
0xe59ff010, //        ldr     pc, [pc, #20]   ; 44 <.text+0x44>
0xe1a00000, //        nop                     r0
0xe1a00000, //        nop                     r1
0xe1a00000, //        nop                     r2
0xe1a00000, //        nop                     r3
0xe1a00000, //        nop                     lr
0xe1a00000, //        nop                     pc
0xe1a00000, //        nop                     sp
0xe1a00000, //        nop                     addr of libname
0xe1a00000, //        nop                     dlopenaddr
};



struct pt_regs2 {
         long uregs[18];
};

#define ARM_cpsr        uregs[16]
#define ARM_pc          uregs[15]
#define ARM_lr          uregs[14]
#define ARM_sp          uregs[13]
#define ARM_ip          uregs[12]
#define ARM_fp          uregs[11]
#define ARM_r10         uregs[10]
#define ARM_r9          uregs[9]
#define ARM_r8          uregs[8]
#define ARM_r7          uregs[7]
#define ARM_r6          uregs[6]
#define ARM_r5          uregs[5]
#define ARM_r4          uregs[4]
#define ARM_r3          uregs[3]
#define ARM_r2          uregs[2]
#define ARM_r1          uregs[1]
#define ARM_r0          uregs[0]
#define ARM_ORIG_r0     uregs[17]

int main(int argc, char *argv[])
{
	pid_t pid = 0;
	struct pt_regs2 regs;
	unsigned long dlopenaddr, mprotectaddr, codeaddr, libaddr;
	unsigned long *p;
	int fd = 0;
	int n = 0;
	char buf[32];
	char *arg;
	int opt;
	char *dumpFolder = NULL;
	int libFd = -1;
	void *mmapAddr = NULL;
	int libLength = 0;
	char *needle =  ".................____________.......................";
	void *startOfNeedle = NULL;
	int result;
	

	// dbg for rwx protection:
	/* printf("---\n"); */
	/* result = mprotect(0xbefdf000, 0x20000, PROT_READ|PROT_WRITE|PROT_EXEC); */
	/* printf("mprotect %d\n", result); */
	/* printf("\t\t%s\n", strerror(*(int*)__errno()) ); */
	  

	/* 1 - parse cmdline */
 	while ((opt = getopt(argc, argv, "p:l:f:d")) != -1) {
	  switch (opt) {
	  case 'p':
	    pid = strtol(optarg, NULL, 0);
	    break;
	  case 'l':
	    n = strlen(optarg)+1;
	    n = n/4 + (n%4 ? 1 : 0);
	    arg = malloc(n*sizeof(unsigned long));
	    memcpy(arg, optarg, n*4);
	    /* arg = strdup(optarg); */
	    /* n  = strlen(arg) */
	    /* printf("%s\n", arg); */
	    break;
	  case 'f':
	    dumpFolder = strdup(optarg);
	    break;
	  case 'd':
	    debug = 1;
	    break;
	  default:
#ifdef DEBUG 
	    fprintf(stderr, "error usage: %s -p PID -l LIBNAME -f DUMP_FOLDER -d (debug on)\n", argv[0]);
#endif
	    
	    exit(0);
	    break;
	  }
	}

	if (pid == 0 || n == 0 || strlen(dumpFolder) == 0) {

#ifdef DEBUG 
	  printf("pid %d\n", pid);
	  fprintf(stderr, "usage: %s -p PID -l LIBNAME -f DUMP_FOLDER -d (debug on)\n", argv[0]);
#endif
	  exit(0);
	}

	if (0 > find_name(pid, "mprotect", &mprotectaddr)) {
#ifdef DEBUG 
		printf("can't find address of mprotect(), error!\n");
#endif
		exit(1);
	}
	
#ifdef DEBUG 
	printf("mprotect: 0x%x\n", mprotectaddr);
#endif


	/* 2 - patch */

#ifdef DEBUG
	printf("[*] Patching %s to dump into folder %s\n", arg, dumpFolder);
#endif
	

	libFd = open(arg, O_RDWR);
	
	if (libFd == -1) {
#ifdef DEBUG
	  printf("[E] Could not open %s %s\n", arg, strerror(*(int*)__errno()));
#endif
	  exit(1);
	}

	libLength = lseek(libFd,0,SEEK_END);
	mmapAddr = mmap(NULL, libLength, PROT_READ|PROT_WRITE, MAP_SHARED, libFd, 0 );
	
	if( mmapAddr == MAP_FAILED ) {
#ifdef DEBUG
	  printf("[E] Map failed %s\n", arg);
#endif
	  exit(1);
	}


#ifdef DEBUG									
	printf("[*] searching %s from %p to %p\n", needle, mmapAddr, mmapAddr + libLength);
#endif
	startOfNeedle = memmem(mmapAddr, libLength, needle, strlen(needle));

	if( startOfNeedle == 0) {
#ifdef DEBUG
	  printf("\tneedle not found, the library might be already patched\n");
#endif
	  ;
	}
	else {
#ifdef DEBUG
	printf("\t found at %p, patching..\n", startOfNeedle);
#endif

	memcpy(startOfNeedle, dumpFolder, strlen(dumpFolder)+1);
	}

	needle =  memmem(mmapAddr, libLength, dumpFolder, strlen(dumpFolder));

#ifdef DEBUG
	printf("\t verify the patch: %s @ %p\n", needle, needle );
#endif
	
	result = munmap(mmapAddr, libLength);

#ifdef DEBUG
	printf("[*] unmap %d\n", result);
#endif
	close(libFd);



	/* 3 - inject */
	void *ldl = dlopen("libdl.so", RTLD_LAZY);
	if (ldl) {
		dlopenaddr = dlsym(ldl, "dlopen");
		dlclose(ldl);
	}
	unsigned long int lkaddr;
	unsigned long int lkaddr2;
	find_linker(getpid(), &lkaddr);
	//printf("own linker: 0x%x\n", lkaddr);
	//printf("offset %x\n", dlopenaddr - lkaddr);
	find_linker(pid, &lkaddr2);
	//printf("tgt linker: %x\n", lkaddr2);
	//printf("tgt dlopen : %x\n", lkaddr2 + (dlopenaddr - lkaddr));
	dlopenaddr = lkaddr2 + (dlopenaddr - lkaddr);
	
	
#ifdef DEBUG 
	printf("dlopen: 0x%x\n", dlopenaddr);
#endif



	// Attach 
	if (0 > ptrace(PTRACE_ATTACH, pid, 0, 0)) {

#ifdef DEBUG 
		printf("cannot attach to %d, error!\n", pid);
#endif
		exit(1);
	}

	waitpid(pid, NULL, 0);
	

	

	sprintf(buf, "/proc/%d/mem", pid);
	fd = open(buf, O_WRONLY);
	if (0 > fd) {
#ifdef DEBUG 
		printf("cannot open %s, error!\n", buf);
#endif
		exit(1);
	}
	result = ptrace(PTRACE_GETREGS, pid, 0, &regs);
	
	
#ifdef DEBUG 
	printf("ptrace getregs %d\n", result);
#endif
	


	sc[11] = regs.ARM_r0;
	sc[12] = regs.ARM_r1;
	sc[13] = regs.ARM_r2;
	sc[14] = regs.ARM_r3;
	sc[15] = regs.ARM_lr;
	sc[16] = regs.ARM_pc;
	sc[17] = regs.ARM_sp;
	sc[19] = dlopenaddr;
		

#ifdef DEBUG 
		printf("pc=%x lr=%x sp=%x fp=%x\n", regs.ARM_pc, regs.ARM_lr, regs.ARM_sp, regs.ARM_fp);
		printf("r0=%x r1=%x\n", regs.ARM_r0, regs.ARM_r1);
		printf("r2=%x r3=%x\n", regs.ARM_r2, regs.ARM_r3);
#endif

	// push library name to stack
	libaddr = regs.ARM_sp - n*4 - sizeof(sc);
	sc[18] = libaddr;	
	//printf("libaddr: %x\n", libaddr);

	if (stack_start == 0) {
		stack_start = (unsigned long int) strtol(argv[3], NULL, 16);
		stack_start = stack_start << 12;
		stack_end = stack_start + strtol(argv[4], NULL, 0);
	}

	
#ifdef DEBUG 
	printf("stack: 0x%x-0x%x leng = %d\n", stack_start, stack_end, stack_end-stack_start);
#endif
	

	// write library name to stack
	if (0 > write_mem(pid, (unsigned long*)arg, n, libaddr)) {

#ifdef DEBUG 
		printf("cannot write library name (%s) to stack, error!\n", arg);
#endif
		exit(1);
	}

	// write code to stack
	codeaddr = regs.ARM_sp - sizeof(sc);
	if (0 > write_mem(pid, (unsigned long*)&sc, sizeof(sc)/sizeof(long), codeaddr)) {

#ifdef DEBUG 
		printf("cannot write code, error!\n");
#endif
		exit(1);
	}
	
	
#ifdef DEBUG 
	printf("executing injection code at 0x%x\n", codeaddr);
#endif

		

	// calc stack pointer
	regs.ARM_sp = regs.ARM_sp - n*4 - sizeof(sc);

	// call mprotect() to make stack executable
	regs.ARM_r0 = stack_start; // want to make stack executable
	//printf("r0 %x\n", regs.ARM_r0);
	regs.ARM_r1 = stack_end - stack_start; // stack size
	//printf("mprotect(%x, %d, ALL)\n", regs.ARM_r0, regs.ARM_r1);
	regs.ARM_r2 = PROT_READ|PROT_WRITE|PROT_EXEC; // protections
	regs.ARM_lr = codeaddr; // points to loading and fixing code
	regs.ARM_pc = mprotectaddr; // execute mprotect()

	
	// detach and continue

	result = ptrace(PTRACE_SETREGS, pid, 0, &regs);

	/* printf("%d\n", result); */
	/* return 0; */

	
	

	
#ifdef DEBUG 
	printf("first ptrace %d\n", result);
	if( result == -1 )
	  printf("\t\t%s\n", strerror(*(int*)__errno()) );
#endif

	result = ptrace(PTRACE_DETACH, pid, 0, 0);
	
	

#ifdef DEBUG 
	printf("second ptrace %d\n", result);
	if( result == -1 )
	  printf("\t\t%s\n", strerror(*(int*)__errno()) );
#endif


	
#ifdef DEBUG 
	printf("library injection completed!\n");
#endif
	

	return 0;
}

















