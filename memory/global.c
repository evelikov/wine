/*
 * Global heap functions
 *
 * Copyright 1995 Alexandre Julliard
 */

#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "windows.h"
#include "global.h"
#include "heap.h"
#include "toolhelp.h"
#include "selectors.h"
#include "miscemu.h"
#include "dde_mem.h"
#include "stackframe.h"
#include "options.h"
#include "stddebug.h"
#include "debug.h"
#include "winerror.h"

  /* Global arena block */
typedef struct
{
    DWORD     base;          /* Base address (0 if discarded) */
    DWORD     size;          /* Size in bytes (0 indicates a free block) */
    HGLOBAL16 handle;        /* Handle for this block */
    HGLOBAL16 hOwner;        /* Owner of this block */
    BYTE      lockCount;     /* Count of GlobalFix() calls */
    BYTE      pageLockCount; /* Count of GlobalPageLock() calls */
    BYTE      flags;         /* Allocation flags */
    BYTE      selCount;      /* Number of selectors allocated for this block */
#ifdef CONFIG_IPC
    int       shmid;
#endif
} GLOBALARENA;

  /* Flags definitions */
#define GA_MOVEABLE     0x02  /* same as GMEM_MOVEABLE */
#define GA_DGROUP       0x04
#define GA_DISCARDABLE  0x08
#define GA_IPCSHARE     0x10  /* same as GMEM_DDESHARE */

  /* Arena array */
static GLOBALARENA *pGlobalArena = NULL;
static int globalArenaSize = 0;

#define GLOBAL_MAX_ALLOC_SIZE 0x00ff0000  /* Largest allocation is 16M - 64K */

#define GET_ARENA_PTR(handle)  (pGlobalArena + ((handle) >> __AHSHIFT))

/***********************************************************************
 *           GLOBAL_GetArena
 *
 * Return the arena for a given selector, growing the arena array if needed.
 */
static GLOBALARENA *GLOBAL_GetArena( WORD sel, WORD selcount )
{
    if (((sel >> __AHSHIFT) + selcount) > globalArenaSize)
    {
        int newsize = ((sel >> __AHSHIFT) + selcount + 0xff) & ~0xff;
        GLOBALARENA *pNewArena = realloc( pGlobalArena,
                                          newsize * sizeof(GLOBALARENA) );
        if (!pNewArena) return 0;
        pGlobalArena = pNewArena;
        memset( pGlobalArena + globalArenaSize, 0,
                (newsize - globalArenaSize) * sizeof(GLOBALARENA) );
        globalArenaSize = newsize;
    }
    return pGlobalArena + (sel >> __AHSHIFT);
}


void debug_handles()
{
    int printed=0;
    int i;
    for (i = globalArenaSize-1 ; i>=0 ; i--) {
	if (pGlobalArena[i].size!=0 && (pGlobalArena[i].handle & 0x8000)){
	    printed=1;
	    printf("0x%08x, ",pGlobalArena[i].handle);
	}
    }
    if (printed)
	printf("\n");
}


/***********************************************************************
 *           GLOBAL_CreateBlock
 *
 * Create a global heap block for a fixed range of linear memory.
 */
HGLOBAL16 GLOBAL_CreateBlock( WORD flags, const void *ptr, DWORD size,
                              HGLOBAL16 hOwner, BOOL16 isCode,
                              BOOL16 is32Bit, BOOL16 isReadOnly,
                              SHMDATA *shmdata  )
{
    WORD sel, selcount;
    GLOBALARENA *pArena;

      /* Allocate the selector(s) */

    sel = SELECTOR_AllocBlock( ptr, size,
			      isCode ? SEGMENT_CODE : SEGMENT_DATA,
			      is32Bit, isReadOnly );
    
    if (!sel) return 0;
    selcount = (size + 0xffff) / 0x10000;

    if (!(pArena = GLOBAL_GetArena( sel, selcount )))
    {
        SELECTOR_FreeBlock( sel, selcount );
        return 0;
    }

      /* Fill the arena block */

    pArena->base = (DWORD)ptr;
    pArena->size = GET_SEL_LIMIT(sel) + 1;

#ifdef CONFIG_IPC
    if ((flags & GMEM_DDESHARE) && Options.ipc)
    {
	pArena->handle = shmdata->handle;
	pArena->shmid  = shmdata->shmid;
	shmdata->sel   = sel;
    }
    else
    {
	pArena->handle = (flags & GMEM_MOVEABLE) ? sel - 1 : sel;
	pArena->shmid  = 0;
    }
#else
    pArena->handle = (flags & GMEM_MOVEABLE) ? sel - 1 : sel;
#endif
    pArena->hOwner = hOwner;
    pArena->lockCount = 0;
    pArena->pageLockCount = 0;
    pArena->flags = flags & GA_MOVEABLE;
    if (flags & GMEM_DISCARDABLE) pArena->flags |= GA_DISCARDABLE;
    if (flags & GMEM_DDESHARE) pArena->flags |= GA_IPCSHARE;
    if (!isCode) pArena->flags |= GA_DGROUP;
    pArena->selCount = selcount;
    if (selcount > 1)  /* clear the next arena blocks */
        memset( pArena + 1, 0, (selcount - 1) * sizeof(GLOBALARENA) );

    return pArena->handle;
}


