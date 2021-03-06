/* Copyright (c) 2001, Stanford University
 * All rights reserved
 *
 * See the file LICENSE.txt for information on redistributing this software.
 */

#include "packspu.h"
#include "cr_mem.h"
#include "cr_packfunctions.h"
#include "cr_string.h"
#include "packspu_proto.h"

#define MAGIC_OFFSET 3000

/*
 * Allocate a new ThreadInfo structure, setup a connection to the
 * server, allocate/init a packer context, bind this ThreadInfo to
 * the calling thread with crSetTSD().
 * We'll always call this function at least once even if we're not
 * using threads.
 */
ThreadInfo *packspuNewThread(
#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
                struct VBOXUHGSMI *pHgsmi
#endif
)
{
    ThreadInfo *thread=NULL;
    int i;

#ifdef CHROMIUM_THREADSAFE
    crLockMutex(&_PackMutex);
#else
    CRASSERT(pack_spu.numThreads == 0);
#endif

#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
    CRASSERT(!CRPACKSPU_IS_WDDM_CRHGSMI() == !pHgsmi);
#endif

    CRASSERT(pack_spu.numThreads < MAX_THREADS);
    for (i=0; i<MAX_THREADS; ++i)
    {
        if (!pack_spu.thread[i].inUse)
        {
            thread = &pack_spu.thread[i];
            break;
        }
    }
    CRASSERT(thread);

    thread->inUse = GL_TRUE;
    if (!CRPACKSPU_IS_WDDM_CRHGSMI())
        thread->id = crThreadID();
    else
        thread->id = THREAD_OFFSET_MAGIC + i;
    thread->currentContext = NULL;
    thread->bInjectThread = GL_FALSE;

    /* connect to the server */
    thread->netServer.name = crStrdup( pack_spu.name );
    thread->netServer.buffer_size = pack_spu.buffer_size;
    packspuConnectToServer( &(thread->netServer)
#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
            , pHgsmi
#endif
            );
    CRASSERT(thread->netServer.conn);
    /* packer setup */
    CRASSERT(thread->packer == NULL);
    thread->packer = crPackNewContext( pack_spu.swap );
    CRASSERT(thread->packer);
    crPackInitBuffer( &(thread->buffer), crNetAlloc(thread->netServer.conn),
                thread->netServer.conn->buffer_size, thread->netServer.conn->mtu );
    thread->buffer.canBarf = thread->netServer.conn->Barf ? GL_TRUE : GL_FALSE;
    crPackSetBuffer( thread->packer, &thread->buffer );
    crPackFlushFunc( thread->packer, packspuFlush );
    crPackFlushArg( thread->packer, (void *) thread );
    crPackSendHugeFunc( thread->packer, packspuHuge );

    if (!CRPACKSPU_IS_WDDM_CRHGSMI())
    {
        crPackSetContext( thread->packer );
    }


#ifdef CHROMIUM_THREADSAFE
    if (!CRPACKSPU_IS_WDDM_CRHGSMI())
    {
        crSetTSD(&_PackTSD, thread);
    }
#endif

    pack_spu.numThreads++;

#ifdef CHROMIUM_THREADSAFE
    crUnlockMutex(&_PackMutex);
#endif
    return thread;
}

GLint PACKSPU_APIENTRY
packspu_VBoxConCreate(struct VBOXUHGSMI *pHgsmi)
{
#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
    ThreadInfo * thread;
    CRASSERT(CRPACKSPU_IS_WDDM_CRHGSMI());
    CRASSERT(pHgsmi);

    thread = packspuNewThread(pHgsmi);

    if (thread)
    {
        CRASSERT(thread->id);
        CRASSERT(thread->id - THREAD_OFFSET_MAGIC < RT_ELEMENTS(pack_spu.thread)
                && GET_THREAD_VAL_ID(thread->id) == thread);
        return thread->id;
    }
    crError("packspuNewThread failed");
#endif
    return 0;
}

