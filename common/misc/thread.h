#ifndef THREAD_H
#define THREAD_H

class Runnable
{
public:
   virtual ~Runnable() { }
   virtual void run() = 0;
   static void threadFunc(void *vpRunnable)
   {
      Runnable *runnable = (Runnable*)vpRunnable;
      runnable->run();
   }
};

class Thread
{
public:
   typedef void (*ThreadFunc)(void*);

   static Thread *create(ThreadFunc func, void *param);
   static Thread *create(Runnable *runnable)
   {
      return create(Runnable::threadFunc, runnable);
   }

   virtual ~Thread() { };

   virtual void run() = 0;
};

#endif // THREAD_H