/***********************************************************************
 *           GLOBAL_FreeBlock
 *
 * Free a block allocated by GLOBAL_CreateBlock, without touching
 * the associated linear memory range.
 */
BOOL16 GLOBAL_FreeBlock( HGLOBAL16 handle )
{
    WORD sel;
    GLOBALARENA *pArena;

    if (!handle) return TRUE;
    sel = GlobalHandleToSel( handle ); 
    pArena = GET_ARENA_PTR(sel);
    SELECTOR_FreeBlock( sel, (pArena->size + 0xffff) / 0x10000 );
    memset( pArena, 0, sizeof(GLOBALARENA) );
    return TRUE;
}


/***********************************************************************
 *           GLOBAL_Alloc
 *
 * Implementation of GlobalAlloc16()
 */
HGLOBAL16 GLOBAL_Alloc( UINT16 flags, DWORD size, HGLOBAL16 hOwner,
                        BOOL16 isCode, BOOL16 is32Bit, BOOL16 isReadOnly )
{
    void *ptr;
    HGLOBAL16 handle;
    SHMDATA shmdata;

    dprintf_global( stddeb, "GlobalAlloc: %ld flags=%04x\n", size, flags );

    /* If size is 0, create a discarded block */

    if (size == 0) return GLOBAL_CreateBlock( flags, NULL, 1, hOwner, isCode,
                                              is32Bit, isReadOnly, NULL );

    /* Fixup the size */

    if (size >= GLOBAL_MAX_ALLOC_SIZE - 0x1f) return 0;
    size = (size + 0x1f) & ~0x1f;

      /* Allocate the linear memory */

#ifdef CONFIG_IPC
    if ((flags & GMEM_DDESHARE) && Options.ipc)
        ptr = DDE_malloc(flags, size, &shmdata);
    else 
#endif  /* CONFIG_IPC */
    {
	ptr = HeapAlloc( SystemHeap, 0, size );
    }
    if (!ptr) return 0;

      /* Allocate the selector(s) */

    handle = GLOBAL_CreateBlock( flags, ptr, size, hOwner,
				isCode, is32Bit, isReadOnly, &shmdata);
    if (!handle)
    {
        HeapFree( SystemHeap, 0, ptr );
        return 0;
    }

    if (flags & GMEM_ZEROINIT) memset( ptr, 0, size );
    return handle;
}


#ifdef CONFIG_IPC
/***********************************************************************
 *           GLOBAL_FindArena
 *
 * Find the arena  for a given handle
 * (when handle is not serial - e.g. DDE)
 */
static GLOBALARENA *GLOBAL_FindArena( HGLOBAL16 handle)
{
    int i;
    for (i = globalArenaSize-1 ; i>=0 ; i--) {
	if (pGlobalArena[i].size!=0 && pGlobalArena[i].handle == handle)
	    return ( &pGlobalArena[i] );
    }
    return NULL;
}


/***********************************************************************
 *           DDE_GlobalHandleToSel
 */

WORD DDE_GlobalHandleToSel( HGLOBAL16 handle )
{
    GLOBALARENA *pArena;
    SEGPTR segptr;
    
    pArena= GLOBAL_FindArena(handle);
    if (pArena) {
	int ArenaIdx = pArena - pGlobalArena;

	/* See if synchronized to the shared memory  */
	return DDE_SyncHandle(handle, ( ArenaIdx << __AHSHIFT) | 7);
    }

    /* attach the block */
    DDE_AttachHandle(handle, &segptr);

    return SELECTOROF( segptr );
}
#endif  /* CONFIG_IPC */


/***********************************************************************
 *           GlobalAlloc16   (KERNEL.15)
 */
HGLOBAL16 GlobalAlloc16( UINT16 flags, DWORD size )
{
    HANDLE16 owner = GetCurrentPDB();

    if (flags & GMEM_DDESHARE)
        owner = GetExePtr(owner);  /* Make it a module handle */
    return GLOBAL_Alloc( flags, size, owner, FALSE, FALSE, FALSE );
}


/***********************************************************************
 *           GlobalReAlloc16   (KERNEL.16)
 */
