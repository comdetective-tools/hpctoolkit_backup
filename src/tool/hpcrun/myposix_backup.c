#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <dlfcn.h>
//#include <cstdint>
#include <string.h>
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <adm_init_fini.h>
#include "env.h"
//#define ENABLE_OBJECT_LEVEL 1

int init_adamant = 0;

static void* (*real_malloc)(size_t)=NULL;

static void* (*real_calloc)(size_t, size_t)=NULL;

static void malloc_init(void)
{
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    if (NULL == real_malloc) {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
    }
}

static void (*real_free)(void*)=NULL;

static void free_init(void)
{
    real_free = dlsym(RTLD_NEXT, "free");
    if (NULL == real_free) {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
    }
}

static void calloc_init(void)
{
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    if (NULL == real_calloc) {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
    }
}

static void* (*real_realloc)(void*, size_t)=NULL;

static void realloc_init(void)
{
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    if (NULL == real_realloc) {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
    }
}

static int (*real_posix_memalign)(void**, size_t, size_t)=NULL;

static void posix_memalign_init(void)
{
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    if (NULL == real_posix_memalign) {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
    }
}

static void* (*real_memalign)(size_t, size_t)=NULL;

static void memalign_init(void)
{
    real_memalign = dlsym(RTLD_NEXT, "memalign");
    if (NULL == real_memalign) {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
    }
}

static void* (*real_aligned_alloc)(size_t, size_t)=NULL;

static void aligned_alloc_init(void)
{
    real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    if (NULL == real_aligned_alloc) {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
    }
}

static void* (*real_valloc)(size_t)=NULL;

static void valloc_init(void)
{
    real_valloc = dlsym(RTLD_NEXT, "valloc");
    if (NULL == real_valloc) {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
    }
}

static void* (*real_pvalloc)(size_t)=NULL;

static void pvalloc_init(void)
{
    real_pvalloc = dlsym(RTLD_NEXT, "pvalloc");
    if (NULL == real_pvalloc) {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
    }
}

/*
void *malloc(size_t size)
{
    void *p = NULL;
    fprintf(stderr, "malloc(%ld) = ", size);
    p = malloc_adm(size);
    fprintf(stderr, "%p\n", p);
    return p;
}*/


void *malloc(size_t size)
{
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(!init_adamant) {
		init_adamant = 1;
		adm_initialize();
    	}
    }

    if(real_malloc==NULL) {
        malloc_init();
    }

    void *p = NULL;
    //fprintf(stderr, "malloc(%ld)\n", size);
    p = real_malloc(size);
    //malloc_adm(p, size);
    //fprintf(stderr, "%p\n", p);
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(size > 1000)
    		malloc_adm(p, size);
    }
    return p;
}

void free(void* ptr)
{
   if (getenv(HPCRUN_OBJECT_LEVEL)) {
   	if(!init_adamant) {
        	init_adamant = 1;
        	adm_initialize();
    	}
   }

   if(real_free==NULL) {
        free_init();
    }

    //fprintf(stderr, "address %p is freed\n", ptr);
    real_free(ptr);
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	free_adm(ptr);
    }
}






void* realloc(void *ptr, size_t size) 
{
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(!init_adamant) {
        	init_adamant = 1;
        	adm_initialize();
    	}
    }

    if(real_realloc==NULL) {
        realloc_init();
    }

    void *p = NULL;
    //fprintf(stderr, "realloc(%p, %ld)\n", ptr, size);
    p = real_realloc(ptr, size);
    //fprintf(stderr, "%p\n", p);
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(size > 1000)
    		realloc_adm(p, ptr, size);
    }
    return p;
}

int posix_memalign(void** memptr, size_t alignment, size_t size)
{
   if (getenv(HPCRUN_OBJECT_LEVEL)) {
   	if(!init_adamant) {
        	init_adamant = 1;
        	adm_initialize();
    	}
   }

   if(real_posix_memalign==NULL) {
        posix_memalign_init();
    }

    int p;
    //fprintf(stderr, "posix_memalign(%p, %ld, %ld) = ", memptr, alignment, size);
    p = real_posix_memalign(memptr, alignment, size);
    //fprintf(stderr, "%d\n", p);

    //int p;
    //fprintf(stderr, "posix_memalign(%p, %ld, %ld) = ", memptr, alignment, size);
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(size > 1000)
    		posix_memalign_adm(p, memptr, alignment, size);
    }
    //fprintf(stderr, "%d\n", p);
    return p;
}

void* memalign(size_t alignment, size_t size)
{
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(!init_adamant) {
        	init_adamant = 1;
        	adm_initialize();
    	}
    }

    if(real_memalign==NULL) {
        memalign_init();
    }

    void* p;
    //fprintf(stderr, "memalign(%ld, %ld) = ", alignment, size);
    p = real_memalign(alignment, size);
    //fprintf(stderr, "%p\n", p);

    //void* p;
    //fprintf(stderr, "memalign(%ld, %ld) = ", alignment, size);
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(size > 1000)
    		memalign_adm(p, size);
    }
    //fprintf(stderr, "%p\n", p);
    return p;
}

void* aligned_alloc(size_t alignment, size_t size)
{
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(!init_adamant) {
        	init_adamant = 1;
        	adm_initialize();
    	}
    }

    if(real_aligned_alloc==NULL) {
        aligned_alloc_init();
    }

    void* p;
    //fprintf(stderr, "aligned_alloc(%ld, %ld) = ", alignment, size);
    p = real_aligned_alloc(alignment, size);
    //fprintf(stderr, "%p\n", p);
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(size > 1000)
    		aligned_alloc_adm(p, size);
    }
    return p;
}


void* valloc(size_t size)
{
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(!init_adamant) {
        	init_adamant = 1;
        	adm_initialize();
    	}
    }

    if(real_valloc==NULL) {
        valloc_init();
    }

    void* p;
    //fprintf(stderr, "valloc(%ld) = ", size);
    p = real_valloc(size);
    //fprintf(stderr, "%p\n", p);
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(size > 1000)
    		valloc_adm(p, size);
    } 
    return p;
}

void* pvalloc(size_t size)
{
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(!init_adamant) {
        	init_adamant = 1;
        	adm_initialize();
    	}
    }

    if(real_pvalloc==NULL) {
        pvalloc_init();
    }

    void* p;
    //fprintf(stderr, "pvalloc(%ld) = ", size);
    p = real_pvalloc(size);
    //fprintf(stderr, "%p\n", p);
    if (getenv(HPCRUN_OBJECT_LEVEL)) {
    	if(size > 1000)
    		pvalloc_adm(p, size);
    }
    return p;
}