void PACKSPU_APIENTRY
packspu_VBoxConFlush(GLint con)
{
#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
    GET_THREAD_ID(thread, con);
    CRASSERT(con);
    CRASSERT(CRPACKSPU_IS_WDDM_CRHGSMI());
    CRASSERT(thread->packer);
    packspuFlush((void *) thread);
#else
    crError("VBoxConFlush not implemented!");
#endif
}

void PACKSPU_APIENTRY
packspu_VBoxConDestroy(GLint con)
{
#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
    GET_THREAD_ID(thread, con);
    CRASSERT(con);
    CRASSERT(CRPACKSPU_IS_WDDM_CRHGSMI());
    CRASSERT(pack_spu.numThreads>0);
    CRASSERT(thread->packer);
    packspuFlush((void *) thread);

    crLockMutex(&_PackMutex);

    crPackDeleteContext(thread->packer);

    if (thread->buffer.pack)
    {
        crNetFree(thread->netServer.conn, thread->buffer.pack);
        thread->buffer.pack = NULL;
    }

    crNetFreeConnection(thread->netServer.conn);

    if (thread->netServer.name)
        crFree(thread->netServer.name);

    pack_spu.numThreads--;
    /*note can't shift the array here, because other threads have TLS references to array elements*/
    crMemZero(thread, sizeof(ThreadInfo));

#if 0
    if (&pack_spu.thread[pack_spu.idxThreadInUse]==thread)
    {
        int i;
        crError("Should not be here since idxThreadInUse should be always 0 for the dummy connection created in packSPUInit!");
        for (i=0; i<MAX_THREADS; ++i)
        {
            if (pack_spu.thread[i].inUse)
            {
                pack_spu.idxThreadInUse=i;
                break;
            }
        }
    }
#endif
    crUnlockMutex(&_PackMutex);
#endif
}

GLvoid PACKSPU_APIENTRY
packspu_VBoxConChromiumParameteriCR(GLint con, GLenum param, GLint value)
{
    GET_THREAD(thread);
    CRPackContext * curPacker = crPackGetContext();
    ThreadInfo *curThread = thread;
    int writeback = 1;
    GLint serverCtx = (GLint) -1;

    CRASSERT(!curThread == !curPacker);
    CRASSERT(!curThread || !curPacker || curThread->packer == curPacker);
#ifdef CHROMIUM_THREADSAFE
    crLockMutex(&_PackMutex);
#endif

#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
    CRASSERT(!con == !CRPACKSPU_IS_WDDM_CRHGSMI());
#endif

    if (CRPACKSPU_IS_WDDM_CRHGSMI())
    {
        if (!con)
        {
            crError("connection should be specified!");
            return;
        }
        thread = GET_THREAD_VAL_ID(con);
    }
    else
    {
        CRASSERT(!con);
        if (!thread)
        {
            thread = packspuNewThread(
#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
                NULL
#endif
                );
        }
    }
    CRASSERT(thread);
    CRASSERT(thread->packer);

    crPackSetContext( thread->packer );

    packspu_ChromiumParameteriCR(param, value);

#ifdef CHROMIUM_THREADSAFE
    crUnlockMutex(&_PackMutex);
#endif

    if (CRPACKSPU_IS_WDDM_CRHGSMI())
    {
        /* restore the packer context to the tls */
        crPackSetContext(curPacker);
    }
}