HGLOBAL16 GlobalReAlloc16( HGLOBAL16 handle, DWORD size, UINT16 flags )
{
    WORD selcount;
    DWORD oldsize;
    void *ptr;
    GLOBALARENA *pArena, *pNewArena;
    WORD sel = GlobalHandleToSel( handle );

    dprintf_global( stddeb, "GlobalReAlloc16: %04x %ld flags=%04x\n",
                    handle, size, flags );
    if (!handle) return 0;
    
#ifdef CONFIG_IPC
    if (Options.ipc && (flags & GMEM_DDESHARE || is_dde_handle(handle))) {
	fprintf(stdnimp,
               "GlobalReAlloc16: shared memory reallocating unimplemented\n"); 
	return 0;
    }
#endif  /* CONFIG_IPC */

    pArena = GET_ARENA_PTR( handle );

      /* Discard the block if requested */

    if ((size == 0) && (flags & GMEM_MOVEABLE) && !(flags & GMEM_MODIFY))
    {
        if (!(pArena->flags & GA_MOVEABLE) ||
            !(pArena->flags & GA_DISCARDABLE) ||
            (pArena->lockCount > 0) || (pArena->pageLockCount > 0)) return 0;
        HeapFree( SystemHeap, 0, (void *)pArena->base );
        pArena->base = 0;
        /* Note: we rely on the fact that SELECTOR_ReallocBlock won't */
        /* change the selector if we are shrinking the block */
        SELECTOR_ReallocBlock( sel, 0, 1, SEGMENT_DATA, 0, 0 );
        return handle;
    }

      /* Fixup the size */

    if (size > GLOBAL_MAX_ALLOC_SIZE - 0x20) return 0;
    if (size == 0) size = 0x20;
    else size = (size + 0x1f) & ~0x1f;

      /* Change the flags */

    if (flags & GMEM_MODIFY)
    {
          /* Change the flags, leaving GA_DGROUP alone */
        pArena->flags = (pArena->flags & GA_DGROUP) | (flags & GA_MOVEABLE);
        if (flags & GMEM_DISCARDABLE) pArena->flags |= GA_DISCARDABLE;
        return handle;
    }

      /* Reallocate the linear memory */

    ptr = (void *)pArena->base;
    oldsize = pArena->size;
    dprintf_global(stddeb,"oldsize %08lx\n",oldsize);
    if (ptr && (size == oldsize)) return handle;  /* Nothing to do */

    ptr = HeapReAlloc( SystemHeap, 0, ptr, size );
    if (!ptr)
    {
        SELECTOR_FreeBlock( sel, (oldsize + 0xffff) / 0x10000 );
        memset( pArena, 0, sizeof(GLOBALARENA) );
        return 0;
    }

      /* Reallocate the selector(s) */

    sel = SELECTOR_ReallocBlock( sel, ptr, size, SEGMENT_DATA, 0, 0 );
    if (!sel)
    {
        HeapFree( SystemHeap, 0, ptr );
        memset( pArena, 0, sizeof(GLOBALARENA) );
        return 0;
    }
    selcount = (size + 0xffff) / 0x10000;

    if (!(pNewArena = GLOBAL_GetArena( sel, selcount )))
    {
        HeapFree( SystemHeap, 0, ptr );
        SELECTOR_FreeBlock( sel, selcount );
        return 0;
    }

      /* Fill the new arena block */

    if (pNewArena != pArena) memcpy( pNewArena, pArena, sizeof(GLOBALARENA) );
    pNewArena->base = (DWORD)ptr;
    pNewArena->size = GET_SEL_LIMIT(sel) + 1;
    pNewArena->selCount = selcount;
    pNewArena->handle = (pNewArena->flags & GA_MOVEABLE) ? sel - 1 : sel;

    if (selcount > 1)  /* clear the next arena blocks */
        memset( pNewArena + 1, 0, (selcount - 1) * sizeof(GLOBALARENA) );

    if ((oldsize < size) && (flags & GMEM_ZEROINIT))
        memset( (char *)ptr + oldsize, 0, size - oldsize );
    return pNewArena->handle;
}


/***********************************************************************
 *           GlobalFree16   (KERNEL.17)
 */
HGLOBAL16 GlobalFree16( HGLOBAL16 handle )
{
    void *ptr = GlobalLock16( handle );

    dprintf_global( stddeb, "GlobalFree16: %04x\n", handle );
    if (!GLOBAL_FreeBlock( handle )) return handle;  /* failed */
#ifdef CONFIG_IPC
    if (is_dde_handle(handle)) return DDE_GlobalFree(handle);
#endif  /* CONFIG_IPC */
    if (ptr) HeapFree( SystemHeap, 0, ptr );
    return 0;
}


/***********************************************************************
 *           WIN16_GlobalLock16   (KERNEL.18)
 *
 * This is the GlobalLock16() function used by 16-bit code.
 */
SEGPTR WIN16_GlobalLock16( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "WIN16_GlobalLock16(%04x) -> %08lx\n",
                    handle, MAKELONG( 0, GlobalHandleToSel(handle)) );
    if (!handle) return 0;

#ifdef CONFIG_IPC
    if (is_dde_handle(handle))
        return PTR_SEG_OFF_TO_SEGPTR( DDE_GlobalHandleToSel(handle), 0 );
#endif  /* CONFIG_IPC */

    if (!GET_ARENA_PTR(handle)->base) return (SEGPTR)0;
    return PTR_SEG_OFF_TO_SEGPTR( GlobalHandleToSel(handle), 0 );
}


/***********************************************************************
 *           GlobalLock16   (KERNEL.18)
 *
 * This is the GlobalLock16() function used by 32-bit code.
 */
LPVOID GlobalLock16( HGLOBAL16 handle )
{
    if (!handle) return 0;
#ifdef CONFIG_IPC
    if (is_dde_handle(handle)) return DDE_AttachHandle(handle, NULL);
#endif
    return (LPVOID)GET_ARENA_PTR(handle)->base;
}


