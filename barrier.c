/*
 * barrier.c
 *
 * Description:
 * This translation unit implements spin locks primitives.
 *
 * Pthreads-win32 - POSIX Threads Library for Win32
 * Copyright (C) 1998
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA
 */

#include "pthread.h"
#include "implement.h"


static int
ptw32_barrier_check_need_init(pthread_barrier_t *barrier)
{
  int result = 0;

  /*
   * The following guarded test is specifically for statically
   * initialised barriers (via PTHREAD_BARRIER_INITIALIZER).
   *
   * Note that by not providing this synchronisation we risk
   * introducing race conditions into applications which are
   * correctly written.
   *
   * Approach
   * --------
   * We know that static barriers will not be PROCESS_SHARED
   * so we can serialise access to internal state using
   * Win32 Critical Sections rather than Win32 Mutexes.
   *
   * If using a single global lock slows applications down too much,
   * multiple global locks could be created and hashed on some random
   * value associated with each barrier, the pointer perhaps. At a guess,
   * a good value for the optimal number of global locks might be
   * the number of processors + 1.
   *
   */
  EnterCriticalSection(&ptw32_barrier_test_init_lock);

  /*
   * We got here possibly under race
   * conditions. Check again inside the critical section
   * and only initialise if the barrier is valid (not been destroyed).
   * If a static barrier has been destroyed, the application can
   * re-initialise it only by calling pthread_barrier_init()
   * explicitly.
   */
  if (*barrier == (pthread_barrier_t) PTW32_OBJECT_AUTO_INIT)
    {
      result = pthread_barrier_init(barrier, NULL);
    }
  else if (*barrier == NULL)
    {
      /*
       * The barrier has been destroyed while we were waiting to
       * initialise it, so the operation that caused the
       * auto-initialisation should fail.
       */
      result = EINVAL;
    }

  LeaveCriticalSection(&ptw32_barrier_test_init_lock);

  return(result);
}


int
pthread_barrier_init(pthread_barrier_t * barrier,
                     const pthread_barrierattr_t * attr,
                     int count)
{
  int result = 0;
  int pshared = PTHREAD_PROCESS_PRIVATE;
  pthread_barrier_t * b;

  if (barrier == NULL)
    {
      return EINVAL;
    }

  b = (pthread_barrier_t *) calloc(1, sizeof(*b));

  if (b == NULL)
    {
      result = ENOMEM;
      goto FAIL0;
    }

  if (attr != NULL && *attr != NULL)
    {
      pshared = (*attr)->pshared;
    }

  b->nCurrentBarrierHeight = b->nInitialBarrierHeight = count;

  result = pthread_mutex_init(&b->mtxExclusiveAccess, NULL);
  if (0 != result)
    {
      goto FAIL0;
    }

  result = sem_init(&(b->semBarrierBreeched), pshared, 0);
  if (0 != result)
    {
      goto FAIL1;
    }

  goto DONE;

 FAIL1:
  (void) pthread_mutex_destroy(&b->mtxExclusiveAccess);

 FAIL0:
  (void) free(b);

 DONE:
  return(result);
}

int
pthread_barrier_destroy(pthread_barrier_t *barrier)
{
  int result = 0;
  pthread_barrier_t b;

  if (barrier == NULL || *barrier == (pthread_barrier_t) PTW32_OBJECT_INVALID)
    {
      return EINVAL;
    }

  if (*barrier != (pthread_barrier_t) PTW32_OBJECT_AUTO_INIT)
    {
      b = *barrier;
  
      if (0 == pthread_mutex_trylock(&b->mtxExclusiveAccess))
        {
          /*
           * FIXME!!!
           * The mutex isn't held by another thread but we could still
           * be too late invalidating the barrier below since another thread
           * may alredy have entered barrier_wait and the check for a valid
           * *barrier != NULL.
           */
          *barrier = NULL;

          (void) sem_destroy(&(b->semBarrierBreeched));
          (void) pthread_mutex_unlock(&(b->mtxExclusiveAccess));
          (void) pthread_mutex_destroy(&(b->mtxExclusiveAccess));
          (void) free(b);
        }
    }
  else
    {
      /*
       * See notes in ptw32_barrier_check_need_init() above also.
       */
      EnterCriticalSection(&ptw32_barrier_test_init_lock);

      /*
       * Check again.
       */
      if (*barrier == (pthread_barrier_t) PTW32_OBJECT_AUTO_INIT)
        {
          /*
           * This is all we need to do to destroy a statically
           * initialised barrier that has not yet been used (initialised).
           * If we get to here, another thread
           * waiting to initialise this barrier will get an EINVAL.
           */
          *barrier = NULL;
        }
      else
        {
          /*
           * The barrier has been initialised while we were waiting
           * so assume it's in use.
           */
          result = EBUSY;
        }

      LeaveCriticalSection(&ptw32_barrier_test_init_lock);
    }

  return(result);
}