GLint PACKSPU_APIENTRY
packspu_VBoxCreateContext( GLint con, const char *dpyName, GLint visual, GLint shareCtx )
{
    GET_THREAD(thread);
    CRPackContext * curPacker = crPackGetContext();
    ThreadInfo *curThread = thread;
    int writeback = 1;
    GLint serverCtx = (GLint) -1;
    int slot;

    CRASSERT(!curThread == !curPacker);
    CRASSERT(!curThread || !curPacker || curThread->packer == curPacker);
#ifdef CHROMIUM_THREADSAFE
    crLockMutex(&_PackMutex);
#endif

#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
    CRASSERT(!con == !CRPACKSPU_IS_WDDM_CRHGSMI());
#endif

    if (CRPACKSPU_IS_WDDM_CRHGSMI())
    {
        if (!con)
        {
            crError("connection should be specified!");
            return -1;
        }
        thread = GET_THREAD_VAL_ID(con);
    }
    else
    {
        CRASSERT(!con);
        if (!thread)
        {
            thread = packspuNewThread(
#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
                NULL
#endif
                );
        }
    }
    CRASSERT(thread);
    CRASSERT(thread->packer);

    if (shareCtx > 0) {
        /* translate to server ctx id */
        shareCtx -= MAGIC_OFFSET;
        if (shareCtx >= 0 && shareCtx < pack_spu.numContexts) {
            shareCtx = pack_spu.context[shareCtx].serverCtx;
        }
    }

    crPackSetContext( thread->packer );

    /* Pack the command */
    if (pack_spu.swap)
        crPackCreateContextSWAP( dpyName, visual, shareCtx, &serverCtx, &writeback );
    else
        crPackCreateContext( dpyName, visual, shareCtx, &serverCtx, &writeback );

    /* Flush buffer and get return value */
    packspuFlush(thread);
    if (!(thread->netServer.conn->actual_network))
    {
        /* HUMUNGOUS HACK TO MATCH SERVER NUMBERING
         *
         * The hack exists solely to make file networking work for now.  This
         * is totally gross, but since the server expects the numbers to start
         * from 5000, we need to write them out this way.  This would be
         * marginally less gross if the numbers (500 and 5000) were maybe
         * some sort of #define'd constants somewhere so the client and the
         * server could be aware of how each other were numbering things in
         * cases like file networking where they actually
         * care. 
         *
         *  -Humper 
         *
         */
        serverCtx = 5000;
    }
    else {
        CRPACKSPU_WRITEBACK_WAIT(thread, writeback);

        if (pack_spu.swap) {
            serverCtx = (GLint) SWAP32(serverCtx);
        }
        if (serverCtx < 0) {
#ifdef CHROMIUM_THREADSAFE
            crUnlockMutex(&_PackMutex);
#endif
            crWarning("Failure in packspu_CreateContext");

            if (CRPACKSPU_IS_WDDM_CRHGSMI())
            {
                /* restore the packer context to the tls */
                crPackSetContext(curPacker);
            }
            return -1;  /* failed */
        }
    }

    /* find an empty context slot */
    for (slot = 0; slot < pack_spu.numContexts; slot++) {
        if (!pack_spu.context[slot].clientState) {
            /* found empty slot */
            break;
        }
    }
    if (slot == pack_spu.numContexts) {
        pack_spu.numContexts++;
    }

    if (CRPACKSPU_IS_WDDM_CRHGSMI())
    {
        thread->currentContext = &pack_spu.context[slot];
        pack_spu.context[slot].currentThread = thread;
    }

    /* Fill in the new context info */
    /* XXX fix-up sharedCtx param here */
    pack_spu.context[slot].clientState = crStateCreateContext(NULL, visual, NULL);
    pack_spu.context[slot].clientState->bufferobject.retainBufferData = GL_TRUE;
    pack_spu.context[slot].serverCtx = serverCtx;

#ifdef CHROMIUM_THREADSAFE
    crUnlockMutex(&_PackMutex);
#endif

    if (CRPACKSPU_IS_WDDM_CRHGSMI())
    {
        /* restore the packer context to the tls */
        crPackSetContext(curPacker);
    }

    return MAGIC_OFFSET + slot;
}

GLint PACKSPU_APIENTRY
packspu_CreateContext( const char *dpyName, GLint visual, GLint shareCtx )
{
    return packspu_VBoxCreateContext( 0, dpyName, visual, shareCtx );
}