/***********************************************************************
 *           GlobalUnlock16   (KERNEL.19)
 */
BOOL16 GlobalUnlock16( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "GlobalUnlock16: %04x\n", handle );
    return 0;
}


/***********************************************************************
 *           GlobalSize16   (KERNEL.20)
 */
DWORD GlobalSize16( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "GlobalSize16: %04x\n", handle );
    if (!handle) return 0;
    return GET_ARENA_PTR(handle)->size;
}


/***********************************************************************
 *           GlobalHandle16   (KERNEL.21)
 */
DWORD GlobalHandle16( WORD sel )
{
    dprintf_global( stddeb, "GlobalHandle16: %04x\n", sel );
    return MAKELONG( GET_ARENA_PTR(sel)->handle, GlobalHandleToSel(sel) );
}


/***********************************************************************
 *           GlobalFlags16   (KERNEL.22)
 */
UINT16 GlobalFlags16( HGLOBAL16 handle )
{
    GLOBALARENA *pArena;

    dprintf_global( stddeb, "GlobalFlags16: %04x\n", handle );
    pArena = GET_ARENA_PTR(handle);
    return pArena->lockCount |
           ((pArena->flags & GA_DISCARDABLE) ? GMEM_DISCARDABLE : 0) |
           ((pArena->base == 0) ? GMEM_DISCARDED : 0);
}


/***********************************************************************
 *           LockSegment16   (KERNEL.23)
 */
HGLOBAL16 LockSegment16( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "LockSegment: %04x\n", handle );
    if (handle == (HGLOBAL16)-1) handle = CURRENT_DS;
    GET_ARENA_PTR(handle)->lockCount++;
    return handle;
}


/***********************************************************************
 *           UnlockSegment16   (KERNEL.24)
 */
void UnlockSegment16( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "UnlockSegment: %04x\n", handle );
    if (handle == (HGLOBAL16)-1) handle = CURRENT_DS;
    GET_ARENA_PTR(handle)->lockCount--;
    /* FIXME: this ought to return the lock count in CX (go figure...) */
}


/***********************************************************************
 *           GlobalCompact16   (KERNEL.25)
 */
DWORD GlobalCompact16( DWORD desired )
{
    return GLOBAL_MAX_ALLOC_SIZE;
}


/***********************************************************************
 *           GlobalFreeAll   (KERNEL.26)
 */
void GlobalFreeAll( HGLOBAL16 owner )
{
    DWORD i;
    GLOBALARENA *pArena;

    pArena = pGlobalArena;
    for (i = 0; i < globalArenaSize; i++, pArena++)
    {
        if ((pArena->size != 0) && (pArena->hOwner == owner))
            GlobalFree16( pArena->handle );
    }
}


/***********************************************************************
 *           GlobalWire16   (KERNEL.111)
 */
SEGPTR GlobalWire16( HGLOBAL16 handle )
{
    return WIN16_GlobalLock16( handle );
}


/***********************************************************************
 *           GlobalUnWire16   (KERNEL.112)
 */
BOOL16 GlobalUnWire16( HGLOBAL16 handle )
{
    return GlobalUnlock16( handle );
}


/***********************************************************************
 *           SetSwapAreaSize   (KERNEL.106)
 */
LONG SetSwapAreaSize( WORD size )
{
    dprintf_global(stdnimp, "STUB: SetSwapAreaSize(%d)\n", size );
    return MAKELONG( size, 0xffff );
}


/***********************************************************************
 *           GlobalLRUOldest   (KERNEL.163)
 */
HGLOBAL16 GlobalLRUOldest( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "GlobalLRUOldest: %04x\n", handle );
    if (handle == (HGLOBAL16)-1) handle = CURRENT_DS;
    return handle;
}


/***********************************************************************
 *           GlobalLRUNewest   (KERNEL.164)
 */
HGLOBAL16 GlobalLRUNewest( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "GlobalLRUNewest: %04x\n", handle );
    if (handle == (HGLOBAL16)-1) handle = CURRENT_DS;
    return handle;
}


/***********************************************************************
 *           GetFreeSpace16   (KERNEL.169)
 */
DWORD GetFreeSpace16( UINT16 wFlags )
{
    MEMORYSTATUS ms;
    GlobalMemoryStatus( &ms );
    return ms.dwAvailVirtual;
}

/***********************************************************************
 *           GlobalDOSAlloc   (KERNEL.184)
 */
DWORD GlobalDOSAlloc(DWORD size)
{
   UINT16    uParagraph;
   LPVOID    lpBlock = DOSMEM_GetBlock( size, &uParagraph );
   
   if( lpBlock )
   {
       HMODULE16 hModule = GetModuleHandle("KERNEL");
       WORD	 wSelector;
   
       wSelector = GLOBAL_CreateBlock(GMEM_FIXED, lpBlock, size, 
				      hModule, 0, 0, 0, NULL );
       return MAKELONG(wSelector,uParagraph);
   }
   return 0;
}

