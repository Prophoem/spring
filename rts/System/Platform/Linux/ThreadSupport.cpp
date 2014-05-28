#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <boost/thread/tss.hpp>
#include "System/Log/ILog.h"
#include "System/Platform/Threading.h"
#include <iostream>

#include <semaphore.h>

namespace Threading {

void ThreadSIGUSR1Handler (int signum, siginfo_t* info, void* pCtx)
{ // Signal handler, so don't use LOG or anything that might write directly to disk. Just use perror().
	int err=0;

	LOG_SL("LinuxCrashHandler", L_DEBUG, "ThreadSIGUSR1Handler[1]");

	std::shared_ptr<Threading::ThreadControls> pThreadCtls = *threadCtls;

	// Fill in ucontext_t structure before locking, this allows stack walking...
	if (err = getcontext(&pThreadCtls->ucontext)) {
		LOG_L(L_ERROR, "Couldn't get thread context within suspend signal handler: %s", strerror(err));
		return;
	}

	// Change the "running" flag to false. Note that we don't own a lock on the suspend mutex, but in order to get here,
	//   it had to have been locked by some other thread.
	pThreadCtls->running = false;

	LOG_SL("LinuxCrashHandler", L_DEBUG, "ThreadSIGUSR1Handler[2]");

	// Wait on the semaphore. This should block the thread.
	{
		int semCnt = 0;
		sem_getvalue(&pThreadCtls->semSuspend, &semCnt);
		assert(semCnt == 0);

		//pThreadCtls->mutSuspend.lock();
		sem_wait(&pThreadCtls->semSuspend);
		//pThreadCtls->mutSuspend.unlock();

		// No need to unlock or post .. the resume function does this for us.
	}

	LOG_SL("LinuxCrashHandler", L_DEBUG, "ThreadSIGUSR1Handler[3]");

	pThreadCtls->running = true;
}



void SetCurrentThreadControls(std::shared_ptr<ThreadControls> * ppThreadCtls)
{
	assert(ppThreadCtls != nullptr);
	auto pThreadCtls = ppThreadCtls->get();
	assert(pThreadCtls != nullptr);

	if (threadCtls.get() != nullptr) {
		LOG_L(L_WARNING, "Setting a ThreadControls object on a thread that already has such an object registered.");
		auto oldPtr = threadCtls.get();
		threadCtls.reset();
		delete oldPtr;
	} else {
		// Installing new ThreadControls object, so install signal handler also
		int err = 0;
		sigset_t sigSet;
		sigemptyset(&sigSet);
		sigaddset(&sigSet, SIGUSR1);

		if (err = pthread_sigmask(SIG_UNBLOCK, &sigSet, NULL)) {
			LOG_L(L_FATAL, "Error while setting new pthread's signal mask: %s", strerror(err));
			return;
		}

		struct sigaction sa;
		memset(&sa, 0, sizeof(struct sigaction));
		sa.sa_sigaction = ThreadSIGUSR1Handler;
		sa.sa_flags |= SA_SIGINFO;
		if (sigaction(SIGUSR1, &sa, NULL)) {
			LOG_L(L_FATAL,"Error while installing pthread SIGUSR1 handler.");
			return;
		}
	}

	pThreadCtls->handle = GetCurrentThread();
	pThreadCtls->running = true;

	threadCtls.reset(ppThreadCtls);
}

/**
 * @brief ThreadStart Entry point for wrapped pthread. Allows us to register signal handlers specific to that thread, enabling suspend/resume functionality.
 * @param ptr Points to a platform-specific ThreadControls object.
 */
void ThreadStart (boost::function<void()> taskFunc, std::shared_ptr<ThreadControls> * ppThreadCtls)
{
	assert(ppThreadCtls != nullptr);
	auto pThreadCtls = ppThreadCtls->get();
	assert(pThreadCtls != nullptr);

	// Lock the thread object so that users can't suspend/resume yet.
	{
		// Install the SIGUSR1 handler:
		SetCurrentThreadControls(ppThreadCtls);

		//pThreadCtls->mutSuspend.lock();
		sem_post(&pThreadCtls->semSuspend);
		pThreadCtls->running = true;

		LOG_I(LOG_LEVEL_DEBUG, "ThreadStart(): New thread's handle is %.4x", pThreadCtls->handle);

		// We are fully initialized, so notify the condition variable. The thread's parent will unblock in whatever function created this thread.
		//pThreadCtls->condInitialized.notify_all();

		// Ok, the thread should be ready to run its task now, so unlock the suspend mutex!
		//pThreadCtls->mutSuspend.unlock();

		// Run the task function...
		taskFunc();
	}

	// Finish up: change the thread's running state to false.
	//pThreadCtls->mutSuspend.lock();
	pThreadCtls->running = false;
	//pThreadCtls->mutSuspend.unlock();

}


SuspendResult ThreadControls::Suspend ()
{
	int err=0;
	int semCnt = 0;

	err = sem_getvalue(&semSuspend, &semCnt);
	if (err) {
		LOG_L(L_ERROR, "Could not get suspend/resume semaphore value, error code = %d", err);
		return Threading::THREADERR_MISC;
	}
	assert(semCnt == 1);

	// Return an error if the running flag is false.
	if (!running) {
		LOG_L(L_ERROR, "Cannot suspend if a thread's running flag is set to false. Refusing to suspend using pthread_kill.");
		return Threading::THREADERR_NOT_RUNNING;
	}

	//mutSuspend.lock();
	err = sem_wait(&semSuspend);
	if (err) {
		LOG_L(L_ERROR, "Error while trying to decrement the suspend/resume semaphore");
		return Threading::THREADERR_MISC;
	}

	LOG_SI("LinuxCrashHandler", LOG_LEVEL_DEBUG, "Sending SIGUSR1 to 0x%x", handle);

	// Send signal to thread to trigger its handler
	if (err = pthread_kill(handle, SIGUSR1)) {
		LOG_L(L_ERROR, "Error while trying to send signal to suspend thread: %s", strerror(err));
		return Threading::THREADERR_MISC;
	}

	// TODO: Before leaving this function, we need some kind of guarantee that the thread has stopped,
	//       otherwise the signal may not be delivered to it before we return and keep working in this thread.
	// spinwait for the semaphore count to decrement one more time
	do {
		err = sem_getvalue(&semSuspend, &semCnt);
		if (err) {
			LOG_L(L_ERROR, "Error while waiting for remote thread to suspend on semaphore. Err code = %d", err);
			return Threading::THREADERR_MISC;
		}
	} while (semCnt != -1);


	return Threading::THREADERR_NONE;
}

SuspendResult ThreadControls::Resume ()
{
	//mutSuspend.unlock();
	int suspendResumeSemaphoreCount = 0;
	sem_getvalue(&semSuspend, &suspendResumeSemaphoreCount);
	assert(suspendResumeSemaphoreCount == -1);

	// twice: once for us and once for the signal handler that was suspended.
	sem_post(&semSuspend);
	sem_post(&semSuspend);

	return Threading::THREADERR_NONE;
}

}