int
pthread_barrier_wait(pthread_barrier_t *barrier)
{
  int result;
  pthread_barrier_t b;

  if (barrier == NULL || *barrier == (pthread_barrier_t) PTW32_OBJECT_INVALID)
    {
      return EINVAL;
    }

  if (*barrier == (pthread_barrier_t) PTHREAD_OBJECT_AUTO_INIT)
    {
      if ((result = ptw32_barrier_check_need_init(barrier)) != 0)
        {
          return(result);
        }
    }

  b = *barrier;

  result = pthread_mutex_lock(b->mtxExclusiveAccess);
  if (0 == result)
    {
      if (0 == --(b->nCurrentBarrierHeight))
        {
          b->nCurrentBarrierHeight = b->nInitialBarrierHeight;
          (void) pthread_mutex_unlock(b->mtxExclusiveAccess);
          (void) sem_post_multiple(&(b->semBarrierBreeched),
                                   b->InitialBarrierHeight);
          /*
           * Would be better if the first thread to return
           * from this routine got this value. On a single
           * processor machine that will be the last thread
           * to reach the barrier (us), most of the time.
           */
          result = PTHREAD_BARRIER_SERIAL_THREAD;
        }
      else
        {
          /*
           * pthread_barrier_wait() is not a cancelation point
           * so temporarily prevent sem_wait() from being one.
           */
          pthread_t self = pthread_self();
          int cancelType = pthread_getcanceltype(self);
          int oldCancelState;

          if (cancelType == PTHREAD_CANCEL_DEFERRED)
            {
              oldCancelState = pthread_setcancelstate(self,
                                                      PTHREAD_CANCEL_DISABLED);
            }

          /* Could still be PTHREAD_CANCEL_ASYNCHRONOUS. */
          pthread_cleanup_push(pthread_mutex_unlock,
                               (void *) &(b->mtxExclusiveAccess));

          if (0 != sem_wait(&(b->semBarrierBreeched)))
            {
              result = errno;
            }

          if (cancelType == PTHREAD_CANCEL_DEFERRED)
            {
              pthread_setcancelstate(self, oldCancelState);
            }

          pthread_cleanup_pop(1);
        }
    }

  return(result);
}


int
pthread_barrierattr_init (pthread_barrierattr_t * attr)
     /*
      * ------------------------------------------------------
      * DOCPUBLIC
      *      Initializes a barrier attributes object with default
      *      attributes.
      *
      * PARAMETERS
      *      attr
      *              pointer to an instance of pthread_barrierattr_t
      *
      *
      * DESCRIPTION
      *      Initializes a barrier attributes object with default
      *      attributes.
      *
      *      NOTES:
      *              1)      Used to define barrier types
      *
      * RESULTS
      *              0               successfully initialized attr,
      *              ENOMEM          insufficient memory for attr.
      *
      * ------------------------------------------------------
      */
{
  pthread_barrierattr_t ba;
  int result = 0;
 
  ba = (pthread_barrierattr_t) calloc (1, sizeof (*ba));
 
  if (ba == NULL)
    {
      result = ENOMEM;
    }
 
  ba->pshared = PTHREAD_PROCESS_PRIVATE;
 
  *attr = ba;
 
  return (result);
 
}                               /* pthread_barrierattr_init */
 
 
int
pthread_barrierattr_destroy (pthread_barrierattr_t * attr)
     /*
      * ------------------------------------------------------
      * DOCPUBLIC
      *      Destroys a barrier attributes object. The object can
      *      no longer be used.
      *
      * PARAMETERS
      *      attr
      *              pointer to an instance of pthread_barrierattr_t
      *
      *
      * DESCRIPTION
      *      Destroys a barrier attributes object. The object can
      *      no longer be used.
      *
      *      NOTES:
      *              1)      Does not affect barrieres created using 'attr'
      *
      * RESULTS
      *              0               successfully released attr,
      *              EINVAL          'attr' is invalid.
      *
      * ------------------------------------------------------
      */
{
  int result = 0;
 
  if (attr == NULL || *attr == NULL)
    {
      result = EINVAL;
    }
  else
    {
      pthread_barrierattr_t ba = *attr;
 
      *attr = NULL;
      free (ba);
 
      result = 0;
    }
 
  return (result);
 
}                               /* pthread_barrierattr_destroy */