/***********************************************************************
 *           GlobalDOSFree      (KERNEL.185)
 */
WORD GlobalDOSFree(WORD sel)
{
   DWORD   block = GetSelectorBase(sel);

   if( block && block < 0x100000 ) 
   {
       LPVOID lpBlock = DOSMEM_MapDosToLinear( block );
       if( DOSMEM_FreeBlock( lpBlock ) )
	   GLOBAL_FreeBlock( sel );
       sel = 0;
   }
   return sel;
}

/***********************************************************************
 *           GlobalPageLock   (KERNEL.191)
 */
WORD GlobalPageLock( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "GlobalPageLock: %04x\n", handle );
    return ++(GET_ARENA_PTR(handle)->pageLockCount);
}


/***********************************************************************
 *           GlobalPageUnlock   (KERNEL.192)
 */
WORD GlobalPageUnlock( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "GlobalPageUnlock: %04x\n", handle );
    return --(GET_ARENA_PTR(handle)->pageLockCount);
}


/***********************************************************************
 *           GlobalFix16   (KERNEL.197)
 */
void GlobalFix16( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "GlobalFix16: %04x\n", handle );
    GET_ARENA_PTR(handle)->lockCount++;
}


/***********************************************************************
 *           GlobalUnfix16   (KERNEL.198)
 */
void GlobalUnfix16( HGLOBAL16 handle )
{
    dprintf_global( stddeb, "GlobalUnfix16: %04x\n", handle );
    GET_ARENA_PTR(handle)->lockCount--;
}


/***********************************************************************
 *           FarSetOwner   (KERNEL.403)
 */
void FarSetOwner( HGLOBAL16 handle, HANDLE16 hOwner )
{
    GET_ARENA_PTR(handle)->hOwner = hOwner;
}


/***********************************************************************
 *           FarGetOwner   (KERNEL.404)
 */
HANDLE16 FarGetOwner( HGLOBAL16 handle )
{
    return GET_ARENA_PTR(handle)->hOwner;
}


/***********************************************************************
 *           GlobalHandleToSel   (TOOLHELP.50)
 */
WORD GlobalHandleToSel( HGLOBAL16 handle )
{
    dprintf_toolhelp( stddeb, "GlobalHandleToSel: %04x\n", handle );
    if (!handle) return 0;
#ifdef CONFIG_IPC
    if (is_dde_handle(handle)) return DDE_GlobalHandleToSel(handle);
#endif
    if (!(handle & 7))
    {
        fprintf( stderr, "Program attempted invalid selector conversion\n" );
        return handle - 1;
    }
    return handle | 7;
}


/***********************************************************************
 *           GlobalFirst   (TOOLHELP.51)
 */
BOOL16 GlobalFirst( GLOBALENTRY *pGlobal, WORD wFlags )
{
    if (wFlags == GLOBAL_LRU) return FALSE;
    pGlobal->dwNext = 0;
    return GlobalNext( pGlobal, wFlags );
}


/***********************************************************************
 *           GlobalNext   (TOOLHELP.52)
 */
BOOL16 GlobalNext( GLOBALENTRY *pGlobal, WORD wFlags)
{
    GLOBALARENA *pArena;

    if (pGlobal->dwNext >= globalArenaSize) return FALSE;
    pArena = pGlobalArena + pGlobal->dwNext;
    if (wFlags == GLOBAL_FREE)  /* only free blocks */
    {
        int i;
        for (i = pGlobal->dwNext; i < globalArenaSize; i++, pArena++)
            if (pArena->size == 0) break;  /* block is free */
        if (i >= globalArenaSize) return FALSE;
        pGlobal->dwNext = i;
    }

    pGlobal->dwAddress    = pArena->base;
    pGlobal->dwBlockSize  = pArena->size;
    pGlobal->hBlock       = pArena->handle;
    pGlobal->wcLock       = pArena->lockCount;
    pGlobal->wcPageLock   = pArena->pageLockCount;
    pGlobal->wFlags       = (GetCurrentPDB() == pArena->hOwner);
    pGlobal->wHeapPresent = FALSE;
    pGlobal->hOwner       = pArena->hOwner;
    pGlobal->wType        = GT_UNKNOWN;
    pGlobal->wData        = 0;
    pGlobal->dwNext++;
    return TRUE;
}


/***********************************************************************
 *           GlobalInfo   (TOOLHELP.53)
 */
BOOL16 GlobalInfo( GLOBALINFO *pInfo )
{
    int i;
    GLOBALARENA *pArena;

    pInfo->wcItems = globalArenaSize;
    pInfo->wcItemsFree = 0;
    pInfo->wcItemsLRU = 0;
    for (i = 0, pArena = pGlobalArena; i < globalArenaSize; i++, pArena++)
        if (pArena->size == 0) pInfo->wcItemsFree++;
    return TRUE;
}


/***********************************************************************
 *           GlobalEntryHandle   (TOOLHELP.54)
 */
BOOL16 GlobalEntryHandle( GLOBALENTRY *pGlobal, HGLOBAL16 hItem )
{
    return FALSE;
}


/***********************************************************************
 *           GlobalEntryModule   (TOOLHELP.55)
 */
