/*******************************************************************************
 * Copyright (c) 2020, 2021 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "control/JitDump.hpp"

#include "codegen/CodeGenerator.hpp"
#include "env/VMJ9.h"
#include "control/MethodToBeCompiled.hpp"
#include "control/CompilationRuntime.hpp"
#include "control/CompilationThread.hpp"
#include "env/ut_j9jit.h"
#include "env/VMAccessCriticalSection.hpp"
#include "ilgen/J9ByteCodeIlGenerator.hpp"
#include "nls/j9dmpnls.h"
#if defined(J9VM_OPT_JITSERVER)
#include "control/JITServerHelpers.hpp"
#include "runtime/JITServerIProfiler.hpp"
#include "runtime/JITServerStatisticsThread.hpp"
#include "runtime/Listener.hpp"
#endif

struct ILOfCrashedThreadParamenters
   {
   ILOfCrashedThreadParamenters(J9VMThread *vmThread, TR::Compilation *comp, TR::FILE *jitdumpFile)
   : vmThread(vmThread), comp(comp), jitdumpFile(jitdumpFile)
      {}

   /// The JVM thread backing the crashed thread
   J9VMThread *vmThread;

   /// The compilation object extracted from the crashed thread
   TR::Compilation *comp;

   /// The jitdump file to trace the IL of the crashed thread to
   TR::FILE *jitdumpFile;
   };

static uintptr_t
traceILOfCrashedThreadProtected(struct J9PortLibrary *portLib, void *handler_arg)
   {
   auto p = *static_cast<ILOfCrashedThreadParamenters*>(handler_arg);

   TR_J9ByteCodeIlGenerator bci(p.comp->ilGenRequest().details(), p.comp->getMethodSymbol(),
      TR_J9VMBase::get(p.vmThread->javaVM->jitConfig, p.vmThread), p.comp, p.comp->getSymRefTab());
   bci.printByteCodes();

   // This call will reset the previously recorded symbol reference size to 0, thus indicating to the debug object that
   // we should print all the symbol references in the symbol reference table when tracing the trees. By default the
   // debug object will only print new symbol references since the last time they were printed. Here we are in a
   // crashed thread state so we can safely reset this coutner so we print all the symbol references.
   p.comp->setPrevSymRefTabSize(0);
   p.comp->dumpMethodTrees("Trees");

   if ((p.vmThread->omrVMThread->vmState & J9VMSTATE_JIT_CODEGEN) == J9VMSTATE_JIT_CODEGEN)
      {
      TR_Debug *debug = p.comp->getDebug();
      debug->dumpMethodInstrs(p.jitdumpFile, "Post Binary Instructions", false, true);
      debug->print(p.jitdumpFile, p.comp->cg()->getSnippetList());
      debug->dumpMixedModeDisassembly();
      }
   else if ((p.vmThread->omrVMThread->vmState & J9VMSTATE_JIT_OPTIMIZER) == J9VMSTATE_JIT_OPTIMIZER)
      {
      // Tree verification is only valid during optimizations as it relies on consistent node counts which are only
      // valid before codegen, since the codegen will decrement the node counts as part of instruction selection
      p.comp->verifyTrees();
      p.comp->verifyBlocks();
      }

   return 0;
   }

static void
traceILOfCrashedThread(J9VMThread *vmThread, TR::Compilation *comp, TR::FILE *jitdumpFile)
   {
   PORT_ACCESS_FROM_VMC(vmThread);
   j9nls_printf(PORTLIB, J9NLS_INFO | J9NLS_STDERR, J9NLS_DMP_JIT_TRACE_IL_CRASHED_THREAD);

   bool alreadyHaveVMAccess = ((vmThread->publicFlags & J9_PUBLIC_FLAGS_VM_ACCESS) != 0);
   bool haveAcquiredVMAccess = false;
   if (!alreadyHaveVMAccess)
      {
      if (0 == vmThread->javaVM->internalVMFunctions->internalTryAcquireVMAccessWithMask(vmThread, J9_PUBLIC_FLAGS_HALT_THREAD_ANY_NO_JAVA_SUSPEND))
         {
         haveAcquiredVMAccess = true;
         }
      }
   
   comp->setOutFile(jitdumpFile);

   TR_Debug *debug = comp->findOrCreateDebug();
   debug->setFile(jitdumpFile);

   TR::Options *options = comp->getOptions();
   options->setOption(TR_TraceAll);
   options->setOption(TR_TraceKnownObjectGraph);

   trfprintf(jitdumpFile, "<ilOfCrashedThread>\n");

   ILOfCrashedThreadParamenters p(vmThread, comp, jitdumpFile);

   U_32 flags = J9PORT_SIG_FLAG_MAY_RETURN |
                J9PORT_SIG_FLAG_SIGSEGV | J9PORT_SIG_FLAG_SIGFPE |
                J9PORT_SIG_FLAG_SIGILL  | J9PORT_SIG_FLAG_SIGBUS | J9PORT_SIG_FLAG_SIGTRAP;

   uintptr_t result = 0;
   j9sig_protect(traceILOfCrashedThreadProtected, static_cast<void *>(&p),
      jitDumpSignalHandler,
      vmThread, flags, &result);

   trfprintf(jitdumpFile, "</ilOfCrashedThread>\n");

   if (!alreadyHaveVMAccess && haveAcquiredVMAccess)
      {
      vmThread->javaVM->internalVMFunctions->internalReleaseVMAccess(vmThread);
      }
   }

static void
jitDumpRecompileWithTracing(J9VMThread *vmThread, J9Method *ramMethod, TR::CompilationInfo *compInfo, TR_Hotness optLevel, bool isProfilingCompile, TR::Options *optionsFromOriginalCompile, bool isAOTBody, void *oldStartPC, TR::FILE *jitdumpFile
   )
   {
   PORT_ACCESS_FROM_VMC(vmThread);
   J9Class *clazz = J9_CLASS_FROM_METHOD(ramMethod);
   J9ROMMethod *romMethod = J9_ROM_METHOD_FROM_RAM_METHOD(ramMethod);
   J9UTF8 *methName = J9ROMMETHOD_NAME(romMethod);
   J9UTF8 *methSig = J9ROMMETHOD_SIGNATURE(romMethod);
   J9UTF8 *className = J9ROMCLASS_CLASSNAME(clazz->romClass);

   j9nls_printf(PORTLIB, J9NLS_INFO | J9NLS_STDERR, J9NLS_DMP_JIT_RECOMPILING_METHOD,
         (UDATA)J9UTF8_LENGTH(className), J9UTF8_DATA(className),
         (UDATA)J9UTF8_LENGTH(methName), J9UTF8_DATA(methName),
         (UDATA)J9UTF8_LENGTH(methSig), J9UTF8_DATA(methSig));

   Trc_JIT_DumpCompilingMethod(vmThread, ramMethod, optLevel, oldStartPC);

   // The request to use a trace log gets passed to the compilation via the optimization plan. The options object
   // created before the compile is issued will use the trace log we provide to initialize IL tracing.
   TR_OptimizationPlan *plan = TR_OptimizationPlan::alloc(optLevel);
   if (NULL == plan)
      {
      j9nls_printf(PORTLIB, J9NLS_INFO | J9NLS_STDERR, J9NLS_DMP_JIT_OPTIMIZATION_PLAN);
      return;
      }

   plan->setInsertInstrumentation(isProfilingCompile);
   plan->setLogCompilation(jitdumpFile);

   trfprintf(jitdumpFile, "<recompilation>\n");

   // Set the VM state of the crashed thread so the diagnostic thread can use consume it
   compInfo->setVMStateOfCrashedThread(vmThread->omrVMThread->vmState);

   J9::JitDumpMethodDetails details(ramMethod, optionsFromOriginalCompile, isAOTBody);
   auto rc = compilationOK;
   auto queued = false;
   compInfo->compileMethod(vmThread, details, oldStartPC, TR_no, &rc, &queued, plan);

   trfprintf(jitdumpFile, "</recompilation rc=%d queued=%d>\n", rc, queued);

   if (!queued)
      {
      TR_OptimizationPlan::freeOptimizationPlan(plan);
      }
   }

uintptr_t
jitDumpSignalHandler(struct J9PortLibrary *portLibrary, uint32_t gpType, void *gpInfo, void *handler_arg)
   {
   PORT_ACCESS_FROM_VMC(reinterpret_cast<J9VMThread*>(handler_arg));
   j9nls_printf(PORTLIB, J9NLS_INFO | J9NLS_STDERR, J9NLS_DMP_JIT_RECURSIVE_CRASH);
   return J9PORT_SIG_EXCEPTION_RETURN;
   }

static UDATA
jitDumpStackFrameIterator(J9VMThread *currentThread, J9StackWalkState *walkState)
   {
   Trc_JIT_DumpWalkingFrame(currentThread);

   if (NULL != walkState->jitInfo)
      {
      auto *bodyInfo = reinterpret_cast<TR_PersistentJittedBodyInfo *>(walkState->jitInfo->bodyInfo);
      if (NULL != bodyInfo)
         {
            jitDumpRecompileWithTracing(
               currentThread,
               walkState->method,
               reinterpret_cast<TR::CompilationInfo*>(walkState->userData1),
               bodyInfo->getHotness(),
               bodyInfo->getIsProfilingBody(),
               NULL,
               bodyInfo->getIsAotedBody(),
               bodyInfo->getStartPCAfterPreviousCompile(),
               reinterpret_cast<TR::FILE*>(walkState->userData2)
            );
         }
      }

   return J9_STACKWALK_KEEP_ITERATING;
   }

omr_error_t
runJitdump(char *label, J9RASdumpContext *context, J9RASdumpAgent *agent)
   {
   PORT_ACCESS_FROM_VMC(context);

   J9VMThread *crashedThread = context->javaVM->internalVMFunctions->currentVMThread(context->javaVM);
   Trc_JIT_DumpStart(crashedThread);

#if defined(J9VM_OPT_JITSERVER)
   if (context && context->javaVM && context->javaVM->jitConfig)
      {
      J9JITConfig *jitConfig = context->javaVM->jitConfig;
      TR::CompilationInfo *compInfo = TR::CompilationInfo::get(context->javaVM->jitConfig);
      if (compInfo)
         {
         static char * isPrintJITServerMsgStats = feGetEnv("TR_PrintJITServerMsgStats");
         if (isPrintJITServerMsgStats)
            JITServerHelpers::printJITServerMsgStats(jitConfig, compInfo);
         if (feGetEnv("TR_PrintJITServerCHTableStats"))
            JITServerHelpers::printJITServerCHTableStats(jitConfig, compInfo);
         if (feGetEnv("TR_PrintJITServerIPMsgStats"))
            {
            if (compInfo->getPersistentInfo()->getRemoteCompilationMode() == JITServer::SERVER)
               {
               TR_J9VMBase * vmj9 = (TR_J9VMBase *)(TR_J9VMBase::get(context->javaVM->jitConfig, 0));
               JITServerIProfiler *iProfiler = (JITServerIProfiler *)vmj9->getIProfiler();
               iProfiler->printStats();
               }
            }
         }
      }
#endif

   char *crashedThreadName = getOMRVMThreadName(crashedThread->omrVMThread);
   j9nls_printf(PORTLIB, J9NLS_INFO | J9NLS_STDERR, J9NLS_DMP_OCCURRED_THREAD_NAME_ID, "JIT", crashedThreadName, crashedThread);

   J9JITConfig *jitConfig = crashedThread->javaVM->jitConfig;
   if (NULL == jitConfig)
      {
      j9nls_printf(PORTLIB, J9NLS_ERROR | J9NLS_STDERR, J9NLS_DMP_ERROR_IN_DUMP_STR, "JIT", "Could not locate J9JITConfig");
      return OMR_ERROR_INTERNAL;
      }
      
   TR::CompilationInfo *compInfo = TR::CompilationInfo::get(jitConfig);
   if (NULL == compInfo)
      {
      j9nls_printf(PORTLIB, J9NLS_ERROR | J9NLS_STDERR, J9NLS_DMP_ERROR_IN_DUMP_STR, "JIT", "Could not locate TR::CompilationInfo");
      return OMR_ERROR_INTERNAL;
      }

   TR_J9VMBase *frontendOfThread = TR_J9VMBase::get(jitConfig, crashedThread);
   if (NULL == frontendOfThread)
      {
      j9nls_printf(PORTLIB, J9NLS_ERROR | J9NLS_STDERR, J9NLS_DMP_ERROR_IN_DUMP_STR, "JIT", "Could not locate TR_J9VMBase");
      return OMR_ERROR_INTERNAL;
      }

   TR::Options *options = TR::Options::getCmdLineOptions();
   if (NULL == options)
      {
      j9nls_printf(PORTLIB, J9NLS_ERROR | J9NLS_STDERR, J9NLS_DMP_ERROR_IN_DUMP_STR, "JIT", "Could not locate TR::Options");
      return OMR_ERROR_INTERNAL;
      }

#if defined(J9VM_OPT_JITSERVER)
   if (compInfo->getPersistentInfo()->getRemoteCompilationMode() == JITServer::SERVER)
      {
      return OMR_ERROR_NONE;
      }
#endif /* defined(J9VM_OPT_JITSERVER) */
   
   // To avoid a deadlock, release compilation monitor until we are no longer holding it
   while (compInfo->getCompilationMonitor()->owned_by_self())
      {
      compInfo->releaseCompMonitor(crashedThread);
      }

   // Release other monitors as well. In particular CHTable and classUnloadMonitor must not be held.
   while (TR::MonitorTable::get()->getClassTableMutex()->owned_by_self())
      {
      frontendOfThread->releaseClassTableMutex(false);
      }

   TR::CompilationInfoPerThread *threadCompInfo = compInfo->getCompInfoForThread(crashedThread);
   if (NULL != threadCompInfo)
      {
      TR_MethodToBeCompiled *methodBeingCompiled = threadCompInfo->getMethodBeingCompiled();

      // If we are currently compiling a method, wake everyone waiting for it to compile
      if (NULL != methodBeingCompiled && NULL != methodBeingCompiled->getMonitor())
         {
         methodBeingCompiled->getMonitor()->enter();
         methodBeingCompiled->getMonitor()->notifyAll();
         methodBeingCompiled->getMonitor()->exit();
         
         j9nls_printf(PORTLIB, J9NLS_INFO | J9NLS_STDERR, J9NLS_DMP_JIT_NOTIFIED_WAITING_THREADS);
         }
      }

   compInfo->getPersistentInfo()->setDisableFurtherCompilation(true);

   TR::CompilationInfoPerThread *recompilationThreadInfo = compInfo->getCompilationInfoForDumpThread();
   if (NULL == recompilationThreadInfo)
      {
      j9nls_printf(PORTLIB, J9NLS_ERROR | J9NLS_STDERR, J9NLS_DMP_ERROR_IN_DUMP_STR, "JIT", "Could not locate the diagnostic thread info");
      return OMR_ERROR_INTERNAL;
      }

   auto *recompilationThread = recompilationThreadInfo->getCompilationThread();
   if (NULL == recompilationThread)
      {
      j9nls_printf(PORTLIB, J9NLS_ERROR | J9NLS_STDERR, J9NLS_DMP_ERROR_IN_DUMP_STR, "JIT", "Could not locate the diagnostic thread");
      return OMR_ERROR_INTERNAL;
      }

   recompilationThreadInfo->resumeCompilationThread();

   compInfo->acquireCompMonitor(crashedThread);
   compInfo->purgeMethodQueue(compilationFailure);
   compInfo->releaseCompMonitor(crashedThread);

   TR::FILE *jitdumpFile = trfopen(label, "ab", false);
   if (NULL == jitdumpFile)
      {
      j9nls_printf(PORTLIB, J9NLS_ERROR | J9NLS_STDERR, J9NLS_DMP_NO_OPEN_READ, label);
      return OMR_ERROR_INTERNAL;
      }
      
   trfprintf(jitdumpFile,
      "<?xml version=\"1.0\" standalone=\"no\"?>\n"
      "<jitDump>\n"
      );

   try
      {
      // Process user defined options first. The user can specify any method signature to be recompiled at an arbitrary
      // dump event. This is typically used for debugging purposes so we process this option first if it exists.
      if (NULL != agent->dumpOptions)
         {
         const char *indexOfDot = strchr(agent->dumpOptions, '.');
         const char *indexOfParen = strchr(agent->dumpOptions, '(');

         if ((NULL != indexOfDot) && (NULL != indexOfParen))
            {
            TR::VMAccessCriticalSection findUserMethod(TR_J9VMBase::get(jitConfig, crashedThread));

            const char *className = agent->dumpOptions;
            U_32 classNameLength = static_cast<U_32>(indexOfDot - agent->dumpOptions);
            const char *methodName = indexOfDot + 1;
            U_32 methodNameLength = static_cast<U_32>(indexOfParen - methodName);
            const char *methodSig = indexOfParen;
            U_32 methodSigLength = strlen(methodSig);

            J9MethodFromSignatureWalkState state;
            J9Method *userMethod = allMethodsFromSignatureStartDo(&state, context->javaVM, 0, className, classNameLength, methodName, methodNameLength, methodSig, methodSigLength);
            while (NULL != userMethod)
               {
               J9JITExceptionTable *metadata = jitConfig->jitGetExceptionTableFromPC(crashedThread, reinterpret_cast<UDATA>(userMethod->extra));
               if (NULL != metadata)
                  {
                  auto *bodyInfo = reinterpret_cast<TR_PersistentJittedBodyInfo *>(metadata->bodyInfo);
                  if (NULL != bodyInfo)
                     {
                     if (NULL == threadCompInfo)
                        {
                        // We are an application thread
                        jitDumpRecompileWithTracing(
                           crashedThread,
                           userMethod,
                           compInfo,
                           bodyInfo->getHotness(),
                           bodyInfo->getIsProfilingBody(),
                           NULL,
                           bodyInfo->getIsAotedBody(),
                           bodyInfo->getStartPCAfterPreviousCompile(),
                           jitdumpFile
                        );
                        }
                     else
                        {
                        // We are a compilation thread

                        // See comment below for the same call on why this is needed
                        TR::CompilationInfoPerThreadBase::UninterruptibleOperation jitDumpForCrashedCompilationThread(*threadCompInfo);

                        // See comment below for the same call on why this is needed
                        TR::VMAccessCriticalSection requestSynchronousCompilation(TR_J9VMBase::get(jitConfig, crashedThread));

                        jitDumpRecompileWithTracing(
                           crashedThread,
                           userMethod,
                           compInfo,
                           bodyInfo->getHotness(),
                           bodyInfo->getIsProfilingBody(),
                           NULL,
                           bodyInfo->getIsAotedBody(),
                           bodyInfo->getStartPCAfterPreviousCompile(),
                           jitdumpFile
                        );
                        }
                     }
                  }
               userMethod = allMethodsFromSignatureNextDo(&state);
               }
            allMethodsFromSignatureEndDo(&state);
            }
         }

      if (NULL == threadCompInfo)
         {
         // We are an application thread

         J9StackWalkState walkState;
         walkState.userData1 = compInfo;
         walkState.userData2 = jitdumpFile;
         walkState.walkThread = crashedThread;
         walkState.skipCount = 0;
         walkState.maxFrames = 16;
         walkState.flags = (
            J9_STACKWALK_VISIBLE_ONLY |
            J9_STACKWALK_SKIP_INLINES |
            J9_STACKWALK_COUNT_SPECIFIED |
            J9_STACKWALK_ITERATE_FRAMES
            );
         walkState.frameWalkFunction = jitDumpStackFrameIterator;
         walkState.errorMode = J9_STACKWALK_ERROR_MODE_IGNORE;

         crashedThread->javaVM->walkStackFrames(crashedThread, &walkState);
         }
      else
         {
         // We are a compilation thread

         // Printing the crashed thread trees or any similar operation on the crashed thread may result in having to
         // acquire VM access (ex. to get a class signature). Other VM events, such as VM shutdown or the GC unloading
         // classes may cause compilations to be interrupted. Because the crashed thread is not a diagnostic thread,
         // the call to print the crashed thread IL may get interrupted and the jitdump will be incomplete. We prevent
         // this from occuring by disallowing interruptions until we are done generating the jitdump.
         TR::CompilationInfoPerThreadBase::UninterruptibleOperation jitDumpForCrashedCompilationThread(*threadCompInfo);

         TR::Compilation *comp = threadCompInfo->getCompilation();
         if (NULL == comp)
            {
            j9nls_printf(PORTLIB, J9NLS_ERROR | J9NLS_STDERR, J9NLS_DMP_ERROR_IN_DUMP_STR, "JIT", "Could not locate the compilation object");
            trfclose(jitdumpFile);
            return OMR_ERROR_INTERNAL;
            }

         traceILOfCrashedThread(crashedThread, comp, jitdumpFile);

         TR_MethodToBeCompiled *methodBeingCompiled = threadCompInfo->getMethodBeingCompiled();
         if (NULL != methodBeingCompiled && methodBeingCompiled->getMethodDetails().isOrdinaryMethod())
            {
            j9nls_printf(PORTLIB, J9NLS_INFO | J9NLS_STDERR, J9NLS_DMP_JIT_ORDINARY_METHOD);

               {
               // The current thread is a compilation thread which may or may not currently hold VM access. The request
               // for recompilation to generate the jitdump will be performed synchronously for which the code path we
               // will take will be the same as if we were an application thread. We will take the synchronous request
               // path in `compileOnSeparateThread` which ends up releasing VM access prior to waiting on the diagnostic
               // thread to finish the queued compile. To avoid deadlocks we must acquire VM access here.
               TR::VMAccessCriticalSection requestSynchronousCompilation(TR_J9VMBase::get(jitConfig, crashedThread));

               // request the compilation
               jitDumpRecompileWithTracing(
                  crashedThread,
                  (J9Method *)(comp->getCurrentMethod()->getPersistentIdentifier()),
                  compInfo,
                  (TR_Hotness)comp->getOptLevel(),
                  comp->isProfilingCompilation(),
                  comp->getOptions(),
                  comp->compileRelocatableCode(),
                  methodBeingCompiled->_oldStartPC,
                  jitdumpFile
               );
               }
            }
         }
      }
   catch (const std::exception &e)
      {
      const char *exceptionName;

#if defined(J9ZOS390)
      // Compiling with -Wc,lp64 results in a crash on z/OS when trying
      // to call the what() virtual method of the exception.
      exceptionName = "std::exception";
#else
      exceptionName = e.what();
#endif

      trfprintf(jitdumpFile, "\n=== EXCEPTION THROWN (%s) ===\n", exceptionName);
      }

   trfprintf(jitdumpFile, "</jitDump>\n");
   trfflush(jitdumpFile);
   trfclose(jitdumpFile);

   compInfo->getPersistentInfo()->setDisableFurtherCompilation(false);

   return OMR_ERROR_NONE;
   }