int
pthread_barrierattr_getpshared (const pthread_barrierattr_t * attr,
                                int *pshared)
     /*
      * ------------------------------------------------------
      * DOCPUBLIC
      *      Determine whether barriers created with 'attr' can be
      *      shared between processes.
      *
      * PARAMETERS
      *      attr
      *              pointer to an instance of pthread_barrierattr_t
      *
      *      pshared
      *              will be set to one of:
      *
      *                      PTHREAD_PROCESS_SHARED
      *                              May be shared if in shared memory
      *
      *                      PTHREAD_PROCESS_PRIVATE
      *                              Cannot be shared.
      *
      *
      * DESCRIPTION
      *      Mutexes creatd with 'attr' can be shared between
      *      processes if pthread_barrier_t variable is allocated
      *      in memory shared by these processes.
      *      NOTES:
      *              1)      pshared barriers MUST be allocated in shared
      *                      memory.
      *              2)      The following macro is defined if shared barriers
      *                      are supported:
      *                              _POSIX_THREAD_PROCESS_SHARED
      *
      * RESULTS
      *              0               successfully retrieved attribute,
      *              EINVAL          'attr' is invalid,
      *
      * ------------------------------------------------------
      */
{
  int result;
 
  if ((attr != NULL && *attr != NULL) &&
      (pshared != NULL))
    {
      *pshared = (*attr)->pshared;
      result = 0;
    }
  else
    {
      *pshared = PTHREAD_PROCESS_PRIVATE;
      result = EINVAL;
    }
 
  return (result);
 
}                               /* pthread_barrierattr_getpshared */


int
pthread_barrierattr_setpshared (pthread_barrierattr_t * attr,
                                int pshared)
     /*
      * ------------------------------------------------------
      * DOCPUBLIC
      *      Barriers created with 'attr' can be shared between
      *      processes if pthread_barrier_t variable is allocated
      *      in memory shared by these processes.
      *
      * PARAMETERS
      *      attr
      *              pointer to an instance of pthread_barrierattr_t
      *
      *      pshared
      *              must be one of:
      *
      *                      PTHREAD_PROCESS_SHARED
      *                              May be shared if in shared memory
      *
      *                      PTHREAD_PROCESS_PRIVATE
      *                              Cannot be shared.
      *
      * DESCRIPTION
      *      Mutexes creatd with 'attr' can be shared between
      *      processes if pthread_barrier_t variable is allocated
      *      in memory shared by these processes.
      *
      *      NOTES:
      *              1)      pshared barriers MUST be allocated in shared
      *                      memory.
      *
      *              2)      The following macro is defined if shared barriers
      *                      are supported:
      *                              _POSIX_THREAD_PROCESS_SHARED
      *
      * RESULTS
      *              0               successfully set attribute,
      *              EINVAL          'attr' or pshared is invalid,
      *              ENOSYS          PTHREAD_PROCESS_SHARED not supported,
      *
      * ------------------------------------------------------
      */
{
  int result;
 
  if ((attr != NULL && *attr != NULL) &&
      ((pshared == PTHREAD_PROCESS_SHARED) ||
       (pshared == PTHREAD_PROCESS_PRIVATE)))
    {
      if (pshared == PTHREAD_PROCESS_SHARED)
        {
 
#if !defined( _POSIX_THREAD_PROCESS_SHARED )
 
          result = ENOSYS;
          pshared = PTHREAD_PROCESS_PRIVATE;
 
#else
 
          result = 0;
 
#endif /* _POSIX_THREAD_PROCESS_SHARED */
 
        }
      else
        {
          result = 0;
        }
 
      (*attr)->pshared = pshared;
    }
  else
    {
      result = EINVAL;
    }
 
  return (result);
 
}                               /* pthread_barrierattr_setpshared */