BOOL16 GlobalEntryModule( GLOBALENTRY *pGlobal, HMODULE16 hModule, WORD wSeg )
{
    return FALSE;
}


/***********************************************************************
 *           MemManInfo   (TOOLHELP.72)
 */
BOOL16 MemManInfo( MEMMANINFO *info )
{
    MEMORYSTATUS status;

    if (info->dwSize < sizeof(MEMMANINFO)) return FALSE;
    GlobalMemoryStatus( &status );
#ifdef __svr4__
    info->wPageSize            = sysconf(_SC_PAGESIZE);
#else
    info->wPageSize            = getpagesize();
#endif
    info->dwLargestFreeBlock   = status.dwAvailVirtual;
    info->dwMaxPagesAvailable  = info->dwLargestFreeBlock / info->wPageSize;
    info->dwMaxPagesLockable   = info->dwMaxPagesAvailable;
    info->dwTotalLinearSpace   = status.dwTotalVirtual / info->wPageSize;
    info->dwTotalUnlockedPages = info->dwTotalLinearSpace;
    info->dwFreePages          = info->dwMaxPagesAvailable;
    info->dwTotalPages         = info->dwTotalLinearSpace;
    info->dwFreeLinearSpace    = info->dwMaxPagesAvailable;
    info->dwSwapFilePages      = status.dwTotalPageFile / info->wPageSize;
    return TRUE;
}

/*
 * Win32 Global heap functions (GlobalXXX).
 * These functions included in Win32 for compatibility with 16 bit Windows
 * Especially the moveable blocks and handles are oldish. 
 * But the ability to directly allocate memory with GPTR and LPTR is widely
 * used.
 *
 * The handle stuff looks horrible, but it's implemented almost like Win95
 * does it. 
 *
 */

#define MAGIC_GLOBAL_USED 0x5342
#define GLOBAL_LOCK_MAX   0xFF
#define HANDLE_TO_INTERN(h)  (PGLOBAL32_INTERN)(((char *)(h))-2)
#define INTERN_TO_HANDLE(i)  ((HGLOBAL32) &((i)->Pointer))
#define POINTER_TO_HANDLE(p) (*(((HGLOBAL32 *)(p))-1))
#define ISHANDLE(h)          (((DWORD)(h)&2)!=0)
#define ISPOINTER(h)         (((DWORD)(h)&2)==0)

typedef struct __GLOBAL32_INTERN
{
   WORD         Magic;
   LPVOID       Pointer WINE_PACKED;
   BYTE         Flags;
   BYTE         LockCount;
} GLOBAL32_INTERN, *PGLOBAL32_INTERN;


/***********************************************************************
 *           GlobalAlloc32   (KERNEL32.315)
 */
HGLOBAL32 GlobalAlloc32(UINT32 flags, DWORD size)
{
   PGLOBAL32_INTERN     pintern;
   DWORD		hpflags;
   LPVOID               palloc;

   if(flags&GMEM_ZEROINIT)
      hpflags=HEAP_ZERO_MEMORY;
   else
      hpflags=0;
   
   if((flags & GMEM_MOVEABLE)==0) /* POINTER */
   {
      palloc=HeapAlloc(GetProcessHeap(), hpflags, size);
      return (HGLOBAL32) palloc;
   }
   else  /* HANDLE */
   {
      /* HeapLock(GetProcessHeap()); */

      pintern=HeapAlloc(GetProcessHeap(), 0,  sizeof(GLOBAL32_INTERN));
      if(size)
      {
	 palloc=HeapAlloc(GetProcessHeap(), 0, size+sizeof(HGLOBAL32));
	 *(HGLOBAL32 *)palloc=INTERN_TO_HANDLE(pintern);
	 pintern->Pointer=palloc+sizeof(HGLOBAL32);
      }
      else
	 pintern->Pointer=NULL;
      pintern->Magic=MAGIC_GLOBAL_USED;
      pintern->Flags=flags>>8;
      pintern->LockCount=0;
      
      /* HeapUnlock(GetProcessHeap()); */
       
      return INTERN_TO_HANDLE(pintern);
   }
}


/***********************************************************************
 *           GlobalLock32   (KERNEL32.326)
 */
LPVOID  GlobalLock32(HGLOBAL32 hmem)
{
   PGLOBAL32_INTERN pintern;
   LPVOID           palloc;

   if(ISPOINTER(hmem))
      return (LPVOID) hmem;

   /* HeapLock(GetProcessHeap()); */
   
   pintern=HANDLE_TO_INTERN(hmem);
   if(pintern->Magic==MAGIC_GLOBAL_USED)
   {
      if(pintern->LockCount<GLOBAL_LOCK_MAX)
	 pintern->LockCount++;
      palloc=pintern->Pointer;
   }
   else
   {
      dprintf_global(stddeb, "GlobalLock32: invalid handle\n");
      palloc=(LPVOID) NULL;
   }
   /* HeapUnlock(GetProcessHeap()); */;
   return palloc;
}


/***********************************************************************
 *           GlobalUnlock32   (KERNEL32.332)
 */
