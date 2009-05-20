/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2009 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact:  Qt Software Information (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at qt-sales@nokia.com.
**
**************************************************************************/

#include "cdbdebugeventcallback.h"
#include "cdbdebugengine.h"
#include "cdbdebugengine_p.h"
#include "debuggermanager.h"
#include "breakhandler.h"
#include "cdbstacktracecontext.h"

enum { cppExceptionCode = 0xe06d7363, startupCompleteTrap = 0x406d1388,
       rpcServerUnavailableExceptionCode = 0x6ba };

#include <QtCore/QDebug>
#include <QtCore/QTextStream>

namespace Debugger {
namespace Internal {

//     CdbDebugEventCallbackBase
CdbDebugEventCallbackBase::CdbDebugEventCallbackBase()
{
}

STDMETHODIMP CdbDebugEventCallbackBase::QueryInterface(
    THIS_
    IN REFIID InterfaceId,
    OUT PVOID* Interface)
{
    *Interface = NULL;

    if (IsEqualIID(InterfaceId, __uuidof(IUnknown)) ||
        IsEqualIID(InterfaceId, __uuidof(IDebugOutputCallbacks)))  {
        *Interface = (IDebugOutputCallbacks *)this;
        AddRef();
        return S_OK;
    } else {
        return E_NOINTERFACE;
    }
}

STDMETHODIMP_(ULONG) CdbDebugEventCallbackBase::AddRef(THIS)
{
    // This class is designed to be static so
    // there's no true refcount.
    return 1;
}

STDMETHODIMP_(ULONG) CdbDebugEventCallbackBase::Release(THIS)
{
    // This class is designed to be static so
    // there's no true refcount.
    return 0;
}

STDMETHODIMP CdbDebugEventCallbackBase::Breakpoint(THIS_ __in PDEBUG_BREAKPOINT2)
{
    return S_OK;
}
STDMETHODIMP CdbDebugEventCallbackBase::Exception(
    THIS_
    __in PEXCEPTION_RECORD64,
    __in ULONG /* FirstChance */
    )
{
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::CreateThread(
    THIS_
    __in ULONG64 /* Handle */,
    __in ULONG64 /* DataOffset */,
    __in ULONG64 /* StartOffset */
    )
{
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::ExitThread(
    THIS_
    __in ULONG /* ExitCode */
    )
{
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::CreateProcess(
    THIS_
    __in ULONG64 /* ImageFileHandle */,
    __in ULONG64 /* Handle */,
    __in ULONG64 /* BaseOffset */,
    __in ULONG /* ModuleSize */,
    __in_opt PCWSTR /* ModuleName */,
    __in_opt PCWSTR /* ImageName */,
    __in ULONG /* CheckSum */,
    __in ULONG /* TimeDateStamp */,
    __in ULONG64 /* InitialThreadHandle */,
    __in ULONG64 /* ThreadDataOffset */,
    __in ULONG64 /* StartOffset */
    )
{    
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::ExitProcess(
    THIS_
    __in ULONG /* ExitCode */
    )
{
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::LoadModule(
    THIS_
    __in ULONG64 /* ImageFileHandle */,
    __in ULONG64 /* BaseOffset */,
    __in ULONG /* ModuleSize */,
    __in_opt PCWSTR /* ModuleName */,
    __in_opt PCWSTR /* ImageName */,
    __in ULONG /* CheckSum */,
    __in ULONG /* TimeDateStamp */
    )
{    
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::UnloadModule(
    THIS_
    __in_opt PCWSTR /* ImageBaseName */,
    __in ULONG64 /* BaseOffset */
    )
{
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::SystemError(
    THIS_
    __in ULONG /* Error */,
    __in ULONG /* Level */
    )
{
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::SessionStatus(
    THIS_
    __in ULONG /* Status */
    )
{
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::ChangeDebuggeeState(
    THIS_
    __in ULONG /* Flags */,
    __in ULONG64 /* Argument */
    )
{
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::ChangeEngineState(
    THIS_
    __in ULONG /* Flags */,
    __in ULONG64 /* Argument */
    )
{
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallbackBase::ChangeSymbolState(
    THIS_
    __in ULONG /* Flags */,
    __in ULONG64 /* Argument */
    )
{    
    return S_OK;
}

IDebugEventCallbacksWide *CdbDebugEventCallbackBase::getEventCallback(CIDebugClient *clnt)
{
    IDebugEventCallbacksWide *rc = 0;
    if (SUCCEEDED(clnt->GetEventCallbacksWide(&rc)))
        return rc;
    return 0;
}

// ---------- CdbDebugEventCallback

CdbDebugEventCallback::CdbDebugEventCallback(CdbDebugEngine* dbg) :
    m_pEngine(dbg)
{
}

STDMETHODIMP CdbDebugEventCallback::GetInterestMask(THIS_ __out PULONG mask)
{
    *mask = DEBUG_EVENT_CREATE_PROCESS  | DEBUG_EVENT_EXIT_PROCESS
            | DEBUG_EVENT_LOAD_MODULE   | DEBUG_EVENT_UNLOAD_MODULE
            | DEBUG_EVENT_CREATE_THREAD | DEBUG_EVENT_EXIT_THREAD
            | DEBUG_EVENT_BREAKPOINT
            | DEBUG_EVENT_EXCEPTION;
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallback::Breakpoint(THIS_ __in PDEBUG_BREAKPOINT2 Bp)
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;
    m_pEngine->m_d->handleBreakpointEvent(Bp);
    return S_OK;
}

// Simple exception formatting
void formatException(const EXCEPTION_RECORD64 *e, QTextStream &str)
{
    str.setIntegerBase(16);
    str << "\nException at 0x"  << e->ExceptionAddress
            <<  ", code: 0x" << e->ExceptionCode << ": ";
    switch (e->ExceptionCode) {
    case cppExceptionCode:
        str << "C++ exception";
        break;
    case startupCompleteTrap:
        str << "Startup complete";
        break;
    case EXCEPTION_ACCESS_VIOLATION: {
            const bool writeOperation = e->ExceptionInformation[0];
            str << (writeOperation ? "write" : "read")
                << " access violation at: 0x" << e->ExceptionInformation[1];
    }
        break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        str << "arrary bounds exceeded";
        break;
    case EXCEPTION_BREAKPOINT:
        str << "breakpoint";
        break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        str << "datatype misalignment";
        break;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        str << "floating point exception";
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        str << "division by zero";
        break;
    case EXCEPTION_FLT_INEXACT_RESULT:
        str << " floating-point operation cannot be represented exactly as a decimal fraction";
        break;
    case EXCEPTION_FLT_INVALID_OPERATION:
        str << "invalid floating-point operation";
        break;
    case EXCEPTION_FLT_OVERFLOW:
        str << "floating-point overflow";
        break;
    case EXCEPTION_FLT_STACK_CHECK:
        str << "floating-point operation stack over/underflow";
        break;
    case  EXCEPTION_FLT_UNDERFLOW:
        str << "floating-point UNDERFLOW";
        break;
    case  EXCEPTION_ILLEGAL_INSTRUCTION:
        str << "invalid instruction";
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        str << "page in error";
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        str << "integer division by zero";
        break;
    case EXCEPTION_INT_OVERFLOW:
        str << "integer overflow";
        break;
    case EXCEPTION_INVALID_DISPOSITION:
        str << "invalid disposition to exception dispatcher";
        break;
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        str << "attempt to continue execution after noncontinuable exception";
        break;
    case EXCEPTION_PRIV_INSTRUCTION:
        str << "privileged instruction";
        break;
    case EXCEPTION_SINGLE_STEP:
        str << "single step";
        break;
    case EXCEPTION_STACK_OVERFLOW:
        str << "stack_overflow";
        break;
    }
    str << ", flags=0x" << e->ExceptionFlags;
    if (e->ExceptionFlags == EXCEPTION_NONCONTINUABLE) {
        str << " (execution cannot be continued)";
    }
    str << "\n\n";
    str.setIntegerBase(10);
}

// Format exception with stacktrace in case of C++ exception
void formatException(const EXCEPTION_RECORD64 *e,
                     const QSharedPointer<CdbDumperHelper> &dumper,
                     QTextStream &str)
{
    formatException(e, str);
    if (e->ExceptionCode == cppExceptionCode) {
        QString errorMessage;
        ULONG currentThreadId = 0;
        dumper->comInterfaces()->debugSystemObjects->GetCurrentThreadId(&currentThreadId);
        if (CdbStackTraceContext *stc = CdbStackTraceContext::create(dumper, currentThreadId, &errorMessage)) {
            str << "at:\n";
            stc->format(str);
            str <<'\n';
            delete stc;
        }
    }
}

static bool isFatalException(LONG code)
{
    switch (code) {
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
    case startupCompleteTrap: // Mysterious exception at start of application
    case rpcServerUnavailableExceptionCode:
        return false;
    default:
        break;
    }
    return true;
}

STDMETHODIMP CdbDebugEventCallback::Exception(
    THIS_
    __in PEXCEPTION_RECORD64 Exception,
    __in ULONG /* FirstChance */
    )
{
    QString msg;
    {
        QTextStream str(&msg);
        formatException(Exception, m_pEngine->m_d->m_dumper, str);
    }
    const bool fatal = isFatalException(Exception->ExceptionCode);
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << "\nex=" << Exception->ExceptionCode << " fatal=" << fatal << msg;
    m_pEngine->m_d->m_debuggerManagerAccess->showApplicationOutput(msg);
    if (fatal)
        m_pEngine->m_d->notifyCrashed();
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallback::CreateThread(
    THIS_
    __in ULONG64 Handle,
    __in ULONG64 DataOffset,
    __in ULONG64 StartOffset
    )
{
    Q_UNUSED(Handle)
    Q_UNUSED(DataOffset)
    Q_UNUSED(StartOffset)
    if (debugCDB)
        qDebug() << Q_FUNC_INFO;
    m_pEngine->m_d->updateThreadList();
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallback::ExitThread(
    THIS_
    __in ULONG ExitCode
    )
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << ExitCode;
    // @TODO: It seems the terminated thread is still in the list...
    m_pEngine->m_d->updateThreadList();
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallback::CreateProcess(
    THIS_
    __in ULONG64 ImageFileHandle,
    __in ULONG64 Handle,
    __in ULONG64 BaseOffset,
    __in ULONG ModuleSize,
    __in_opt PCWSTR ModuleName,
    __in_opt PCWSTR ImageName,
    __in ULONG CheckSum,
    __in ULONG TimeDateStamp,
    __in ULONG64 InitialThreadHandle,
    __in ULONG64 ThreadDataOffset,
    __in ULONG64 StartOffset
    )
{
    Q_UNUSED(ImageFileHandle)
    Q_UNUSED(BaseOffset)
    Q_UNUSED(ModuleSize)
    Q_UNUSED(ModuleName)
    Q_UNUSED(ImageName)
    Q_UNUSED(CheckSum)
    Q_UNUSED(TimeDateStamp)
    Q_UNUSED(ThreadDataOffset)
    Q_UNUSED(StartOffset)
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << ModuleName;
    m_pEngine->m_d->processCreatedAttached(Handle, InitialThreadHandle);
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallback::ExitProcess(
    THIS_
    __in ULONG ExitCode
    )
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << ExitCode;
    m_pEngine->processTerminated(ExitCode);
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallback::LoadModule(
    THIS_
    __in ULONG64 ImageFileHandle,
    __in ULONG64 BaseOffset,
    __in ULONG ModuleSize,
    __in_opt PCWSTR ModuleName,
    __in_opt PCWSTR ImageName,
    __in ULONG CheckSum,
    __in ULONG TimeDateStamp
    )
{
    Q_UNUSED(ImageFileHandle)
    Q_UNUSED(BaseOffset)
    Q_UNUSED(ModuleName)
    Q_UNUSED(ModuleSize)
    Q_UNUSED(ImageName)
    Q_UNUSED(CheckSum)
    Q_UNUSED(TimeDateStamp)
    if (debugCDB > 1)
        qDebug() << Q_FUNC_INFO << ModuleName;
    m_pEngine->m_d->handleModuleLoad(QString::fromUtf16(ModuleName));
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallback::UnloadModule(
    THIS_
    __in_opt PCWSTR ImageBaseName,
    __in ULONG64 BaseOffset
    )
{
    Q_UNUSED(ImageBaseName)
    Q_UNUSED(BaseOffset)
    if (debugCDB > 1)
        qDebug() << Q_FUNC_INFO << ImageBaseName;
    m_pEngine->m_d->updateModules();
    return S_OK;
}

STDMETHODIMP CdbDebugEventCallback::SystemError(
    THIS_
    __in ULONG Error,
    __in ULONG Level
    )
{
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << Error << Level;
    return S_OK;
}

// -----------ExceptionLoggerEventCallback
CdbExceptionLoggerEventCallback::CdbExceptionLoggerEventCallback(const QString &logPrefix,
                                                           IDebuggerManagerAccessForEngines *access) :
    m_logPrefix(logPrefix),
    m_access(access)
{
}

STDMETHODIMP CdbExceptionLoggerEventCallback::GetInterestMask(THIS_ __out PULONG mask)
{
    *mask = DEBUG_EVENT_EXCEPTION;
    return S_OK;
}

STDMETHODIMP CdbExceptionLoggerEventCallback::Exception(
    THIS_
    __in PEXCEPTION_RECORD64 Exception,
    __in ULONG /* FirstChance */
    )
{
    m_exceptionMessages.push_back(QString());
    {
        QTextStream str(&m_exceptionMessages.back());
        formatException(Exception, str);
    }
    if (debugCDB)
        qDebug() << Q_FUNC_INFO << '\n' << m_exceptionMessages.back();
    m_access->showDebuggerOutput(m_logPrefix, m_exceptionMessages.back());
    return S_OK;
}

// -----------IgnoreDebugEventCallback
IgnoreDebugEventCallback::IgnoreDebugEventCallback()
{
}

STDMETHODIMP IgnoreDebugEventCallback::GetInterestMask(THIS_ __out PULONG mask)
{
    *mask = 0;
    return S_OK;
}

// --------- EventCallbackRedirector
EventCallbackRedirector::EventCallbackRedirector(CIDebugClient *client,  IDebugEventCallbacksWide *cb) :
        m_client(client),
        m_oldCb(CdbDebugEventCallbackBase::getEventCallback(client))
{
    client->SetEventCallbacksWide(cb);
}

EventCallbackRedirector::~EventCallbackRedirector()
{
    m_client->SetEventCallbacksWide(m_oldCb);
}

} // namespace Internal
} // namespace Debugger