void PACKSPU_APIENTRY packspu_DestroyContext( GLint ctx )
{
    GET_THREAD(thread);
    ThreadInfo *curThread = thread;
    const int slot = ctx - MAGIC_OFFSET;
    ContextInfo *context, *curContext;
    CRPackContext * curPacker = crPackGetContext();

    CRASSERT(slot >= 0);
    CRASSERT(slot < pack_spu.numContexts);

    context = &(pack_spu.context[slot]);

    if (CRPACKSPU_IS_WDDM_CRHGSMI())
    {
        thread = context->currentThread;
        crPackSetContext(thread->packer);
        CRASSERT(!(thread->packer == curPacker) == !(thread == curThread));
    }
    CRASSERT(thread);
    curContext = curThread ? curThread->currentContext : NULL;

    if (pack_spu.swap)
        crPackDestroyContextSWAP( context->serverCtx );
    else
        crPackDestroyContext( context->serverCtx );

    crStateDestroyContext( context->clientState );

    context->clientState = NULL;
    context->serverCtx = 0;
    context->currentThread = NULL;

    if (curContext == context)
    {
        if (!CRPACKSPU_IS_WDDM_CRHGSMI())
        {
            curThread->currentContext = NULL;
        }
        else
        {
            CRASSERT(thread == curThread);
            crSetTSD(&_PackTSD, NULL);
            crPackSetContext(NULL);
        }
        crStateMakeCurrent( NULL );
    }
    else
    {
        if (CRPACKSPU_IS_WDDM_CRHGSMI())
        {
            crPackSetContext(curPacker);
        }
    }
}

void PACKSPU_APIENTRY packspu_MakeCurrent( GLint window, GLint nativeWindow, GLint ctx )
{
    ThreadInfo *thread;
    GLint serverCtx;
    ContextInfo *newCtx;

    if (!CRPACKSPU_IS_WDDM_CRHGSMI())
    {
        thread = GET_THREAD_VAL();
        if (!thread) {
            thread = packspuNewThread(
#if defined(VBOX_WITH_CRHGSMI) && defined(IN_GUEST)
                    NULL
#endif
                    );
        }
        CRASSERT(thread);
        CRASSERT(thread->packer);
    }

    if (ctx) {
        const int slot = ctx - MAGIC_OFFSET;

        CRASSERT(slot >= 0);
        CRASSERT(slot < pack_spu.numContexts);

        newCtx = &pack_spu.context[slot];
        CRASSERT(newCtx->clientState);  /* verify valid */

        if (CRPACKSPU_IS_WDDM_CRHGSMI())
        {
            thread = newCtx->currentThread;
            CRASSERT(thread);
            crSetTSD(&_PackTSD, thread);
            crPackSetContext( thread->packer );
        }
        else
        {
            if (newCtx->fAutoFlush)
            {
                if (newCtx->currentThread && newCtx->currentThread != thread)
                {
                    crLockMutex(&_PackMutex);
                    /* do a flush for the previously assigned thread
                     * to ensure all commands issued there are submitted */
                    if (newCtx->currentThread
                        && newCtx->currentThread->inUse
                        && newCtx->currentThread->netServer.conn
                        && newCtx->currentThread->packer && newCtx->currentThread->packer->currentBuffer)
                    {
                        packspuFlush((void *) newCtx->currentThread);
                    }
                    crUnlockMutex(&_PackMutex);
                }
                newCtx->currentThread = thread;
            }

            thread->currentContext = newCtx;
            crPackSetContext( thread->packer );
        }

        crStateMakeCurrent( newCtx->clientState );
        //crStateSetCurrentPointers(newCtx->clientState, &thread->packer->current);
        serverCtx = pack_spu.context[slot].serverCtx;
    }
    else {
        crStateMakeCurrent( NULL );
        if (CRPACKSPU_IS_WDDM_CRHGSMI())
        {
            thread = GET_THREAD_VAL();
            if (!thread)
            {
                CRASSERT(crPackGetContext() == NULL);
                return;
            }
            CRASSERT(thread->currentContext);
            CRASSERT(thread->packer == crPackGetContext());
        }
        else
        {
            thread->currentContext = NULL;
        }
        newCtx = NULL;
        serverCtx = 0;
    }

    if (pack_spu.swap)
        crPackMakeCurrentSWAP( window, nativeWindow, serverCtx );
    else
        crPackMakeCurrent( window, nativeWindow, serverCtx );

    {
        GET_THREAD(t);
        (void) t;
        CRASSERT(t);
    }
}