BOOL32 GlobalUnlock32(HGLOBAL32 hmem)
{
   PGLOBAL32_INTERN       pintern;
   BOOL32                 locked;

   if(ISPOINTER(hmem))
      return FALSE;

   /* HeapLock(GetProcessHeap()); */
   pintern=HANDLE_TO_INTERN(hmem);
   
   if(pintern->Magic==MAGIC_GLOBAL_USED)
   {
      if((pintern->LockCount<GLOBAL_LOCK_MAX)&&(pintern->LockCount>0))
	 pintern->LockCount--;

      locked=(pintern->LockCount==0) ? FALSE : TRUE;
   }
   else
   {
      dprintf_global(stddeb, "GlobalUnlock32: invalid handle\n");
      locked=FALSE;
   }
   /* HeapUnlock(GetProcessHeap()); */
   return locked;
}


/***********************************************************************
 *           GlobalHandle32   (KERNEL32.325)
 */
HGLOBAL32 GlobalHandle32(LPCVOID pmem)
{
   return (HGLOBAL32) POINTER_TO_HANDLE(pmem);
}


/***********************************************************************
 *           GlobalReAlloc32   (KERNEL32.328)
 */
HGLOBAL32 GlobalReAlloc32(HGLOBAL32 hmem, DWORD size, UINT32 flags)
{
   LPVOID               palloc;
   HGLOBAL32            hnew;
   PGLOBAL32_INTERN     pintern;

   hnew=NULL;
   /* HeapLock(GetProcessHeap()); */
   if(flags & GMEM_MODIFY) /* modify flags */
   {
      if( ISPOINTER(hmem) && (flags & GMEM_MOVEABLE))
      {
	 /* make a fixed block moveable
	  * actually only NT is able to do this. But it's soo simple
	  */
	 size=HeapSize(GetProcessHeap(), 0, (LPVOID) hmem);
	 hnew=GlobalAlloc32( flags, size);
	 palloc=GlobalLock32(hnew);
	 memcpy(palloc, (LPVOID) hmem, size);
	 GlobalUnlock32(hnew);
	 GlobalFree32(hmem);
      }
      else if( ISPOINTER(hmem) &&(flags & GMEM_DISCARDABLE))
      {
	 /* change the flags to make our block "discardable" */
	 pintern=HANDLE_TO_INTERN(hmem);
	 pintern->Flags = pintern->Flags | (GMEM_DISCARDABLE >> 8);
	 hnew=hmem;
      }
      else
      {
	 SetLastError(ERROR_INVALID_PARAMETER);
	 hnew=NULL;
      }
   }
   else
   {
      if(ISPOINTER(hmem))
      {
	 /* reallocate fixed memory */
	 hnew=(HGLOBAL32)HeapReAlloc(GetProcessHeap(), 0, (LPVOID) hmem, size);
      }
      else
      {
	 /* reallocate a moveable block */
	 pintern=HANDLE_TO_INTERN(hmem);
	 if(pintern->LockCount!=0)
	    SetLastError(ERROR_INVALID_HANDLE);
	 else if(size!=0)
	 {
	    hnew=hmem;
	    if(pintern->Pointer)
	    {
	       palloc=HeapReAlloc(GetProcessHeap(), 0,
				  pintern->Pointer-sizeof(HGLOBAL32),
				  size+sizeof(HGLOBAL32) );
	       pintern->Pointer=palloc+sizeof(HGLOBAL32);
	    }
	    else
	    {
	       palloc=HeapAlloc(GetProcessHeap(), 0, size+sizeof(HGLOBAL32));
	       *(HGLOBAL32 *)palloc=hmem;
	       pintern->Pointer=palloc+sizeof(HGLOBAL32);
	    }
	 }
	 else
	 {
	    if(pintern->Pointer)
	    {
	       HeapFree(GetProcessHeap(), 0, pintern->Pointer-sizeof(HGLOBAL32));
	       pintern->Pointer=NULL;
	    }
	 }
      }
   }
   /* HeapUnlock(GetProcessHeap()); */
   return hnew;
}


/***********************************************************************
 *           GlobalFree32   (KERNEL32.322)
 */
HGLOBAL32 GlobalFree32(HGLOBAL32 hmem)
{
   PGLOBAL32_INTERN pintern;
   HGLOBAL32        hreturned=NULL;
   
   if(ISPOINTER(hmem)) /* POINTER */
   {
      if(!HeapFree(GetProcessHeap(), 0, (LPVOID) hmem))
         hmem=NULL;
   }
   else  /* HANDLE */
   {
      /* HeapLock(GetProcessHeap()); */      
      pintern=HANDLE_TO_INTERN(hmem);
      
      if(pintern->Magic==MAGIC_GLOBAL_USED)
      {	 
	 if(pintern->LockCount!=0)
	    SetLastError(ERROR_INVALID_HANDLE);
	 if(pintern->Pointer)
	    if(!HeapFree(GetProcessHeap(), 0, 
	                 (char *)(pintern->Pointer)-sizeof(HGLOBAL32)))
	       hreturned=hmem;
	 if(!HeapFree(GetProcessHeap(), 0, pintern)) 
	    hreturned=hmem;
      }      
      /* HeapUnlock(GetProcessHeap()); */
   }
   return hreturned;
}


/***********************************************************************
 *           GlobalSize32   (KERNEL32.329)
 */
DWORD  GlobalSize32(HGLOBAL32 hmem)
{
   DWORD                retval;
   PGLOBAL32_INTERN     pintern;

   if(ISPOINTER(hmem)) 
   {
      retval=HeapSize(GetProcessHeap(), 0,  (LPVOID) hmem);
   }
   else
   {
      /* HeapLock(GetProcessHeap()); */
      pintern=HANDLE_TO_INTERN(hmem);
      
      if(pintern->Magic==MAGIC_GLOBAL_USED)
      {
	 retval=HeapSize(GetProcessHeap(), 0, 
	                 (char *)(pintern->Pointer)-sizeof(HGLOBAL32))-4;
      }
      else
      {
	 dprintf_global(stddeb, "GlobalSize32: invalid handle\n");
	 retval=0;
      }
      /* HeapUnlock(GetProcessHeap()); */
   }
   return retval;
}


/***********************************************************************
 *           GlobalWire32   (KERNEL32.333)
 */
LPVOID  GlobalWire32(HGLOBAL32 hmem)
{
   return GlobalLock32( hmem );
}


/***********************************************************************
 *           GlobalUnWire32   (KERNEL32.330)
 */
BOOL32  GlobalUnWire32(HGLOBAL32 hmem)
{
   return GlobalUnlock32( hmem);
}


/***********************************************************************
 *           GlobalFix32   (KERNEL32.320)
 */
VOID  GlobalFix32(HGLOBAL32 hmem)
{
    GlobalLock32( hmem );
}


/***********************************************************************
 *           GlobalUnfix32   (KERNEL32.331)
 */
VOID  GlobalUnfix32(HGLOBAL32 hmem)
{
   GlobalUnlock32( hmem);
}


/***********************************************************************
 *           GlobalFlags32   (KERNEL32.321)
 */
UINT32  GlobalFlags32(HGLOBAL32 hmem)
{
   DWORD                retval;
   PGLOBAL32_INTERN     pintern;
   
   if(ISPOINTER(hmem))
   {
      retval=0;
   }
   else
   {
      /* HeapLock(GetProcessHeap()); */
      pintern=HANDLE_TO_INTERN(hmem);
      if(pintern->Magic==MAGIC_GLOBAL_USED)
      {               
	 retval=pintern->LockCount + (pintern->Flags<<8);
	 if(pintern->Pointer==0)
	    retval|= GMEM_DISCARDED;
      }
      else
      {
	 dprintf_global(stddeb,"GlobalFlags32: invalid handle\n");
	 retval=0;
      }
      /* HeapUnlock(GetProcessHeap()); */
   }
   return retval;
}


/***********************************************************************
 *           GlobalCompact32   (KERNEL32.316)
 */
DWORD GlobalCompact32( DWORD minfree )
{
    return 0;  /* GlobalCompact does nothing in Win32 */
}


/***********************************************************************
 *           GlobalMemoryStatus   (KERNEL32.327)
 */
VOID GlobalMemoryStatus( LPMEMORYSTATUS lpmem )
{
#ifdef linux
    FILE *f = fopen( "/proc/meminfo", "r" );
    if (f)
    {
        char buffer[256];
        int total, used, free;

        lpmem->dwTotalPhys = lpmem->dwAvailPhys = 0;
        lpmem->dwTotalPageFile = lpmem->dwAvailPageFile = 0;
        while (fgets( buffer, sizeof(buffer), f ))
        {
            if (sscanf( buffer, "Mem: %d %d %d", &total, &used, &free ))
            {
                lpmem->dwTotalPhys += total;
                lpmem->dwAvailPhys += free;
            }
            else if (sscanf( buffer, "Swap: %d %d %d", &total, &used, &free ))
            {
                lpmem->dwTotalPageFile += total;
                lpmem->dwAvailPageFile += free;
            }
        }
        fclose( f );

        if (lpmem->dwTotalPhys)
        {
            lpmem->dwTotalVirtual = lpmem->dwTotalPhys+lpmem->dwTotalPageFile;
            lpmem->dwAvailVirtual = lpmem->dwAvailPhys+lpmem->dwAvailPageFile;
            lpmem->dwMemoryLoad = (lpmem->dwTotalVirtual-lpmem->dwAvailVirtual)
                                      * 100 / lpmem->dwTotalVirtual;
            return;
        }
    }
#endif
    /* FIXME: should do something for other systems */
    lpmem->dwMemoryLoad    = 0;
    lpmem->dwTotalPhys     = 16*1024*1024;
    lpmem->dwAvailPhys     = 16*1024*1024;
    lpmem->dwTotalPageFile = 16*1024*1024;
    lpmem->dwAvailPageFile = 16*1024*1024;
    lpmem->dwTotalVirtual  = 32*1024*1024;
    lpmem->dwAvailVirtual  = 32*1024*1024;
}
